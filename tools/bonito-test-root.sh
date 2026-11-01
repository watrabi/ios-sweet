#!/bin/bash
# Build the 8 MiB HFS+ test root for the first boot from internal eMMC:
# /sbin/launchd = tools/sweet-init (prints "init running as PID 1" and a
# heartbeat), plus /dev for devfs. Case-sensitive like iOS.
#   sudo tools/bonito-test-root.sh [OUT]     (default shim/out/root-hfs.img)
# Then:  fastboot flash userdata OUT   and boot with rd=ufsroot0 (ROOT=emmc).
# Needs hfsprogs (mkfs.hfsplus, fsck.hfsplus) and root for the loop mount.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${1:-shim/out/root-hfs.img}
INIT=shim/out/sweet-init
[ "$(id -u)" = 0 ] || { echo "run with sudo (loop mount)" >&2; exit 1; }
[ -f "$INIT" ] || { echo "$INIT missing: run tools/bonito-kernel-image.sh once" >&2; exit 1; }

rm -f "$OUT"
truncate -s 8M "$OUT"
mkfs.hfsplus -s -v SweetRoot "$OUT" >/dev/null
MNT=$(mktemp -d)
mount -t hfsplus -o loop "$OUT" "$MNT"
trap 'umount "$MNT" 2>/dev/null; rmdir "$MNT"' EXIT
mkdir -p "$MNT/sbin" "$MNT/dev"
install -m 0755 -o 0 -g 0 "$INIT" "$MNT/sbin/launchd"
sync
umount "$MNT"
trap - EXIT
rmdir "$MNT"
fsck.hfsplus -n "$OUT"
chown "${SUDO_UID:-0}:${SUDO_GID:-0}" "$OUT"
sha256sum "$OUT"
echo "ok: $OUT"
