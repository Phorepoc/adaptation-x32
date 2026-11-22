/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: MIT */
// ri915channel.h -- OURS: the user-side API of the i915 render-only kernel driver (tools/phorepoc-x86/ekern/xk_i915.cpp, device
// "I915"; gpd-win2-port.md section 9). The Symbian counterpart of the DRM_IOCTL_I915_* calls Mesa's iris winsys makes, in the
// shape of RVmwSvgaChannel; mesa-symbian's drm_symbian_i915.c maps the ioctls onto it:
//   GetParam      -> I915_PARAM_* (CHIPSET_ID, REVISION, HAS_EXEC_SOFTPIN = 1, HAS_EXECBUF2 ...): what iris asks at screen creation
//   GemCreate     -> DRM_IOCTL_I915_GEM_CREATE: a buffer object of whole pages (zeroed), a handle (1 ...)
//   GemClose      -> DRM_IOCTL_GEM_CLOSE (unbinds it from this channel's address space)
//   GemMap        -> DRM_IOCTL_I915_GEM_MMAP_OFFSET + mmap: a shared chunk handle over the object's pages (write-back: the GPU
//                    snoops the LLC on Kaby Lake, so iris's WB maps need no clflush)
//   Execbuffer    -> DRM_IOCTL_I915_GEM_EXECBUFFER2 with I915_EXEC_NO_RELOC + softpin: every object comes with the 48-bit
//                    address iris chose (EXEC_OBJECT_PINNED); the last one is the batch. Returns a seqno
//   Wait          -> DRM_IOCTL_I915_GEM_WAIT on a seqno (timeout in us, 0 = poll)
// Each channel has its own PPGTT (one context = one address space, as iris's one context per screen). Open never needs
// User::LoadLogicalDevice (the kernel extension installs the device when it finds the GPU).
#ifndef RI915CHANNEL_H
#define RI915CHANNEL_H
#include <e32cmn.h>
#ifndef __KERNEL_MODE__
#include <e32std.h>
#endif

_LIT(KI915LddName, "I915");
const TInt KI915MajorVersion = 1;
const TInt KI915MaxExecObjects = 512;

// I915_PARAM_* values (include/uapi/drm/i915_drm.h)
enum TI915Param { EI915ParamChipsetId = 4, EI915ParamHasExecbuf2 = 9, EI915ParamRevision = 32, EI915ParamHasExecSoftpin = 37,
	EI915ParamCsTimestampFrequency = 51, EI915ParamMmapGttVersion = 40 };

struct TI915GetParam { TInt iParam; TInt iValue; };
struct TI915GemCreate { TUint64 iSize; TUint32 iHandle; };		// in: size (rounded up to pages); out: size, handle
struct TI915ExecObject { TUint32 iHandle; TUint32 iFlags; TUint64 iOffset; };	// iOffset: the softpinned GPU address
struct TI915Execbuffer
	{
	TUint32 iCount;				// objects in the user array iObjects points to; the last is the batch
	TUint32 iBatchStart, iBatchLen;	// bytes into the batch object
	TUint32 iSeqno;				// out
	TUint64 iObjects;			// user address of TI915ExecObject[iCount]
	};
struct TI915Wait { TUint32 iSeqno; TUint32 iTimeoutUs; };

enum TI915Control { EI915CtrlGetParam, EI915CtrlGemCreate, EI915CtrlGemClose, EI915CtrlGemMap, EI915CtrlExecbuffer, EI915CtrlWait };

#ifndef __KERNEL_MODE__
class RI915Channel : public RBusLogicalChannel
	{
public:
	inline static TVersion VersionRequired() { return TVersion(KI915MajorVersion, 0, 0); }
	inline TInt Open() { return DoCreate(KI915LddName, VersionRequired(), KNullUnit, NULL, NULL, EOwnerProcess); }
	inline TInt GetParam(TInt aParam, TInt& aValue)
		{ TI915GetParam g = { aParam, 0 }; TPckg<TI915GetParam> p(g); TInt r = DoControl(EI915CtrlGetParam, &p); aValue = g.iValue; return r; }
	inline TInt GemCreate(TI915GemCreate& aCreate) { TPckg<TI915GemCreate> p(aCreate); return DoControl(EI915CtrlGemCreate, &p); }
	inline TInt GemClose(TUint32 aHandle) { return DoControl(EI915CtrlGemClose, (TAny*)aHandle); }
	inline TInt GemMap(TUint32 aHandle, RChunk& aChunk)
		{ TInt h = DoControl(EI915CtrlGemMap, (TAny*)aHandle); return h < 0 ? h : aChunk.SetReturnedHandle(h); }
	inline TInt Execbuffer(TI915Execbuffer& aExec) { TPckg<TI915Execbuffer> p(aExec); return DoControl(EI915CtrlExecbuffer, &p); }
	inline TInt Wait(TUint32 aSeqno, TUint32 aTimeoutUs)
		{ TI915Wait w = { aSeqno, aTimeoutUs }; TPckg<TI915Wait> p(w); return DoControl(EI915CtrlWait, &p); }
	};
#endif

#endif
