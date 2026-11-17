# freebsd-usb-xhci (unmodified import)

FreeBSD's xHCI host controller driver, imported unmodified for the PC image's USB host: `xhci.c`, `xhci.h` and
`xhcireg.h` from `sys/dev/usb/controller`. Each file keeps its own BSD-2-Clause licence header.

Pin: https://github.com/freebsd/freebsd-src commit 043d6753ca7c1c1e3ffa09a9f953f0df50d3a22e, the same commit as the
USB core and kshim in the ARM USB host's `os/third_party/freebsd-usb`. This directory holds only what that import lacks;
the core, the kshim and `usb_busdma_loader.c` come from there. Check: `sh check.sh` (every file in SHA256SUMS).

Proposal: when the USB host moves to a repository both architectures share, these three files join the core import.
Our glue is separate: `os/tools/phorepoc-x86/usbhost`.
