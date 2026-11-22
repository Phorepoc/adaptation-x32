// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_netlog.cpp -- the test rig's debug link over a USB Ethernet adapter (pheusb phe_usb_ure.c: RTL8152/8153): the kernel
// log as UDP broadcasts and a small command channel (reboot, thread dump, screenshot, input, a hang for the watchdog
// test). The wire format is xk_netlog_proto.h. Built with NETLOG=1 (make_ekern.sh; needs USB_HOST=1).
//
// Every byte DebugOutputChar sends to COM1 also lands in a 256 KB ring (X64NetLogChar, lock-free: a byte is reserved with
// an atomic add and stored non-zero; the sender takes bytes up to the first zero), so the boot log from the first line
// on goes out once the adapter has a link. The sender runs on the USB thread, in the adapter's per-pass poll.
#include <kernel/kernel.h>
#include <e32keys.h>
#include "xk_netlog_proto.h"
extern "C" {
#include "phe_usb_ure.h"
}

using namespace NetLog;

void X64RequestThreadDump();								// xk_threaddump.cpp
void X64TcoStop();											// xk_tco.cpp (a no-op without a watchdog)
void X64ScreenInfo(TLinAddr& aAddr, TInt& aW, TInt& aH, TInt& aPitch);	// xk_display.cpp: the OS screen, XRGB

#ifndef NETLOG_TOKEN
#define NETLOG_TOKEN 0
#endif

const TUint32 KRing = 256 * 1024;
static volatile TUint8 gRing[KRing];
static volatile TUint32 gHead;								// bytes reserved so far (wraps)
static TUint32 gTail;										// bytes sent (the USB thread's)

void X64NetLogChar(TUint aChar)
	{
	const TUint8 c = (TUint8)aChar;
	if (!c)
		return;
	const TUint32 at = __atomic_fetch_add(&gHead, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&gRing[at % KRing], c, __ATOMIC_RELEASE);
	}

static TUint8 gMac[6], gIp[4];
static const TUint8 KBcastMac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const TUint8 KBcastIp[4] = { 255, 255, 255, 255 };
static TUint32 gBoot, gSeq, gPolls, gRebootAt;
static TBool gLink;
static TUint8 gFrame[KHdr + 1472 + 8];

// the screenshot stream: the sender, and how far it got
static TCmd gShotTo;
static TBool gShot;
static TLinAddr gShotAddr;
static TInt gShotW, gShotH, gShotPitch;
static TUint32 gShotNext;									// next pixel

static void Send(const TUint8* aDstMac, const TUint8* aDstIp, TUint32 aSport, TUint32 aDport, TUint32 aLen)
	{
	const TUint32 n = BuildUdp(gFrame, gMac, aDstMac, gIp, aDstIp, aSport, aDport, aLen);
	phe_usb_ure_tx(gFrame, n);
	}

static void Reply(const TCmd& aC, TInt aStatus, const TUint8* aData, TUint32 aLen)
	{
	TUint8* p = gFrame + KHdr;
	p[0] = 'P'; p[1] = 'H'; p[2] = 'R'; p[3] = '1';
	Put32le(p + 4, aC.iId); p[8] = aC.iOp; Put32le(p + 9, (TUint32)aStatus);
	for (TUint32 i = 0; i < aLen && 13 + i < 1472; ++i)
		p[13 + i] = aData[i];
	Send(aC.iMac, aC.iIp, KCmdPort, aC.iPort, 13 + aLen);
	}

static void Reboot()
	{
	NKern::DisableAllInterrupts();
	__asm__ __volatile__("out dx, al" : : "d"((TUint16)0xCF9), "a"((TUint8)0x02));	// reset control: hard reset
	__asm__ __volatile__("out dx, al" : : "d"((TUint16)0xCF9), "a"((TUint8)0x06));
	__asm__ __volatile__("out dx, al" : : "d"((TUint16)0x64), "a"((TUint8)0xFE));	// the i8042's reset line
	for (;;)
		__asm__ __volatile__("hlt");
	}

static void AddPointer(TRawEvent::TType aType, TInt aX, TInt aY)
	{
	TRawEvent e;
	e.Set(aType, aX, aY, 0);
	Kern::AddEvent(e);
	}

static void Exec(const TCmd& c)
	{
	if (NETLOG_TOKEN && c.iToken != (TUint32)NETLOG_TOKEN)
		{ Reply(c, -1, 0, 0); return; }
	switch (c.iOp)
		{
	case 'P':
		{
		TUint8 d[10];
		Put32le(d, gBoot);
		for (TInt i = 0; i < 6; ++i) d[4 + i] = gMac[i];
		Reply(c, 0, d, sizeof(d));
		return;
		}
	case 'R':
		Reply(c, 0, 0, 0);
		Kern::Printf("[netlog] reboot requested");
		gRebootAt = gPolls + 50;							// after the reply and the log are out
		return;
	case 'D':
		Reply(c, 0, 0, 0);
		X64RequestThreadDump();
		return;
	case 'H':
		Reply(c, 0, 0, 0);
		Kern::Printf("[netlog] hang requested: interrupts off, the watchdog kick stopped");
#ifdef XK_TCO
		X64TcoStop();
#endif
		gRebootAt = 0xFFFFFFFFu;							// the next poll hangs (the reply goes out first)
		return;
	case 'K':
		if (c.iArgLen < 3)
			{ Reply(c, KErrArgument, 0, 0); return; }
		{
		TRawEvent e;
		e.Set(c.iArgs[2] ? TRawEvent::EKeyDown : TRawEvent::EKeyUp, (TInt)Get16le(c.iArgs), 0);
		Reply(c, Kern::AddEvent(e), 0, 0);
		}
		return;
	case 'T':
		if (c.iArgLen < 5)
			{ Reply(c, KErrArgument, 0, 0); return; }
		{
		const TInt x = (TInt)Get16le(c.iArgs), y = (TInt)Get16le(c.iArgs + 2);
		AddPointer(TRawEvent::EPointerMove, x, y);
		if (c.iArgs[4] == 1)
			AddPointer(TRawEvent::EButton1Down, x, y);
		else if (c.iArgs[4] == 2)
			AddPointer(TRawEvent::EButton1Up, x, y);
		Reply(c, KErrNone, 0, 0);
		}
		return;
	case 'S':
		X64ScreenInfo(gShotAddr, gShotW, gShotH, gShotPitch);
		if (!gShotAddr || gShotW <= 0)
			{ Reply(c, KErrNotReady, 0, 0); return; }
		{
		TUint8 d[4];										// width, height (u16 each)
		d[0] = (TUint8)gShotW; d[1] = (TUint8)(gShotW >> 8); d[2] = (TUint8)gShotH; d[3] = (TUint8)(gShotH >> 8);
		Reply(c, KErrNone, d, 4);
		}
		gShotTo = c;
		gShotNext = 0;
		gShot = ETrue;
		return;
	default:
		Reply(c, KErrNotSupported, 0, 0);
		return;
		}
	}

// one screenshot packet: "PHF1" | id | w | h | first pixel u32 | count u16 | XRGB pixels; count 0 = the end
static void ShotPacket()
	{
	TUint8* p = gFrame + KHdr;
	const TUint32 total = (TUint32)gShotW * (TUint32)gShotH;
	TUint32 n = (1472 - 18) / 4;
	if (n > total - gShotNext)
		n = total - gShotNext;
	p[0] = 'P'; p[1] = 'H'; p[2] = 'F'; p[3] = '1';
	Put32le(p + 4, gShotTo.iId);
	p[8] = (TUint8)gShotW; p[9] = (TUint8)(gShotW >> 8); p[10] = (TUint8)gShotH; p[11] = (TUint8)(gShotH >> 8);
	Put32le(p + 12, gShotNext);
	p[16] = (TUint8)n; p[17] = (TUint8)(n >> 8);
	for (TUint32 i = 0; i < n; ++i)
		{
		const TUint32 px = gShotNext + i, y = px / (TUint32)gShotW, x = px % (TUint32)gShotW;
		Put32le(p + 18 + i * 4, *(const TUint32*)(gShotAddr + y * (TUint32)gShotPitch + x * 4));
		}
	Send(gShotTo.iMac, gShotTo.iIp, KCmdPort, gShotTo.iPort, 18 + n * 4);
	gShotNext += n;
	if (!n)
		gShot = EFalse;
	}

static TBool LogPacket()
	{
	TUint32 head = __atomic_load_n(&gHead, __ATOMIC_ACQUIRE);
	if (head - gTail > KRing)								// overrun: skip to what is still there
		gTail = head - KRing;
	TUint8* p = gFrame + KHdr;
	TUint32 n = 0;
	while (n < 1472 - 12 && gTail != head)
		{
		const TUint8 c = __atomic_load_n(&gRing[gTail % KRing], __ATOMIC_ACQUIRE);
		if (!c)
			break;											// reserved, not stored yet
		gRing[gTail % KRing] = 0;
		p[12 + n++] = c;
		++gTail;
		}
	if (!n)
		return EFalse;
	p[0] = 'P'; p[1] = 'H'; p[2] = 'L'; p[3] = '1';
	Put32le(p + 4, gBoot); Put32le(p + 8, gSeq++);
	Send(KBcastMac, KBcastIp, KLogPort, KLogPort, 12 + n);
	return ETrue;
	}

static void OnState(void*, int aPresent, int aLink, const unsigned char aMac[6])
	{
	for (TInt i = 0; i < 6; ++i)
		gMac[i] = aMac[i];
	LinkLocal(gMac, gIp);
	gLink = aPresent && aLink;
	if (gLink)
		Kern::Printf("[netlog] link up: %d.%d.%d.%d, log -> udp broadcast :%d, commands on :%d, boot id %08x",
			gIp[0], gIp[1], gIp[2], gIp[3], KLogPort, KCmdPort, gBoot);
	}

static void OnRx(void*, const unsigned char* aFrame, unsigned int aLen)
	{
	TUint8 arp[60];
	TUint32 n = ArpReply(aFrame, aLen, gMac, gIp, arp);
	if (n)
		{
		phe_usb_ure_tx(arp, n);
		return;
		}
	TCmd c;
	if (ParseCmd(aFrame, aLen, gMac, c))
		Exec(c);
	}

static void OnPoll(void*)
	{
	++gPolls;
	if (gRebootAt == 0xFFFFFFFFu)
		{
		NKern::DisableAllInterrupts();
		for (;;)
			__asm__ __volatile__("cli; hlt");
		}
	if (gRebootAt && gPolls >= gRebootAt)
		Reboot();
	for (TInt i = 0; i < 4; ++i)							// up to 4 frames a pass (the adapter queues 8)
		{
		if (gShot)
			ShotPacket();
		else if (!LogPacket())
			break;
		}
	}

static const phe_usb_net_sink KSink = { &OnState, &OnRx, &OnPoll, 0 };

void X64StartNetLog()
	{
	TUint32 lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	gBoot = lo ^ (hi << 16) ^ 0x50484C31u;
	phe_usb_ure_set_sink(&KSink);
	Kern::Printf("[netlog] waiting for a USB Ethernet adapter (RTL8152/8153), boot id %08x", gBoot);
	}
