// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_i2ctouch.cpp -- an I2C touch screen on a real PC: a Goodix GT9xx (xk_gt9xx.h) behind an Intel LPSS DesignWare I2C
// controller (xk_dwi2c.h), polled every KPollMs and turned into the pointer events the PS/2 / VMMDev pointers give
// (EButton1Down, EPointerMove, EButton1Up through Kern::AddEvent). First board: the GPD Win 2 (Kaby Lake-Y).
//
// No ACPI yet: every LPSS I2C function on the PCI bus (Sunrise Point-LP 8086:9d60..9d63, Sunrise Point-H a368..a36b) is
// brought up and probed for a GT9xx at 0x5D / 0x14, which is what the ACPI device (GDIX1001, _CRS = the bus + address)
// would have named. The INT GPIO is not used; polling at 100 Hz is enough for a pointer. X64_TODO: _CRS + the GPIO
// interrupt, multi-touch (only the first point drives the pointer now).
//
// XK_TOUCH_ROTATE = quarter turns clockwise from the controller's coordinates to the screen (the board's setting).
// Quiet when there is no LPSS I2C controller (QEMU, VirtualBox): one log line.
#include <kernel/kern_priv.h>
#include "xk_pci.h"
#include "xk_dwi2c.h"
#include "xk_gt9xx.h"

// the screen the OS draws (make_ekern.sh SCREEN=WxH)
#ifndef XK_SCREEN_W
#define XK_SCREEN_W 360
#define XK_SCREEN_H 640
#endif
#ifndef XK_TOUCH_ROTATE
#define XK_TOUCH_ROTATE 0
#endif

static const TInt KPollMs = 10;
static const TInt KLpssClkMhz = 120;

static TUint32 MmioRead(void* aCtx, TInt aReg) { return *(volatile TUint32*)((TLinAddr)aCtx + aReg); }
static void MmioWrite(void* aCtx, TInt aReg, TUint32 aVal) { *(volatile TUint32*)((TLinAddr)aCtx + aReg) = aVal; }
static TInt I2cXfer(void* aCtx, TUint8 aAddr, const TUint8* aW, TInt aWlen, TUint8* aR, TInt aRlen)
	{ return DwI2c::Transfer(*(const DwI2c::TDwRegs*)aCtx, aAddr, aW, aWlen, aR, aRlen); }

class DI2cTouch
	{
public:
	DI2cTouch() : iTimer(&TimerCb, this), iDfc(&PollDfc, this, 1), iDown(EFalse) {}
	DwI2c::TDwRegs iRegs;
	Gt9xx::TDevice iDev;
	NTimer iTimer;
	TDfc iDfc;
	TBool iDown;
	TInt iW, iH;
	static void TimerCb(TAny* aSelf) { ((DI2cTouch*)aSelf)->iDfc.Add(); }
	static void PollDfc(TAny* aSelf) { ((DI2cTouch*)aSelf)->Poll(); }
	void Poll();
	};

void DI2cTouch::Poll()
	{
	Gt9xx::TPoint p[1];
	const TInt n = iDev.Poll(p, 1);
	if (n >= 0)
		{
		TRawEvent e;
		if (n > 0)
			{
			TInt x, y;
			Gt9xx::ToScreen(iDev, p[0], XK_TOUCH_ROTATE, iW, iH, x, y);
			e.Set(iDown ? TRawEvent::EPointerMove : TRawEvent::EButton1Down, x, y, 0);
			Kern::AddEvent(e);
			iDown = ETrue;
			}
		else if (iDown)
			{
			e.Set(TRawEvent::EButton1Up);
			Kern::AddEvent(e);
			iDown = EFalse;
			}
		}
	iTimer.OneShot(NKern::TimerTicks(KPollMs));
	}

static TBool IsLpssI2c(TUint32 aId)
	{
	if ((aId & 0xFFFF) != 0x8086)
		return EFalse;
	const TUint32 d = aId >> 16;
	return (d >= 0x9D60 && d <= 0x9D63) || (d >= 0xA368 && d <= 0xA36B);
	}

void X64StartI2cTouch()
	{
	TInt found = 0;
	for (TInt dev = 0; dev < 32; ++dev)
		for (TInt fn = 0; fn < 8; ++fn)
			{
			const TUint32 id = XkPciRead(0, dev, fn, 0);
			if (!IsLpssI2c(id))
				continue;
			++found;
			const TUint32 bar = XkPciRead(0, dev, fn, 0x10) & ~0xFu;
			XkPciWrite(0, dev, fn, 0x04, XkPciRead(0, dev, fn, 0x04) | 0x6);
			DPlatChunkHw* mmio;
			if (!bar || DPlatChunkHw::New(mmio, bar, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking) != KErrNone)
				continue;
			DI2cTouch* t = new DI2cTouch;
			if (!t)
				return;
			t->iRegs.iRead = MmioRead; t->iRegs.iWrite = MmioWrite; t->iRegs.iCtx = (void*)mmio->LinearAddress();
			const TInt r = DwI2c::Init(t->iRegs, KLpssClkMhz, ETrue);
			t->iDev.iXfer = I2cXfer; t->iDev.iCtx = &t->iRegs;
			if (r != DwI2c::EOk || !t->iDev.Probe())
				{
				Kern::Printf("[i2ctouch] LPSS I2C 00:%02x.%d (%04x): init %d, no GT9xx", dev, fn, id >> 16, r);
				delete t;
				continue;
				}
			t->iW = XK_SCREEN_W; t->iH = XK_SCREEN_H;
			Kern::Printf("[i2ctouch] GT%s at 0x%02x on 00:%02x.%d: %dx%d%s -> screen %dx%d, rotate %d", t->iDev.iProduct,
							t->iDev.iAddr, dev, fn, t->iDev.iXRes, t->iDev.iYRes, t->iDev.iSwapXY ? " (x/y swapped)" : "",
							t->iW, t->iH, XK_TOUCH_ROTATE);
			t->iDfc.SetDfcQ(Kern::DfcQue0());
			t->iTimer.OneShot(NKern::TimerTicks(KPollMs));
			return;
			}
	Kern::Printf("[i2ctouch] %d LPSS I2C controller(s), no touch screen", found);
	}
