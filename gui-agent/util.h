/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#pragma once
#include <windef.h>

extern DWORD g_DisableCursor;

ULONG HideCursors(void);
ULONG EnableEffects(void);    // undo DisableEffects when the guest leaves seamless
ULONG DisableEffects(void);
HANDLE CreateNamedEvent(IN const WCHAR *name); // returns NULL on failure
HANDLE CreateNamedMailslot(IN const WCHAR *name); // returns NULL on failure
ULONG IncreaseProcessWorkingSetSize(IN SIZE_T minimumSize, IN SIZE_T maximumSize);
ULONG AttachToInputDesktop(void);

/* HOW an activation request ended. Named rather than a BOOL because the two successes are not the
 * same event: PLAIN means Windows let us have it, ATTACHED means we had to share an input queue
 * with the foreground thread to be allowed, and a path that must stay cheap wants to know which. */
typedef enum {
    FOREGROUND_REFUSED = 0,   /* the foreground lock held and the window did NOT come forward */
    FOREGROUND_PLAIN,         /* SetForegroundWindow succeeded on its own */
    FOREGROUND_ATTACHED       /* succeeded only inside an AttachThreadInput window */
} FOREGROUND_HOW;

/* GIVE A WINDOW THE FOREGROUND, the documented way through the foreground lock.
 *
 * ONE BODY, TWO CALLERS, on purpose: HandleFocus applies dom0's focus decisions with it, and the
 * hidden-Start dismissal closes a light-dismiss surface by returning activation elsewhere with it.
 * Those are the same operation on the same lock, and two copies of it would drift - the attach
 * fallback in particular is easy to write without its detach.
 *
 * `how` is optional. The caller logs: this decides nothing and says nothing, so the two callers
 * keep their own wording and their own levels.
 *
 * NOT BOUNDED, and the caller is told here rather than discovering it: SetForegroundWindow has no
 * timeout, so a wedged target application can block the calling thread for as long as it is wedged.
 * This has always been true of the dom0-focus path on the vchan thread; any new caller inherits it.
 * Never pass NULL - an activation of nothing is not a request, and the callers' predicates are
 * responsible for never producing one. */
BOOL ApplyForeground(IN HWND window, OUT FOREGROUND_HOW *how);
