// Copyright The Phorepoc Authors
// SPDX-License-Identifier: MIT

// xk_i915_core.h -- OURS: the hardware-independent core of the i915 LDD (rung 4; gpd-win2-port.md section 9), the part of
// DI915Channel::Request that decides; xk_i915.cpp is the thin kernel shell around it (copying in from the client, the
// mutex, MMIO). Host-testable: tests/i915_core.
//   * Execbuffer (DRM_IOCTL_I915_GEM_EXECBUFFER2, softpin only, as Linux's eb_validate_vma + eb_pin_vma for
//     EXEC_OBJECT_PINNED with I915_EXEC_NO_RELOC): every handle must exist and appear once, addresses 4 KB aligned,
//     canonical 48-bit (bit 47 sign-extended: gen8_noncanonical_addr strips it), ranges inside 2^48 and not overlapping
//     each other; the batch (the last object) must hold [start, start + len), len 8-byte multiple. Then each object is
//     (re)bound at its address and the ring gets MI_BATCH_BUFFER_START + the seqno store + MI_USER_INTERRUPT
//   * Wait: seqnos wrap; i915_seqno_passed = (s32)(hw - want) >= 0
#ifndef XK_I915_CORE_H
#define XK_I915_CORE_H

namespace I915
	{
	const TUint64 KVaMask = (1ull << 48) - 1;
	inline TBool IsCanonical(TUint64 a) { const TUint64 hi = a >> 47; return hi == 0 || hi == 0x1FFFF; }
	inline TBool SeqnoPassed(TUint32 aHw, TUint32 aWant) { return (TInt)(aHw - aWant) >= 0; }

	struct TExecObj { TUint32 iHandle; TUint32 iFlags; TUint64 iOffset; };

	// the ring the CPU writes and the engine reads (a GGTT-mapped object); iHead = the engine's last reported head
	struct TRing
		{
		TUint32* iCpu;
		TUint32 iDwords;		// power of two
		TUint32 iTail, iHead;		// in dwords
		TUint32 Space() const { return (iHead - iTail - 1) & (iDwords - 1); }
		};

	struct TI915Ctx
		{
		TGemTable* iGem;
		TPpgtt* iVm;
		TRing iRing;
		TUint32 iHwspGgtt, iSeqnoOffset;
		TUint32 iNextSeqno;

		// the checks of Execbuffer; aSeen = scratch of TGemTable::KMaxObjects bits (zeroed by the caller)
		TInt Validate(const TExecObj* aObjs, TUint32 aCount, TUint32 aBatchStart, TUint32 aBatchLen, TUint32* aSeen) const
			{
			if (!aCount || aCount > 512)
				return KErrArgument;
			for (TUint32 i = 0; i < aCount; ++i)
				{
				const TExecObj& e = aObjs[i];
				const TGemObject* o = iGem->Get(e.iHandle);
				if (!o)
					return KErrBadHandle;
				const TUint32 bit = e.iHandle - 1;
				if (aSeen[bit >> 5] & (1u << (bit & 31)))
					return KErrArgument;				// -EINVAL: the same object twice
				aSeen[bit >> 5] |= 1u << (bit & 31);
				if (!IsCanonical(e.iOffset) || (e.iOffset & 0xFFF))
					return KErrArgument;
				const TUint64 va = e.iOffset & KVaMask, end = va + ((TUint64)o->iPages << 12);
				if (end > (1ull << 48))
					return KErrArgument;
				for (TUint32 j = 0; j < i; ++j)
					{
					const TUint64 vb = aObjs[j].iOffset & KVaMask;
					const TUint64 eb = vb + ((TUint64)iGem->Get(aObjs[j].iHandle)->iPages << 12);
					if (va < eb && vb < end)
						return KErrArgument;			// overlapping softpins (-EINVAL from eb_pin_vma)
					}
				}
			const TGemObject* b = iGem->Get(aObjs[aCount - 1].iHandle);
			if (!aBatchLen || (aBatchLen & 7) || (aBatchStart & 3) || (TUint64)aBatchStart + aBatchLen > ((TUint64)b->iPages << 12))
				return KErrArgument;
			return KErrNone;
			}

		// bind + emit; returns KErrNone and the seqno, or the first error (nothing emitted then)
		TInt Execbuffer(const TExecObj* aObjs, TUint32 aCount, TUint32 aBatchStart, TUint32 aBatchLen, TUint32* aSeen, TUint32& aSeqno)
			{
			TInt r = Validate(aObjs, aCount, aBatchStart, aBatchLen, aSeen);
			if (r != KErrNone)
				return r;
			if (iRing.Space() < 4 + 6)
				return KErrServerBusy;					// the caller waits for the head to move and retries
			for (TUint32 i = 0; i < aCount; ++i)
				{
				r = iGem->Bind(aObjs[i].iHandle, *iVm, aObjs[i].iOffset & KVaMask);
				if (r != KErrNone)
					return r;
				}
			const TUint64 batch = (aObjs[aCount - 1].iOffset & KVaMask) + aBatchStart;
			iRing.iTail = (iRing.iTail + EmitBatchStart(iRing.iCpu, iRing.iTail, iRing.iDwords, batch)) & (iRing.iDwords - 1);
			aSeqno = ++iNextSeqno;
			iRing.iTail = (iRing.iTail + EmitSeqno(iRing.iCpu, iRing.iTail, iRing.iDwords, iHwspGgtt, iSeqnoOffset, aSeqno))
				& (iRing.iDwords - 1);
			return KErrNone;
			}
		};
	}

#endif
