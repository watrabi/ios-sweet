#!/bin/bash
# Boot an existing SWEET XNU kernel on bonito with the tiny RAM-disk init as root.
#   tools/bonito-kernel-image.sh KERNEL [upper|wide] [extra boot-args]
# -> shim/out/shim-boot-xnu.img   (then: fastboot boot shim/out/shim-boot-xnu.img)
# ROOT=emmc: no RAM disk, root = userdata on eMMC (rd=ufsroot0, needs patch 0082);
#   read-only unless the extra boot-args say ufsrw=1 rootrw=1.
# TOUCH=1: keep the touch driver on (bonito RMI4, patch 0084).
# KEYS=1: keep the key driver on (power/volume, patch 0085 for bonito).
# CPUS=N: start N cores (default 1; bonito runs fine with 6, the A55s).
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
# USB=1: report USB state; USB=2: USB Ethernet 192.168.7.2 + file server tcp 2223 (0087)
# GPU=1: Adreno 615 firmware into /chosen + boot-arg sweetgpu=1 (patch 0086)
if [ "${GPU:-0}" = 1 ]; then flag="$flag --gpufw research/gpu/fw"; fi
# METAL=1: GPU buffer pool carved from the top of RAM (512 MB, --gpupool) + boot-arg sweetmtl=2 (0088, SweetMetal2.bundle)
if [ "${METAL:-0}" = 1 ]; then flag="$flag --gpupool ${GPUPOOL_MB:-512}"; fi
# SCALE=2: iOS renders the iPhone X layout at @2x (750x1624), the GPU present scales it up (needs GPU=1)
if [ "${SCALE:-3}" = 2 ]; then flag="$flag --scale2"; fi
python3 tools/mkadt.py $O/bonito-$MEM.adt $flag 2>/dev/null

# The iOS root needs these (launchd exec on this macOS-target kernel, rw root, fb); IOS=0 to omit.
IOSARGS="unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1 ufsrw=1 rootrw=1 sweetfb_noconsole=1 sweetfb_hz=${FBHZ:-60}"
if [ "${ROOT:-md}" = emmc ] && [ "${IOS:-1}" = 1 ]; then EXTRA="$IOSARGS $EXTRA"; fi
if [ "${ROOT:-md}" = emmc ]; then RD=rd=ufsroot0; RDARG=(); else RD=rd=md0; RDARG=(--ramdisk $O/sweet-init); fi
ARGS="debug=0xc serial=3 cpus=${CPUS:-8} keepsyms=1 -enable_kprintf_spam -noprogress $RD"
# TOUCH=1: enable the bonito RMI4 touch driver (patch 0084); keys/USB stay off.
if [ "${TOUCH:-0}" = 1 ]; then TOUCHARG=""; else TOUCHARG="sweettouch=0 "; fi
if [ "${KEYS:-0}" = 1 ]; then KEYSARG=""; else KEYSARG="sweetkeys=0 "; fi
if [ "${GPU:-0}" = 1 ]; then GPUARG="sweetgpu=1 "; else GPUARG=""; fi
if [ "${METAL:-0}" = 1 ]; then GPUARG="${GPUARG}sweetmtl=2 "; fi
if [ "${SCALE:-3}" = 2 ]; then GPUARG="${GPUARG}sweetfb_scale=2 "; fi
# GPUDBG=<s>: GPU bring-up that long after boot, TZ calls logged + paced, log flushed every second
if [ -n "${GPUDBG:-}" ]; then GPUARG="${GPUARG}sweetgpudbg=$GPUDBG ufslogsecs=1 "; fi
ARGS="$ARGS ${TOUCHARG}${KEYSARG}${GPUARG}sweetusb=${USB:-0} wdt=-1 wdt_test=no_user_checkin $EXTRA"
python3 tools/mkpayload.py "$K" $O/bonito-$MEM.adt $O/payload-xnu.bin "${RDARG[@]}" --cmdline "$ARGS"
(cd shim && ./build.sh ../$O/payload-xnu.bin >/dev/null)
mv $O/shim-boot.img $O/shim-boot-xnu.img
rm -f $O/*.ppm
echo "boot-args: $ARGS"
ls -l $O/shim-boot-xnu.img
