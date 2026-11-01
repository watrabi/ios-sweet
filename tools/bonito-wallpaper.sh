#!/bin/sh
# The default iOS 16 wallpaper (WWDC 2022, iPhone X size) decoded to PNG over its .HEIC layers: this phone has
# no HEVC decoder, so HEIC images decode to nothing (ios-flame HANDOFF.md). Needs the SD-card root image
# mounted read-only at /mnt/iosro (original layers), libheif's heif-dec, and the phone's file server (0087+;
# the space in "Head Mask" needs the percent-decoding kernel).
set -e
cd "$(dirname "$0")/.."
W="/Library/Wallpaper/Collections/iOS_16~iphone.wallpaperCollection/Wallpapers/7400.WWDC_2022-375w-812h@3x~iphone.wallpaper"
T=${TMPDIR:-/tmp}/bonito-wp; mkdir -p "$T"
for x in "Background:Head" "Background:Head Mask" "Background:Leg" "Floating:Arm" "Foreground:Body"; do
	L=${x%%:*}; N=${x#*:}
	src="/mnt/iosro$W/7400.WWDC_2022_$L-375w-812h@3x~iphone.ca/assets/$N.HEIC"
	heif-dec "$src" "$T/$N.png" >/dev/null
	python3 tools/bonito-files.py put "$T/$N.png" "$W/7400.WWDC_2022_$L-375w-812h@3x~iphone.ca/assets/$N.HEIC" 0644
done
python3 tools/bonito-cmd.py sync >/dev/null 2>&1 || true
