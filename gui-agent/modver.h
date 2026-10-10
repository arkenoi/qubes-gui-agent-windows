/*
 * modver.h - the running image's own file version, for the technical line's "build <m.m.p.b>" (Win32 only).
 *
 * WHY (owner, 2026-10-10: "i see win11-acc error on screen, but since dom0 toasts do not have timestamps, i
 * cannot figure out if it is a botched fix or control reproduction run"): a dom0 notification must name the
 * BUILD that produced it. Not a timestamp - these guests come up about three hours ahead until a boot task
 * corrects the clock (+10803 s measured the same day), so a guest time in a user-facing line would be wrong in
 * a way that looks authoritative (Jev: version-only 1.00, a clock would mislead 0.95). The source is the one
 * windows-utils' LogInit already prints at every start as "Module version: 4.3.36.915": the VS_FIXEDFILEINFO
 * of the running image (GetModuleFileName -> GetFileVersionInfo -> VerQueryValue "\\"), spelled the same way,
 * so the toast and the agent log agree at a glance. The fourth part is the release run's build number
 * (tools/stamp-version.ps1 BUILD_REV = github.run_number), which is what separates a fix build from its
 * control. windows-utils keeps its reader static, so the same four calls live here, shared by notifyerr.c
 * (the agent) and notifhost.cpp; the plain-C test layer never includes this file and pins a value instead.
 */
#ifndef QWT_MODVER_H
#define QWT_MODVER_H

#ifdef _WIN32
#include <windows.h>
#include <winver.h>   /* GetFileVersionInfo/VerQueryValue; version.lib is pulled in here, not by any vcxproj */
#include <stdio.h>
#include <stdlib.h>
#pragma comment(lib, "version.lib")

/* "M.m.p.b" of this process's image into out; 1 = read, 0 = not readable (out = "", the line says unknown). */
static int QerrModuleVersion(char* out, size_t cap)
{
    WCHAR path[MAX_PATH];
    DWORD handle = 0, size;
    void* buf;
    VS_FIXEDFILEINFO* ffi = NULL;
    UINT ffiLen = 0;
    int ok = 0;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return 0;
    size = GetFileVersionInfoSizeW(path, &handle);
    if (!size) return 0;
    buf = malloc(size);
    if (!buf) return 0;
    if (GetFileVersionInfoW(path, 0, size, buf) && VerQueryValueW(buf, L"\\", (LPVOID*)&ffi, &ffiLen) &&
        ffi && ffiLen >= sizeof(*ffi))
    {
        int n = snprintf(out, cap, "%u.%u.%u.%u",
                         (unsigned)HIWORD(ffi->dwFileVersionMS), (unsigned)LOWORD(ffi->dwFileVersionMS),
                         (unsigned)HIWORD(ffi->dwFileVersionLS), (unsigned)LOWORD(ffi->dwFileVersionLS));
        ok = (n > 0 && (size_t)n < cap) ? 1 : 0;
        if (!ok) out[0] = 0;
    }
    free(buf);
    return ok;
}
#endif /* _WIN32 */

#endif /* QWT_MODVER_H */
