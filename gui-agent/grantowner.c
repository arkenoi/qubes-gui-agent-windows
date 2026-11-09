/*
 * grantowner - see grantowner.h. One thread owns a xeniface handle; nothing else touches it.
 */

#include <windows.h>
#include <stdlib.h>
#include "grantowner.h"
#include <log.h>

// NO TIMEOUT, DELIBERATELY. A caller waits for the worker exactly as long as it used to wait for
// the ioctl it made itself, so this change costs nothing and adds no new failure mode: it moves
// WHICH THREAD issues the ioctl and nothing else. Bounding the caller's wait would be an
// improvement on the status quo rather than a relocation of it - it needs somewhere to put the
// buffers of a request still in flight, which means a new leak path on every call site - and Jev
// rated the bound a guard knob (0.81), i.e. the owner's call. It is left for the owner to decide,
// separately from this fix.

typedef enum { GO_OP_GRANT = 1, GO_OP_REVOKE, GO_OP_CLOSE } GO_OP;

struct _GRANT_OWNER
{
    HANDLE Thread;
    HANDLE Request;        // caller -> worker
    HANDLE Done;           // worker -> caller
    CRITICAL_SECTION Lock; // serialises callers; the worker never takes it
    // Set under Lock by GoClose BEFORE it posts GO_OP_CLOSE, because the worker RETURNS on that op.
    // A post after it would set Request with nobody left to answer: the caller would wait on Done
    // forever holding Lock, and every other caller would then block on Lock forever. Checked by the
    // two public entry points under the same lock, so a refusal is a failed call, never a park.
    volatile LONG Closing;
    PXENCONTROL_CONTEXT Xc;
    const WCHAR* Name;

    volatile GO_OP Op;
    volatile DWORD Status;
    // GO_OP_GRANT arguments and results
    USHORT RemoteDomain;
    PVOID Address;
    ULONG NumberPages;
    ULONG NotifyOffset;
    ULONG NotifyPort;
    ULONG Flags;
    PVOID* SharedAddress;
    ULONG* References;
    // GO_OP_REVOKE argument
    PVOID Revoke;

    // Set once the worker has opened the handle, so GoOpen can report a failed open.
    HANDLE Ready;
    DWORD OpenStatus;
    XENCONTROL_LOGGER* Logger;
    XENCONTROL_LOG_LEVEL Level;
};

static DWORD WINAPI GoWorker(LPVOID param)
{
    GRANT_OWNER* o = (GRANT_OWNER*)param;

    o->OpenStatus = XcOpen(o->Logger, &o->Xc);
    if (o->OpenStatus == ERROR_SUCCESS && !o->Xc)
        o->OpenStatus = ERROR_INVALID_HANDLE;   // XcOpen said yes and handed back nothing
    if (o->OpenStatus == ERROR_SUCCESS)
        XcSetLogLevel(o->Xc, o->Level);
    SetEvent(o->Ready);
    if (o->OpenStatus != ERROR_SUCCESS)
        return o->OpenStatus;

    LogDebug("GRANTOWNER '%s' thread %lu owns its xeniface handle", o->Name, GetCurrentThreadId());

    for (;;)
    {
        WaitForSingleObject(o->Request, INFINITE);
        switch (o->Op)
        {
        case GO_OP_GRANT:
            o->Status = XcGnttabPermitForeignAccess2(o->Xc, o->RemoteDomain, o->Address,
                                                     o->NumberPages, o->NotifyOffset,
                                                     o->NotifyPort, o->Flags,
                                                     o->SharedAddress, o->References);
            break;
        case GO_OP_REVOKE:
            o->Status = XcGnttabRevokeForeignAccess(o->Xc, o->Revoke);
            break;
        case GO_OP_CLOSE:
            // EVERY grant this owner made dies with this call, by the xeniface contract: the driver
            // completes a permit IRP when the grant is revoked OR THE CALLING THREAD TERMINATES,
            // and that thread is this one. So this runs last and the thread returns straight after.
            XcClose(o->Xc);
            o->Xc = NULL;
            o->Status = ERROR_SUCCESS;
            SetEvent(o->Done);
            return ERROR_SUCCESS;
        default:
            o->Status = ERROR_INVALID_FUNCTION;
            break;
        }
        SetEvent(o->Done);
    }
}

// Post one request and wait for it. THE CALLER MUST ALREADY HOLD o->Lock: there is one request
// slot, and the arguments are written into it before this is called, so releasing the lock in
// between would let a second caller overwrite the request this one is about to post.
static DWORD GoCallLocked(IN GRANT_OWNER* o, IN GO_OP op)
{
    HANDLE waits[2];
    DWORD w;

    o->Op = op;
    ResetEvent(o->Done);
    SetEvent(o->Request);
    // THE WORKER'S DEATH IS AN EXIT, AND IT IS NOT A BOUND. The header's deferral of a TIMEOUT to
    // the owner stands untouched; nothing here times anything out. What it restores is the
    // invariant the direct ioctl had and this rendezvous silently dropped: the wait ended when the
    // WORK ended. Posting to another thread couples it to that thread still being alive to answer,
    // and nothing checked that - so if the worker exits without setting Done (it has already
    // RETURNED after GO_OP_CLOSE, or it died), Done is never set, this parks FOREVER with Lock
    // HELD, and every later caller then blocks on Lock forever. One stuck grant becomes the whole
    // agent, which is why "it moves WHICH THREAD issues the ioctl and nothing else" was not the
    // whole story.
    // Thread is signalled when the worker exits, so a wait on both cannot park. GO_OP_CLOSE signals
    // both; with bWaitAll FALSE the LOWEST index wins, so Done still reports the real status there.
    waits[0] = o->Done;
    waits[1] = o->Thread;
    w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (w != WAIT_OBJECT_0)
    {
        LogError("GRANTOWNER '%s' worker is gone without answering op %d (wait %lu) - failing the "
                 "call instead of waiting for an answer that cannot come", o->Name, (int)op, w);
        return ERROR_NO_SUCH_DEVICE;
    }
    return o->Status;
}

DWORD GoOpen(IN const WCHAR* name, IN XENCONTROL_LOGGER* logger, IN XENCONTROL_LOG_LEVEL level,
             OUT GRANT_OWNER** owner)
{
    GRANT_OWNER* o;
    DWORD status;

    *owner = NULL;
    o = (GRANT_OWNER*)calloc(1, sizeof(*o));
    if (!o)
        return ERROR_OUTOFMEMORY;

    o->Name = name;
    o->Logger = logger;
    o->Level = level;
    InitializeCriticalSection(&o->Lock);
    o->Request = CreateEvent(NULL, FALSE, FALSE, NULL);   // auto-reset
    o->Done = CreateEvent(NULL, TRUE, FALSE, NULL);       // manual-reset, cleared before each post
    o->Ready = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!o->Request || !o->Done || !o->Ready)
    {
        status = GetLastError();
        goto fail;
    }

    o->Thread = CreateThread(NULL, 0, GoWorker, o, 0, NULL);
    if (!o->Thread)
    {
        status = GetLastError();
        goto fail;
    }

    // The open happens on the worker, so the handle belongs to it from the first ioctl onwards.
    WaitForSingleObject(o->Ready, INFINITE);
    if (o->OpenStatus != ERROR_SUCCESS)
    {
        status = o->OpenStatus;
        WaitForSingleObject(o->Thread, INFINITE);   // it returns immediately on a failed open
        CloseHandle(o->Thread);
        goto fail;
    }

    *owner = o;
    return ERROR_SUCCESS;

fail:
    if (o->Request) CloseHandle(o->Request);
    if (o->Done) CloseHandle(o->Done);
    if (o->Ready) CloseHandle(o->Ready);
    DeleteCriticalSection(&o->Lock);
    free(o);
    return status ? status : ERROR_GEN_FAILURE;
}

DWORD GoPermitForeignAccess(IN GRANT_OWNER* owner, IN USHORT remoteDomain, IN PVOID address,
                            IN ULONG numberPages, IN ULONG notifyOffset, IN ULONG notifyPort,
                            IN ULONG flags, OUT PVOID* sharedAddress, OUT ULONG* references)
{
    DWORD status;

    if (!owner)
        return ERROR_INVALID_HANDLE;
    EnterCriticalSection(&owner->Lock);
    if (owner->Closing)
    {
        // Read UNDER the lock, which is what makes it sound: GoClose sets it holding this lock, so a
        // caller that got in first is already past here and finishes before the close posts.
        LeaveCriticalSection(&owner->Lock);
        return ERROR_NO_SUCH_DEVICE;
    }
    owner->RemoteDomain = remoteDomain;
    owner->Address = address;
    owner->NumberPages = numberPages;
    owner->NotifyOffset = notifyOffset;
    owner->NotifyPort = notifyPort;
    owner->Flags = flags;
    owner->SharedAddress = sharedAddress;
    owner->References = references;
    status = GoCallLocked(owner, GO_OP_GRANT);
    LeaveCriticalSection(&owner->Lock);
    return status;
}

DWORD GoRevokeForeignAccess(IN GRANT_OWNER* owner, IN PVOID sharedAddress)
{
    DWORD status;

    if (!owner)
        return ERROR_INVALID_HANDLE;
    EnterCriticalSection(&owner->Lock);
    if (owner->Closing)
    {
        LeaveCriticalSection(&owner->Lock);
        return ERROR_NO_SUCH_DEVICE;
    }
    owner->Revoke = sharedAddress;
    status = GoCallLocked(owner, GO_OP_REVOKE);
    LeaveCriticalSection(&owner->Lock);
    return status;
}

void GoClose(IN OUT GRANT_OWNER** owner)
{
    GRANT_OWNER* o = *owner;

    if (!o)
        return;
    *owner = NULL;

    EnterCriticalSection(&o->Lock);
    // Before the post, because the worker returns on GO_OP_CLOSE: from here on the rendezvous can
    // no longer answer anybody, so a later grant or revoke must be REFUSED rather than posted.
    o->Closing = 1;
    (void)GoCallLocked(o, GO_OP_CLOSE);
    LeaveCriticalSection(&o->Lock);

    WaitForSingleObject(o->Thread, INFINITE);
    CloseHandle(o->Thread);
    CloseHandle(o->Request);
    CloseHandle(o->Done);
    CloseHandle(o->Ready);
    DeleteCriticalSection(&o->Lock);
    free(o);
}
