#!/bin/sh
# Give this PC 192.168.7.1/24 on the phone's USB Ethernet (kernel 0087, boot-arg sweetusb=2:
# CDC ECM gadget VID 0x1209 PID 0x0001, phone at 192.168.7.2). Waits up to 3 minutes.
#   tools/bonito-usbnet.sh        then: ping 192.168.7.2; tools/bonito-files.py ...
for i in $(seq 1 90); do
	for n in /sys/class/net/*; do
		d=$(readlink -f "$n/device/.." 2>/dev/null) || continue
		[ "$(cat "$d/idVendor" 2>/dev/null)" = 1209 ] && [ "$(cat "$d/idProduct" 2>/dev/null)" = 0001 ] || continue
		IF=$(basename "$n")
		command -v nmcli >/dev/null && sudo nmcli dev set "$IF" managed no 2>/dev/null   # else NetworkManager drops the address
		sudo ip link set "$IF" up
		ip -4 addr show dev "$IF" | grep -q 192.168.7.1/ || sudo ip addr add 192.168.7.1/24 dev "$IF"
		echo "phone link: $IF"; ip -4 addr show dev "$IF" | grep inet
		exec ping -c 3 -W 2 192.168.7.2
	done
	sleep 2
done
echo "no USB Ethernet from the phone (lsusb | grep 1209:0001)" >&2
exit 1
