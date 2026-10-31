#!/bin/bash
# Boot an existing SWEET XNU kernel on bonito with the tiny RAM-disk init as root.
#   tools/bonito-kernel-image.sh KERNEL [upper|wide] [extra boot-args]
# -> shim/out/shim-boot-xnu.img   (then: fastboot boot shim/out/shim-boot-xnu.img)
#
# Board drivers written for sweet are switched off: touch (Goodix + an RPMh
# regulator vote that is a different rail on SDM670), SPMI keys (PM6150 layout)
# and USB (sweet GCC offsets). Root = mockfs RAM disk serving tools/sweet-init
# as /sbin/launchd (rd=md0). Needs CROSS=<toolchain prefix> like shim/build.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
K=${1:?kernel path}
MEM=${2:-upper}
EXTRA=${3:-}
O=shim/out
mkdir -p $O

# init: GNU as instead of an Apple-target clang (tools/mkinit.py --raw)
${CROSS}gcc -c tools/sweet-init.S -o $O/init.o
${CROSS}objcopy -O binary -j .text $O/init.o $O/init.text
python3 tools/mkinit.py --raw $O/init.text 0 $O/sweet-init

if [ "$MEM" = wide ]; then flag=--bonito-wide; else flag=--bonito; fi
python3 tools/mkadt.py $O/bonito-$MEM.adt $flag 2>/dev/null

ARGS="debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0"
ARGS="$ARGS sweettouch=0 sweetkeys=0 sweetusb=0 wdt=-1 wdt_test=no_user_checkin $EXTRA"
python3 tools/mkpayload.py "$K" $O/bonito-$MEM.adt $O/payload-xnu.bin --ramdisk $O/sweet-init --cmdline "$ARGS"
(cd shim && ./build.sh ../$O/payload-xnu.bin >/dev/null)
mv $O/shim-boot.img $O/shim-boot-xnu.img
rm -f $O/*.ppm
echo "boot-args: $ARGS"
ls -l $O/shim-boot-xnu.img
