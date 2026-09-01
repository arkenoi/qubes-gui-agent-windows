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

struct _CAPTURE_CONTEXT; // capture.h; only a pointer is needed here

DWORD HandleVersion(void);
DWORD HandleXconf(void);

// The BODY of the two input handlers, split out from the vchan receive so something other
// than the vchan reader can drive them: dragsim.c plays dom0's input half to reproduce a
// guest-native drag without a human hand (see dragsim.h for why that was necessary).
// x/y are WINDOW-RELATIVE, exactly as msg_motion/msg_button carry them.
DWORD ProcessMotionEvent(IN HWND window, IN int x, IN int y, IN BOOL isHint);
DWORD ProcessButtonEvent(IN HWND window, IN int x, IN int y, IN unsigned int button,
    IN unsigned int type);
// capture may be NULL (no active capture context); it is only used to revoke parked
// screen grants when the daemon acks the window-0 MSG_WINDOW_DUMP (A6).
DWORD HandleServerData(BOOL replyToMessages, IN OUT struct _CAPTURE_CONTEXT* capture OPTIONAL,
    OUT BOOL* screenDestroyed);
