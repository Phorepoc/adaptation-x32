// Copyright The Phorepoc Authors
// Copyright (c) 2014-2020 Intel Corporation (the context image layout, the descriptor, the ELSP/CSB protocol, ported from
//   Linux drivers/gpu/drm/i915/gt/intel_lrc.c, intel_lrc.h, intel_lrc_reg.h, intel_execlists_submission.c,
//   intel_engine_regs.h, intel_gpu_commands.h, v6.12)
// SPDX-License-Identifier: MIT

// xk_i915_lrc.h -- rung 2 of the i915 render-only port (gpd-win2-port.md section 9): the Gen9 render engine's logical
// ring context (LRC) and execlists submission.
//   * the context image: page 0 = the per-process HW status page (PPHWSP), page 1 on = the register state, which is a
//     batch of MI_LOAD_REGISTER_IMM (offset, value) pairs the GPU loads on a context restore (set_offsets +
//     gen9_rcs_offsets); its total size on Gen9 RCS is 22 pages + the PPHWSP
//   * the reg state values for a first restore: CONTEXT_CONTROL, the ring (START/HEAD/TAIL/CTL), the PML4 in PDP0,
//     MI_MODE's STOP_RING cleared (__lrc_init_regs)
//   * the descriptor (lrc_descriptor: legacy 64-bit addressing, valid, privileged + the state's GGTT address, a context
//     id in the upper dword), the ELSP write (upper then lower dword, port 1 then port 0), and the CSB (6 entries,
//     the read/write pointers in RING_CONTEXT_STATUS_PTR)
// Header-only, no MMIO: the driver writes the words this computes; tests/i915_lrc checks them.
#ifndef XK_I915_LRC_H
#define XK_I915_LRC_H

namespace I915
	{
	// ---- MI commands (intel_gpu_commands.h) ---------------------------------------------------------------------------
	inline TUint32 MiInstr(TUint32 aOp, TUint32 aFlags) { return (aOp << 23) | aFlags; }
	const TUint32 KMiNoop = 0;
	const TUint32 KMiUserInterrupt = 0x02 << 23;
	const TUint32 KMiBatchBufferEnd = 0x0A << 23;
	const TUint32 KMiLriForcePosted = 1 << 12;
	inline TUint32 MiLoadRegisterImm(TUint32 aCount) { return MiInstr(0x22, 2 * aCount - 1); }
	const TUint32 KMiStoreDwordImmGen4 = (0x20 << 23) | 2;	// + MI_MEM_VIRTUAL (1<<22) for a PPGTT address
	const TUint32 KMiMemVirtual = 1 << 22;
	const TUint32 KMiUseGgtt = 1 << 22;
	const TUint32 KMiBatchBufferStartGen8 = (0x31 << 23) | 1;
	const TUint32 KMiBatchPpgtt = 1 << 8;			// MI_BATCH_PPGTT_HSW

	// ---- engine registers (intel_engine_regs.h), relative to the engine's mmio base --------------------------------------
	const TUint32 KRenderRingBase = 0x02000;
	const TUint32 KRingTail = 0x30, KRingHead = 0x34, KRingStart = 0x38, KRingCtl = 0x3C;
	const TUint32 KRingHwsPga = 0x80, KRingElsp = 0x230, KRingExeclistStatusLo = 0x234, KRingContextControl = 0x244;
	const TUint32 KRingMode = 0x29C, KRingContextStatusPtr = 0x3A0, KRingContextStatusBuf = 0x370;
	const TUint32 KGfxRunListEnable = 1 << 15;
	const TUint32 KStopRing = 1 << 8;
	const TUint32 KRingValid = 1;
	inline TUint32 Masked(TUint32 aBits, TBool aOn) { return (aBits << 16) | (aOn ? aBits : 0); }	// _MASKED_BIT_*
	inline TUint32 RingCtl(TUint32 aBytes) { return (aBytes - 4096) | KRingValid; }			// RING_CTL_SIZE | VALID

	const TUint32 KCtxCtrlRestoreInhibit = 1 << 0, KCtxCtrlRsCtxEnable = 1 << 1, KCtxCtrlSaveInhibit = 1 << 2,
		KCtxCtrlInhibitSynCtxSwitch = 1 << 3;

	// ---- the register state: indices into the state page (intel_lrc_reg.h, Gen9) ---------------------------------------
	enum
		{
		ECtxContextControl = 0x02 + 1, ECtxRingHead = 0x04 + 1, ECtxRingTail = 0x06 + 1, ECtxRingStart = 0x08 + 1,
		ECtxRingCtl = 0x0A + 1, ECtxBbState = 0x10 + 1, ECtxTimestamp = 0x22 + 1,
		ECtxPdp3Udw = 0x24 + 1, ECtxPdp3Ldw = 0x26 + 1, ECtxPdp2Udw = 0x28 + 1, ECtxPdp2Ldw = 0x2A + 1,
		ECtxPdp1Udw = 0x2C + 1, ECtxPdp1Ldw = 0x2E + 1, ECtxPdp0Udw = 0x30 + 1, ECtxPdp0Ldw = 0x32 + 1,
		ECtxRPwrClkState = 0x42 + 1,
		EGen9CtxMiMode = 0x54,		// lrc_ring_mi_mode (the offset; the value is at +1)
		EGen9CtxBbOffset = 0x64,	// lrc_ring_bb_offset
		EGen9CtxGpr0 = 0x68		// lrc_ring_gpr0
		};
	const TUint32 KGen9RcsContextBytes = 22 * 4096;	// GEN9_LR_CONTEXT_RENDER_SIZE
	const TUint32 KPphwspPage = 0, KStatePage = 1;	// LRC_PPHWSP_PN, LRC_STATE_PN

	// gen9_rcs_offsets (intel_lrc.c): NOP(n) = skip n dwords, LRI(count, POSTED) = an MI_LOAD_REGISTER_IMM of count
	// registers, each REG(x) one byte (x >> 2) or REG16(x) two (7 bits each, the first with bit 7 set)
	#define XK_NOP(x) (0x80 | (x))
	#define XK_LRI(c, posted) (((posted) << 6) | (c))
	#define XK_REG(x) ((x) >> 2)
	#define XK_REG16(x) (((x) >> 9) | 0x80), (((x) >> 2) & 0x7F)
	static const TUint8 KGen9RcsOffsets[] =
		{
		XK_NOP(1),
		XK_LRI(14, 1),
		XK_REG16(0x244), XK_REG(0x34), XK_REG(0x30), XK_REG(0x38), XK_REG(0x3c), XK_REG(0x168), XK_REG(0x140),
		XK_REG(0x110), XK_REG(0x11c), XK_REG(0x114), XK_REG(0x118), XK_REG(0x1c0), XK_REG(0x1c4), XK_REG(0x1c8),
		XK_NOP(3),
		XK_LRI(9, 1),
		XK_REG16(0x3a8), XK_REG16(0x28c), XK_REG16(0x288), XK_REG16(0x284), XK_REG16(0x280), XK_REG16(0x27c),
		XK_REG16(0x278), XK_REG16(0x274), XK_REG16(0x270),
		XK_NOP(13),
		XK_LRI(1, 0),
		XK_REG(0xc8),
		XK_NOP(13),
		XK_LRI(44, 1),
		XK_REG(0x28), XK_REG(0x9c), XK_REG(0xc0), XK_REG(0x178), XK_REG(0x17c), XK_REG16(0x358), XK_REG(0x170),
		XK_REG(0x150), XK_REG(0x154), XK_REG(0x158), XK_REG16(0x41c),
		XK_REG16(0x600), XK_REG16(0x604), XK_REG16(0x608), XK_REG16(0x60c), XK_REG16(0x610), XK_REG16(0x614),
		XK_REG16(0x618), XK_REG16(0x61c), XK_REG16(0x620), XK_REG16(0x624), XK_REG16(0x628), XK_REG16(0x62c),
		XK_REG16(0x630), XK_REG16(0x634), XK_REG16(0x638), XK_REG16(0x63c), XK_REG16(0x640), XK_REG16(0x644),
		XK_REG16(0x648), XK_REG16(0x64c), XK_REG16(0x650), XK_REG16(0x654), XK_REG16(0x658), XK_REG16(0x65c),
		XK_REG16(0x660), XK_REG16(0x664), XK_REG16(0x668), XK_REG16(0x66c), XK_REG16(0x670), XK_REG16(0x674),
		XK_REG16(0x678), XK_REG16(0x67c),
		XK_REG(0x68),
		0
		};
	#undef XK_NOP
	#undef XK_LRI
	#undef XK_REG
	#undef XK_REG16

	// set_offsets: write the LRI headers and the register offsets (the values stay as they are); returns the dwords used
	inline TInt SetOffsets(TUint32* aRegs, const TUint8* aData, TUint32 aBase, TBool aClose)
		{
		TUint32* const start = aRegs;
		while (*aData)
			{
			if (*aData & 0x80)
				{
				aRegs += *aData++ & 0x7F;
				continue;
				}
			TUint32 count = *aData & 0x3F;
			const TUint32 flags = *aData >> 6;
			++aData;
			*aRegs = MiLoadRegisterImm(count) | ((flags & 1) ? KMiLriForcePosted : 0);
			++aRegs;
			do
				{
				TUint32 off = 0;
				TUint8 v;
				do
					{
					v = *aData++;
					off = (off << 7) | (v & 0x7F);
					}
				while (v & 0x80);
				aRegs[0] = aBase + (off << 2);
				aRegs += 2;
				}
			while (--count);
			}
		if (aClose)
			*aRegs++ = KMiBatchBufferEnd;
		return (TInt)(aRegs - start);
		}

	// the first-restore register state of a Gen9 RCS context (__lrc_init_regs with inhibit: a fresh context, nothing
	// to restore but these registers). aRegs = the state page (zeroed by the caller), aRingGgtt = the ring's GGTT address
	inline void InitRcsRegState(TUint32* aRegs, TUint32 aRingGgtt, TUint32 aRingBytes, TUint64 aPml4)
		{
		SetOffsets(aRegs, KGen9RcsOffsets, KRenderRingBase, ETrue);
		aRegs[ECtxContextControl] = Masked(KCtxCtrlInhibitSynCtxSwitch, ETrue)
			| Masked(KCtxCtrlRestoreInhibit, EFalse) | KCtxCtrlRestoreInhibit	// the first restore loads no engine state
			| Masked(KCtxCtrlSaveInhibit | KCtxCtrlRsCtxEnable, EFalse);		// (Gen < 11)
		aRegs[ECtxTimestamp] = 0;
		aRegs[EGen9CtxBbOffset + 1] = 0;
		aRegs[ECtxPdp0Udw] = (TUint32)(aPml4 >> 32);					// ASSIGN_CTX_PML4
		aRegs[ECtxPdp0Ldw] = (TUint32)aPml4;
		aRegs[EGen9CtxMiMode + 1] = (aRegs[EGen9CtxMiMode + 1] & ~KStopRing) | (KStopRing << 16);	// __reset_stop_ring
		aRegs[ECtxRingStart] = aRingGgtt;
		aRegs[ECtxRingHead] = 0;
		aRegs[ECtxRingTail] = 0;
		aRegs[ECtxRingCtl] = RingCtl(aRingBytes);
		}
	// lrc_update_regs: the tail moves on every submission
	inline void SetRingTail(TUint32* aRegs, TUint32 aTail) { aRegs[ECtxRingTail] = aTail; }

	// ---- the descriptor + ELSP ----------------------------------------------------------------------------------------
	const TUint64 KCtxValid = 1 << 0, KCtxForceRestore = 1 << 2, KCtxPrivilege = 1 << 8;
	const TUint32 KCtxAddressingShift = 3, KLegacy64bContext = 3;
	const TUint32 KCtxIdShift = 32, KCtxIdWidth = 21;
	// aStateGgtt = the GGTT address of the context image's first page (the PPHWSP), 4 KB aligned; aId = the SW context id
	inline TUint64 Descriptor(TUint32 aStateGgtt, TUint32 aId)
		{
		TUint64 d = (KLegacy64bContext << KCtxAddressingShift) | KCtxValid | KCtxPrivilege | (aStateGgtt & ~0xFFFu);
		return d | ((TUint64)(aId & ((1u << KCtxIdWidth) - 1)) << KCtxIdShift);
		}
	// the four ELSP writes, in order (execlists_submit_ports + write_desc, no ELSQ: port 1 first, upper dword first)
	inline void ElspWords(TUint64 aPort0, TUint64 aPort1, TUint32 aOut[4])
		{
		aOut[0] = (TUint32)(aPort1 >> 32); aOut[1] = (TUint32)aPort1;
		aOut[2] = (TUint32)(aPort0 >> 32); aOut[3] = (TUint32)aPort0;
		}

	// ---- the CSB ------------------------------------------------------------------------------------------------------
	const TUint32 KCsbIdleActive = 1 << 0, KCsbPreempted = 1 << 1, KCsbElementSwitch = 1 << 2, KCsbActiveIdle = 1 << 3,
		KCsbComplete = 1 << 4, KCsbLiteRestore = 1 << 15;
	const TInt KCsbEntries = 6;
	// RING_CONTEXT_STATUS_PTR: write pointer in bits 2:0, read pointer in bits 10:8 (GEN8_CSB_*_PTR_MASK)
	inline TInt CsbWritePtr(TUint32 aPtr) { return (TInt)(aPtr & 7); }
	inline TInt CsbReadPtr(TUint32 aPtr) { return (TInt)((aPtr >> 8) & 7); }
	// the driver's write-back of the read pointer: _MASKED_FIELD(GEN8_CSB_READ_PTR_MASK, read << 8)
	inline TUint32 CsbReadPtrUpdate(TInt aRead) { return (0x700u << 16) | (((TUint32)aRead & 7) << 8); }
	inline TBool CsbPromotes(TUint32 aStatus) { return (aStatus & (KCsbIdleActive | KCsbPreempted)) != 0; }	// gen8_csb_parse
	inline TBool CsbCompleted(TUint32 aStatus) { return (aStatus & (KCsbComplete | KCsbPreempted)) != 0; }

	// walk the CSB from the last read entry to the hardware's write pointer (entries are 2 dwords: status, context id);
	// calls aOn(status, ctx id) for each, returns the new read pointer. A write pointer of 7 = reset value: nothing yet.
	template <class F> inline TInt CsbDrain(const volatile TUint32* aCsb, TInt aRead, TInt aWrite, F aOn)
		{
		if (aWrite == 7 || aWrite == aRead)
			return aRead;
		TInt r = aRead;
		do
			{
			if (++r >= KCsbEntries)
				r = 0;
			aOn(aCsb[2 * r], aCsb[2 * r + 1]);
			}
		while (r != aWrite);
		return r;
		}

	// ---- a ring --------------------------------------------------------------------------------------------------------
	// the seqno batch the driver ends every submission with: store aSeqno at aHwspGgtt + aOffset (GGTT), then a user
	// interrupt; MI_STORE_DWORD_IMM with MI_USE_GGTT writes through the GGTT (the HWSP lives there)
	inline TInt EmitSeqno(TUint32* aRing, TUint32 aAt, TUint32 aRingDwords, TUint32 aHwspGgtt, TUint32 aOffset, TUint32 aSeqno)
		{
		const TUint32 cmd[6] = { KMiStoreDwordImmGen4 | KMiUseGgtt, aHwspGgtt + aOffset, 0, aSeqno, KMiUserInterrupt, KMiNoop };
		for (TInt i = 0; i < 6; ++i)
			aRing[(aAt + i) % aRingDwords] = cmd[i];
		return 6;
		}
	// start a user batch at its PPGTT address (MI_BATCH_BUFFER_START_GEN8 + PPGTT, a 48-bit address in 2 dwords)
	inline TInt EmitBatchStart(TUint32* aRing, TUint32 aAt, TUint32 aRingDwords, TUint64 aBatchVa)
		{
		const TUint32 cmd[4] = { KMiBatchBufferStartGen8 | KMiBatchPpgtt, (TUint32)aBatchVa, (TUint32)(aBatchVa >> 32), KMiNoop };
		for (TInt i = 0; i < 4; ++i)
			aRing[(aAt + i) % aRingDwords] = cmd[i];
		return 4;
		}
	}

#endif
