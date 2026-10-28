/*
 * grantowner.h - ONE thread owns a xeniface handle, and nothing else touches it.
 *
 * WHY THIS EXISTS. The vendored xeniface header says of
 * IOCTL_XENIFACE_GNTTAB_PERMIT_FOREIGN_ACCESS_V2, verbatim: "This IOCTL must be asynchronous. The
 * driver doesn't complete the request until the grant is explicitly revoked or the calling thread
 * terminates." So a granting thread carries a PENDING IRP for as long as the grant lives, and the
 * grant dies with that thread.
 *
 * A core taken from a wedged gui-agent (findings, prevent2) showed one thread holding TWO
 * outstanding IRPs on the same xeniface DeviceObject: the pending grant above, and a synchronous
 * IOCTL_XENIFACE_EVTCHN_CLOSE that never returned. That thread was the MAIN thread, and it still
 * is: perwindow.c grants per-window slabs from PwAttachWindow (main.c:6824, main.c:9773) and NEVER
 * revokes them, so those IRPs are pinned to main for the life of the process, while main also
 * calls libvchan_close - which is where that evtchn close comes from.
 *
 * THE INVARIANT, stated once so it can be checked by reading one file: every ioctl on an owned
 * handle - open, grant, revoke, close - is issued on that handle's own thread, and that thread
 * does nothing else. Callers post a request and wait.
 *
 * NOT A FIX FOR THE 2026-10-28 win10-acc WEDGE, which is a different shape (the whole domain stops
 * accruing cputime and the stall is below the guest). This removes one guest-side way of pinning a
 * thread inside a non-returning xeniface ioctl, which is a defect on its own merits.
 */
#pragma once
#include <windows.h>
#include <xencontrol.h>

// An owned handle. Opaque to callers; one worker thread per instance.
typedef struct _GRANT_OWNER GRANT_OWNER;

// Start a worker and open a xeniface handle ON IT. Returns ERROR_SUCCESS and *owner on success.
// `name` is for log lines only.
DWORD GoOpen(IN const WCHAR* name, IN XENCONTROL_LOGGER* logger, IN XENCONTROL_LOG_LEVEL level,
             OUT GRANT_OWNER** owner);

// XcGnttabPermitForeignAccess2 on the owner's thread. Same arguments, same return, same blocking
// behaviour as calling it directly - only the issuing thread changes.
DWORD GoPermitForeignAccess(IN GRANT_OWNER* owner, IN USHORT remoteDomain, IN PVOID address,
                            IN ULONG numberPages, IN ULONG notifyOffset, IN ULONG notifyPort,
                            IN ULONG flags, OUT PVOID* sharedAddress, OUT ULONG* references);

// XcGnttabRevokeForeignAccess on the owner's thread.
DWORD GoRevokeForeignAccess(IN GRANT_OWNER* owner, IN PVOID sharedAddress);

// Close the handle ON THE OWNER'S THREAD and stop the worker. Every grant this owner made dies
// here, by the header contract above, so nothing may call this while a grant is still wanted.
void GoClose(IN OUT GRANT_OWNER** owner);
