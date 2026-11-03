#!/usr/bin/env python3
"""Patch a built kernel so the phone's USB Ethernet gadget (0075/0087) reports vendor-specific
classes instead of CDC ECM. A ChromeOS host binds its own cdc_ether to a CDC ECM device and then
will not share it with the Linux VM; with class 0xff no host driver claims it, the device can be
shared, and tools/ecm-tap.py drives it (the wire protocol is unchanged).

  tools/usb-vendor-class.py KERNEL_IN KERNEL_OUT
"""
import sys

DEV = bytes([18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64, 0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1])
CFG = bytes([9, 2, 80, 0, 2, 1, 0, 0x80, 250,
             9, 4, 0, 0, 1, 0x02, 0x06, 0x00, 0,           # comm interface 0: CDC ECM
             5, 0x24, 0x00, 0x10, 0x01,
             5, 0x24, 0x06, 0, 1,
             13, 0x24, 0x0f, 4, 0, 0, 0, 0, 0xea, 0x05, 0, 0, 0,
             7, 5, 0x82, 0x03, 16, 0, 9,
             9, 4, 1, 0, 0, 0x0a, 0x00, 0x00, 0,           # data interface 1 alt 0
             9, 4, 1, 1, 2, 0x0a, 0x00, 0x00, 0])          # data interface 1 alt 1


def patch_once(buf, pat, edits):
    i = buf.find(pat)
    if i < 0 or buf.find(pat, i + 1) >= 0:
        sys.exit(f'descriptor not found exactly once ({pat[:6].hex()}...)')
    for off, val in edits:
        buf[i + off] = val
    return i


k = bytearray(open(sys.argv[1], 'rb').read())
d = patch_once(k, DEV, [(4, 0x00)])                        # bDeviceClass: per interface
c = patch_once(k, CFG, [(9 + 5, 0xff), (9 + 6, 0x00),     # comm interface: vendor
                        (48 + 5, 0xff), (57 + 5, 0xff)])   # data interface alts: vendor
open(sys.argv[2], 'wb').write(k)
print(f'device descriptor at {d:#x}, config descriptor at {c:#x}: vendor class')
