// Copyright The Phorepoc Authors
// Copyright (c) 2013-2019 Intel Corporation (Gen9 forcewake and the render-engine reset, ported from Linux
//   drivers/gpu/drm/i915/intel_uncore.c, gt/intel_reset.c, gt/intel_engine_cs.c, gt/intel_gt_regs.h, v6.12)
// SPDX-License-Identifier: MIT

// xk_i915_uncore.h -- rung 4 of the i915 render-only port (gpd-win2-port.md section 9): forcewake and reset.
//   * forcewake (fw_domains_get_with_fallback, Gen9): wake the render / GT domain before touching its registers:
//     wait for the ack bit to clear, masked-write FORCEWAKE_KERNEL, wait (50 ms) for the ack; put = masked clear.
//     Reference counted per domain, as intel_uncore_forcewake_get/put
//   * the render reset (intel_engine_stop_cs + gen8_engine_reset_prepare + gen6_hw_domain_reset + finish):
//     STOP_RING and wait MODE_IDLE, RESET_CTL request -> READY_TO_RESET (700 us), GDRST = GRDOM_RENDER and wait for
//     the bit to self-clear (2 ms, twice on Gen9), 50 us settle, release the request
// Uses TRegs from xk_i915_wa.h. Header-only; tests/i915_uncore drives it against a register model.
#ifndef XK_I915_UNCORE_H
#define XK_I915_UNCORE_H

namespace I915
	{
	const TUint32 KFwRenderGen9 = 0xA278, KFwAckRenderGen9 = 0x0D84;
	const TUint32 KFwGtGen9 = 0xA188, KFwAckGtGen9 = 0x130044;
	const TUint32 KFwMediaGen9 = 0xA270, KFwAckMediaGen9 = 0x0D88;
	const TUint32 KFwKernel = 1 << 0, KFwKernelFallback = 1 << 15;
	const TUint32 KGdrst = 0x941C, KGrdomFull = 1 << 0, KGrdomRender = 1 << 1;
	const TUint32 KRingMiMode = 0x9C, KModeIdle = 1 << 9, KRingResetCtl = 0xD0;
	const TUint32 KResetCtlRequest = 1 << 0, KResetCtlReady = 1 << 1, KResetCtlCatError = 1 << 2;
	const TInt KErrTimedOut = -33;

	// __intel_wait_for_register_fw: poll (aReg & aMask) == aValue, in 1 us steps up to aUs
	inline TInt WaitFor(const TRegs& r, TUint32 aReg, TUint32 aMask, TUint32 aValue, TUint32 aUs)
		{
		for (TUint32 t = 0;; ++t)
			{
			if ((r.R(aReg) & aMask) == aValue)
				return KErrNone;
			if (t >= aUs)
				return KErrTimedOut;
			r.iDelayUs(r.iCtx, 1);
			}
		}

	enum TFwDomain { EFwRender = 0, EFwGt = 1, EFwMedia = 2, EFwDomains = 3 };
	struct TForcewake
		{
		TRegs iRegs;
		TInt iWake[EFwDomains];
		TInt iFallbacks;
		static TUint32 Set(TInt d) { return d == EFwRender ? KFwRenderGen9 : d == EFwGt ? KFwGtGen9 : KFwMediaGen9; }
		static TUint32 Ack(TInt d) { return d == EFwRender ? KFwAckRenderGen9 : d == EFwGt ? KFwAckGtGen9 : KFwAckMediaGen9; }
		void Init(const TRegs& r)
			{
			iRegs = r; iFallbacks = 0;
			for (TInt d = 0; d < EFwDomains; ++d)
				{
				iWake[d] = 0;
				iRegs.W(Set(d), 0xFFFFu << 16);		// fw_domain_reset: every bit of the request cleared
				}
			}
		// fw_domain_get + a simplified fw_domain_wait_ack_with_fallback: if the ack does not come, pulse the fallback bit
		// to kick the PM unit (Linux pulses it with growing delays; two passes here)
		TInt Get(TInt d)
			{
			if (iWake[d]++)
				return KErrNone;
			const TUint32 s = Set(d), a = Ack(d);
			const TUint32 ms = 50 * 1000;
			if (WaitFor(iRegs, a, KFwKernel, 0, ms) != KErrNone)
				{ --iWake[d]; return KErrTimedOut; }
			iRegs.W(s, (KFwKernel << 16) | KFwKernel);
			if (WaitFor(iRegs, a, KFwKernel, KFwKernel, ms) == KErrNone)
				return KErrNone;
			++iFallbacks;
			for (TInt pass = 0; pass < 2; ++pass)
				{
				iRegs.W(s, (KFwKernelFallback << 16) | KFwKernelFallback);
				iRegs.iDelayUs(iRegs.iCtx, 10);
				iRegs.W(s, KFwKernelFallback << 16);
				if (WaitFor(iRegs, a, KFwKernel, KFwKernel, ms) == KErrNone)
					return KErrNone;
				}
			--iWake[d];
			return KErrTimedOut;
			}
		void Put(TInt d)
			{
			if (iWake[d] > 0 && --iWake[d] == 0)
				iRegs.W(Set(d), KFwKernel << 16);
			}
		};

	// the render engine reset; aBase = KRenderRingBase. Returns KErrNone or KErrTimedOut (the step that timed out
	// is in aStep: 1 = stop, 2 = ready-to-reset, 3 = GDRST)
	inline TInt ResetRender(const TRegs& r, TUint32 aBase, TInt& aStep)
		{
		aStep = 1;
		r.W(aBase + KRingMiMode, (KStopRing << 16) | KStopRing);		// intel_engine_stop_cs
		const TInt stopped = WaitFor(r, aBase + KRingMiMode, KModeIdle, KModeIdle, 1000);
		(void)stopped;							// Linux goes on regardless: the reset is the remedy
		aStep = 2;
		TUint32 ctl = r.R(aBase + KRingResetCtl);
		if (ctl & KResetCtlCatError)
			{
			r.W(aBase + KRingResetCtl, (KResetCtlCatError << 16) | KResetCtlCatError);
			if (WaitFor(r, aBase + KRingResetCtl, KResetCtlCatError, 0, 700) != KErrNone)
				return KErrTimedOut;
			}
		else if (!(ctl & KResetCtlReady))
			{
			r.W(aBase + KRingResetCtl, (KResetCtlRequest << 16) | KResetCtlRequest);
			if (WaitFor(r, aBase + KRingResetCtl, KResetCtlReady, KResetCtlReady, 700) != KErrNone)
				return KErrTimedOut;
			}
		aStep = 3;
		TInt err = KErrNone;
		for (TInt loops = 2; loops-- && err == KErrNone;)		// Gen < 12.70: twice
			{
			r.W(KGdrst, KGrdomRender);
			err = WaitFor(r, KGdrst, KGrdomRender, 0, 2000);
			}
		r.iDelayUs(r.iCtx, 50);
		r.W(aBase + KRingResetCtl, KResetCtlRequest << 16);			// gen8_engine_reset_cancel
		return err;
		}
	}

#endif
