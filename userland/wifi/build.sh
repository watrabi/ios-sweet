#!/bin/sh
# Build wland for iOS 16 arm64 into userland/wifi/out (signed ad hoc). From ios-flame phone/wifi (MIT), bonito constants.
set -e
cd "$(dirname "$0")"
mkdir -p out
${IOSCC:-../../work/userland/bin/ios-cc} -O2 -Wall -Wextra -Wno-unused-parameter -o out/wland wland.c smem.c pmem.c glink.c link.c qmi.c rmtfs.c pdmapper.c tftp.c scm.c wlfw.c radio.c ipnet.c bridge.c
codesign -f -s - out/wland 2>/dev/null
ls -la out/wland
