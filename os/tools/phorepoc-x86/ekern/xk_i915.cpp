// Copyright The Phorepoc Authors
// SPDX-License-Identifier: MIT

// xk_i915.cpp -- OURS: the i915 render-only driver's kernel shell (gpd-win2-port.md section 9). Built with I915=1
// (make_ekern.sh); the decisions live in the host-tested headers, this file is the Symbian kernel around them.
//   * X64StartI915: finds the Intel GPU at 00:02.0 (vendor 8086, class 0300), maps BAR0 (GTTMMADR: the registers in the
//     first 8 MB, the GGTT PTEs in the second), sets up the GGTT from GMCH_CTRL (every entry = the scratch page), wakes
//     the render domain, applies the GT workarounds, MOCS and the whitelist, starts the render engine on a status page
//     in the GGTT, installs the "I915" logical device
//   * memory: every GEM object (and the driver's own rings, context images, status page) is one shared chunk over
//     pages from anywhere in RAM (TObjMem): GEM_MMAP hands the client a handle to that chunk, and the pages are freed
//     by the chunk's destroyed-DFC -- after GEM_CLOSE AND the client's last mapping are gone
//   * DI915Channel: one PPGTT + one context (image + ring) per channel; its first request is the context workarounds;
//     Execbuffer = the core's checks + binding + ring, then the context's tail + ELSP. One context on the engine at a
//     time: a submission from another channel first waits for the engine's last seqno (polled; the CSB/interrupt path
//     is in xk_i915_engine.h for the IRQ rung)
//   NOT verified on hardware: QEMU has no Intel GPU (a QEMU boot logs "[i915] no Intel GPU at 00:02.0").
#include <kernel/kern_priv.h>
#include <assp.h>
#include "xk_pci.h"
#include "ri915channel.h"
#include "xk_i915_gtt.h"
#include "xk_i915_lrc.h"
#include "xk_i915_wa.h"
#include "xk_i915_uncore.h"
#include "xk_i915_core.h"
#include "xk_i915_engine.h"

namespace
{
const TUint32 KMmioBytes = 2u << 20;			// the register part of BAR0 the driver touches
const TUint32 KGsmMapBytes = 4u << 20;			// the PTEs mapped: 4 MB = 2 GB of GGTT (all the driver needs)
const TUint32 KGfxFlshCntl = 0x101008;			// GFX_FLSH_CNTL_GEN6: the GGTT write flush
const TInt KRingPages = 4, KCtxPages = 23;		// 16 KB rings; PPHWSP + GEN9_LR_CONTEXT_RENDER_SIZE

// ---- per-object memory: one shared chunk -------------------------------------------------------------------------------
struct TChunkMem
	{
	DChunk* iChunk;
	TPhysAddr* iList;
	TInt iPages;
	TLinAddr iLin;
	TDfc iDfc;
	static void Destroyed(TAny* aPtr)
		{
		TChunkMem* m = (TChunkMem*)aPtr;
		Epoc::FreePhysicalRam(m->iPages, m->iList);
		Kern::Free(m->iList);
		delete m;
		}
	TChunkMem() : iChunk(NULL), iList(NULL), iPages(0), iLin(0), iDfc(Destroyed, this, Kern::DfcQue0(), 1) {}
	};

// (in a critical section) aPages pages, zeroed, mapped contiguously in the kernel; NULL on failure
TChunkMem* ChunkMemNew(TInt aPages)
	{
	TChunkMem* m = new TChunkMem;
	if (!m)
		return NULL;
	m->iPages = aPages;
	m->iList = (TPhysAddr*)Kern::Alloc(aPages * sizeof(TPhysAddr));
	TInt r = m->iList ? Epoc::AllocPhysicalRam(aPages, m->iList) : KErrNoMemory;
	if (r != KErrNone)
		{
		Kern::Free(m->iList);
		delete m;
		return NULL;
		}
	TChunkCreateInfo info;
	info.iType = TChunkCreateInfo::ESharedKernelMultiple;
	info.iMaxSize = aPages << 12;
	info.iMapAttr = EMapAttrCachedMax;				// Kaby Lake: the GPU snoops the LLC
	info.iOwnsMemory = EFalse;
	info.iDestroyedDfc = &m->iDfc;
	TUint32 attr;
	r = Kern::ChunkCreate(info, m->iChunk, m->iLin, attr);
	if (r == KErrNone)
		r = Kern::ChunkCommitPhysical(m->iChunk, 0, aPages << 12, m->iList);
	if (r == KErrNone)
		r = Kern::ChunkAddress(m->iChunk, 0, aPages << 12, m->iLin);
	if (r != KErrNone)
		{
		if (m->iChunk)
			Kern::ChunkClose(m->iChunk);		// the DFC frees the pages and m
		else
			TChunkMem::Destroyed(m);
		return NULL;
		}
	memclr((TAny*)m->iLin, aPages << 12);
	return m;
	}

TInt ObjAlloc(void*, TUint32 aPages, I915::TPhys* aPhys, TUint64** aVirt, void*& aCookie)
	{
	TChunkMem* m = ChunkMemNew((TInt)aPages);
	if (!m)
		return KErrNoMemory;
	for (TUint32 i = 0; i < aPages; ++i)
		{
		aPhys[i] = m->iList[i];
		aVirt[i] = (TUint64*)(m->iLin + (i << 12));
		}
	aCookie = m;
	return KErrNone;
	}
void ObjRelease(void*, void* aCookie) { Kern::ChunkClose(((TChunkMem*)aCookie)->iChunk); }

// page tables: one page each, a chunk per page (few: 1 PML4 + one table per 512 GB / 1 GB / 2 MB in use); the
// allocator needs the chunk back at free time: a small map from the page's physical address
const TInt KMaxPt = 4096;
struct TPtSlot { TPhysAddr iPhys; TChunkMem* iMem; };
TPtSlot ThePts[KMaxPt];
TInt PtAllocTracked(void* aCtx, I915::TPhys& aPhys, TUint64*& aVirt)
	{
	TInt slot = -1;
	for (TInt i = 0; i < KMaxPt && slot < 0; ++i)
		if (!ThePts[i].iMem)
			slot = i;
	if (slot < 0)
		return KErrNoMemory;
	TChunkMem* m = ChunkMemNew(1);
	if (!m)
		return KErrNoMemory;
	(void)aCtx;
	ThePts[slot].iPhys = m->iList[0];
	ThePts[slot].iMem = m;
	aPhys = m->iList[0];
	aVirt = (TUint64*)m->iLin;
	return KErrNone;
	}
void PtFree(void*, I915::TPhys aPhys, TUint64*)
	{
	for (TInt i = 0; i < KMaxPt; ++i)
		if (ThePts[i].iMem && ThePts[i].iPhys == (TPhysAddr)aPhys)
			{
			Kern::ChunkClose(ThePts[i].iMem->iChunk);
			ThePts[i].iMem = NULL;
			return;
			}
	}
void* AllocArray(TUint32 aBytes) { return Kern::Alloc(aBytes); }
void FreeArray(void* aPtr) { Kern::Free(aPtr); }

// ---- the device ---------------------------------------------------------------------------------------------------------
struct TI915Dev
	{
	SPciLoc iLoc;
	TUint32 iDeviceId, iRevision;
	volatile TUint32* iMmio;
	I915::TForcewake iFw;
	I915::TGgttSpace iGgtt;
	I915::TEngine iEngine;
	TChunkMem* iHwsp;
	TChunkMem* iScratch;
	DMutex* iLock;
	TAny* iOnEngine;			// the channel whose context ran last
	TUint32 iLastSeqno;			// the last seqno submitted (one counter for all channels: the HWSP slot is shared)
	TUint32 iNextCtxId;
	};
TI915Dev* TheI915;

TUint32 MmioRead(void* aCtx, TUint32 aOff) { return ((TI915Dev*)aCtx)->iMmio[aOff >> 2]; }
void MmioWrite(void* aCtx, TUint32 aOff, TUint32 aVal) { ((TI915Dev*)aCtx)->iMmio[aOff >> 2] = aVal; }
void DelayUs(void*, TUint32 aUs) { Kern::NanoWait(aUs * 1000); }
void GgttFlush(void* aCtx)
	{
	TI915Dev* d = (TI915Dev*)aCtx;
	d->iMmio[KGfxFlshCntl >> 2] = 1;
	(void)d->iMmio[KGfxFlshCntl >> 2];
	}

// the GGTT range for a driver chunk; 0 = none
TUint32 GgttMap(TI915Dev& d, TChunkMem* aMem, TUint32 aAlign = 4096)
	{
	const TUint32 at = d.iGgtt.Alloc(aMem->iPages, aAlign);
	if (!at)
		return 0;
	I915::TPhys pages[KCtxPages];
	for (TInt i = 0; i < aMem->iPages && i < KCtxPages; ++i)
		pages[i] = aMem->iList[i];
	return d.iGgtt.iGgtt.Insert(at, pages, aMem->iPages) == KErrNone ? at : 0;
	}

// wait (polling, up to aUs) until the HWSP seqno has passed aSeqno
TInt WaitSeqno(TI915Dev& d, TUint32 aSeqno, TUint32 aUs)
	{
	for (TUint32 t = 0;; t += 1000)
		{
		if (I915::SeqnoPassed(d.iEngine.Seqno(), aSeqno))
			return KErrNone;
		if (t >= aUs)
			return KErrTimedOut;
		NKern::Sleep(1);
		}
	}
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
	TInt Execbuffer(DThread* aClient, TAny* aArg);
	I915::TPpgtt* iVm;
	I915::TGemTable* iGem;
	I915::TI915Ctx iCtx;
	TChunkMem* iRing;
	TChunkMem* iImage;
	TUint32 iImageGgtt, iCtxId;
	TBool iWaDone;
	TUint32* iSeen;
	I915::TExecObj* iObjs;
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
	TI915Dev& d = *TheI915;
	const I915::TMem mem = { PtAllocTracked, PtFree, NULL };
	iVm = new I915::TPpgtt;
	iGem = new I915::TGemTable;
	iSeen = (TUint32*)Kern::Alloc(I915::TGemTable::KMaxObjects / 8);
	iObjs = (I915::TExecObj*)Kern::Alloc(KI915MaxExecObjects * sizeof(I915::TExecObj));
	if (!iVm || !iGem || !iSeen || !iObjs)
		return KErrNoMemory;
	iGem->Init(mem, AllocArray, FreeArray);
	iGem->iObjMem.iAlloc = ObjAlloc;
	iGem->iObjMem.iRelease = ObjRelease;
	TInt r = iVm->Create(mem);
	if (r != KErrNone)
		return r;
	iRing = ChunkMemNew(KRingPages);
	iImage = ChunkMemNew(KCtxPages);
	if (!iRing || !iImage)
		return KErrNoMemory;
	Kern::MutexWait(*d.iLock);
	const TUint32 ringGgtt = GgttMap(d, iRing);
	iImageGgtt = GgttMap(d, iImage);
	iCtxId = ++d.iNextCtxId;
	Kern::MutexSignal(*d.iLock);
	if (!ringGgtt || !iImageGgtt)
		return KErrNoMemory;				// (the GGTT space is not given back yet: a bump allocator)
	I915::InitRcsRegState((TUint32*)(iImage->iLin + I915::KStatePage * 4096), ringGgtt, KRingPages * 4096, iVm->iPml4);
	iCtx.iGem = iGem;
	iCtx.iVm = iVm;
	iCtx.iRing.iCpu = (TUint32*)iRing->iLin;
	iCtx.iRing.iDwords = KRingPages * 1024;
	iCtx.iRing.iTail = iCtx.iRing.iHead = 0;
	iCtx.iHwspGgtt = d.iEngine.iHwspGgtt;
	iCtx.iSeqnoOffset = I915::KHwsSeqno * 4;
	iCtx.iNextSeqno = 0;
	iWaDone = EFalse;
	return KErrNone;
	}

DI915Channel::~DI915Channel()
	{
	TI915Dev* d = TheI915;
	if (d && d->iOnEngine == this)
		{
		WaitSeqno(*d, d->iLastSeqno, 2000000);		// the engine must be done with this context's pages
		d->iOnEngine = NULL;
		}
	if (iGem && iVm)
		for (TUint32 h = 1; h <= (TUint32)I915::TGemTable::KMaxObjects; ++h)
			if (iGem->Get(h))
				iGem->Close(h, iVm);
	if (iVm)
		iVm->Destroy();
	if (iRing)
		Kern::ChunkClose(iRing->iChunk);
	if (iImage)
		Kern::ChunkClose(iImage->iChunk);
	Kern::Free(iSeen);
	Kern::Free(iObjs);
	delete iGem;
	delete iVm;
	}

// (in a critical section, the device mutex held) the core's ring emission happened: point the context at the new tail,
// make sure the engine is free of other contexts, submit
static TInt Kick(TI915Dev& d, DI915Channel& c, TUint32 aSeqno)
	{
	if (d.iOnEngine && d.iOnEngine != &c && WaitSeqno(d, d.iLastSeqno, 2000000) != KErrNone)
		return KErrTimedOut;
	I915::SetRingTail((TUint32*)(c.iImage->iLin + I915::KStatePage * 4096), c.iCtx.iRing.iTail * 4);
	d.iOnEngine = &c;
	d.iLastSeqno = aSeqno;
	d.iFw.Get(I915::EFwRender);
	d.iEngine.Submit(I915::Descriptor(c.iImageGgtt, c.iCtxId));
	d.iFw.Put(I915::EFwRender);
	return KErrNone;
	}

TInt DI915Channel::Execbuffer(DThread* aClient, TAny* aArg)
	{
	TI915Dev& d = *TheI915;
	TI915Execbuffer eb;
	TPckg<TI915Execbuffer> p(eb);
	TInt r = Kern::ThreadDesRead(aClient, aArg, p, 0, KChunkShiftBy0);
	if (r != KErrNone)
		return r;
	if (!eb.iCount || eb.iCount > (TUint32)KI915MaxExecObjects)
		return KErrArgument;
	r = Kern::ThreadRawRead(aClient, (const TAny*)(TLinAddr)eb.iObjects, iObjs, eb.iCount * sizeof(TI915ExecObject));
	if (r != KErrNone)
		return r;
	NKern::ThreadEnterCS();
	Kern::MutexWait(*d.iLock);
	// the ring has room once the engine has caught up with this context's requests
	iCtx.iRing.iHead = I915::SeqnoPassed(d.iEngine.Seqno(), d.iLastSeqno) ? iCtx.iRing.iTail : iCtx.iRing.iHead;
	TUint32 seqno = 0;
	if (!iWaDone)
		{
		I915::TWaList wa;
		I915::KblCtxWorkarounds(wa, d.iRevision >= 2);		// KBL C0 = revision 2 (intel_step.c kbl_revids)
		r = iCtx.EmitCtxWa(wa, seqno);
		if (r == KErrNone)
			r = Kick(d, *this, seqno);
		if (r == KErrNone)
			iWaDone = ETrue;
		}
	if (r == KErrNone)
		{
		memclr(iSeen, I915::TGemTable::KMaxObjects / 8);
		r = iCtx.Execbuffer(iObjs, eb.iCount, eb.iBatchStart, eb.iBatchLen, iSeen, seqno);
		if (r == KErrServerBusy && WaitSeqno(d, d.iLastSeqno, 2000000) == KErrNone)
			{
			iCtx.iRing.iHead = iCtx.iRing.iTail;
			memclr(iSeen, I915::TGemTable::KMaxObjects / 8);
			r = iCtx.Execbuffer(iObjs, eb.iCount, eb.iBatchStart, eb.iBatchLen, iSeen, seqno);
			}
		if (r == KErrNone)
			r = Kick(d, *this, seqno);
		}
	Kern::MutexSignal(*d.iLock);
	NKern::ThreadLeaveCS();
	if (r != KErrNone)
		return r;
	eb.iSeqno = seqno;
	return Kern::ThreadDesWrite(aClient, aArg, p, 0, KChunkShiftBy0, NULL);
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
		// the GPU may still use it: the last request must be done before the PPGTT entries go (the pages themselves
		// stay while the client maps them -- the chunk's reference)
		if (d.iOnEngine == this)
			WaitSeqno(d, d.iLastSeqno, 2000000);
		r = iGem->Close((TUint32)(TLinAddr)a1, iVm);
		Kern::MutexSignal(*d.iLock);
		NKern::ThreadLeaveCS();
		return r;
	case EI915CtrlGemMap:
		{
		NKern::ThreadEnterCS();
		Kern::MutexWait(*d.iLock);
		I915::TGemObject* o = iGem->Get((TUint32)(TLinAddr)a1);
		r = o ? Kern::MakeHandleAndOpen(NULL, ((TChunkMem*)o->iCookie)->iChunk) : KErrBadHandle;
		Kern::MutexSignal(*d.iLock);
		NKern::ThreadLeaveCS();
		return r;									// the chunk handle (RChunk::SetReturnedHandle)
		}
	case EI915CtrlExecbuffer:
		return Execbuffer(t, a1);
	case EI915CtrlWait:
		{
		TI915Wait w;
		TPckg<TI915Wait> p(w);
		r = Kern::ThreadDesRead(t, a1, p, 0, KChunkShiftBy0);
		return r != KErrNone ? r : WaitSeqno(d, w.iSeqno, w.iTimeoutUs);
		}
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
	const TUint32 base = bar & ~0xFu;
	const TUint16 gmch = (TUint16)(XkPciRead(0, 2, 0, I915::KGmchCtrl) & 0xFFFF);
	TI915Dev* d = new TI915Dev;
	if (!d)
		return;
	memclr(d, sizeof(*d));
	d->iLoc = loc;
	d->iDeviceId = id >> 16;
	d->iRevision = XkPciRead(0, 2, 0, 8) & 0xFF;
	XkPciWrite16(0, 2, 0, 0x04, (TUint16)(XkPciRead(0, 2, 0, 0x04) | 0x6));	// memory decode + bus master
	DPlatChunkHw* regs = NULL;
	DPlatChunkHw* gsm = NULL;
	TInt r = DPlatChunkHw::New(regs, base, KMmioBytes, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r == KErrNone)
		r = DPlatChunkHw::New(gsm, I915::GgttPtesPhys(base, 16u << 20), KGsmMapBytes, EMapAttrSupRw | EMapAttrFullyBlocking);
	NKern::ThreadEnterCS();
	d->iScratch = r == KErrNone ? ChunkMemNew(1) : NULL;
	d->iHwsp = d->iScratch ? ChunkMemNew(1) : NULL;
	NKern::ThreadLeaveCS();
	if (r != KErrNone || !d->iHwsp)
		{
		Kern::Printf("[i915] %04x: mapping GTTMMADR / allocating failed (%d)", id >> 16, r);
		return;
		}
	d->iMmio = (volatile TUint32*)regs->LinearAddress();
	const I915::TRegs mmio = { MmioRead, MmioWrite, DelayUs, d };
	r = d->iGgtt.Init(gmch, (TUint64*)gsm->LinearAddress(), KGsmMapBytes, d->iScratch->iList[0], GgttFlush, d);
	Kern::Printf("[i915] %04x rev %02x, GTTMMADR %08x, GMCH_CTRL %04x: GGTT %u MB of address space -> %d", d->iDeviceId,
					d->iRevision, base, gmch, d->iGgtt.iGgtt.iEntries >> 8, r);
	if (r != KErrNone)
		return;
	d->iFw.Init(mmio);
	r = d->iFw.Get(I915::EFwRender);
	if (r == KErrNone)
		r = d->iFw.Get(I915::EFwGt);
	Kern::Printf("[i915] forcewake render+GT -> %d (fallbacks %d)", r, d->iFw.iFallbacks);
	if (r != KErrNone)
		return;
	I915::TWaList gt;
	I915::KblGtWorkarounds(gt, d->iRevision < 2);
	I915::WaApply(gt, mmio);
	I915::MocsApply(mmio);
	I915::WhitelistApply(mmio, I915::KRenderRingBase);
	d->iEngine.iRegs = mmio;
	d->iEngine.iBase = I915::KRenderRingBase;
	d->iEngine.iHwsp = (volatile TUint32*)d->iHwsp->iLin;
	d->iEngine.iHwspGgtt = GgttMap(*d, d->iHwsp);
	if (!d->iEngine.iHwspGgtt)
		return;
	d->iEngine.Start();
	d->iFw.Put(I915::EFwGt);
	d->iFw.Put(I915::EFwRender);
	Kern::Printf("[i915] render engine started: HWSP at GGTT %08x, RING_MODE %08x", d->iEngine.iHwspGgtt,
					MmioRead(d, I915::KRenderRingBase + I915::KRingMode));
	_LIT(KLockName, "I915Lock");
	r = Kern::MutexCreate(d->iLock, KLockName, KMutexOrdGeneral0);
	if (r != KErrNone)
		return;
	TheI915 = d;
	DI915Device* dev = new DI915Device;
	r = dev ? Kern::InstallLogicalDevice(dev) : KErrNoMemory;
	Kern::Printf("[i915] logical device I915 installed (r=%d)", r);
	}
