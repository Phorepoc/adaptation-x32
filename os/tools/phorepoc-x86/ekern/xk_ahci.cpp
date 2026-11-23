// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_ahci.cpp -- an AHCI (SATA) back end for the board's disk driver (xk_ata.cpp): the first SATA disk on the first
// AHCI controller (PCI class 01.06), one port, one command slot, polled. Real PCs (an M.2 SATA SSD on a
// Kaby Lake PCH, 8086:9d03) have no legacy IDE channel, so xk_ata.cpp asks here when its IDE probe finds nothing.
//
// Derived from the AHCI 1.3.1 specification (sections 3, 4, 5, 10.1.2) and the init order of Linux's libahci.c
// (ahci_stop_engine / ahci_start_fis_rx / ahci_start_engine). LP64-clean: physical addresses are TPhysAddr / TUint64,
// linear ones TLinAddr; nothing is cast through 32 bits.
//
// Memory: one physically contiguous block, cached (x86 DMA is coherent): the command list (1 KB aligned), the received
// FIS area (256 B aligned), one command table (128 B aligned, one PRDT entry) and a KAhciBuf bounce buffer. Commands:
// IDENTIFY DEVICE, READ/WRITE DMA EXT (LBA48), FLUSH CACHE EXT. X64_TODO: the port interrupt + a semaphore, NCQ.
#include <kernel/kern_priv.h>
#include "xk_pci.h"
#include "xk_stage.h"

static const TInt KAhciBuf = 128 * 512;						// = xk_ata.cpp's KBufSectors * KSectorSize
static const TInt KOffClb = 0x0000, KOffFis = 0x0400, KOffTbl = 0x0800, KOffBuf = 0x1000;
static const TInt KAhciAlloc = KOffBuf + KAhciBuf;
// HBA + port registers (AHCI 1.3.1 section 3)
static const TInt KGhc = 0x04, KPi = 0x0C;
static const TInt KPxClb = 0x00, KPxClbu = 0x04, KPxFb = 0x08, KPxFbu = 0x0C, KPxIs = 0x10, KPxIe = 0x14, KPxCmd = 0x18,
				  KPxTfd = 0x20, KPxSig = 0x24, KPxSsts = 0x28, KPxSerr = 0x30, KPxCi = 0x38;
static const TUint32 KCmdSt = 1u << 0, KCmdFre = 1u << 4, KCmdFr = 1u << 14, KCmdCr = 1u << 15;
static const TUint32 KIsTfes = 1u << 30;
static const TInt KTimeoutMs = 5000;

static TLinAddr AhciPort;									// the chosen port's register block; 0 = no AHCI disk
static TUint8* AhciMem;										// the DMA block, linear
static TPhysAddr AhciPhys;									// ... and physical

static inline TUint32 Rd(TInt aReg) { return *(volatile TUint32*)(AhciPort + aReg); }
static inline void Wr(TInt aReg, TUint32 aVal) { *(volatile TUint32*)(AhciPort + aReg) = aVal; }
static inline void Fence() { __asm__ __volatile__("mfence" : : : "memory"); }

// wait until (reg & aMask) == aWant, a tick at a time after a short spin
static TBool WaitReg(TInt aReg, TUint32 aMask, TUint32 aWant)
	{
	const TUint32 start = NKern::TickCount();
	for (TInt spin = 0;; ++spin)
		{
		if ((Rd(aReg) & aMask) == aWant)
			return ETrue;
		if (spin > 2000)
			{
			if ((TInt)(NKern::TickCount() - start) > KTimeoutMs)
				return EFalse;
			NKern::Sleep(1);
			}
		}
	}

static void PutPhys(TUint32* aLoHi, TUint64 aPhys)
	{ aLoHi[0] = (TUint32)aPhys; aLoHi[1] = (TUint32)(aPhys >> 32); }

// One command in slot 0. aBytes > 0 = a data phase through the bounce buffer (aWrite: host -> device).
static TInt Issue(TUint8 aCmd, TUint64 aLba, TInt aSectors, TInt aBytes, TBool aWrite)
	{
	TUint32* hdr = (TUint32*)(AhciMem + KOffClb);
	TUint8* tbl = AhciMem + KOffTbl;
	memclr(tbl, 0x80 + 16);
	hdr[0] = 5 | (aWrite ? (1u << 6) : 0) | ((aBytes > 0 ? 1u : 0u) << 16);	// CFL = 5 dwords, W, PRDTL
	hdr[1] = 0;																// PRDBC
	PutPhys(hdr + 2, (TUint64)AhciPhys + KOffTbl);							// CTBA / CTBAU
	hdr[4] = hdr[5] = hdr[6] = hdr[7] = 0;
	TUint8* f = tbl;														// Register H2D FIS
	f[0] = 0x27; f[1] = 0x80; f[2] = aCmd;
	f[4] = (TUint8)aLba; f[5] = (TUint8)(aLba >> 8); f[6] = (TUint8)(aLba >> 16);
	f[7] = 0x40;															// LBA mode
	f[8] = (TUint8)(aLba >> 24); f[9] = (TUint8)(aLba >> 32); f[10] = (TUint8)(aLba >> 40);
	f[12] = (TUint8)aSectors; f[13] = (TUint8)(aSectors >> 8);
	if (aBytes > 0)
		{
		TUint32* prd = (TUint32*)(tbl + 0x80);
		PutPhys(prd, (TUint64)AhciPhys + KOffBuf);
		prd[2] = 0;
		prd[3] = (TUint32)(aBytes - 1);										// DBC (byte count - 1), no interrupt
		}
	Wr(KPxIs, 0xFFFFFFFFu);
	Fence();
	Wr(KPxCi, 1);
	const TUint32 start = NKern::TickCount();
	for (TInt spin = 0;; ++spin)
		{
		if (Rd(KPxIs) & KIsTfes)
			return KErrCorrupt;
		if (!(Rd(KPxCi) & 1))
			break;
		if (spin > 2000)
			{
			if ((TInt)(NKern::TickCount() - start) > KTimeoutMs)
				return KErrTimedOut;
			NKern::Sleep(1);
			}
		}
	Fence();
	return (Rd(KPxTfd) & 0x01) ? KErrCorrupt : KErrNone;					// ERR
	}

static TBool StartPort()
	{
	Wr(KPxCmd, Rd(KPxCmd) & ~KCmdSt);										// ahci_stop_engine
	if (!WaitReg(KPxCmd, KCmdCr, 0))
		return EFalse;
	Wr(KPxCmd, Rd(KPxCmd) & ~KCmdFre);
	if (!WaitReg(KPxCmd, KCmdFr, 0))
		return EFalse;
	memclr(AhciMem, KOffBuf);
	TUint32 lohi[2];
	PutPhys(lohi, (TUint64)AhciPhys + KOffClb); Wr(KPxClb, lohi[0]); Wr(KPxClbu, lohi[1]);
	PutPhys(lohi, (TUint64)AhciPhys + KOffFis); Wr(KPxFb, lohi[0]); Wr(KPxFbu, lohi[1]);
	Wr(KPxSerr, 0xFFFFFFFFu);
	Wr(KPxIs, 0xFFFFFFFFu);
	Wr(KPxIe, 0);
	Wr(KPxCmd, Rd(KPxCmd) | KCmdFre);										// ahci_start_fis_rx
	if (!WaitReg(KPxTfd, 0x88, 0))											// BSY | DRQ clear
		return EFalse;
	Wr(KPxCmd, Rd(KPxCmd) | KCmdSt);										// ahci_start_engine
	return ETrue;
	}

// Probe once: the disk's capacity in sectors (0 = no AHCI disk). Clamped to 32 bits (xk_ata.cpp's unit size).
TUint32 XkAhciProbe()
	{
	static TBool done;
	static TUint32 sectors;
	if (done)
		return sectors;
	done = ETrue;
	SPciLoc loc;
	if (!XkPciFind(0, 0x0106, loc))
		return 0;
	const TUint32 bar5 = XkPciRead(loc.iBus, loc.iDev, loc.iFn, 0x24) & ~0xFu;
	XkPciWrite(loc.iBus, loc.iDev, loc.iFn, 0x04, XkPciRead(loc.iBus, loc.iDev, loc.iFn, 0x04) | 0x6);	// memory + bus master
	DPlatChunkHw* mmio;
	if (!bar5 || DPlatChunkHw::New(mmio, bar5, 0x2000, EMapAttrSupRw | EMapAttrFullyBlocking) != KErrNone)
		return 0;
	const TLinAddr abar = mmio->LinearAddress();
	*(volatile TUint32*)(abar + KGhc) |= 0x80000000u;						// AE: AHCI mode
	const TUint32 pi = *(volatile TUint32*)(abar + KPi);
	for (TInt i = 0; i < 32 && !AhciPort; ++i)
		{
		if (!(pi & (1u << i)))
			continue;
		const TLinAddr p = abar + 0x100 + i * 0x80;
		const TUint32 ssts = *(volatile TUint32*)(p + KPxSsts), sig = *(volatile TUint32*)(p + KPxSig);
		if ((ssts & 0xF) == 3 && sig == 0x00000101)							// device present + PHY up, an ATA disk
			AhciPort = p;
		}
	Kern::Printf("[ahci] %02x:%02x.%d ABAR %08x PI %08x -> port %s", loc.iBus, loc.iDev, loc.iFn, bar5, pi,
					AhciPort ? "found" : "none");
	if (!AhciPort)
		return 0;
	DPlatChunkHw* dma;
	if (Epoc::AllocPhysicalRam(KAhciAlloc, AhciPhys, 12) != KErrNone
	 || DPlatChunkHw::New(dma, AhciPhys, KAhciAlloc, EMapAttrSupRw | EMapAttrCachedMax) != KErrNone)
		{ AhciPort = 0; return 0; }
	AhciMem = (TUint8*)dma->LinearAddress();
	if (!StartPort() || Issue(0xEC, 0, 0, 512, EFalse) != KErrNone)		// IDENTIFY DEVICE
		{
		Kern::Printf("[ahci] port start / IDENTIFY failed (TFD %08x)", Rd(KPxTfd));
		AhciPort = 0;
		return 0;
		}
	const TUint16* id = (const TUint16*)(AhciMem + KOffBuf);
	TUint64 n = (TUint64)id[60] | ((TUint64)id[61] << 16);
	if (id[83] & (1u << 10))
		n = (TUint64)id[100] | ((TUint64)id[101] << 16) | ((TUint64)id[102] << 32) | ((TUint64)id[103] << 48);
	sectors = n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (TUint32)n;
	Kern::Printf("[ahci] SATA disk: %u sectors (%u MB)", sectors, sectors >> 11);
	XSTAGE(EStAhciDisk);
	return sectors;
	}

// aCount sectors (1..128) at aLba to/from aBuf; the caller serialises (xk_ata.cpp's AtaLock)
TInt XkAhciTransfer(TBool aWrite, TUint32 aLba, TInt aCount, TUint8* aBuf)
	{
	if (!AhciPort || aCount < 1 || aCount * 512 > KAhciBuf)
		return KErrArgument;
	if (aWrite)
		memcpy(AhciMem + KOffBuf, aBuf, aCount * 512);
	TInt r = Issue(aWrite ? 0x35 : 0x25, aLba, aCount, aCount * 512, aWrite);
	if (r == KErrNone && !aWrite)
		memcpy(aBuf, AhciMem + KOffBuf, aCount * 512);
	return r;
	}

TInt XkAhciFlush()
	{
	return AhciPort ? Issue(0xEA, 0, 0, 0, EFalse) : KErrNone;
	}
