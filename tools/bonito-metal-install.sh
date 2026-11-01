#!/bin/sh
# Install SweetMetal2.bundle (Metal on metal2vulkan + Turnip; .github/workflows/metal.yml) into the running
# phone's /System/Library/Extensions over the USB file server (kernel 0087+, boot with USB=2), plus the
# config SweetMetal2 reads. Takes effect on the next boot with METAL=1 (boot-arg sweetmtl=2).
#   tools/bonito-metal-install.sh [BUNDLE_DIR]   (default: the latest metal CI artifact, fetched)
#   CLIENTS="backboardd" (default) | "*" (every process gets a Metal device) | "backboardd SpringBoard ..."
set -e
cd "$(dirname "$0")/.."
F="python3 tools/bonito-files.py"
B=$1
if [ -z "$B" ]; then
	D=${TMPDIR:-/tmp}/bonito-metal; rm -rf "$D"
	id=$(gh run list -R watrabi/ios-sweet --workflow metal.yml --status success -L 1 --json databaseId -q '.[0].databaseId')
	gh run download "$id" -R watrabi/ios-sweet -n sweetmetal2 -D "$D"
	B=$D/metal2/out/SweetMetal2.bundle
fi
for f in SweetMetal2 libvulkan_freedreno.dylib Info.plist _CodeSignature/CodeResources; do
	[ -s "$B/$f" ] || { echo "incomplete bundle: no $B/$f" >&2; exit 1; }
done
ping -c 1 -W 2 192.168.7.2 >/dev/null || { echo "phone not reachable: tools/bonito-usbnet.sh" >&2; exit 1; }
E=/System/Library/Extensions/SweetMetal2.bundle
$F mkdir $E 755
$F mkdir $E/_CodeSignature 755
for f in Info.plist _CodeSignature/CodeResources libvulkan_freedreno.dylib SweetMetal2; do
	mode=644; case $f in SweetMetal2|*.dylib) mode=755;; esac
	echo "put $f ($(stat -c %s "$B/$f") bytes)"
	$F put "$B/$f" "$E/$f" $mode
done
$F mkdir /var/local 755
# backboardd and apps are not root: the shader caches (SweetMetal2: /var/local/m2v-cache/<proc>) must be writable
$F mkdir /var/local/m2v-cache 777
$F chmod /var/local/m2v-cache 777
for p in backboardd SpringBoard; do $F mkdir /var/local/m2v-cache/$p 777; $F chmod /var/local/m2v-cache/$p 777; done
printf '%s\n' "${CLIENTS:-backboardd}" > "${TMPDIR:-/tmp}/mtl2-clients"
$F put "${TMPDIR:-/tmp}/mtl2-clients" /var/local/mtl2-clients 644
: > "${TMPDIR:-/tmp}/mtl2.log"
$F put "${TMPDIR:-/tmp}/mtl2.log" /var/local/mtl2.log 666
$F chmod /var/local/mtl2.log 666
python3 tools/bonito-cmd.py sync >/dev/null 2>&1 || true
echo "installed; reboot (tools/bonito-cmd.py reboot) and boot shim-boot-0088-metal.img"
