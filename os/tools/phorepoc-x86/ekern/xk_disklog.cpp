// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_disklog.cpp -- the GPD Win 2 rig's disk log (DISKLOG=1): the kernel log ring (xk_logring.cpp) written to the SSD's
// "phorepoc-log" GPT partition (type 5048524c-4f47-4c4f-8750-484f5245504c, made once from Linux with sgdisk:
// tools/gpd-rig/setup-linux.sh), so every boot -- also one ended by holding the power button -- leaves its log for the
// next Linux boot (tools/gpd-rig/phorepoc-collect). From the moment the AHCI disk answers, every second (a DfcQue0
// DFC, an NTimer) and once more on a kernel crash (X64DiskLogCrashFlush, from X64_CrashWho, polled, no lock).
//
// Layout: sector 0 = the header (below), sectors 1.. = the log, byte i of the boot at data offset i % data size (64 MB
// - 512 bytes: no wrap in practice). Each boot starts again at 0 with a new boot id; the header is written last.
//   "PHDLOG1\0" | boot id u32 | bytes u32 | data sectors u32 | flushes u32 | crashed u32      (little endian)
#include <kernel/kernel.h>
#include "xk_logring.h"

TInt X64DiskRaw(TBool aWrite, TUint32 aLba, TInt aCount, TUint8* aBuf, TBool aLocked);	// xk_ata.cpp

// the type GUID in GPT's on-disk byte order (the first three fields little endian)
static const TUint8 KLogType[16] = { 0x4c, 0x52, 0x48, 0x50, 0x47, 0x4f, 0x4f, 0x4c,
									 0x87, 0x50, 0x48, 0x4f, 0x52, 0x45, 0x50, 0x4c };
const TInt KChunk = 64;										// sectors a transfer (32 KB)

static TUint32 TheFirst, TheSectors;						// the partition (LBA, data sectors after the header); 0 = not found
static TBool TheLookedUp;
static TUint32 TheFlushed, TheFlushes;						// log bytes on the disk
static TUint8* TheBuf;										// KChunk sectors
static NTimer* TheTimer;
static TDfc* TheDfc;

static void Put32(TUint8* p, TUint32 v) { p[0] = (TUint8)v; p[1] = (TUint8)(v >> 8); p[2] = (TUint8)(v >> 16); p[3] = (TUint8)(v >> 24); }
static TUint32 Get32(const TUint8* p) { return p[0] | (TUint32)p[1] << 8 | (TUint32)p[2] << 16 | (TUint32)p[3] << 24; }

// the GPT: header at LBA 1, entries from its partition-entry LBA
static TBool Find()
	{
	if (X64DiskRaw(EFalse, 1, 1, TheBuf, ETrue) != KErrNone || memcompare(TheBuf, 8, (const TUint8*)"EFI PART", 8) != 0)
		return EFalse;
	const TUint32 elba = Get32(TheBuf + 72), count = Get32(TheBuf + 80), esize = Get32(TheBuf + 84);
	if (esize < 128 || esize > 512 || count > 256)
		return EFalse;
	const TUint32 perSector = 512 / esize;
	for (TUint32 i = 0; i < count; i += perSector)
		{
		if (X64DiskRaw(EFalse, elba + i / perSector, 1, TheBuf, ETrue) != KErrNone)
			return EFalse;
		for (TUint32 k = 0; k < perSector; ++k)
			{
			const TUint8* e = TheBuf + k * esize;
			if (memcompare(e, 16, KLogType, 16) != 0)
				continue;
			const TUint32 first = Get32(e + 32), last = Get32(e + 40);
			if (Get32(e + 36) || Get32(e + 44) || last <= first + 8)
				return EFalse;								// above 2 TB, or too small
			TheFirst = first;
			TheSectors = last - first;						// sectors 1 .. last-first after the header
			return ETrue;
			}
		}
	return EFalse;
	}

static void WriteHeader(TBool aCrash, TBool aLocked)
	{
	memclr(TheBuf, 512);
	memcpy(TheBuf, "PHDLOG1", 8);
	Put32(TheBuf + 8, X64LogBootId());
	Put32(TheBuf + 12, TheFlushed);
	Put32(TheBuf + 16, TheSectors);
	Put32(TheBuf + 20, ++TheFlushes);
	Put32(TheBuf + 24, aCrash);
	X64DiskRaw(ETrue, TheFirst, 1, TheBuf, aLocked);
	}

// write the sectors holding [TheFlushed, committed): the last one again each time (it is partial)
static void Flush(TBool aCrash, TBool aLocked)
	{
	const TUint32 end = X64LogCommitted();
	if (end - TheFlushed > KLogRing)						// fell behind the ring: what is gone is gone
		TheFlushed = end - KLogRing;
	if (end == TheFlushed && !aCrash)
		return;
	const TUint32 data = TheSectors * 512;
	TUint32 at = TheFlushed & ~511u;						// sector-aligned start
	while (at < end)
		{
		TInt n = 0;
		const TUint32 sector = (at % data) / 512;
		while (n < KChunk && at + n * 512 < end && sector + n < TheSectors)
			{
			TUint8* s = TheBuf + n * 512;
			for (TUint32 i = 0; i < 512; ++i)
				{
				const TUint32 b = at + n * 512 + i;
				s[i] = (b < end && end - b <= KLogRing) ? X64LogAt(b) : 0;
				}
			++n;
			}
		if (X64DiskRaw(ETrue, TheFirst + 1 + sector, n, TheBuf, aLocked) != KErrNone)
			return;
		at += n * 512;
		}
	TheFlushed = end;
	WriteHeader(aCrash, aLocked);
	}

static void TickDfc(TAny*)
	{
	if (!TheLookedUp)
		{
		if (X64DiskRaw(EFalse, 0, 1, TheBuf, ETrue) == KErrNotReady)
			{
			TheTimer->Again(NKern::TimerTicks(1000));		// no disk yet
			return;
			}
		TheLookedUp = ETrue;
		if (!Find())
			{
			Kern::Printf("[disklog] no phorepoc-log partition: the log stays in RAM");
			return;
			}
		Kern::Printf("[disklog] phorepoc-log at LBA %u, %u MB: the log goes there every second (boot id %08x)", TheFirst,
			TheSectors >> 11, X64LogBootId());
		}
	Flush(EFalse, ETrue);
	TheTimer->Again(NKern::TimerTicks(1000));
	}

static void TickTimer(TAny*)
	{
	TheDfc->Add();
	}

void X64DiskLogCrashFlush()
	{
	if (TheFirst && TheBuf)
		Flush(ETrue, EFalse);
	}

void X64StartDiskLog()
	{
	TheBuf = (TUint8*)Kern::Alloc(KChunk * 512);
	TheDfc = new TDfc(TickDfc, NULL, Kern::DfcQue0(), 1);
	TheTimer = new NTimer(TickTimer, NULL);
	if (!TheBuf || !TheDfc || !TheTimer)
		return;
	TheTimer->OneShot(NKern::TimerTicks(1000));
	}

// an orderly power-off / restart (xk_power.cpp): the last lines, polled, not marked as a crash
void X64DiskLogFinalFlush()
	{
	if (TheFirst && TheBuf)
		Flush(EFalse, EFalse);
	}
