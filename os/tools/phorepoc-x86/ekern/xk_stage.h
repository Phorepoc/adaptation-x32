// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_stage.h -- boot breadcrumbs for a machine with no serial port (the GPD Win 2 rig): every boot stage writes its code
// to CMOS (index KStageCmos = the stage, KStageCmos + 1 = its complement; survives a reset, read by the rig's Linux
// collector), and with BANDS=1 paints a stripe plus the code as two big hex digits on the framebuffer. Used by the
// bootstrap (xboot.cpp, identity-mapped framebuffer) and the kernel (xk_stage.cpp). The firmware of the GPD uses CMOS
// up to 0x49; 0x70..0x7F are zero there (checked from Linux, /dev/port).
#ifndef XK_STAGE_H
#define XK_STAGE_H

const TUint8 KStageCmos = 0x70;

// the codes (the collector's table: tools/gpd-rig/linux/phorepoc-collect)
enum TX64Stage
	{
	EStBootEntry = 0x01, EStBootMem = 0x02, EStBootRom = 0x03, EStBootTables = 0x04, EStBootSuper = 0x05, EStBootJump = 0x06,
	EStBootDie = 0x0E,
	EStKernEntry = 0x10, EStKernExt = 0x11, EStPower = 0x12, EStMedia = 0x13, EStRigLog = 0x14, EStDisplay = 0x15,
	EStKeyboard = 0x16, EStTouch = 0x17, EStNetwork = 0x18, EStUsbHost = 0x19, EStSound = 0x1A, EStGraphics = 0x1B,
	EStExtDone = 0x1C,
	EStDispMapped = 0x20, EStDispMeasured = 0x21, EStDispShadow = 0x22, EStDispHal = 0x23,
	EStAhciDisk = 0x30, EStDiskLog = 0x31, EStNetLog = 0x32, EStUsbUp = 0x33,
	EStUiFrame = 0x40,
	EStCrash = 0xEE
	};

static inline void StageCmos(TUint8 aCode)
	{
	TUint64 fl;
	__asm__ __volatile__("pushfq; pop %0; cli" : "=r"(fl) : : "memory");
	__asm__ __volatile__("out 0x70, al" : : "a"((TUint8)KStageCmos));
	__asm__ __volatile__("out 0x71, al" : : "a"(aCode));
	__asm__ __volatile__("out 0x70, al" : : "a"((TUint8)(KStageCmos + 1)));
	__asm__ __volatile__("out 0x71, al" : : "a"((TUint8)~aCode));
	if (fl & 0x200)
		__asm__ __volatile__("sti" : : : "memory");
	}

static inline void StageCrashCmos()
	{
	__asm__ __volatile__("cli; mov al, 0x72; out 0x70, al; mov al, 0xEE; out 0x71, al" : : : "eax", "memory");
	}

// a 3x5 hex font, one row per 3 bits
static const TUint8 KStageFont[16][5] = {
	{7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},
	{7,5,7,5,7},{7,5,7,1,7},{7,5,7,5,5},{6,5,6,5,6},{7,4,4,4,7},{6,5,5,5,6},{7,4,7,4,7},{7,4,7,4,4} };

// stripe (code & 15) across the width, colour by (code >> 4); two hex digits, 24-pixel cells, at the top right
static inline void StagePaint(TLinAddr aFb, TUint32 aPitch, TUint32 aW, TUint32 aH, TUint8 aCode)
	{
	if (!aFb || aW < 200 || aH < 200)
		return;
	static const TUint32 KCol[5] = { 0xFF0000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0xFFFFFF };
	const TUint32 col = aCode == EStCrash ? 0xFF00FF : KCol[(aCode >> 4) % 5];
	const TUint32 n = aCode & 15;
	for (TUint32 y = n * aH / 16; y + 2 < (n + 1) * aH / 16; ++y)
		for (TUint32 x = 0; x < aW / 4; ++x)					// the left quarter only: the digits stay readable
			*(volatile TUint32*)(aFb + y * aPitch + x * 4) = col;
	const TUint32 cell = 24, x0 = aW - 8 * cell, y0 = cell;
	for (TUint32 d = 0; d < 2; ++d)
		{
		const TUint8* g = KStageFont[(aCode >> (d ? 0 : 4)) & 15];
		for (TUint32 r = 0; r < 7; ++r)
			for (TUint32 c = 0; c < 4; ++c)
				{
				const TBool on = r >= 1 && r <= 5 && c < 3 && (g[r - 1] >> (2 - c) & 1);
				const TUint32 px = on ? 0xFFFFFF : 0x000000;
				for (TUint32 yy = 0; yy < cell; ++yy)
					for (TUint32 xx = 0; xx < cell; ++xx)
						*(volatile TUint32*)(aFb + (y0 + r * cell + yy) * aPitch + (x0 + (d * 4 + c) * cell + xx) * 4) = px;
				}
		}
	}

// kernel files: XSTAGE(code), a no-op without STAGES
#ifdef XK_STAGES
extern "C" void X64Stage(TUint aCode);
#define XSTAGE(c) X64Stage(c)
#else
#define XSTAGE(c) ((void)0)
#endif

#endif
