#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later
#
# build_bsd_x32.sh <out-dir> -- the C side of the PC kernel's USB host: the unmodified FreeBSD USB stack on its kshim
# (the ARM USB host's third_party/freebsd-usb), FreeBSD's xHCI driver (third_party/freebsd-usb-xhci, unmodified), our
# xk_usb_hc.c and the ARM USB host's phe_usb_libc.c, compiled for the x32 kernel and collected into ONE relocatable
# object, <out-dir>/usbhost_bsd.o, with the kshim's SYSINIT tables generated as FreeBSD's stand/kshim/kshim.mk does.
# usbd_get_page is wrapped inside the object (xk_usb_hc.c). Called by make_ekern.sh (USB_HOST=1).
# Env: FB (the freebsd-usb import), XH (the xhci import), A64USB (the ARM USB host's src/phorepoc-arm/a64/usbhost:
#      phe_usb_port.h, phe_usb_libc.c), GCC (gcc), HOSTCC (cc).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
O="$1"; mkdir -p "$O"
: "${FB:?FB = the USB core import, os/third_party/freebsd-usb}"
: "${A64USB:?A64USB = the ARM USB host seam, os/src/phorepoc-arm/a64/usbhost}"
XH="${XH:-$HERE/../../../third_party/freebsd-usb-xhci}"
GCC="${GCC:-gcc}"
HOSTCC="${HOSTCC:-cc}"
U="$FB/sys/dev/usb"
KSRCS=(
  "$FB/stand/kshim/bsd_kernel.c" "$FB/stand/kshim/bsd_busspace.c"
  "$U/controller/usb_controller.c" "$XH/sys/dev/usb/controller/xhci.c"
  "$U/usb_core.c" "$U/usb_debug.c" "$U/usb_device.c" "$U/usb_dynamic.c" "$U/usb_error.c"
  "$U/usb_handle_request.c" "$U/usb_hid.c" "$U/usb_hub.c" "$U/usb_lookup.c" "$U/usb_msctest.c"
  "$U/usb_parse.c" "$U/usb_request.c" "$U/usb_transfer.c" "$U/usb_util.c"
  "$HERE/xk_usb_hc.c"
)
SRCS=( "$FB/stand/usb/usb_busdma_loader.c" )
# the kernel's ABI: x32 (long mode, 32-bit pointers), no red zone, no SSE, no libc headers
ARCH=( -mx32 -mno-red-zone -mgeneral-regs-only -ffreestanding -fno-strict-aliasing -fno-stack-protector
  -fno-tree-loop-distribute-patterns -O2 -fno-pic -fno-pie -fno-common )
CFLAGS=( "${ARCH[@]}" -nostdinc -isystem "$("$GCC" -print-file-name=include)" -D_STANDALONE -DHAVE_MALLOC
  -Dprintf=phe_usb_printf -Dsnprintf=phe_usb_snprintf
  -Wall -Wno-unused -Wno-pointer-sign -Wno-unused-but-set-variable
  -I"$FB/stand/kshim" -I"$FB/sys" -I"$XH/sys" -I"$A64USB" -I"$A64USB/kinc"
  -D'DECLARE_CLASS(x)=' -D'MPASS(x)=((void)0)'
  -DUSB_GLOBAL_INCLUDE_FILE='"bsd_global.h"' -D'__uintptr_t=unsigned long' -DUSB_MSCTEST_BULK_SIZE=65536
  ${USBHOST_DEFS:-} )
objs=(); sysobjs=()
for s in "${KSRCS[@]}" "${SRCS[@]}"; do
  o="$O/$(basename "${s%.c}").o"; "$GCC" "${CFLAGS[@]}" -c "$s" -o "$o"; objs+=("$o")
done
for s in "${KSRCS[@]}"; do
  o="$O/$(basename "${s%.c}").o"; objcopy -j .debug.sysinit -O binary "$o" "${o%.o}.osys"; sysobjs+=("${o%.o}.osys")
done
cat "${sysobjs[@]}" > "$O/sysinit.bin"
[ -x "$O/sysinit" ] || "$HOSTCC" -O2 -I"$FB/stand/kshim" -o "$O/sysinit" "$FB/stand/usb/tools/sysinit.c"
"$O/sysinit" -i "$O/sysinit.bin" -o "$O/sysinit_data.c" -k sysinit -s sysinit_data
"$O/sysinit" -i "$O/sysinit.bin" -o "$O/sysuninit_data.c" -R -k sysuninit -s sysuninit_data
for s in "$O/sysinit_data.c" "$O/sysuninit_data.c"; do
  o="${s%.c}.o"; "$GCC" "${CFLAGS[@]}" -c "$s" -o "$o"; objs+=("$o")
done
"$GCC" "${ARCH[@]}" -Wall -c "$A64USB/ekern/phe_usb_libc.c" -o "$O/phe_usb_libc.o"; objs+=("$O/phe_usb_libc.o")
# one object; the .debug.sysinit records stay out of the kernel image
ld -m elf32_x86_64 -r --wrap=usbd_get_page -o "$O/usbhost_bsd.o" "${objs[@]}"
objcopy --remove-section=.debug.sysinit "$O/usbhost_bsd.o"
echo "usbhost: $O/usbhost_bsd.o"
