// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_vmmchannel.cpp -- OURS: the VirtualBox guest-additions device (VMMDev, xk_absmouse.cpp finds it) as a thin request channel
// for a user-side program: the logical device "PheVmmDev" (rvmmdevchannel.h). The shape of the other board channels (VmwSvga):
// DoCreate gives the client an ARENA, a shared chunk over one physically contiguous RAM block; the client builds VMMDev requests
// in it and Submit writes a request's physical address to the device's request port, as xk_absmouse.cpp's VmmRequest does for the
// kernel's own requests. No protocol is parsed here: HGCM (what the shared clipboard, shared folders and drag and drop ride on)
// is spoken by the package that installs from Catalogue. The device is told "guest additions present" by xk_absmouse.cpp
// (ReportGuestInfo) before this can be opened.
//
// What this adds to the kernel is a bound, not a protocol: only the HGCM request types are accepted, inside the arena, and for
// a call every page list must name pages of the arena, so a client's bug cannot make the host read or write other RAM. (The
// arena is the client's memory too, so a client that races its own request defeats the check: the client is trusted, it needs
// CommDD; the bound is no security boundary against it.) The device completes a request the host works on later (a parked clipboard call) by writing into the arena; so a channel
// that closes without saying Idle keeps its arena rather than free memory the host may still write into.
#include <kernel/kern_priv.h>
#include <assp.h>
#include "xk_absmouse.h"
#include "xk_pci.h"
#include "rvmmdevchannel.h"

// VMMDev.h (the host's ABI): request types and the HGCM call layout
enum { EReqHgcmConnect = 60, EReqHgcmDisconnect = 61, EReqHgcmCall32 = 62, EReqHgcmCancel = 64 };
enum { EParm32 = 1, EParm64 = 2, EParmPageList = 10, EParmEmbedded = 11 };
const TInt KReqHdr = 24;							// VMMDevRequestHeader
const TInt KHgcmCallFixed = 44;						// + flags, result, client id, function, parameter count
const TInt KHgcmParm = 12;							// HGCMFunctionParameter32 (packed to 4)
const TInt KMaxHgcmParms = 32;
const TInt KArenaBytes = 256 << 10, KArenaMin = 64 << 10;

static inline TUint32 Rd(const TUint8* aBase, TInt aOff) { return *(const TUint32*)(aBase + aOff); }

// the arena: its pages are freed when the chunk is gone (a DFC: the last user mapping may go at any time), unless it is kept
class TPheVmmArena
	{
public:
	static void DestroyedFn(TAny* aPtr)
		{
		TPheVmmArena* a = (TPheVmmArena*)aPtr;
		if (a->iPhys && !a->iKeep)
			Epoc::FreePhysicalRam(a->iPhys, a->iBytes);
		delete a;
		}
	TPheVmmArena() : iDfc(DestroyedFn, this, Kern::DfcQue0(), 1), iChunk(NULL), iPhys(0), iBytes(0), iKeep(EFalse), iKernAddr(0) {}
	TDfc iDfc;
	DChunk* iChunk;
	TPhysAddr iPhys;
	TInt iBytes;
	TBool iKeep;
	TLinAddr iKernAddr;
	};

class DPheVmmDevice : public DLogicalDevice
	{
public:
	DPheVmmDevice() { iVersion = TVersion(KPheVmmDevMajorVersion, 0, 0); }
	virtual TInt Install()						{ return SetName(&KPheVmmDevName); }
	virtual void GetCaps(TDes8&) const			{}
	virtual TInt Create(DLogicalChannelBase*& aChannel);
	};

class DPheVmmChannel : public DLogicalChannelBase
	{
public:
	virtual ~DPheVmmChannel();
	virtual TInt DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer);
	virtual TInt Request(TInt aReqNo, TAny* a1, TAny* a2);
	TInt Validate(TInt aOff);
	TPheVmmArena* iArena;
	TBool iIdle;
	TBool iSubmitted;
	TUint16 iPort;
	};

TInt DPheVmmDevice::Create(DLogicalChannelBase*& aChannel)
	{
	aChannel = new DPheVmmChannel;
	return aChannel ? KErrNone : KErrNoMemory;
	}

static TPheVmmArena* NewArena(TInt aBytes)
	{
	TPheVmmArena* a = new TPheVmmArena;
	if (!a)
		return NULL;
	a->iBytes = aBytes;
	TChunkCreateInfo info;
	info.iType = TChunkCreateInfo::ESharedKernelMultiple;
	info.iMaxSize = aBytes;
	info.iMapAttr = EMapAttrCachedMax;				// x86 DMA is cache-coherent
	info.iOwnsMemory = EFalse;						// the RAM is ours, not the chunk's
	info.iDestroyedDfc = &a->iDfc;
	TLinAddr lin;
	TUint32 attr;
	if (Kern::ChunkCreate(info, a->iChunk, lin, attr) != KErrNone)
		{
		delete a;
		return NULL;
		}
	TInt r = Epoc::AllocPhysicalRam(aBytes, a->iPhys);
	if (r == KErrNone)
		r = Kern::ChunkCommitPhysical(a->iChunk, 0, aBytes, a->iPhys);
	if (r == KErrNone)
		r = Kern::ChunkAddress(a->iChunk, 0, aBytes, a->iKernAddr);
	if (r != KErrNone)
		{
		DChunk* c = a->iChunk;
		a->iChunk = NULL;
		if (a->iPhys)
			Epoc::FreePhysicalRam(a->iPhys, aBytes);
		a->iPhys = 0;
		Kern::ChunkClose(c);						// the destroyed-DFC deletes the arena object
		return NULL;
		}
	memclr((TAny*)a->iKernAddr, aBytes);
	return a;
	}

TInt DPheVmmChannel::DoCreate(TInt, const TDesC8*, const TVersion& aVer)
	{
	iArena = NULL;
	iIdle = ETrue;
	iSubmitted = EFalse;
	if (!Kern::QueryVersionSupported(TVersion(KPheVmmDevMajorVersion, 0, 0), aVer))
		return KErrNotSupported;
	if (!Kern::CurrentThreadHasCapability(ECapabilityCommDD, __PLATSEC_DIAGNOSTIC_STRING("Checked by PheVmmDev")))
		return KErrPermissionDenied;
	iPort = (TUint16)X64VmmDevPort();
	if (!iPort)
		return KErrNotSupported;					// no guest-additions device here (not VirtualBox)
	NKern::ThreadEnterCS();
	iArena = NewArena(KArenaBytes);
	if (!iArena)
		iArena = NewArena(KArenaMin);
	NKern::ThreadLeaveCS();
	return iArena ? KErrNone : KErrNoMemory;
	}

DPheVmmChannel::~DPheVmmChannel()
	{
	NKern::ThreadEnterCS();
	if (iArena)
		{
		if (iSubmitted && !iIdle)
			{
			iArena->iKeep = ETrue;					// the host may still complete a request into it
			Kern::Printf("[vmmch] channel closed with requests outstanding: arena %u KB at %08x kept", iArena->iBytes >> 10, iArena->iPhys);
			}
		DChunk* c = iArena->iChunk;
		iArena->iChunk = NULL;
		Kern::ChunkClose(c);						// the destroyed-DFC frees the RAM (unless kept) and the object
		}
	NKern::ThreadLeaveCS();
	}

// Is the request at aOff acceptable? An HGCM request inside the arena; a call's page lists point into the arena.
TInt DPheVmmChannel::Validate(TInt aOff)
	{
	const TInt total = iArena->iBytes;
	if (aOff < 0 || (aOff & 3) || aOff + KReqHdr + 12 > total)
		return KErrArgument;
	const TUint8* req = (const TUint8*)iArena->iKernAddr + aOff;
	const TUint32 size = Rd(req, 0);
	const TUint32 type = Rd(req, 8);
	if (size < (TUint32)(KReqHdr + 8) || size > (TUint32)(total - aOff))
		return KErrArgument;
	switch (type)
		{
	case EReqHgcmConnect: case EReqHgcmDisconnect: case EReqHgcmCancel:
		return KErrNone;
	case EReqHgcmCall32:
		break;
	default:
		return KErrNotSupported;
		}
	if (size < (TUint32)KHgcmCallFixed)
		return KErrArgument;
	const TUint32 cParms = Rd(req, 40);
	if (cParms > (TUint32)KMaxHgcmParms || size < KHgcmCallFixed + cParms * KHgcmParm)
		return KErrArgument;
	for (TUint32 i = 0; i < cParms; ++i)
		{
		const TInt p = KHgcmCallFixed + i * KHgcmParm;
		const TUint32 pt = Rd(req, p);
		if (pt == EParm32 || pt == EParm64 || pt == EParmEmbedded)
			continue;							// an embedded buffer lies in the request itself (the host bounds it by the request size)
		if (pt != EParmPageList)
			return KErrNotSupported;			// no linear or physical address parameters
		const TUint32 pcb = Rd(req, p + 4), poff = Rd(req, p + 8);
		if (!pcb)
			continue;
		if (poff < (TUint32)KHgcmCallFixed || poff + 8 > size || (poff & 3))
			return KErrArgument;
		const TUint32 cPages = (Rd(req, poff + 4) >> 16) & 0xFFFF;		// HGCMPageListInfo: flags, offFirstPage (u16), cPages (u16)
		if (!cPages || poff + 8 + cPages * 8 > size)
			return KErrArgument;
		for (TUint32 k = 0; k < cPages; ++k)
			{
			const TUint32 lo = Rd(req, poff + 8 + k * 8), hi = Rd(req, poff + 12 + k * 8);
			if (hi || (lo & 0xFFF) || lo < iArena->iPhys || lo + 0x1000 > iArena->iPhys + (TUint32)total)
				return KErrArgument;
			}
		}
	return KErrNone;
	}

TInt DPheVmmChannel::Request(TInt aReqNo, TAny* a1, TAny*)
	{
	if (aReqNo < 0)
		return KErrNone;
	DThread* const t = &Kern::CurrentThread();
	switch (aReqNo)
		{
	case EPheVmmCtrlGetInfo:
		{
		TPheVmmDevInfo info;
		memclr(&info, sizeof(info));
		info.iVersion = 0x00010004;
		info.iPort = iPort;
		info.iArenaPhys = iArena->iPhys;
		info.iArenaSize = iArena->iBytes;
		TPckgC<TPheVmmDevInfo> p(info);
		return Kern::ThreadDesWrite(t, a1, p, 0, KChunkShiftBy0, NULL);
		}
	case EPheVmmCtrlOpenArena:
		{
		NKern::ThreadEnterCS();
		TInt h = Kern::MakeHandleAndOpen(NULL, iArena->iChunk);
		NKern::ThreadLeaveCS();
		return h;
		}
	case EPheVmmCtrlSubmit:
		{
		const TInt off = (TInt)a1;
		TInt r = Validate(off);
		if (r != KErrNone)
			return r;
		iSubmitted = ETrue;
		iIdle = EFalse;
		XkPortOutl(iPort, iArena->iPhys + off);		// the device takes the request; the host completes it in the arena
		return KErrNone;
		}
	case EPheVmmCtrlIdle:
		iIdle = ETrue;
		return KErrNone;
	default:
		return KErrNotSupported;
		}
	}

void X64StartVmmChannel()
	{
	DPheVmmDevice* dev = new DPheVmmDevice;
	if (!dev)
		return;
	TInt r = Kern::InstallLogicalDevice(dev);
	Kern::Printf("[vmmch] logical device PheVmmDev installed (r=%d)", r);
	}
