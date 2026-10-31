#!/bin/bash
# Build the shim and wrap it in a boot.img v2 that mirrors the stock bonito header
# (Pixel 3a XL, SP2A.220505.008: header v2, base 0, kernel 0x8000, ramdisk 0x1000000,
# tags 0x100, dtb 0x1f00000, page 4096). CROSS defaults to a Debian/Arch-style cross gcc;
# Arm's bare-metal toolchain works too: CROSS=/path/to/bin/aarch64-none-elf- ./build.sh
set -euo pipefail
cd "$(dirname "$0")"
CROSS=${CROSS:-aarch64-linux-gnu-}
CC=${CROSS}gcc
OBJCOPY=${CROSS}objcopy
DTB=${DTB:-../dtb/bonito-stock.dtb}
# bonito ABL rejects a raw Image ("Error verifying the received boot.img: Unsupported");
# stock ships an LZ4 frame (magic 04 22 4d 18). KCOMP=lz4|gzip|none.
KCOMP=${KCOMP:-lz4}
CFLAGS="${EXTRA_CFLAGS:-} -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c emmc.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(${CROSS}readelf -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/emmc.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/emmc.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(${CROSS}nm out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image
# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio (ABL dislikes a zero-size ramdisk on some builds) -> diagnostics.
if [ -n "${1:-}" ]; then
	cp "$1" out/ramdisk.bin
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
case "$KCOMP" in
	lz4)  lz4 -q -9 -f out/Image out/Image.k ;;
	gzip) gzip -9 -n -c out/Image > out/Image.k ;;
	none) cp out/Image out/Image.k ;;
	*) echo "bad KCOMP=$KCOMP" >&2; exit 1 ;;
esac
mkbootimg --header_version 2 --kernel out/Image.k --ramdisk out/ramdisk.bin \
	--dtb "$DTB" --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
	--tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
	--os_version 12.0.0 --os_patch_level 2022-05 \
	--cmdline "console=null" -o out/shim-boot.img
# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
gcc -O1 -Wall -o out/host_fdt console.c fdt.c host_fdt.c && ./out/host_fdt "$DTB"
ls -l out/Image out/shim-boot.img
