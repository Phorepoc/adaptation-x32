// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_stage.cpp -- the kernel's half of the boot breadcrumbs (xk_stage.h; STAGES, set by make_ekern.sh for the rig
// profiles): X64Stage(code) = CMOS + (BANDS=1) the stripe and the digits. Before the display driver maps the framebuffer
// the bootstrap's picture stays; afterwards xk_display.cpp hands over the mapping (X64StageSetFb) and repaints the last
// stage after each flush until the UI draws its first real frame (X64StageRepaint / X64StageUiFrame).
#include <kernel/kernel.h>
#include "xk_stage.h"

#ifndef XK_BANDS
#define XK_BANDS 0
#endif

static TLinAddr TheFb;
static TUint32 ThePitch, TheW, TheH;
static volatile TUint8 TheStage;
static volatile TBool TheUi;

extern "C" void X64Stage(TUint aCode)
	{
	if (aCode == EStCrash)
		{
		StageCrashCmos();									// CMOS 0x72: the crash; 0x70 keeps how far the boot got
		if (XK_BANDS && TheFb)
			StagePaint(TheFb, ThePitch, TheW, TheH, (TUint8)aCode);
		return;
		}
	TheStage = (TUint8)aCode;
	StageCmos((TUint8)aCode);
	if (XK_BANDS && TheFb && !TheUi)
		StagePaint(TheFb, ThePitch, TheW, TheH, (TUint8)aCode);
	}

extern "C" void X64StageSetFb(TLinAddr aFb, TUint32 aPitch, TUint32 aW, TUint32 aH)
	{
	TheFb = aFb; ThePitch = aPitch; TheW = aW; TheH = aH;
	}

// after a flush of the shadow screen over the framebuffer: put the breadcrumb back until the UI has drawn
extern "C" void X64StageRepaint()
	{
	if (XK_BANDS && TheFb && !TheUi && TheStage)
		StagePaint(TheFb, ThePitch, TheW, TheH, TheStage);
	}

extern "C" void X64StageUiFrame()
	{
	if (TheUi)
		return;
	X64Stage(EStUiFrame);
	TheUi = ETrue;
	}

// BOOTGUARD=<s> (the rig; the GPD's TCO timer does not count, not even under Linux): a boot whose UI has not drawn its
// first frame after <s> seconds is a fault -- the crash path flushes the disk log and (FAULTRESET=1) resets. It needs the
// kernel's timer interrupt to run, so it covers user-side hangs, not a dead kernel.
#ifdef XK_BOOT_GUARD
static void GuardDfcFn(TAny*)
	{
	if (!TheUi)
		Kern::Fault("BOOTGUARD", TheStage);
	}
static TDfc* TheGuardDfc;
static void GuardTimerFn(TAny*) { TheGuardDfc->Add(); }
static NTimer* TheGuardTimer;
#endif
extern "C" void X64StartBootGuard()
	{
#ifdef XK_BOOT_GUARD
	TheGuardDfc = new TDfc(GuardDfcFn, NULL, Kern::DfcQue0(), 1);
	TheGuardTimer = new NTimer(GuardTimerFn, NULL);
	if (TheGuardDfc && TheGuardTimer)
		{
		TheGuardTimer->OneShot(NKern::TimerTicks(XK_BOOT_GUARD * 1000));
		Kern::Printf("[stage] boot guard: a fault unless the UI draws within %d s", XK_BOOT_GUARD);
		}
#endif
	}
