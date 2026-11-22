/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

// xk_gt9xx.h -- the Goodix GT9xx capacitive touch controllers (GT911, GT912, GT927, GT9271, GT928, GT967 ...), over any
// I2C master. A clean write from Goodix's GT9xx programming guide (the register map below); Linux's goodix.c
// (GPL-2.0-only) is not used.
//
//   - Registers have 16-bit big-endian addresses: an I2C write of the address, then a repeated start and a read.
//   - The device answers at 7-bit 0x5D or 0x14, chosen by the INT pin level at reset. The firmware has already chosen
//     by the time the OS runs, so both are probed.
//   - 0x8140: product id, 4 ASCII bytes ("911\0", "9271" ...). 0x8144: firmware version, LE16.
//   - 0x8047: the config block. Its version is at 0x8047, X/Y output resolution (LE16) at 0x8048/0x804A, the touch count at
//     0x804C (low nibble), and module switch 1 at 0x804D (bit 3 = X/Y swapped).
//   - 0x814E: status. Bit 7 = buffer ready, bit 4 = a key, low nibble = the number of points (0..10). The host writes 0
//     there once it has read the points, or the device does not report again.
//   - 0x814F: the points, 8 bytes each: track id, X LE16, Y LE16, size LE16, reserved.
//
// Header-only and OS-free: the I2C master is a callback pair, so the kernel glue uses xk_dwi2c.h and the host test a mock.
#ifndef XK_GT9XX_H
#define XK_GT9XX_H

namespace Gt9xx {

static const TUint16 KRegConfig = 0x8047, KRegXRes = 0x8048, KRegProductId = 0x8140, KRegStatus = 0x814E, KRegPoints = 0x814F;
static const TUint8 KAddrs[2] = { 0x5D, 0x14 };
static const TInt KMaxPoints = 10, KPointSize = 8;

// one I2C transaction: aW written, then aRlen bytes read after a repeated start (0 = a plain write); 0 = success
typedef TInt (*TI2cXfer)(void* aCtx, TUint8 aAddr, const TUint8* aW, TInt aWlen, TUint8* aR, TInt aRlen);

struct TPoint { TInt iId, iX, iY; };

struct TDevice
	{
	TI2cXfer iXfer;
	void* iCtx;
	TUint8 iAddr;
	char iProduct[5];
	TInt iXRes, iYRes;			// the controller's coordinate space (config 0x8048/0x804A)
	TBool iSwapXY;				// config 0x804D bit 3

	TInt Read(TUint16 aReg, TUint8* aBuf, TInt aLen) const
		{ const TUint8 a[2] = { (TUint8)(aReg >> 8), (TUint8)aReg }; return iXfer(iCtx, iAddr, a, 2, aBuf, aLen); }
	TInt Write8(TUint16 aReg, TUint8 aVal) const
		{ const TUint8 a[3] = { (TUint8)(aReg >> 8), (TUint8)aReg, aVal }; return iXfer(iCtx, iAddr, a, 3, 0, 0); }

	// find the controller at either address and read its id + config; ETrue = found
	TBool Probe()
		{
		for (TInt i = 0; i < 2; ++i)
			{
			iAddr = KAddrs[i];
			TUint8 id[4];
			if (Read(KRegProductId, id, 4) != 0 || id[0] < '0' || id[0] > '9')
				continue;
			for (TInt k = 0; k < 4; ++k)
				iProduct[k] = (char)id[k];
			iProduct[4] = 0;
			TUint8 cfg[7];
			if (Read(KRegConfig, cfg, 7) != 0)
				continue;
			iXRes = cfg[1] | (cfg[2] << 8);
			iYRes = cfg[3] | (cfg[4] << 8);
			iSwapXY = (cfg[6] & 0x08) != 0;
			if (iXRes <= 0 || iYRes <= 0)
				continue;
			return ETrue;
			}
		return EFalse;
		}

	// One poll. Returns the number of points written to aPts (0 = the screen is not touched), or -1 = nothing new (the
	// buffer was not ready: keep the previous state), or -2 = an I2C error.
	TInt Poll(TPoint* aPts, TInt aMax)
		{
		TUint8 st;
		if (Read(KRegStatus, &st, 1) != 0)
			return -2;
		if (!(st & 0x80))
			return -1;
		TInt n = st & 0x0F;
		if (n > KMaxPoints)
			n = KMaxPoints;
		if (n > aMax)
			n = aMax;
		TUint8 buf[KMaxPoints * KPointSize];
		if (n && Read(KRegPoints, buf, n * KPointSize) != 0)
			return -2;
		for (TInt i = 0; i < n; ++i)
			{
			const TUint8* p = buf + i * KPointSize;
			aPts[i].iId = p[0] & 0x0F;
			TInt x = p[1] | (p[2] << 8), y = p[3] | (p[4] << 8);
			if (iSwapXY) { const TInt t = x; x = y; y = t; }
			aPts[i].iX = x; aPts[i].iY = y;
			}
		(void)Write8(KRegStatus, 0);
		return n;
		}
	};

// The controller's coordinates -> screen pixels. aRotate = quarter turns clockwise from the panel's native scan (the board's
// panel orientation); aW/aH = the screen as the OS draws it.
inline void ToScreen(const TDevice& aD, const TPoint& aP, TInt aRotate, TInt aW, TInt aH, TInt& aX, TInt& aY)
	{
	TInt xr = aD.iSwapXY ? aD.iYRes : aD.iXRes, yr = aD.iSwapXY ? aD.iXRes : aD.iYRes;
	TInt x = aP.iX, y = aP.iY;
	switch (aRotate & 3)
		{
	case 1: { const TInt t = x; x = yr - 1 - y; y = t; const TInt r = xr; xr = yr; yr = r; break; }
	case 2: x = xr - 1 - x; y = yr - 1 - y; break;
	case 3: { const TInt t = x; x = y; y = xr - 1 - t; const TInt r = xr; xr = yr; yr = r; break; }
	default: break;
		}
	aX = xr > 1 ? x * (aW - 1) / (xr - 1) : 0;
	aY = yr > 1 ? y * (aH - 1) / (yr - 1) : 0;
	if (aX < 0) aX = 0; else if (aX >= aW) aX = aW - 1;
	if (aY < 0) aY = 0; else if (aY >= aH) aY = aH - 1;
	}

} // namespace Gt9xx

#endif
