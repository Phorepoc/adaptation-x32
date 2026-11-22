// Copyright The Phorepoc Authors
// Copyright (c) 2014-2020 Intel Corporation (the PTE/PDE encodings and the PPAT table, ported from Linux
//   drivers/gpu/drm/i915/gt/gen8_ppgtt.c, intel_ggtt.c, intel_gtt.c, intel_gtt.h, v6.12)
// SPDX-License-Identifier: MIT

// xk_i915_gtt.h -- the GEM/GTT layer of the i915 render-only port (gpd-win2-port.md section 9): Gen8+ page-table
// entries, the global GTT (GGTT: PTEs in BAR0's upper half), the 4-level 48-bit per-process GTT (PPGTT) that iris
// softpins its buffers into, and the GEM object table. Header-only; memory and MMIO go through callbacks, so the host
// test (tests/i915_gtt) compiles it unmodified against a memory mock.
//
// The kernel side gives it: page allocation (physical address + a kernel mapping of a 4 KB zeroed page), and the GGTT's
// PTE window. No locking here: the driver's channel serialises calls (one DFC queue).
#ifndef XK_I915_GTT_H
#define XK_I915_GTT_H

namespace I915
	{
	typedef TUint64 TPhys;

	// ---- page-table entries (intel_gtt.h, gen8_ppgtt.c, intel_ggtt.c) -------------------------------------------------
	const TUint64 KPagePresent = 1ull << 0;			// GEN8_PAGE_PRESENT
	const TUint64 KPageRw      = 1ull << 1;			// GEN8_PAGE_RW
	const TUint64 KPagePwt     = 1ull << 3;			// x86 _PAGE_PWT  \  the PPAT index bits of a Gen8 PTE
	const TUint64 KPagePcd     = 1ull << 4;			// x86 _PAGE_PCD   } (PAT, PCD, PWT = index bits 2, 1, 0)
	const TUint64 KPagePat     = 1ull << 7;			// x86 _PAGE_PAT  /
	const TUint64 KPpatUncached  = KPagePwt | KPagePcd;	// PPAT index 3 (UC)
	const TUint64 KPpatCachedPde = 0;			// index 0 (WB LLC)
	const TUint64 KPpatCached    = KPagePat;		// index 4 (WB LLC/eLLC)
	const TUint64 KPpatDisplay   = KPagePcd;		// index 2 (eLLC WT; WB on Gen9)
	const TUint64 KAddrMask      = 0x0000FFFFFFFFF000ull;	// bits 47:12

	enum TCache { ECacheNone = 0, ECacheLlc = 1, ECacheL3Llc = 2, ECacheWt = 3 };	// enum i915_cache_level

	inline TUint64 PteEncode(TPhys aAddr, TCache aCache, TBool aReadOnly = EFalse)	// gen8_pte_encode
		{
		TUint64 pte = (aAddr & KAddrMask) | KPagePresent | KPageRw;
		if (aReadOnly)
			pte &= ~KPageRw;
		switch (aCache)
			{
			case ECacheNone: pte |= KPpatUncached; break;
			case ECacheWt:   pte |= KPpatDisplay; break;
			default:         pte |= KPpatCached; break;
			}
		return pte;
		}
	inline TUint64 PdeEncode(TPhys aAddr, TCache aCache)		// gen8_pde_encode
		{
		return (aAddr & KAddrMask) | KPagePresent | KPageRw | (aCache != ECacheNone ? KPpatCachedPde : KPpatUncached);
		}
	inline TUint64 GgttPteEncode(TPhys aAddr)			// gen8_ggtt_pte_encode (no PAT bits: the GGTT uses PAT 0)
		{
		return (aAddr & KAddrMask) | KPagePresent;
		}

	// the private PPAT (bdw_setup_private_ppat, Gen9): GEN8_PRIVATE_PAT_LO = low 32 bits, _HI = high
	inline TUint64 Gen9PrivatePpat()
		{
		const TUint64 wb = 3, wt = 2, wc = 1, uc = 0, llc = 1 << 2, llcellc = 3 << 2;	// GEN8_PPAT_* (LLCeLLC = 3<<2)
		#define XK_PPAT(i, x) ((TUint64)(x) << ((i) * 8))
		TUint64 pat = XK_PPAT(0, wb | llc) | XK_PPAT(1, wc | llcellc) | XK_PPAT(3, uc)
			| XK_PPAT(4, wb | llcellc | (0 << 4)) | XK_PPAT(5, wb | llcellc | (1 << 4))
			| XK_PPAT(6, wb | llcellc | (2 << 4)) | XK_PPAT(7, wb | llcellc | (3 << 4))
			| XK_PPAT(2, wb);					// Gen9: index 2 = WB + eLLC override (0)
		#undef XK_PPAT
		(void)wt;
		return pat;
		}

	// GGTT size from the PCI config word SNB_GMCH_CTRL (0x50): GGMS (bits 7:6) -> 2^GGMS MB of PTEs, 8 bytes per 4 KB page
	inline TUint64 GgttBytesOfPtes(TUint16 aGmchCtl)		// gen8_get_total_gtt_size
		{
		TUint g = (aGmchCtl >> 6) & 3;
		return g ? (1ull << g) << 20 : 0;
		}
	inline TUint64 GgttSpace(TUint16 aGmchCtl)			// the GPU address range the GGTT maps
		{
		return GgttBytesOfPtes(aGmchCtl) / 8 * 4096;
		}

	// ---- memory: what the kernel side provides ------------------------------------------------------------------------
	struct TMem
		{
		// a zeroed 4 KB page: its physical address and a kernel pointer to it; KErrNoMemory -> aPhys = 0
		TInt (*iAllocPage)(void* aCtx, TPhys& aPhys, TUint64*& aVirt);
		void (*iFreePage)(void* aCtx, TPhys aPhys, TUint64* aVirt);
		void* iCtx;
		};

	// ---- the GGTT -----------------------------------------------------------------------------------------------------
	struct TGgtt
		{
		volatile TUint64* iPtes;		// BAR0 + 8 MB (GTTADR), uncached
		TUint32 iEntries;			// pages it maps
		TPhys iScratch;				// the page every unused entry points at
		void (*iFlush)(void* aCtx);		// GFX_FLSH_CNTL write + posting read
		void* iCtx;

		void Clear(TUint32 aFirst, TUint32 aCount)
			{
			const TUint64 s = GgttPteEncode(iScratch);
			for (TUint32 i = aFirst; i < aFirst + aCount && i < iEntries; ++i)
				iPtes[i] = s;
			if (iFlush) iFlush(iCtx);
			}
		// map aCount pages (aPages[]) at GPU address aGpuAddr (4 KB aligned)
		TInt Insert(TUint64 aGpuAddr, const TPhys* aPages, TUint32 aCount)
			{
			const TUint64 first = aGpuAddr >> 12;
			if ((aGpuAddr & 0xFFF) || first + aCount > iEntries)
				return KErrArgument;
			for (TUint32 i = 0; i < aCount; ++i)
				iPtes[first + i] = GgttPteEncode(aPages[i]);
			if (iFlush) iFlush(iCtx);
			return KErrNone;
			}
		};

	// ---- the PPGTT: 4 levels of 512 entries, 48-bit GPU addresses ------------------------------------------------------
	const TInt KLevels = 4;
	inline TUint Index(TUint64 aVa, TInt aLevel)	// level 3 = PML4 (bits 47:39) .. level 0 = PT (bits 20:12)
		{
		return (TUint)((aVa >> (12 + 9 * aLevel)) & 511);
		}

	struct TPpgtt
		{
		TMem iMem;
		TPhys iScratchPage;			// the data page unmapped PTEs point at (reads give zero, writes go nowhere)
		TPhys iScratchTable[KLevels - 1];	// scratch PT / PD / PDP: a table full of entries to the level below's scratch
		TUint64* iScratchVirt[KLevels];		// [0] = the page, [1..3] = the tables
		TPhys iPml4;				// the context's PML4 (the LRC's PDP0 register pair holds it)
		TUint64* iPml4Virt;

		// the kernel pointers of the page tables we allocated, by physical address (a small open table: the
		// driver keeps one TPpgtt per GEM VM; 4096 tables = 8 GB of mapped GPU space at 4 KB pages)
		enum { KMaxTables = 4096 };
		TPhys iTabPhys[KMaxTables];
		TUint64* iTabVirt[KMaxTables];
		TInt iTables;

		TInt Create(const TMem& aMem)
			{
			iMem = aMem; iTables = 0;
			TInt r = iMem.iAllocPage(iMem.iCtx, iScratchPage, iScratchVirt[0]);
			for (TInt l = 1; l < KLevels && r == KErrNone; ++l)
				{
				r = iMem.iAllocPage(iMem.iCtx, iScratchTable[l - 1], iScratchVirt[l]);
				if (r != KErrNone)
					break;
				const TUint64 e = l == 1 ? PteEncode(iScratchPage, ECacheLlc) : PdeEncode(iScratchTable[l - 2], ECacheLlc);
				for (TInt i = 0; i < 512; ++i)
					iScratchVirt[l][i] = e;
				}
			if (r == KErrNone)
				r = NewTable(3, iPml4, iPml4Virt);
			return r;
			}

		// a table at aLevel (3 = PML4 .. 0 = PT) pre-filled with entries to the scratch of the level below
		TInt NewTable(TInt aLevel, TPhys& aPhys, TUint64*& aVirt)
			{
			if (iTables >= KMaxTables)
				return KErrNoMemory;
			TInt r = iMem.iAllocPage(iMem.iCtx, aPhys, aVirt);
			if (r != KErrNone)
				return r;
			const TUint64 e = aLevel == 0 ? PteEncode(iScratchPage, ECacheLlc) : PdeEncode(iScratchTable[aLevel - 1], ECacheLlc);
			for (TInt i = 0; i < 512; ++i)
				aVirt[i] = e;
			iTabPhys[iTables] = aPhys; iTabVirt[iTables] = aVirt; ++iTables;
			return KErrNone;
			}
		TUint64* VirtOf(TPhys aPhys) const
			{
			for (TInt i = 0; i < iTables; ++i)
				if (iTabPhys[i] == aPhys)
					return iTabVirt[i];
			return 0;
			}
		TBool IsScratch(TUint64 aEntry, TInt aLevel) const	// aLevel = the level the entry lives in
			{
			const TPhys a = aEntry & KAddrMask;
			return aLevel == 0 ? a == iScratchPage : a == iScratchTable[aLevel - 1];
			}

		// the PT that covers aVa, created on the way down when aCreate
		TUint64* WalkToPt(TUint64 aVa, TBool aCreate)
			{
			TUint64* t = iPml4Virt;
			for (TInt l = 3; l > 0; --l)
				{
				TUint64& e = t[Index(aVa, l)];
				if (IsScratch(e, l))
					{
					if (!aCreate)
						return 0;
					TPhys p; TUint64* v;
					if (NewTable(l - 1, p, v) != KErrNone)
						return 0;
					e = PdeEncode(p, ECacheLlc);
					}
				t = VirtOf(e & KAddrMask);
				if (!t)
					return 0;
				}
			return t;
			}

		// map aCount pages at aVa (48-bit, 4 KB aligned)
		TInt Map(TUint64 aVa, const TPhys* aPages, TUint32 aCount, TCache aCache)
			{
			if ((aVa & 0xFFF) || aVa + ((TUint64)aCount << 12) > (1ull << 48))
				return KErrArgument;
			for (TUint32 i = 0; i < aCount; ++i)
				{
				const TUint64 va = aVa + ((TUint64)i << 12);
				TUint64* pt = WalkToPt(va, ETrue);
				if (!pt)
					return KErrNoMemory;
				pt[Index(va, 0)] = PteEncode(aPages[i], aCache);
				}
			return KErrNone;
			}
		// back to scratch (the tables stay: iris reuses its VA ranges)
		void Unmap(TUint64 aVa, TUint32 aCount)
			{
			for (TUint32 i = 0; i < aCount; ++i)
				{
				const TUint64 va = aVa + ((TUint64)i << 12);
				TUint64* pt = WalkToPt(va, EFalse);
				if (pt)
					pt[Index(va, 0)] = PteEncode(iScratchPage, ECacheLlc);
				}
			}
		// the physical page aVa maps to (0 = scratch / unmapped)
		TPhys Lookup(TUint64 aVa)
			{
			TUint64* pt = WalkToPt(aVa, EFalse);
			if (!pt)
				return 0;
			const TUint64 e = pt[Index(aVa, 0)];
			return IsScratch(e, 0) ? 0 : (e & KAddrMask) | (aVa & 0xFFF);
			}
		void Destroy()
			{
			for (TInt i = 0; i < iTables; ++i)
				iMem.iFreePage(iMem.iCtx, iTabPhys[i], iTabVirt[i]);
			iTables = 0;
			iMem.iFreePage(iMem.iCtx, iScratchPage, iScratchVirt[0]);
			for (TInt l = 1; l < KLevels; ++l)
				iMem.iFreePage(iMem.iCtx, iScratchTable[l - 1], iScratchVirt[l]);
			}
		};

	// ---- GEM objects --------------------------------------------------------------------------------------------------
	// A buffer object: its pages (not contiguous), a kernel mapping per page, the GPU address iris softpinned it at
	// (0 = not bound). Handles are table index + 1 (0 is never a handle, as in DRM).
	struct TGemObject
		{
		TUint32 iPages;			// 0 = free slot
		TPhys* iPhys;
		TUint64** iVirt;
		TUint64 iGpuAddr;
		TCache iCache;
		void* iCookie;			// the object allocator's (TObjMem): the kernel's shared chunk
		};

	// Optional: allocate a whole object at once (the kernel: one shared chunk per object, so GEM_MMAP can hand the
	// client a handle to it and the pages outlive GEM_CLOSE while the client still maps them -- they are freed when the
	// chunk goes). Fills aPhys[0..n) and aVirt[0..n) (the kernel mapping of each page).
	struct TObjMem
		{
		TInt (*iAlloc)(void* aCtx, TUint32 aPages, TPhys* aPhys, TUint64** aVirt, void*& aCookie);
		void (*iRelease)(void* aCtx, void* aCookie);	// the table lets go; the memory goes when the last user does
		void* iCtx;
		};

	struct TGemTable
		{
		enum { KMaxObjects = 4096 };
		TGemObject iObj[KMaxObjects];
		TMem iMem;
		TObjMem iObjMem;			// iAlloc == 0: page by page through iMem
		void* (*iAllocArray)(TUint32 aBytes);	// kernel heap for the page lists
		void (*iFreeArray)(void* aPtr);

		void Init(const TMem& aMem, void* (*aAlloc)(TUint32), void (*aFree)(void*))
			{
			iMem = aMem; iAllocArray = aAlloc; iFreeArray = aFree;
			iObjMem.iAlloc = 0; iObjMem.iRelease = 0; iObjMem.iCtx = 0;
			for (TInt i = 0; i < KMaxObjects; ++i)
				iObj[i].iPages = 0;
			}
		TGemObject* Get(TUint32 aHandle)
			{
			return aHandle && aHandle <= (TUint32)KMaxObjects && iObj[aHandle - 1].iPages ? &iObj[aHandle - 1] : 0;
			}
		// GEM_CREATE: aSize rounded up to pages; aHandle out
		TInt Create(TUint64 aSize, TUint32& aHandle)
			{
			const TUint64 n64 = (aSize + 4095) >> 12;
			if (!n64 || n64 > (1u << 20))			// 4 GB per object at most
				return KErrArgument;
			const TUint32 n = (TUint32)n64;
			TInt slot = -1;
			for (TInt i = 0; i < KMaxObjects && slot < 0; ++i)
				if (!iObj[i].iPages)
					slot = i;
			if (slot < 0)
				return KErrNoMemory;
			TGemObject& o = iObj[slot];
			o.iPhys = (TPhys*)iAllocArray(n * sizeof(TPhys));
			o.iVirt = (TUint64**)iAllocArray(n * sizeof(TUint64*));
			if (!o.iPhys || !o.iVirt)
				{
				if (o.iPhys) iFreeArray(o.iPhys);
				if (o.iVirt) iFreeArray(o.iVirt);
				return KErrNoMemory;
				}
			o.iCookie = 0;
			if (iObjMem.iAlloc)
				{
				if (iObjMem.iAlloc(iObjMem.iCtx, n, o.iPhys, o.iVirt, o.iCookie) != KErrNone)
					{
					iFreeArray(o.iPhys); iFreeArray(o.iVirt);
					return KErrNoMemory;
					}
				}
			else for (TUint32 i = 0; i < n; ++i)
				if (iMem.iAllocPage(iMem.iCtx, o.iPhys[i], o.iVirt[i]) != KErrNone)
					{
					while (i--)
						iMem.iFreePage(iMem.iCtx, o.iPhys[i], o.iVirt[i]);
					iFreeArray(o.iPhys); iFreeArray(o.iVirt);
					return KErrNoMemory;
					}
			o.iPages = n; o.iGpuAddr = 0; o.iCache = ECacheLlc;
			aHandle = (TUint32)slot + 1;
			return KErrNone;
			}
		// bind (softpin) at aGpuAddr in aVm; a rebind at another address unmaps the old range first
		TInt Bind(TUint32 aHandle, TPpgtt& aVm, TUint64 aGpuAddr)
			{
			TGemObject* o = Get(aHandle);
			if (!o)
				return KErrBadHandle;
			if (o->iGpuAddr == aGpuAddr && aGpuAddr)
				return KErrNone;
			if (o->iGpuAddr)
				aVm.Unmap(o->iGpuAddr, o->iPages);
			TInt r = aVm.Map(aGpuAddr, o->iPhys, o->iPages, o->iCache);
			o->iGpuAddr = r == KErrNone ? aGpuAddr : 0;
			return r;
			}
		// GEM_CLOSE: unbind from aVm (when given) and free the pages
		TInt Close(TUint32 aHandle, TPpgtt* aVm)
			{
			TGemObject* o = Get(aHandle);
			if (!o)
				return KErrBadHandle;
			if (aVm && o->iGpuAddr)
				aVm->Unmap(o->iGpuAddr, o->iPages);
			if (o->iCookie)
				iObjMem.iRelease(iObjMem.iCtx, o->iCookie);
			else for (TUint32 i = 0; i < o->iPages; ++i)
				iMem.iFreePage(iMem.iCtx, o->iPhys[i], o->iVirt[i]);
			iFreeArray(o->iPhys); iFreeArray(o->iVirt);
			o->iPages = 0;
			return KErrNone;
			}
		};
	}

#endif
