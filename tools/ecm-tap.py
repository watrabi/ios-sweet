#!/usr/bin/env python3
"""Userspace CDC ECM host for the phone's USB Ethernet (kernel 0087, sweetusb=2), for hosts whose
kernel has no usbnet/cdc_ether (ChromeOS Crostini). Bridges the gadget's bulk endpoints to a TAP
interface and gives it 192.168.7.1/24, so tools/bonito-files.py etc. work unchanged.

  sudo tools/ecm-tap.py [IFNAME]        (default sweet0; Ctrl-C to stop)

Gadget (xnu/patches/0075): VID 0x1209 PID 0x0001, data interface 1 alt 1, bulk IN 0x81 /
OUT 0x01 (512 B packets), one Ethernet frame per transfer, frames <= 0x800 bytes.
Needs python3-usb (pyusb) and /dev/net/tun.
"""
import fcntl, os, struct, subprocess, sys, threading, time

import usb.core, usb.util

VID, PID = 0x1209, 0x0001
DATA_IF, DATA_ALT, EP_IN, EP_OUT = 1, 1, 0x81, 0x01
FRAME_MAX = 0x800
TUNSETIFF, IFF_TAP, IFF_NO_PI = 0x400454ca, 0x0002, 0x1000
HOST_IP = '192.168.7.1/24'


def wait_device():
    shown = False
    while True:
        dev = usb.core.find(idVendor=VID, idProduct=PID)
        if dev is not None:
            return dev
        if not shown:
            print('waiting for USB 1209:0001 (phone booted with sweetusb=2, device shared with Linux)')
            shown = True
        time.sleep(1)


def open_tap(name):
    fd = os.open('/dev/net/tun', os.O_RDWR)
    ifr = fcntl.ioctl(fd, TUNSETIFF, struct.pack('16sH', name.encode(), IFF_TAP | IFF_NO_PI))
    name = ifr[:16].split(b'\0', 1)[0].decode()
    subprocess.run(['ip', 'link', 'set', name, 'up'], check=True)
    subprocess.run(['ip', 'addr', 'replace', HOST_IP, 'dev', name], check=True)
    return fd, name


def main():
    ifname = sys.argv[1] if len(sys.argv) > 1 else 'sweet0'
    tap, ifname = open_tap(ifname)
    print(f'{ifname}: {HOST_IP}')
    threading.Thread(target=tap_to_usb, args=(tap,), daemon=True).start()
    while True:
        dev = wait_device()
        try:
            run(dev, tap)
        except usb.core.USBError as e:
            print(f'USB gone ({e}); waiting for the phone again', flush=True)
            with LOCK:
                CUR[0] = None
                usb.util.dispose_resources(dev)
            time.sleep(1)


def run(dev, tap):
    for i in (0, 1):
        try:
            if dev.is_kernel_driver_active(i):
                dev.detach_kernel_driver(i)
        except (usb.core.USBError, NotImplementedError):
            pass
    try:
        dev.set_configuration(1)
    except usb.core.USBError:
        pass                                    # already configured
    usb.util.claim_interface(dev, 0)
    usb.util.claim_interface(dev, DATA_IF)
    dev.set_interface_altsetting(interface=DATA_IF, alternate_setting=DATA_ALT)
    # SET_ETHERNET_PACKET_FILTER (directed | broadcast | all-multicast), as Linux cdc_ether sends
    try:
        dev.ctrl_transfer(0x21, 0x43, 0x000e, 0, b'')
    except usb.core.USBError:
        pass
    print('phone connected: 192.168.7.2', flush=True)
    CUR[0] = dev
    try:
        while True:
            try:
                frame = dev.read(EP_IN, FRAME_MAX, timeout=500)
            except usb.core.USBTimeoutError:
                continue
            if len(frame):
                os.write(tap, bytes(frame))
    finally:
        with LOCK:
            CUR[0] = None


CUR = [None]                                    # the connected phone, or None
LOCK = threading.Lock()                         # libusb segfaults if a handle is freed mid-write


def tap_to_usb(tap):
    """One reader for the TAP for the whole run (frames are dropped while no phone is attached)."""
    while True:
        frame = os.read(tap, FRAME_MAX)
        with LOCK:
            dev = CUR[0]
            if dev is None:
                continue
            try:
                dev.write(EP_OUT, frame, timeout=2000)
                if len(frame) % 512 == 0:
                    dev.write(EP_OUT, b'', timeout=2000)     # end the transfer with a ZLP
            except usb.core.USBError:
                pass                            # timeout or unplug; the reader loop notices

if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
