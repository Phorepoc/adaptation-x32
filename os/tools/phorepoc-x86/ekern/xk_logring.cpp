// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_logring.cpp -- every byte the kernel logs (DebugOutputChar), kept from the first one in a 256 KB ring for the GPD
// Win 2 rig's readers: the network link (usbhost/xk_netlog.cpp, NETLOG=1) and the disk log (xk_disklog.cpp,
// DISKLOG=1). One writer at a time (interrupts off around the store; a CPU spins until the earlier reservations are
// stored), so a reader may take everything below X64LogCommitted() and nothing else; each reader keeps its own position.
#include <kernel/kernel.h>
#include "xk_logring.h"

static volatile TUint8 gRing[KLogRing];
static volatile TUint32 gHead, gCommit;
static TUint32 gBootId;

void X64LogChar(TUint aChar)
	{
	TUint64 flags;
	__asm__ __volatile__("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
	const TUint32 at = __atomic_fetch_add(&gHead, 1, __ATOMIC_RELAXED);
	gRing[at % KLogRing] = (TUint8)aChar;
	for (TInt i = 0; i < 1000000 && __atomic_load_n(&gCommit, __ATOMIC_ACQUIRE) != at; ++i)
		__asm__ __volatile__("pause");
	__atomic_store_n(&gCommit, at + 1, __ATOMIC_RELEASE);
	if (flags & 0x200)
		__asm__ __volatile__("sti" : : : "memory");
	}

TUint32 X64LogCommitted()
	{
	return __atomic_load_n(&gCommit, __ATOMIC_ACQUIRE);
	}

TUint8 X64LogAt(TUint32 aIndex)
	{
	return gRing[aIndex % KLogRing];
	}

TUint32 X64LogBootId()
	{
	if (!gBootId)
		{
		TUint32 lo, hi;
		__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
		gBootId = (lo ^ (hi << 16) ^ 0x50484C31u) | 1;
		}
	return gBootId;
	}
