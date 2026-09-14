#!/usr/bin/env sh
# unwedge.sh — re-arm the 9260's page scan (wedge rescue, no rfkill).
#
# While wedged, the radio's scan_enable register silently drops to 0x00 and
# the kernel never re-asserts it. This fixes it:
#   1. read   Scan Enable (0x03 0x0019)   -> 0x00 when wedged
#   2. write  Scan Enable = Page Scan (0x03 0x001a 0x02)
#   3. read   again                       -> 0x02
# Then press the controller button until it reconnects.
#
# For a passwordless hotkey: add a sudoers rule covering hcitool
# (e.g.  %wheel ALL=(root) NOPASSWD: /run/current-system/sw/bin/hcitool).
#
# Usage:  sudo ./unwedge.sh [BD_ADDR]   (self-elevates if run without sudo)
# Default BD_ADDR: 20:0B:CF:34:F1:BD

set -u

ADDR="${1:-20:0B:CF:34:F1:BD}"

[ "$(id -u)" -eq 0 ] || exec sudo "$0" "$@"

read_se() {
	timeout 5 hcitool cmd 0x03 0x0019 </dev/null 2>&1
}

se_val() {
	read_se | awk '$1 ~ /^[0-9a-fA-F]{2}$/ { print $5; exit }'
}

echo "== unwedge: re-arm page scan ($ADDR) =="

SE="$(se_val)"
echo "scan_enable before: ${SE:+0x}$SE"

case "$SE" in
	00)
		echo "-> wedged signature (scans off); writing Page Scan..."
		timeout 5 hcitool cmd 0x03 0x001a 0x02 </dev/null 2>&1
		SE2="$(se_val)"
		echo "scan_enable after:  ${SE2:+0x}$SE2"
		if [ "$SE2" = "02" ]; then
			echo "== re-armed OK. Press the controller button until it connects. =="
		else
			echo "!! write did not stick (0x$SE2) — try: sudo modprobe -r btusb && sudo modprobe btusb"
			exit 1
		fi
		;;
	02)
		echo "-> scan already on. If it still won't connect, the controller likely"
		echo "   stopped paging — press its button repeatedly."
		;;
	*)
		echo "!! could not parse the reply:"
		read_se
		exit 1
		;;
esac

if command -v bluetoothctl >/dev/null 2>&1; then
	echo "-- watching up to 20 s for 'Connected: yes' (press the button!) --"
	i=0
	while [ "$i" -lt 20 ]; do
		if timeout 3 bluetoothctl info "$ADDR" </dev/null 2>&1 | grep -q "Connected: yes"; then
			echo "== connected =="
			exit 0
		fi
		sleep 1
		i=$((i + 1))
	done
	echo "(not connected after 20 s — keep pressing, or run again)"
fi