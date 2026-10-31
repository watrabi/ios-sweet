#!/bin/bash
# Back up bonito's per-device partitions (calibration / IMEI / modem state that
# cannot be downloaded anywhere) from a temporarily booted TWRP:
#   fastboot boot twrp-3.7.1_12-1-bonito.img      (nothing is flashed)
#   tools/bonito-backup.sh [OUTDIR]               (default: backup/, gitignored)
# Read-only on the phone: only dd from /dev/block/by-name/* to stdout.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${1:-backup}
PARTS="persist modemst1 modemst2 fsg fsc modemcal devinfo"
mkdir -p "$OUT"

adb wait-for-recovery
[ "$(adb shell id -u | tr -d '\r')" = 0 ] || { echo "adb is not root (is this TWRP?)" >&2; exit 1; }

for p in $PARTS; do
	dev=/dev/block/by-name/$p
	if ! adb shell "test -e $dev" ; then
		echo "skip $p (no $dev)"
		continue
	fi
	want=$(adb shell "blockdev --getsize64 $dev" | tr -d '\r')
	adb exec-out "dd if=$dev bs=1048576 2>/dev/null" > "$OUT/$p.img"
	got=$(stat -c %s "$OUT/$p.img")
	if [ "$got" != "$want" ]; then
		echo "ERROR: $p: got $got bytes, partition is $want" >&2
		exit 1
	fi
	echo "ok   $p ($got bytes)"
done
(cd "$OUT" && sha256sum *.img > SHA256SUMS)
echo "saved to $OUT/ (SHA256SUMS written) - keep a copy off this PC too"
