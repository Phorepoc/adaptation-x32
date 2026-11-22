// Copyright The Phorepoc Authors
// Copyright (c) 2014-2018 Intel Corporation (the Kaby Lake workaround lists, the MOCS table and their programming, ported
//   from Linux drivers/gpu/drm/i915/gt/intel_workarounds.c, intel_mocs.c, intel_gt_regs.h, v6.12)
// SPDX-License-Identifier: MIT

// xk_i915_wa.h -- rung 3 of the i915 render-only port (gpd-win2-port.md section 9): the Kaby Lake (Gen9.5, GT2)
// workarounds and MOCS as data.
//   * TWaList: Linux's i915_wa_list (_wa_add): sorted by register, writes to one register merge (set |= set, clr |= clr)
//   * the context list (gen9_ctx_workarounds_init + kbl_ctx_workarounds_init): masked registers saved in the context
//     image, so they are emitted once as MI_LOAD_REGISTER_IMM into the first request (intel_engine_emit_ctx_wa)
//   * the GT list (gen9_gt_workarounds_init + kbl_gt_workarounds_init): MMIO read-modify-writes after every reset
//   * the RCS whitelist (gen9_whitelist_build + kbl): the registers user batches may write, into RING_FORCE_TO_NONPRIV
//   * MOCS (skl_mocs_table, GEN9 BC): 64 control values at RCS 0xC800 and 32 combined L3CC words at 0xB020
// Header-only, no MMIO of its own; tests/i915_wa checks it against a register mock.
#ifndef XK_I915_WA_H
#define XK_I915_WA_H

namespace I915
	{
	struct TWa { TUint32 iReg, iClr, iSet; TBool iMasked; };
	struct TWaList
		{
		enum { KMax = 32 };
		TWa iList[KMax];
		TInt iCount;
		TInt iWaCount;			// adds, merged ones included (wal->wa_count)
		void Reset() { iCount = 0; iWaCount = 0; }
		TInt Add(TUint32 aReg, TUint32 aClr, TUint32 aSet, TBool aMasked)
			{
			TInt lo = 0, hi = iCount;
			while (lo < hi)
				{
				const TInt mid = lo + (hi - lo) / 2;
				if (iList[mid].iReg < aReg) lo = mid + 1;
				else if (iList[mid].iReg > aReg) hi = mid;
				else
					{
					TWa& w = iList[mid];
					if ((aClr | w.iClr) && !(aClr & ~w.iClr))
						w.iSet &= ~aClr;			// "Discarding overwritten w/a"
					w.iSet |= aSet; w.iClr |= aClr;
					++iWaCount;
					return KErrNone;
					}
				}
			if (iCount >= KMax)
				return KErrNoMemory;
			for (TInt i = iCount; i > lo; --i)
				iList[i] = iList[i - 1];
			iList[lo].iReg = aReg; iList[lo].iClr = aClr; iList[lo].iSet = aSet; iList[lo].iMasked = aMasked;
			++iCount; ++iWaCount;
			return KErrNone;
			}
		// wa_masked_en / wa_masked_dis / wa_masked_field_set / wa_write_or
		void MaskedEn(TUint32 aReg, TUint32 aBits) { Add(aReg, 0, (aBits << 16) | aBits, ETrue); }
		void MaskedDis(TUint32 aReg, TUint32 aBits) { Add(aReg, 0, aBits << 16, ETrue); }
		void MaskedField(TUint32 aReg, TUint32 aMask, TUint32 aVal) { Add(aReg, 0, (aMask << 16) | aVal, ETrue); }
		void WriteOr(TUint32 aReg, TUint32 aSet) { Add(aReg, aSet, aSet, EFalse); }
		const TWa* Find(TUint32 aReg) const
			{ for (TInt i = 0; i < iCount; ++i) if (iList[i].iReg == aReg) return &iList[i]; return 0; }
		};

	// ---- registers (intel_gt_regs.h) ------------------------------------------------------------------------------------
	const TUint32 KCacheMode1 = 0x7004, KCommonSliceChicken2 = 0x7014, KHdcChicken0 = 0x7300, KHdcChicken1 = 0x7304;
	const TUint32 KCsChicken1 = 0x2580, KCtxPreemptReg = 0x2248;
	const TUint32 KHalfSliceChicken1 = 0xE100, KHalfSliceChicken2 = 0xE180, KHalfSliceChicken3 = 0xE184,
		KHalfSliceChicken5 = 0xE188, KHalfSliceChicken7 = 0xE194, KRowChicken = 0xE4F0, KL3SqcReg4 = 0xB118;
	const TUint32 KGamEcochk = 0x4090, KGamtEcoRegRwIa = 0x4AB0, KGamtChknBitReg = 0x4AB8, KMmcdMiscCtrl = 0x4DDC,
		KUcgctl4 = 0x940C;
	const TUint32 KRingForceToNonpriv = 0x4D0, KRingNopid = 0x94;	// + the engine base
	const TInt KRingMaxNonprivSlots = 12;

	// gen9_ctx_workarounds_init (HAS_LLC; KBL) + kbl_ctx_workarounds_init; aStepC0OrLater = IS_GRAPHICS_STEP(C0, FOREVER)
	inline void KblCtxWorkarounds(TWaList& w, TBool aStepC0OrLater)
		{
		w.Reset();
		w.MaskedEn(KCommonSliceChicken2, 1 << 13);			// GEN9_PBE_COMPRESSED_HASH_SELECTION
		w.MaskedEn(KHalfSliceChicken7, 1 << 8);			// GEN9_SAMPLER_HASH_COMPRESSED_READ_ADDR
		w.MaskedEn(KRowChicken, (1 << 15) | (1 << 8));		// FLOW_CONTROL_ENABLE | PARTIAL_INSTRUCTION_SHOOTDOWN_DISABLE
		w.MaskedEn(KHalfSliceChicken7, (1 << 4) | (1 << 2));	// GEN9_ENABLE_YV12_BUGFIX | GEN9_ENABLE_GPGPU_PREEMPTION
		w.MaskedEn(KCacheMode1, (1 << 6) | (1 << 1));		// 4x4_STC_OPTIMIZATION_DISABLE | PARTIAL_RESOLVE_IN_VC_DISABLE
		w.MaskedDis(KHalfSliceChicken5, 1 << 3);			// GEN9_CCS_TLB_PREFETCH_ENABLE
		w.MaskedEn(KHdcChicken0, (1 << 5) | (1 << 15));		// FORCE_CONTEXT_SAVE_RESTORE_NON_COHERENT | CSR_NON_COHERENT_OVR_DISABLE
		w.MaskedEn(KHdcChicken0, 1 << 4);				// HDC_FORCE_NON_COHERENT
		w.MaskedEn(KHalfSliceChicken3, 1 << 1);			// GEN8_SAMPLER_POWER_BYPASS_DIS (SKL/KBL/CFL/CML)
		w.MaskedEn(KHalfSliceChicken2, 1 << 13);			// GEN8_ST_PO_DISABLE
		w.MaskedDis(KCsChicken1, 1 << 0);				// GEN9_PREEMPT_3D_OBJECT_LEVEL
		w.MaskedField(KCsChicken1, 0x6, 0x4);			// GEN9_PREEMPT_GPGPU_LEVEL_MASK, _COMMAND_LEVEL
		if (aStepC0OrLater)
			w.MaskedEn(KCommonSliceChicken2, 1 << 8);		// GEN8_SBE_DISABLE_REPLAY_BUF_OPTIMIZATION
		w.MaskedEn(KHalfSliceChicken1, 1 << 4);			// GEN7_SBE_SS_CACHE_DISPATCH_PORT_SHARING_DISABLE
		}

	// gen9_gt_workarounds_init (not CFL/CML; HAS_LLC) + kbl_gt_workarounds_init. (gen9_wa_init_mcr's MCR steering is
	// a function of the fused slice mask: the driver adds it from the fuse registers, not part of this table.)
	inline void KblGtWorkarounds(TWaList& w, TBool aStepBeforeC0)
		{
		w.Reset();
		w.WriteOr(KGamEcochk, 1 << 8);				// ECOCHK_DIS_TLB
		w.WriteOr(KMmcdMiscCtrl, (1u << 31) | (1 << 27));	// MMCD_PCLA | MMCD_HOTSPOT_EN
		w.WriteOr(KGamEcochk, 1 << 25);				// BDW_DISABLE_HDC_INVALIDATION
		if (aStepBeforeC0)
			w.WriteOr(KGamtChknBitReg, 1 << 28);		// GAMT_CHKN_DISABLE_DYNAMIC_CREDIT_SHARING
		w.WriteOr(KUcgctl4, 1 << 14);				// GEN8_EU_GAUNIT_CLOCK_GATE_DISABLE
		w.WriteOr(KGamtEcoRegRwIa, 1 << 18);			// GAMT_ECO_ENABLE_IN_PLACE_DECOMPRESS
		}

	// gen9_whitelist_build + kbl_whitelist_build (RCS); kept in the order Linux adds them (whitelist_reg appends)
	static const TUint32 KKblRcsWhitelist[] = { KCtxPreemptReg, KCsChicken1, KHdcChicken1, KCommonSliceChicken2, KL3SqcReg4 };
	const TInt KKblRcsWhitelistCount = 5;

	// ---- applying ------------------------------------------------------------------------------------------------------
	struct TRegs
		{
		TUint32 (*iRead)(void* aCtx, TUint32 aOff);
		void (*iWrite)(void* aCtx, TUint32 aOff, TUint32 aVal);
		void (*iDelayUs)(void* aCtx, TUint32 aUs);
		void* iCtx;
		TUint32 R(TUint32 a) const { return iRead(iCtx, a); }
		void W(TUint32 a, TUint32 v) const { iWrite(iCtx, a, v); }
		};
	// wa_list_apply: masked or full-mask entries write iSet as is, the rest read-modify-write
	inline void WaApply(const TWaList& w, const TRegs& aRegs)
		{
		for (TInt i = 0; i < w.iCount; ++i)
			{
			const TWa& a = w.iList[i];
			const TUint32 v = (a.iMasked || (a.iClr | a.iSet) == 0xFFFFFFFFu) ? a.iSet : ((aRegs.R(a.iReg) & ~a.iClr) | a.iSet);
			aRegs.W(a.iReg, v);
			}
		}
	// intel_engine_emit_ctx_wa: LRI(count), (reg, value) pairs, MI_NOOP; returns the dwords written (count * 2 + 2)
	inline TInt WaEmitCtx(const TWaList& w, TUint32* aCs)
		{
		if (!w.iCount)
			return 0;
		TInt n = 0;
		aCs[n++] = (0x22u << 23) | (2 * w.iCount - 1);
		for (TInt i = 0; i < w.iCount; ++i)
			{
			aCs[n++] = w.iList[i].iReg;
			aCs[n++] = w.iList[i].iSet;		// all context entries are masked: no read
			}
		aCs[n++] = 0;
		return n;
		}
	// intel_engine_apply_whitelist
	inline void WhitelistApply(const TRegs& aRegs, TUint32 aBase)
		{
		TInt i = 0;
		for (; i < KKblRcsWhitelistCount; ++i)
			aRegs.W(aBase + KRingForceToNonpriv + 4 * i, KKblRcsWhitelist[i]);
		for (; i < KRingMaxNonprivSlots; ++i)
			aRegs.W(aBase + KRingForceToNonpriv + 4 * i, aBase + KRingNopid);
		}

	// ---- MOCS (intel_mocs.c) -------------------------------------------------------------------------------------------
	enum { EMocsUncached = 0, EMocsPte = 1, EMocsCached = 2 };	// uapi i915_mocs_table_index
	const TInt KGen9MocsEntries = 64;
	const TUint32 KGen9RcsMocs0 = 0xC800, KGen9Lncfcmocs0 = 0xB020;
	struct TMocs { TUint32 iControl; TUint16 iL3cc; TUint8 iUsed; };
	// LE_* cacheability bits 1:0, target cache 3:2, LRUM 5:4; L3 cacheability bits 5:4
	static const TMocs KSklMocs[] =
		{
		/* 0 UNCACHED */ { 1 | (2 << 2), 1 << 4, 1 },			// LE_1_UC | LE_TC_2_LLC_ELLC, L3_1_UC
		/* 1 PTE */      { 0 | (0 << 2) | (3 << 4), 3 << 4, 1 },	// LE_0_PAGETABLE | LE_TC_0 | LE_LRUM(3), L3_3_WB
		/* 2 CACHED */   { 3 | (2 << 2) | (3 << 4), 3 << 4, 1 },	// LE_3_WB | LE_TC_2_LLC_ELLC | LE_LRUM(3), L3_3_WB
		{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
		{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
		{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
		{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
		{0,0,0},{0,0,0},{0,0,0},{0,0,0},
		/* 63 */ { 3 | (1 << 2) | (3 << 4), 1 << 4, 1 }			// LE_3_WB | LE_TC_1_LLC | LE_LRUM(3), L3_1_UC
		};
	// get_entry_control / get_entry_l3cc: an unused index takes the PTE entry (unused_entries_index = I915_MOCS_PTE)
	typedef char TMocsTableIs64[sizeof(KSklMocs) / sizeof(KSklMocs[0]) == 64 ? 1 : -1];
	inline TUint32 MocsControl(TInt i) { return KSklMocs[i].iUsed ? KSklMocs[i].iControl : KSklMocs[EMocsPte].iControl; }
	inline TUint16 MocsL3cc(TInt i) { return KSklMocs[i].iUsed ? KSklMocs[i].iL3cc : KSklMocs[EMocsPte].iL3cc; }
	// __init_mocs_table (the engine table) + init_l3cc_table
	inline void MocsApply(const TRegs& aRegs)
		{
		for (TInt i = 0; i < KGen9MocsEntries; ++i)
			aRegs.W(KGen9RcsMocs0 + 4 * i, MocsControl(i));
		for (TInt i = 0; i < KGen9MocsEntries / 2; ++i)
			aRegs.W(KGen9Lncfcmocs0 + 4 * i, MocsL3cc(2 * i) | ((TUint32)MocsL3cc(2 * i + 1) << 16));
		}
	}

#endif
