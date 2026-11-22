// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_logring.h -- the kernel log ring (xk_logring.cpp) for the rig's readers.
#ifndef XK_LOGRING_H
#define XK_LOGRING_H
const TUint32 KLogRing = 256 * 1024;
void X64LogChar(TUint aChar);			// DebugOutputChar's copy
TUint32 X64LogCommitted();				// bytes logged so far (an index, wraps at 4 GB); [n - KLogRing, n) is in the ring
TUint8 X64LogAt(TUint32 aIndex);
TUint32 X64LogBootId();					// random per boot, never 0
#endif
