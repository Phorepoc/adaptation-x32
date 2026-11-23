// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_tco.cpp -- the chipset's TCO watchdog (Intel ICH / PCH), so a hung kernel reboots by itself (the GPD Win 2 test
// rig: no hands at the machine). Built with TCO=1 (make_ekern.sh). Written from Intel's datasheets: ICH9 (82801I) section
// 12.9 "TCO registers" and 13.1.x LPC config (PMBASE 0x40, RCBA 0xF0, GCS = RCBA + 0x3410, NO_REBOOT bit 5); the 100-series
// PCH (Sunrise Point, the GPD Win 2's Kaby Lake-Y) vol. 2: SMBus config TCOBASE 0x50 / TCOCTL 0x54 (TCO_BASE_EN bit 8),
// NO_REBOOT in the SMBus private config register SMBCTRL (sideband port 0xC6, offset 0x0C, bit 0... see Arm()).
//
// The TCO timer counts down in 0.6 s ticks from TCO_TMR; at the first zero it sets TIMEOUT (TCO1_STS bit 3) and reloads,
// at the second (SECOND_TO_STS) the chipset resets the machine -- unless NO_REBOOT is set. The kick (TCO_RLD) comes
// from a DFC on DfcQue0 every second, queued by an NTimer: a kernel whose DFC thread no longer runs (a deadlock, a CPU
// spinning with interrupts off, a fault) stops kicking and is reset after ~2 x TCO_SECS / 2.
#include <kernel/kernel.h>
#include <platform.h>
#include "xk_pci.h"

#ifndef XK_TCO_SECS
#define XK_TCO_SECS 30			// first + second timeout together
#endif

static inline void Outw(TUint16 aPort, TUint16 aVal) { __asm__ __volatile__("out dx, ax" : : "d"(aPort), "a"(aVal)); }
static inline TUint16 Inw(TUint16 aPort) { TUint16 v; __asm__ __volatile__("in ax, dx" : "=a"(v) : "d"(aPort)); return v; }

// TCO registers (I/O, from TCOBASE)
const TUint KTcoRld = 0x00, KTco1Sts = 0x04, KTco2Sts = 0x06, KTco1Cnt = 0x08, KTcoTmr = 0x12;
const TUint16 KTcoTmrHlt = 1u << 11;			// TCO1_CNT: the timer halted
const TUint16 KTimeoutSts = 1u << 3;			// TCO1_STS
const TUint16 KSecondToSts = 1u << 1;			// TCO2_STS

static TUint16 TheTco;							// TCOBASE, 0 = none
static NTimer* TheTimer;
static TDfc* TheDfc;
static volatile TBool TheStop;

static void Kick()
	{
	Outw((TUint16)(TheTco + KTcoRld), 1);
	}

#ifndef XK_TCO_TEST
#define XK_TCO_TEST 0			// > 0: stop kicking after that many seconds (the QEMU test of the reset, TCO_TEST=<s>)
#endif
static TInt TheKicks;

static void KickDfc(TAny*)
	{
	++TheKicks;
#ifdef XK_CRASH_TEST
	if (TheKicks + 1 == XK_CRASH_TEST)
		Kern::Fault("RIGTEST", 1);							// CRASH_TEST=<s>: the fault -> disk log -> reset path
#endif
	if (XK_TCO_TEST && TheKicks == XK_TCO_TEST)
		{
		Kern::Printf("[tco] test: kicks stop now, the reset is due in ~%d s", XK_TCO_SECS);
		TheStop = ETrue;
		}
	if (TheStop)
		return;
	Kick();
	TheTimer->Again(NKern::TimerTicks(1000));
	}

static void KickTimer(TAny*)
	{
	TheDfc->Add();
	}

// the debug link's 'H' command (usbhost/xk_netlog.cpp): stop kicking, so the hang it then makes is reset
void X64TcoStop()
	{
	TheStop = ETrue;
	}

// NO_REBOOT on ICH9: GCS (RCBA + 0x3410) bit 5. 1 = was set (now cleared), 0 = clear, -1 = no RCBA
static TInt Ich9ClearNoReboot()
	{
	const TUint32 rcba = XkPciRead(0, 31, 0, 0xF0);
	if (!(rcba & 1))
		return -1;
	DPlatChunkHw* ch = NULL;
	if (DPlatChunkHw::New(ch, (rcba & ~0x3FFFu) + 0x3000, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking) != KErrNone)
		return -1;
	volatile TUint32* gcs = (volatile TUint32*)(ch->LinearAddress() + 0x410);
	const TUint32 v = *gcs;
	if (v & (1u << 5))
		*gcs = v & ~(1u << 5);
	const TInt was = (v >> 5) & 1;
	Kern::Printf("[tco] ICH9 GCS %08x -> %08x", v, *gcs);
	ch->Close(NULL);
	return was;
	}

// NO_REBOOT on Sunrise Point: the SMBus's SMBCTRL private config register, through the P2SB sideband window (P2SB =
// 00:1f.1, hidden by firmware: config 0xE1 bit 0 hides it; SBREG_BAR = its BAR0; a register = SBREG + port << 16 + offset).
// NO_REBOOT is bit 1 (0x2): Linux drivers/watchdog/iTCO_wdt.c no_reboot_bit() for TCO version 4 (Sunrise Point, as
// i2c-i801 registers it: SBREG + SBREG_SMBCTRL 0xc6000c). Both values are logged.
static TInt SptClearNoReboot()
	{
	const TUint32 e0 = XkPciRead(0, 31, 1, 0xE0);
	XkPciWrite(0, 31, 1, 0xE0, e0 & ~0x100u);				// unhide P2SB (0xE1 bit 0)
	const TUint32 bar = XkPciRead(0, 31, 1, 0x10) & ~0xFu;
	const TUint32 id = XkPciRead(0, 31, 1, 0);
	XkPciWrite(0, 31, 1, 0xE0, e0);							// hide it again as firmware left it
	if ((id & 0xFFFF) != 0x8086 || !bar)
		{
		Kern::Printf("[tco] P2SB not found (id %08x, SBREG %08x): NO_REBOOT left alone", id, bar);
		return -1;
		}
	DPlatChunkHw* ch = NULL;
	if (DPlatChunkHw::New(ch, bar + (0xC6u << 16), 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking) != KErrNone)
		return -1;
	volatile TUint32* smbctrl = (volatile TUint32*)(ch->LinearAddress() + 0x0C);
	const TUint32 v = *smbctrl;
	if (v & 2)
		*smbctrl = v & ~2u;
	Kern::Printf("[tco] SPT SMBCTRL %08x -> %08x (SBREG %08x)", v, *smbctrl, bar);
	const TInt was = (v >> 1) & 1;
	ch->Close(NULL);
	return was;
	}

static TBool Arm()
	{
	const TUint32 lpc = XkPciRead(0, 31, 0, 0);
	TInt nr = -1;
	if (lpc == 0x29188086)									// ICH9 LPC (QEMU q35): TCOBASE = PMBASE + 0x60
		{
		TheTco = (TUint16)((XkPciRead(0, 31, 0, 0x40) & 0xFF80u) + 0x60);
		nr = Ich9ClearNoReboot();
		}
	else if ((XkPciRead(0, 31, 4, 0) & 0xFFFF) == 0x8086 && (XkPciRead(0, 31, 4, 8) >> 8) == 0x0C0500)
		{
		const TUint32 ctl = XkPciRead(0, 31, 4, 0x54);		// SMBus: TCOCTL bit 8 = TCO_BASE_EN
		const TUint32 base = XkPciRead(0, 31, 4, 0x50) & 0xFFE0u;
		if (!(ctl & 0x100) || !base)
			{
			Kern::Printf("[tco] SMBus TCO base not enabled (TCOCTL %08x, TCOBASE %08x)", ctl, base);
			return EFalse;
			}
		TheTco = (TUint16)base;
		nr = SptClearNoReboot();
		}
	else
		{
		Kern::Printf("[tco] no Intel TCO (00:1f.0 = %08x)", lpc);
		return EFalse;
		}
	TUint ticks = (XK_TCO_SECS * 10 / 6) / 2;				// two timeouts make the reset
	if (ticks < 4) ticks = 4;
	if (ticks > 0x3FF) ticks = 0x3FF;
	TUint16 cnt = Inw((TUint16)(TheTco + KTco1Cnt));
	Outw((TUint16)(TheTco + KTco1Cnt), cnt | KTcoTmrHlt);	// halt, program, clear the status, reload, run
	Outw((TUint16)(TheTco + KTcoTmr), (TUint16)((Inw((TUint16)(TheTco + KTcoTmr)) & ~0x3FFu) | ticks));
	Outw((TUint16)(TheTco + KTco1Sts), KTimeoutSts);
	Outw((TUint16)(TheTco + KTco2Sts), KSecondToSts);
	Kick();
	Outw((TUint16)(TheTco + KTco1Cnt), cnt & ~KTcoTmrHlt);
	cnt = Inw((TUint16)(TheTco + KTco1Cnt));
	Kern::Printf("[tco] armed at TCOBASE %04x: %u x 0.6 s twice (~%d s), NO_REBOOT was %d, TCO1_CNT %04x", TheTco, ticks,
		(TInt)(ticks * 12 / 10), nr, cnt);
	return !(cnt & KTcoTmrHlt);
	}

void X64StartTco()
	{
	if (!Arm())
		return;
	TheDfc = new TDfc(KickDfc, NULL, Kern::DfcQue0(), 1);
	TheTimer = new NTimer(KickTimer, NULL);
	if (!TheDfc || !TheTimer)
		return;
	TheTimer->OneShot(NKern::TimerTicks(1000));
	}
