// Copyright The Phorepoc Authors
// SPDX-License-Identifier: MIT

// xk_i915.cpp -- OURS: the i915 render-only driver's kernel shell (rung 4 skeleton; gpd-win2-port.md section 9). NOT YET BUILT
// (make_ekern.sh does not list it; rung 5 wires it up with I915=1 on the gpd-win2 profile).
//   * X64StartI915: finds the Intel GPU on the PCI bus (8086, class 0300, device 0:2.0), sizes and maps BAR0 (GTTMMADR:
//     16 MB, the registers in the first 8 MB, the GGTT PTEs in the second), wakes the render forcewake domain and logs
//     the device id; installs the "I915" logical device
//   * DI915Channel: one PPGTT per channel; Request decodes RI915Channel's controls, copies arguments in and out, holds the
//     device mutex, and hands the decisions to xk_i915_core.h (Execbuffer validation, binding, ring emission)
//   Still to come (rung 5): the GGTT setup (stolen-memory size from GMCH_CTRL, scratch), the engine init (HWSP,
//   RING_MODE run-list enable, the context image, workarounds + MOCS + whitelist at the first request), the ELSP
//   kick + CSB interrupt, GemMap as a shared chunk over the object's pages.
#include <kernel/kern_priv.h>
#include <assp.h>
#include "xk_pci.h"
#include "ri915channel.h"
#include "xk_i915_gtt.h"
#include "xk_i915_lrc.h"
#include "xk_i915_wa.h"
#include "xk_i915_uncore.h"
#include "xk_i915_core.h"

namespace
{
const TUint32 KMmioBytes = 2u << 20;			// the register half of BAR0 we map for now

struct TI915Dev
	{
	SPciLoc iLoc;
	TUint32 iDeviceId, iRevision;
	volatile TUint32* iMmio;
	I915::TForcewake iFw;
	DMutex* iLock;
	};
TI915Dev* TheI915;

TUint32 MmioRead(void* aCtx, TUint32 aOff) { return ((TI915Dev*)aCtx)->iMmio[aOff >> 2]; }
void MmioWrite(void* aCtx, TUint32 aOff, TUint32 aVal) { ((TI915Dev*)aCtx)->iMmio[aOff >> 2] = aVal; }
void DelayUs(void*, TUint32 aUs) { Kern::NanoWait(aUs * 1000); }

// GEM pages: one physical page each, mapped cached (the GPU snoops the LLC)
TInt AllocPage(void*, I915::TPhys& aPhys, TUint64*& aVirt)
	{
	TPhysAddr p;
	TInt r = Epoc::AllocPhysicalRam(4096, p);
	if (r != KErrNone)
		return r;
	DPlatChunkHw* ch = NULL;
	r = DPlatChunkHw::New(ch, p, 4096, EMapAttrSupRw | EMapAttrCachedMax);
	if (r != KErrNone)
		{
		Epoc::FreePhysicalRam(p, 4096);
		return r;
		}
	aPhys = p;
	aVirt = (TUint64*)ch->LinearAddress();
	memclr(aVirt, 4096);
	return KErrNone;
	}
void FreePage(void*, I915::TPhys aPhys, TUint64*)
	{
	Epoc::FreePhysicalRam((TPhysAddr)aPhys, 4096);			// (rung 5: the DPlatChunkHw goes with it)
	}
void* AllocArray(TUint32 aBytes) { return Kern::Alloc(aBytes); }
void FreeArray(void* aPtr) { Kern::Free(aPtr); }
}

class DI915Device : public DLogicalDevice
	{
public:
	DI915Device() { iVersion = TVersion(KI915MajorVersion, 0, 0); }
	virtual TInt Install() { return SetName(&KI915LddName); }
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DLogicalChannelBase*& aChannel);
	};

class DI915Channel : public DLogicalChannelBase
	{
public:
	virtual ~DI915Channel();
	virtual TInt DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer);
	virtual TInt Request(TInt aReqNo, TAny* a1, TAny* a2);
	I915::TPpgtt* iVm;
	I915::TGemTable* iGem;
	};

TInt DI915Device::Create(DLogicalChannelBase*& aChannel)
	{
	aChannel = new DI915Channel;
	return aChannel ? KErrNone : KErrNoMemory;
	}

TInt DI915Channel::DoCreate(TInt, const TDesC8*, const TVersion& aVer)
	{
	if (!TheI915 || !Kern::QueryVersionSupported(TVersion(KI915MajorVersion, 0, 0), aVer))
		return KErrNotSupported;
	const I915::TMem mem = { AllocPage, FreePage, NULL };
	iVm = new I915::TPpgtt;
	iGem = new I915::TGemTable;
	if (!iVm || !iGem)
		return KErrNoMemory;
	iGem->Init(mem, AllocArray, FreeArray);
	return iVm->Create(mem);
	}

DI915Channel::~DI915Channel()
	{
	if (iGem && iVm)
		for (TUint32 h = 1; h <= (TUint32)I915::TGemTable::KMaxObjects; ++h)
			if (iGem->Get(h))
				iGem->Close(h, iVm);
	if (iVm)
		iVm->Destroy();
	delete iGem;
	delete iVm;
	}

TInt DI915Channel::Request(TInt aReqNo, TAny* a1, TAny*)
	{
	if (aReqNo < 0)
		return KErrNotSupported;
	DThread* const t = &Kern::CurrentThread();
	TI915Dev& d = *TheI915;
	TInt r = KErrNotSupported;
	switch (aReqNo)
		{
	case EI915CtrlGetParam:
		{
		TI915GetParam g;
		TPckg<TI915GetParam> p(g);
		r = Kern::ThreadDesRead(t, a1, p, 0, KChunkShiftBy0);
		if (r != KErrNone)
			return r;
		switch (g.iParam)
			{
		case EI915ParamChipsetId: g.iValue = (TInt)d.iDeviceId; break;
		case EI915ParamRevision: g.iValue = (TInt)d.iRevision; break;
		case EI915ParamHasExecbuf2: case EI915ParamHasExecSoftpin: g.iValue = 1; break;
		case EI915ParamCsTimestampFrequency: g.iValue = 12000000; break;	// Gen9: 12 MHz (read_reference_ts_freq)
		default: return KErrArgument;						// -EINVAL, as i915_getparam_ioctl
			}
		return Kern::ThreadDesWrite(t, a1, p, 0, KChunkShiftBy0, NULL);
		}
	case EI915CtrlGemCreate:
		{
		TI915GemCreate c;
		TPckg<TI915GemCreate> p(c);
		r = Kern::ThreadDesRead(t, a1, p, 0, KChunkShiftBy0);
		if (r != KErrNone)
			return r;
		NKern::ThreadEnterCS();
		Kern::MutexWait(*d.iLock);
		r = iGem->Create(c.iSize, c.iHandle);
		if (r == KErrNone)
			c.iSize = (TUint64)iGem->Get(c.iHandle)->iPages << 12;
		Kern::MutexSignal(*d.iLock);
		NKern::ThreadLeaveCS();
		return r != KErrNone ? r : Kern::ThreadDesWrite(t, a1, p, 0, KChunkShiftBy0, NULL);
		}
	case EI915CtrlGemClose:
		NKern::ThreadEnterCS();
		Kern::MutexWait(*d.iLock);
		r = iGem->Close((TUint32)(TLinAddr)a1, iVm);
		Kern::MutexSignal(*d.iLock);
		NKern::ThreadLeaveCS();
		return r;
	case EI915CtrlGemMap:
	case EI915CtrlExecbuffer:
	case EI915CtrlWait:
		return KErrNotReady;							// rung 5 (the engine is not started yet)
	default:
		return KErrNotSupported;
		}
	}

void X64StartI915()
	{
	SPciLoc loc;
	if (!XkPciFind(0, 0x0300, loc) || loc.iBus != 0 || loc.iDev != 2 || (XkPciRead(0, 2, 0, 0) & 0xFFFF) != 0x8086)
		{
		Kern::Printf("[i915] no Intel GPU at 00:02.0");
		return;
		}
	const TUint32 id = XkPciRead(0, 2, 0, 0);
	const TUint32 bar = XkPciRead(0, 2, 0, 0x10), hi = XkPciRead(0, 2, 0, 0x14);
	if (hi || !(bar & ~0xFu))
		{
		Kern::Printf("[i915] %04x: GTTMMADR %08x:%08x above 4 GB or unassigned", id >> 16, hi, bar);
		return;
		}
	TI915Dev* d = new TI915Dev;
	if (!d)
		return;
	d->iLoc = loc;
	d->iDeviceId = id >> 16;
	d->iRevision = XkPciRead(0, 2, 0, 8) & 0xFF;
	XkPciWrite16(0, 2, 0, 0x04, (TUint16)(XkPciRead(0, 2, 0, 0x04) | 0x6));	// memory decode + bus master
	DPlatChunkHw* ch = NULL;
	TInt r = DPlatChunkHw::New(ch, bar & ~0xFu, KMmioBytes, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone)
		{
		Kern::Printf("[i915] mapping GTTMMADR failed (%d)", r);
		delete d;
		return;
		}
	d->iMmio = (volatile TUint32*)ch->LinearAddress();
	const I915::TRegs regs = { MmioRead, MmioWrite, DelayUs, d };
	d->iFw.Init(regs);
	r = d->iFw.Get(I915::EFwRender);
	Kern::Printf("[i915] %04x rev %02x at GTTMMADR %08x; render forcewake -> %d (fallbacks %d)", d->iDeviceId, d->iRevision,
					bar & ~0xFu, r, d->iFw.iFallbacks);
	if (r == KErrNone)
		d->iFw.Put(I915::EFwRender);
	_LIT(KLockName, "I915Lock");
	r = Kern::MutexCreate(d->iLock, KLockName, KMutexOrdGeneral0);
	if (r != KErrNone)
		return;
	TheI915 = d;
	DI915Device* dev = new DI915Device;
	r = dev ? Kern::InstallLogicalDevice(dev) : KErrNoMemory;
	Kern::Printf("[i915] logical device I915 installed (r=%d)", r);
	}
