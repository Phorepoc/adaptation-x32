// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// i2c_touch_test.cpp -- host test of the board's touch path: xk_dwi2c.h (the DesignWare I2C master) and xk_gt9xx.h (the
// Goodix GT9xx protocol), unmodified, against a register-level mock of a DW_apb_i2c controller with a GT911 behind it.
//
// The mock models what the driver relies on: COMP_TYPE, ENABLE/ENABLE_STATUS, TAR, DATA_CMD (a write queues a byte or a
// read; RESTART starts the read phase; STOP ends the transaction), the RX FIFO + STATUS.RFNE, RAW_INTR_STAT STOP_DET and
// TX_ABRT (a TAR nobody answers = an address NACK), and the read-to-clear registers. The GT911 is a 16-bit-addressed
// register file: auto-incrementing reads, writes, and 0x814E's buffer-ready bit cleared by the host's write of 0.
//
//   g++ -std=c++11 -Wall -I ../../os/tools/phorepoc-x86/ekern i2c_touch_test.cpp && ./a.out
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
typedef int32_t TInt; typedef uint32_t TUint32; typedef uint16_t TUint16; typedef uint8_t TUint8; typedef int TBool;
enum { EFalse = 0, ETrue = 1 };
#define XK_DWI2C_HOST
#include "xk_dwi2c.h"
#include "xk_gt9xx.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

struct Gt911
	{
	TUint8 addr = 0x14;
	TUint8 regs[0x10000] = {};
	TUint16 ptr = 0;
	int statusClears = 0;
	Gt911()
		{
		memcpy(regs + 0x8140, "911\0", 4);
		regs[0x8047] = 0x41;
		regs[0x8048] = 1280 & 0xFF; regs[0x8049] = 1280 >> 8;		// the controller's X range (the panel's long side)
		regs[0x804A] = 720 & 0xFF;  regs[0x804B] = 720 >> 8;
		regs[0x804C] = 5;
		regs[0x804D] = 0x00;
		}
	void Touch(int n, const int* xy)
		{
		regs[0x814E] = (TUint8)(0x80 | n);
		for (int i = 0; i < n; ++i)
			{
			TUint8* p = regs + 0x814F + i * 8;
			p[0] = (TUint8)i; p[1] = (TUint8)xy[2*i]; p[2] = (TUint8)(xy[2*i] >> 8); p[3] = (TUint8)xy[2*i+1]; p[4] = (TUint8)(xy[2*i+1] >> 8);
			}
		}
	};

struct DwMock
	{
	Gt911* dev;
	TUint32 en = 0, tar = 0, raw = 0, resets = 0;
	std::vector<TUint8> wr;			// the write phase of the current transaction
	std::vector<TUint8> rx;			// the RX FIFO
	bool reading = false, nacked = false, started = false;
	int reads = 0;
	void Start()
		{
		started = true; wr.clear(); reading = false;
		nacked = (tar != dev->addr);
		if (nacked) raw |= DwI2c::KIntrTxAbrt;
		}
	void FlushWrite()
		{
		if (nacked || wr.size() < 2) return;
		dev->ptr = (TUint16)(wr[0] << 8 | wr[1]);
		for (size_t i = 2; i < wr.size(); ++i)
			{
			if (dev->ptr == 0x814E && wr[i] == 0) dev->statusClears++;
			dev->regs[dev->ptr++] = wr[i];
			}
		}
	void Cmd(TUint32 v)
		{
		if (!started) Start();
		if (v & DwI2c::KCmdRead)
			{
			if (!reading) { FlushWrite(); reading = true; }
			if (!nacked) rx.push_back(dev->regs[dev->ptr++]);
			}
		else
			wr.push_back((TUint8)v);
		if (v & DwI2c::KCmdStop)
			{
			if (!reading) FlushWrite();
			started = false;
			raw |= DwI2c::KIntrStopDet;
			}
		}
	static TUint32 Rd(void* c, TInt r) { return ((DwMock*)c)->Read(r); }
	static void Wr(void* c, TInt r, TUint32 v) { ((DwMock*)c)->Write(r, v); }
	TUint32 Read(TInt r)
		{
		switch (r)
			{
		case DwI2c::ECompType: return DwI2c::KCompType;
		case DwI2c::EEnableStatus: return en;
		case DwI2c::EStatus: return (rx.empty() ? 0 : DwI2c::KStatusRfne) | DwI2c::KStatusTfe;
		case DwI2c::ETxflr: return 0;
		case DwI2c::ERawIntrStat: return raw;
		case DwI2c::EClrIntr: raw = 0; return 0;
		case DwI2c::EClrTxAbrt: raw &= ~DwI2c::KIntrTxAbrt; return 0;
		case DwI2c::EDataCmd: { TUint8 b = rx.empty() ? 0xFF : rx.front(); if (!rx.empty()) rx.erase(rx.begin()); return b; }
		default: return 0;
			}
		}
	void Write(TInt r, TUint32 v)
		{
		switch (r)
			{
		case DwI2c::EEnable: en = v & 1; if (!en) { started = false; rx.clear(); } break;
		case DwI2c::ETar: if (!en) tar = v; break;			// TAR is only writable while disabled
		case DwI2c::EDataCmd: if (en) Cmd(v); break;
		case DwI2c::ELpssResets: resets = v; break;
		default: break;
			}
		}
	};

static TInt Xfer(void* c, TUint8 a, const TUint8* w, TInt wl, TUint8* r, TInt rl)
	{ return DwI2c::Transfer(*(DwI2c::TDwRegs*)c, a, w, wl, r, rl); }

int main()
	{
	Gt911 gt; DwMock m; m.dev = &gt;
	DwI2c::TDwRegs regs = { &DwMock::Rd, &DwMock::Wr, &m };

	CHECK(DwI2c::Init(regs, 120, ETrue) == DwI2c::EOk);
	CHECK(m.resets == 3);

	// a NACKed address is an error, not a hang
	TUint8 b[4];
	const TUint8 a[2] = { 0x81, 0x40 };
	CHECK(DwI2c::Transfer(regs, 0x5D, a, 2, b, 4) == DwI2c::ENack);

	Gt9xx::TDevice d = {};
	d.iXfer = Xfer; d.iCtx = &regs;
	CHECK(d.Probe());
	CHECK(d.iAddr == 0x14);					// 0x5D NACKed first, then 0x14
	CHECK(strcmp(d.iProduct, "911") == 0);
	CHECK(d.iXRes == 1280 && d.iYRes == 720 && !d.iSwapXY);

	Gt9xx::TPoint p[10];
	CHECK(d.Poll(p, 10) == -1);				// nothing ready
	const int one[2] = { 640, 100 };
	gt.Touch(1, one);
	CHECK(d.Poll(p, 10) == 1);
	CHECK(p[0].iX == 640 && p[0].iY == 100);
	CHECK(gt.statusClears == 1 && gt.regs[0x814E] == 0);
	CHECK(d.Poll(p, 10) == -1);				// consumed

	const int two[4] = { 0, 0, 1279, 719 };
	gt.Touch(2, two);
	CHECK(d.Poll(p, 10) == 2);
	CHECK(p[1].iX == 1279 && p[1].iY == 719);

	gt.regs[0x814E] = 0x80;					// released: ready, zero points
	CHECK(d.Poll(p, 10) == 0);

	// mapping: a 1280x720 coordinate space on a 720x1280 portrait screen (a quarter turn)
	TInt x, y;
	Gt9xx::TPoint c = { 0, 0, 0 };
	Gt9xx::ToScreen(d, c, 1, 720, 1280, x, y);
	CHECK(x == 719 && y == 0);
	c.iX = 1279; c.iY = 719;
	Gt9xx::ToScreen(d, c, 1, 720, 1280, x, y);
	CHECK(x == 0 && y == 1279);
	Gt9xx::ToScreen(d, c, 3, 720, 1280, x, y);
	CHECK(x == 719 && y == 0);
	Gt9xx::ToScreen(d, c, 0, 1280, 720, x, y);
	CHECK(x == 1279 && y == 719);
	c.iX = 640; c.iY = 360;
	Gt9xx::ToScreen(d, c, 2, 1280, 720, x, y);
	CHECK(x == 639 && y == 359);

	// X/Y swap from the module switch
	gt.regs[0x804D] = 0x08;
	CHECK(d.Probe() && d.iSwapXY);
	gt.Touch(1, one);
	CHECK(d.Poll(p, 10) == 1 && p[0].iX == 100 && p[0].iY == 640);

	printf("i2c_touch: %s\n", fails ? "FAIL" : "PASS");
	return fails ? 1 : 0;
	}
