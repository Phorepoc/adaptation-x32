# usbhost: the FreeBSD USB host on the PC's xHCI controller

`USB_HOST=1` (make_ekern.sh, off by default) puts FreeBSD's USB stack in the kernel. The stack, the controller attach, the kernel
seam and the HID client come from the **pheusb** layer (its `os/src/usbhost` and `os/third_party/freebsd-usb`, merged into the
OS tree; the board lists it in REPOS). What is here is the board's part:

| File | What |
|---|---|
| `xk_usbhost.cpp` | the board layer of `phe_usb_port.h`: finds the xHCI controllers on the PCI bus (class 0C0330, `xk_pci.h`), enables memory decode and bus mastering, maps BAR0 uncached, keeps INTx off; the debug output; the HID client's key map (F9 = the phone's power key, F10 = its Menu key); `X64StartUsbHost()` |

make_ekern.sh builds the stack with pheusb's `build_bsd.sh` (ARCH=x32, HC=xhci) and links `phe_usb_ekern.cpp` and
`phe_usb_hid_ekern.cpp` (USB keyboards, mice, tablets, XInput gamepads; it registers the mouse HAL when the board has none).

Test (QEMU): `-device qemu-xhci,id=x -device usb-kbd,bus=x.0 -device usb-tablet,bus=x.0`; the serial console logs the
`[usbh]` device lines a few seconds after the USB host starts.
