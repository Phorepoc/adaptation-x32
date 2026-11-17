// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_usbhost.cpp -- the PC board's side of phe_usb_port.h for the FreeBSD USB host (xk_usb_hc.c attaches the xHCI
// driver, the ARM USB host's phe_usb_ekern.cpp runs the stack in a kernel DFC thread). It finds the xHCI controllers on
// the PCI bus (class 0C0330, through the scan in xk_pci.h), turns on memory decode and bus mastering, sizes and maps
// BAR0 uncached, and lists them for the stack as "xhci" controllers; the stack then takes them from the BIOS, resets
// and starts them. The INTx line stays disabled at the device (command bit 10): the stack polls at 1 ms.
// Built only with USB_HOST=1 (make_ekern.sh); called from the kernel's extension start (X64StartUsbHost).
#include <kernel/kern_priv.h>
#include <assp.h>
#include "xk_pci.h"

extern "C" {
#include "phe_usb_port.h"
}

void DebugOutputChar(TUint aChar);			// xk_entry.cpp (a C++ function)

static const TInt KMaxXhci = 4;
static const TUint32 KClassXhci = 0x0C0330;		// serial bus, USB, programming interface 0x30 = xHCI

struct SXhci
	{
	TLinAddr iBase;
	TUint iSize;
	SPciLoc iLoc;
	};
static SXhci gXhci[KMaxXhci];
static TInt gXhciCount;

extern "C" int phe_usb_board_init(void)
	{
	for (TInt n = 0; n < KMaxXhci; ++n)
		{
		SPciLoc loc;
		if (!XkPciFindNth(KClassXhci, n, loc))
			break;
		const TInt b = loc.iBus, d = loc.iDev, f = loc.iFn;
		const TUint32 id = XkPciRead(b, d, f, 0);
		const TUint32 cmd = XkPciRead(b, d, f, 0x04);
		XkPciWrite16(b, d, f, 0x04, (TUint16)(cmd & ~0x7u));		// decode off while BAR0 is sized
		const TUint32 bar = XkPciRead(b, d, f, 0x10);
		XkPciWrite(b, d, f, 0x10, 0xFFFFFFFFu);
		const TUint32 sz = XkPciRead(b, d, f, 0x10);
		XkPciWrite(b, d, f, 0x10, bar);
		const TBool is64 = ((bar >> 1) & 3) == 2;
		const TUint32 hi = is64 ? XkPciRead(b, d, f, 0x14) : 0;
		const TUint32 base = bar & ~0xFu;
		const TUint size = (~(sz & ~0xFu)) + 1;
		if (hi != 0 || base == 0 || size < 0x1000 || (bar & 1))
			{
			Kern::Printf("[usbh] xHCI %04x:%04x at %02x:%02x.%d: BAR0 %08x:%08x unusable (above 4 GB, unassigned or I/O)",
							id & 0xFFFF, id >> 16, b, d, f, hi, bar);
			continue;
			}
		XkPciWrite16(b, d, f, 0x04, (TUint16)(cmd | 0x6u | 0x400u));	// memory decode + bus master, INTx off
		DPlatChunkHw* ch = NULL;
		TInt r = DPlatChunkHw::New(ch, base, size, EMapAttrSupRw | EMapAttrFullyBlocking);
		if (r != KErrNone)
			{
			Kern::Printf("[usbh] xHCI at %02x:%02x.%d: mapping BAR0 failed (%d)", b, d, f, r);
			continue;
			}
		SXhci& x = gXhci[gXhciCount++];
		x.iBase = ch->LinearAddress();
		x.iSize = size;
		x.iLoc = loc;
		Kern::Printf("[usbh] xHCI %04x:%04x at %02x:%02x.%d, BAR0 %08x (%x bytes) mapped uncached at %08x",
						id & 0xFFFF, id >> 16, b, d, f, base, size, (TUint)x.iBase);
		}
	if (!gXhciCount)
		{
		Kern::Printf("[usbh] no xHCI controller on the PCI bus");
		return KErrNotFound;
		}
	return KErrNone;
	}

extern "C" int phe_usb_board_hc(const char* aKind, int aN, struct phe_usb_hc_cfg* aCfg)
	{
	if (aKind[0] != 'x' || aN < 0 || aN >= gXhciCount)
		return -1;
	aCfg->base = gXhci[aN].iBase;
	aCfg->size = gXhci[aN].iSize;
	aCfg->irq = -1;							// polled
	return 0;
	}

// the stack's debug output (phe_usb_printf) goes to the kernel's debug serial
extern "C" void ekseam_puts(const char* aStr)
	{
	// one string at a time: the USB thread and the extension start run on different CPUs
	static volatile TInt lock;
	const TInt irq = NKern::DisableAllInterrupts();
	while (__sync_lock_test_and_set(&lock, 1))
		{}
	for (; *aStr; ++aStr)
		{
		if (*aStr == '\n')
			DebugOutputChar('\r');
		DebugOutputChar((TUint8)*aStr);
		}
	__sync_lock_release(&lock);
	NKern::RestoreInterrupts(irq);
	}

// no serial ports (phe_usb_tty.c) in this build: the service pass calls this weakly, and a weak call to 0 cannot be linked in long mode
extern "C" void phe_usb_tty_service(void) {}

TInt UsbCommInstall()						// the ARM USB host's phe_usb_comm.cpp (the serial ports as a Comm PDD): not on the PC
	{
	return KErrNone;
	}

TInt InitExtension_usbhost();				// the ARM USB host's phe_usb_ekern.cpp

void X64StartUsbHost()
	{
	NKern::ThreadEnterCS();
	TInt r = InitExtension_usbhost();
	NKern::ThreadLeaveCS();
	Kern::Printf("[usbh] InitExtension_usbhost -> %d", r);
	}
