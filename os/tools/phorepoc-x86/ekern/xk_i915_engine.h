// Copyright The Phorepoc Authors
// Copyright (c) 2014-2020 Intel Corporation (the execlists engine start, CSB reset/processing and the Gen8 GT interrupt
//   layout, ported from Linux drivers/gpu/drm/i915/gt/intel_execlists_submission.c, intel_engine.h, intel_engine_regs.h,
//   gt/gen8_engine_cs.c, i915_reg.h, v6.12)
// SPDX-License-Identifier: MIT

// xk_i915_engine.h -- rung 5 of the i915 render-only port (gpd-win2-port.md section 9): the render engine itself.
//   * Start (enable_execlists + reset_csb_pointers): HWSTAM all masked, RING_MODE run-list enable, STOP_RING cleared,
//     the HW status page's GGTT address into RING_HWS_PGA; the CSB pointers reset to entries - 1 both in the register
//     and in the HWSP copy the engine maintains (dword 0x1f), the CSB slots in the HWSP (dword 0x10 on) set to -1
//   * Submit: one context into ELSP port 0 (port 1 empty), the four writes in Linux's order
//   * ProcessCsb (process_csb, Gen9): the engine writes the CSB into the HWSP and its write pointer at dword 0x1f;
//     the driver keeps the head. Each 64-bit entry: low dword = status, high dword = context id
//   * interrupts: the render user interrupt + context switch in GEN8_GT_IER/IMR(0), the master enable in
//     GEN8_MASTER_IRQ; the handler masks the master, acks GT_IIR(0), unmasks
//   * the GGTT size from GMCH_CTRL (gen8_get_total_gtt_size) and where its PTEs are: the second half of BAR0
// Uses TRegs (xk_i915_wa.h), KRingMiMode (xk_i915_uncore.h) and the LRC helpers (xk_i915_lrc.h). Header-only; tests/i915_engine drives it against a
// fake engine.
#ifndef XK_I915_ENGINE_H
#define XK_I915_ENGINE_H

namespace I915
	{
	const TUint32 KRingHwstam = 0x98;
	const TUint32 KHwsCsbBuf0 = 0x10, KHwsCsbWrite = 0x1F;	// I915_HWS_CSB_BUF0_INDEX, I915_HWS_CSB_WRITE_INDEX (Gen < 11)
	const TUint32 KHwsSeqno = 0x40;				// I915_GEM_HWS_SEQNO (dwords)
	const TUint32 KGen8MasterIrq = 0x44200, KGen8MasterIrqControl = 1u << 31;
	const TUint32 KGen8GtImr0 = 0x44304, KGen8GtIir0 = 0x44308, KGen8GtIer0 = 0x4430C;
	const TUint32 KGtRenderUserInterrupt = 1 << 0, KGtContextSwitchInterrupt = 1 << 8;
	const TUint32 KGmchCtrl = 0x50;				// PCI config of 00:02.0 (SNB_GMCH_CTRL)

	// gen8_get_total_gtt_size + the GSM location: the PTE array is the upper half of GTTMMADR (BAR0)
	inline TUint32 GgttPteBytes(TUint16 aGmchCtrl) { return GgttBytesOfPtes(aGmchCtrl); }
	inline TUint64 GgttPtesPhys(TUint64 aBar0, TUint64 aBar0Size) { return aBar0 + aBar0Size / 2; }

	// the GGTT the driver owns (gen8_gmch_probe + ggtt_probe_common + the scratch clear of gen8_ggtt_clear_range):
	// aPtes = the kernel mapping of the PTE half of BAR0; every entry points at the scratch page until something is
	// inserted. Then a bump allocator over the GGTT for the driver's own objects (HWSP, rings, context images): the
	// first 1 MB is left alone (the boot firmware's framebuffer lives in the stolen-memory range the BIOS mapped at 0).
	struct TGgttSpace
		{
		TGgtt iGgtt;
		TUint32 iNext, iEnd;
		TInt Init(TUint16 aGmchCtrl, TUint64* aPtes, TUint32 aPtesMapped, TPhys aScratch, void (*aFlush)(void*), void* aCtx)
			{
			const TUint32 bytes = GgttPteBytes(aGmchCtrl);
			if (!bytes)
				return KErrNotSupported;				// GGMS 0: no GGTT (the BIOS disabled the GPU's memory)
			TUint32 entries = bytes / 8;
			if (entries > aPtesMapped / 8)
				entries = aPtesMapped / 8;			// only as much as the caller mapped (2 GB of GGTT = 4 MB of PTEs)
			iGgtt.iPtes = aPtes; iGgtt.iEntries = entries; iGgtt.iScratch = aScratch; iGgtt.iFlush = aFlush; iGgtt.iCtx = aCtx;
			iGgtt.Clear(0, entries);
			iNext = 1u << 20;
			iEnd = entries >= (1u << 20) ? 0xFFFFF000u : entries << 12;
			return KErrNone;
			}
		// a GGTT range for aCount pages (aAlign bytes, a power of two >= 4 KB); 0 = full
		TUint32 Alloc(TUint32 aCount, TUint32 aAlign = 4096)
			{
			const TUint32 at = (iNext + aAlign - 1) & ~(aAlign - 1);
			if (at < iNext || (TUint64)at + ((TUint64)aCount << 12) > iEnd)
				return 0;
			iNext = at + (aCount << 12);
			return at;
			}
		};

	struct TEngine
		{
		TRegs iRegs;
		TUint32 iBase;			// KRenderRingBase
		volatile TUint32* iHwsp;	// the CPU view of the status page
		TUint32 iHwspGgtt;
		TUint32 iCsbHead;

		void Start()
			{
			const TRegs& r = iRegs;
			r.W(iBase + KRingHwstam, 0xFFFFFFFFu);				// intel_engine_set_hwsp_writemask(~0)
			r.W(iBase + KRingMode, Masked(KGfxRunListEnable, ETrue));
			r.W(iBase + KRingMiMode, Masked(KStopRing, EFalse));
			r.W(iBase + KRingHwsPga, iHwspGgtt);
			(void)r.R(iBase + KRingHwsPga);					// posting read
			ResetCsb();
			}
		void ResetCsb()
			{
			const TUint32 reset = KCsbEntries - 1;
			iRegs.W(iBase + KRingContextStatusPtr, 0xFFFFu << 16 | reset << 8 | reset);
			iCsbHead = reset;
			iHwsp[KHwsCsbWrite] = reset;
			for (TInt i = 0; i < 2 * KCsbEntries; ++i)
				iHwsp[KHwsCsbBuf0 + i] = 0xFFFFFFFFu;
			iRegs.W(iBase + KRingContextStatusPtr, 0xFFFFu << 16 | reset << 8 | reset);
			}
		void Submit(TUint64 aDesc)
			{
			TUint32 w[4];
			ElspWords(aDesc, 0, w);
			for (TInt i = 0; i < 4; ++i)
				iRegs.W(iBase + KRingElsp, w[i]);
			}
		// returns the number of CSB events; aCompleted = the context id of the last completion (or 0xFFFFFFFF)
		TInt ProcessCsb(TUint32& aCompleted)
			{
			aCompleted = 0xFFFFFFFFu;
			const TUint32 tail = iHwsp[KHwsCsbWrite];
			if (tail >= (TUint32)KCsbEntries || tail == iCsbHead)
				return 0;
			TInt n = 0;
			TUint32 h = iCsbHead;
			do
				{
				if (++h >= (TUint32)KCsbEntries)
					h = 0;
				const TUint32 status = iHwsp[KHwsCsbBuf0 + 2 * h], id = iHwsp[KHwsCsbBuf0 + 2 * h + 1];
				iHwsp[KHwsCsbBuf0 + 2 * h] = 0xFFFFFFFFu;			// as Linux: invalidate after reading
				iHwsp[KHwsCsbBuf0 + 2 * h + 1] = 0xFFFFFFFFu;
				if (CsbCompleted(status))
					aCompleted = id;
				++n;
				}
			while (h != tail);
			iCsbHead = tail;
			return n;
			}
		TUint32 Seqno() const { return iHwsp[KHwsSeqno]; }

		// gen8_gt_irq_postinstall (the RCS part) + gen8_irq_postinstall's master enable
		void IrqEnable()
			{
			const TUint32 bits = KGtRenderUserInterrupt | KGtContextSwitchInterrupt;
			iRegs.W(KGen8GtIir0, 0xFFFFFFFFu);
			iRegs.W(KGen8GtIer0, bits);
			iRegs.W(KGen8GtImr0, ~bits);
			iRegs.W(KGen8MasterIrq, KGen8MasterIrqControl);
			}
		// gen8_irq_handler, the GT part: returns the acked RCS bits (0 = not ours)
		TUint32 IrqAck()
			{
			const TUint32 master = iRegs.R(KGen8MasterIrq) & ~KGen8MasterIrqControl;
			if (!(master & 1))								// GEN8_GT_RCS_IRQ (bit 0)
				return 0;
			iRegs.W(KGen8MasterIrq, 0);
			const TUint32 iir = iRegs.R(KGen8GtIir0);
			if (iir)
				iRegs.W(KGen8GtIir0, iir);
			iRegs.W(KGen8MasterIrq, KGen8MasterIrqControl);
			return iir & 0xFFFF;
			}
		};
	}

#endif
