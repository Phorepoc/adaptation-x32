// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_bdchannel.cpp -- OURS: the VMware backdoor (port 0x5658, the "vmmouse" xk_absmouse.cpp already speaks) as a thin channel for a
// user-side program: the logical device "PheBackdoor" (rphebackdoor.h). The backdoor is an IN with the registers as the message, which
// is privileged, so the kernel does the IN for the client. No protocol is parsed here: GuestRPC (RPCI, TCLO: copy and paste,
// drag and drop) is spoken by the package that installs from Catalogue.
//
// What this adds to the kernel is a bound, not a protocol: only GETVERSION and the GuestRPC MESSAGE command are executed, a message
// channel can only be opened for the RPCI or TCLO protocol, and every later message type must name a channel this client opened. The
// channels a client leaves open are closed when its handle goes away (the host keeps channel slots otherwise).
#include <kernel/kern_priv.h>
#include <assp.h>
#include "rphebackdoor.h"

static const TUint32 KBdMagic = 0x564D5868, KBdPort = 0x5658;
static const TUint32 KProtoRpci = 0x49435052, KProtoTclo = 0x4F4C4354, KCookieFlag = 0x80000000;
enum { EBdGetVersion = 10, EBdMessage = 30 };
enum { EMsgOpen = 0, EMsgClose = 6 };
static const TUint32 KMsgSuccess = 1;
static const TInt KMaxChans = 8;

static void BdIn(TPheBdRegs& r)
	{
	TUint32 a = KBdMagic, b = r.iEbx, c = r.iEcx, d = (r.iEdx & 0xFFFF0000u) | KBdPort, s = r.iEsi, di = r.iEdi;
	__asm__ __volatile__("in eax, dx" : "+a"(a), "+b"(b), "+c"(c), "+d"(d), "+S"(s), "+D"(di) : : "memory");
	r.iEax = a; r.iEbx = b; r.iEcx = c; r.iEdx = d; r.iEsi = s; r.iEdi = di;
	}

static TBool BackdoorPresent()
	{
	TPheBdRegs r;
	memclr(&r, sizeof(r));
	r.iEbx = ~KBdMagic;
	r.iEcx = EBdGetVersion;
	BdIn(r);
	return r.iEbx == KBdMagic;
	}

struct TBdChan { TUint32 iId, iCookieHi, iCookieLo; };

class DPheBdDevice : public DLogicalDevice
	{
public:
	DPheBdDevice() { iVersion = TVersion(KPheBackdoorMajorVersion, 0, 0); }
	virtual TInt Install()						{ return SetName(&KPheBackdoorName); }
	virtual void GetCaps(TDes8&) const			{}
	virtual TInt Create(DLogicalChannelBase*& aChannel);
	};

class DPheBdChannel : public DLogicalChannelBase
	{
public:
	virtual ~DPheBdChannel();
	virtual TInt DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer);
	virtual TInt Request(TInt aReqNo, TAny* a1, TAny* a2);
	TInt Find(TUint32 aId);
	TBdChan iChan[KMaxChans];
	TInt iNum;
	};

TInt DPheBdDevice::Create(DLogicalChannelBase*& aChannel)
	{
	aChannel = new DPheBdChannel;
	return aChannel ? KErrNone : KErrNoMemory;
	}

TInt DPheBdChannel::DoCreate(TInt, const TDesC8*, const TVersion& aVer)
	{
	iNum = 0;
	if (!Kern::QueryVersionSupported(TVersion(KPheBackdoorMajorVersion, 0, 0), aVer))
		return KErrNotSupported;
	if (!Kern::CurrentThreadHasCapability(ECapabilityCommDD, __PLATSEC_DIAGNOSTIC_STRING("Checked by PheBackdoor")))
		return KErrPermissionDenied;
	if (!BackdoorPresent())
		return KErrNotSupported;					// no VMware backdoor here
	return KErrNone;
	}

DPheBdChannel::~DPheBdChannel()
	{
	for (TInt i = 0; i < iNum; ++i)
		{
		TPheBdRegs r;
		memclr(&r, sizeof(r));
		r.iEcx = (EMsgClose << 16) | EBdMessage;
		r.iEdx = iChan[i].iId << 16;
		r.iEsi = iChan[i].iCookieHi;
		r.iEdi = iChan[i].iCookieLo;
		BdIn(r);
		}
	if (iNum)
		Kern::Printf("[bdch] closed %d message channel(s) the client left open", iNum);
	}

TInt DPheBdChannel::Find(TUint32 aId)
	{
	for (TInt i = 0; i < iNum; ++i)
		if (iChan[i].iId == aId)
			return i;
	return -1;
	}

TInt DPheBdChannel::Request(TInt aReqNo, TAny* a1, TAny*)
	{
	if (aReqNo < 0)
		return KErrNone;
	if (aReqNo != EPheBdCtrlCall)
		return KErrNotSupported;
	DThread* const t = &Kern::CurrentThread();
	TPheBdRegs r;
	TPckg<TPheBdRegs> p(r);
	TInt res = Kern::ThreadDesRead(t, a1, p, 0, KChunkShiftBy0);
	if (res != KErrNone)
		return res;
	const TUint32 cmd = r.iEcx & 0xFFFF, type = r.iEcx >> 16;
	TInt slot = -1;
	if (cmd == EBdGetVersion)
		{
		if (type)
			return KErrArgument;
		}
	else if (cmd == EBdMessage)
		{
		if (type > EMsgClose)
			return KErrArgument;
		if (type == EMsgOpen)
			{
			const TUint32 proto = r.iEbx & ~KCookieFlag;
			if (proto != KProtoRpci && proto != KProtoTclo)
				return KErrNotSupported;
			if (iNum >= KMaxChans)
				return KErrOverflow;
			}
		else
			{
			slot = Find(r.iEdx >> 16);
			if (slot < 0)
				return KErrNotFound;				// a channel this client did not open
			}
		}
	else
		return KErrNotSupported;
	BdIn(r);
	if (cmd == EBdMessage)
		{
		if (type == EMsgOpen && (r.iEcx >> 16 & KMsgSuccess))
			{
			iChan[iNum].iId = r.iEdx >> 16;
			iChan[iNum].iCookieHi = r.iEsi;
			iChan[iNum].iCookieLo = r.iEdi;
			++iNum;
			}
		else if (type == EMsgClose && slot >= 0 && (r.iEcx >> 16 & KMsgSuccess))
			{
			iChan[slot] = iChan[--iNum];
			}
		}
	return Kern::ThreadDesWrite(t, a1, p, 0, KChunkShiftBy0, NULL);
	}

void X64StartBackdoorChannel()
	{
	DPheBdDevice* dev = new DPheBdDevice;
	if (!dev)
		return;
	TInt r = Kern::InstallLogicalDevice(dev);
	Kern::Printf("[bdch] logical device PheBackdoor installed (r=%d)", r);
	}
