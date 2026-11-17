# usbhost: the FreeBSD USB host on the PC's xHCI controller

`USB_HOST=1` (make_ekern.sh, off by default) puts FreeBSD's USB stack in the kernel. The stack is FreeBSD's, imported
unmodified: the core and its kernel shim (`os/third_party/freebsd-usb`, shared with the ARM USB host) and the xHCI driver
(`os/third_party/freebsd-usb-xhci`, the same pin). What is here is ours:

| File | What |
|---|---|
| `xk_usbhost.cpp` | the board layer of `phe_usb_port.h`: finds the xHCI controllers on the PCI bus (class 0C0330, `xk_pci.h`), enables memory decode and bus mastering, maps BAR0 uncached, keeps INTx off; the debug output; `X64StartUsbHost()` |
| `xk_usb_hc.c` | attaches FreeBSD's `xhci.c` to those controllers on the kshim's `pci` root: BIOS hand-off, halt, start (which resets), polled interrupts, the service pass and the device dump |
| `build_bsd_x32.sh` | compiles the stack for the x32 kernel into one object, with the kshim's SYSINIT tables |
| `kinc/` | a header stand-in so the ARM USB host's `phe_usb_ekern.cpp` (the DFC thread, the memory pool, the clock) builds here unmodified |

The ARM USB host's seam files (`phe_usb_port.h`, `ekern/phe_usb_ekern.cpp`, `ekern/phe_usb_libc.c`) and the core import must be in
the OS tree (`src/phorepoc-arm/a64/usbhost`, `third_party/freebsd-usb`). Proposal: move the core, the seam, the shared
service pass / device dump / `pci` root and the xHCI files into one repository both architectures use.

Test (QEMU): `-device qemu-xhci -device usb-serial,chardev=c0,always-plugged=true -chardev null,id=c0 -device usb-kbd`;
the serial console logs `[usbh] dev addr=1 VID:PID=0403:6001 ...` about 8 s after the USB host starts, ending with `[usbh] dump done`.
