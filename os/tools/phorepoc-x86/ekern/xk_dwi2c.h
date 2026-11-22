/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

// xk_dwi2c.h -- a polled master for the Synopsys DesignWare APB I2C controller, the I2C block of Intel's LPSS (Skylake /
// Kaby Lake PCH-LP: PCI 8086:9d60..9d63 = I2C0..3, BAR0 = the registers, the Intel private block at +0x200).
//
// Written from the DW_apb_i2c register map. The bring-up order and the transfer rules follow FreeBSD's ig4(4)
// (sys/dev/ichiic, BSD-3-Clause, the DragonFly Project): assert + release the LPSS reset, check COMP_TYPE, disable the
// controller, clear + mask the interrupts, program the SCL counts and SDA hold, FIFO thresholds 0, CON = master +
// slave-disable + restart-enable + speed; a write-then-read is one transaction with RESTART on the first read command and
// STOP on the last command; TX_ABRT is read from TX_ABRT_SOURCE and cleared by reading CLR_TX_ABRT. No code is copied.
//
// Header-only and OS-free (only the TDwRegs accessor below), so the same code runs in the kernel and in the host test
// against a register mock (tests/i2c_touch). Polled: no interrupt, a bounded spin per FIFO step.
#ifndef XK_DWI2C_H
#define XK_DWI2C_H

#ifndef XK_DWI2C_HOST
#include <e32def.h>
#endif

namespace DwI2c {

enum TReg
	{
	ECon = 0x00, ETar = 0x04, EDataCmd = 0x10, ESsHcnt = 0x14, ESsLcnt = 0x18, EFsHcnt = 0x1C, EFsLcnt = 0x20,
	EIntrStat = 0x2C, EIntrMask = 0x30, ERawIntrStat = 0x34, ERxTl = 0x38, ETxTl = 0x3C, EClrIntr = 0x40,
	EClrTxAbrt = 0x54, EEnable = 0x6C, EStatus = 0x70, ETxflr = 0x74, ERxflr = 0x78, ESdaHold = 0x7C,
	ETxAbrtSource = 0x80, EEnableStatus = 0x9C, ECompParam1 = 0xF4, ECompVersion = 0xF8, ECompType = 0xFC,
	ELpssResets = 0x204				// Intel LPSS private: bits 0-1 = the controller + DMA out of reset (Skylake and later)
	};
static const TUint32 KCompType = 0x44570140;			// "DW" + 0x0140
static const TUint32 KConMaster = 1u << 0, KConSpeedStd = 1u << 1, KConSpeedFast = 2u << 1, KConRestartEn = 1u << 5,
					 KConSlaveDisable = 1u << 6;
static const TUint32 KCmdRead = 1u << 8, KCmdStop = 1u << 9, KCmdRestart = 1u << 10;
static const TUint32 KIntrTxAbrt = 1u << 6, KIntrStopDet = 1u << 9;
static const TUint32 KStatusActivity = 1u << 0, KStatusTfe = 1u << 2, KStatusRfne = 1u << 3;

enum TResult { EOk = 0, ENoDevice = -1, ENack = -2, ETimeout = -3, EArg = -4 };

// The register accessor: the kernel passes the mapped BAR, the host test its mock.
struct TDwRegs
	{
	TUint32 (*iRead)(void* aCtx, TInt aReg);
	void (*iWrite)(void* aCtx, TInt aReg, TUint32 aVal);
	void* iCtx;
	TUint32 R(TInt aReg) const { return iRead(iCtx, aReg); }
	void W(TInt aReg, TUint32 aVal) const { iWrite(iCtx, aReg, aVal); }
	};

static const TInt KSpin = 200000;						// polls per wait (a 32-byte read at 100 kHz is ~3 ms)

inline TBool SetEnable(const TDwRegs& aR, TBool aOn)
	{
	aR.W(EEnable, aOn ? 1 : 0);
	for (TInt i = 0; i < KSpin; ++i)
		if ((aR.R(EEnableStatus) & 1) == (aOn ? 1u : 0u))
			return ETrue;
	return EFalse;
	}

// aClkMhz = the controller's input clock (120 MHz on the Skylake/Kaby Lake LPSS). Standard mode (100 kHz) counts from the
// I2C minimums (tHIGH 4.0 us, tLOW 4.7 us, + tf 0.3 us), as the firmware's SSCN would give; SDA hold 300 ns.
inline TInt Init(const TDwRegs& aR, TInt aClkMhz, TBool aLpss)
	{
	if (aLpss)
		{
		aR.W(ELpssResets, 0);
		aR.W(ELpssResets, 3);
		}
	if (aR.R(ECompType) != KCompType)
		return ENoDevice;
	if (!SetEnable(aR, EFalse))
		return ETimeout;
	aR.W(EIntrMask, 0);
	(void)aR.R(EClrIntr);
	aR.W(ESsHcnt, (TUint32)(aClkMhz * 43 / 10 - 3));
	aR.W(ESsLcnt, (TUint32)(aClkMhz * 50 / 10 - 1));
	aR.W(EFsHcnt, (TUint32)(aClkMhz * 9 / 10 - 3));
	aR.W(EFsLcnt, (TUint32)(aClkMhz * 16 / 10 - 1));
	aR.W(ESdaHold, (TUint32)(aClkMhz * 3 / 10));
	aR.W(ERxTl, 0);
	aR.W(ETxTl, 0);
	aR.W(ECon, KConMaster | KConSlaveDisable | KConRestartEn | KConSpeedStd);
	return EOk;
	}

// TX_ABRT -> ENack (the address or a data byte was not acknowledged), cleared by reading CLR_TX_ABRT
inline TBool Aborted(const TDwRegs& aR)
	{
	if (!(aR.R(ERawIntrStat) & KIntrTxAbrt))
		return EFalse;
	(void)aR.R(ETxAbrtSource);
	(void)aR.R(EClrTxAbrt);
	return ETrue;
	}

// One transaction to 7-bit aAddr: aWlen bytes written, then (aRlen > 0) a repeated start and aRlen bytes read. The last
// command carries STOP. TX and RX are interleaved so neither FIFO overruns (FIFOs are 64 deep on the LPSS, 16 on others;
// at most one command is queued ahead of the RX drain, which holds for any depth).
inline TInt Transfer(const TDwRegs& aR, TUint8 aAddr, const TUint8* aW, TInt aWlen, TUint8* aRd, TInt aRlen)
	{
	if (aWlen < 0 || aRlen < 0 || aWlen + aRlen == 0)
		return EArg;
	if (!SetEnable(aR, EFalse))
		return ETimeout;
	aR.W(ETar, aAddr);
	if (!SetEnable(aR, ETrue))
		return ETimeout;
	(void)aR.R(EClrTxAbrt);
	(void)aR.R(EClrIntr);
	const TInt total = aWlen + aRlen;
	TInt rx = 0;
	for (TInt i = 0; i < total; ++i)
		{
		TUint32 cmd = i < aWlen ? aW[i] : KCmdRead;
		if (i == aWlen && aWlen > 0)
			cmd |= KCmdRestart;
		if (i == total - 1)
			cmd |= KCmdStop;
		TInt spin = 0;
		while (aR.R(ETxflr) >= 8)						// keep the queue short (any FIFO depth)
			if (Aborted(aR)) return ENack; else if (++spin > KSpin) return ETimeout;
		aR.W(EDataCmd, cmd);
		if (i >= aWlen)
			{
			for (spin = 0; !(aR.R(EStatus) & KStatusRfne); )
				if (Aborted(aR)) return ENack; else if (++spin > KSpin) return ETimeout;
			aRd[rx++] = (TUint8)aR.R(EDataCmd);
			}
		}
	for (TInt spin = 0; !(aR.R(ERawIntrStat) & KIntrStopDet); )
		if (Aborted(aR)) return ENack; else if (++spin > KSpin) return ETimeout;
	(void)aR.R(EClrIntr);
	return Aborted(aR) ? ENack : EOk;
	}

} // namespace DwI2c

#endif
