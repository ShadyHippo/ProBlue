#!/usr/bin/env sh
# wedge-probe.sh — capture the broken host-radio state DURING a wedge.
#
# Run this the moment the controller won't reconnect (before touching any
# rescue!). It snapshots every observable layer of the Intel 9260 BT stack so
# we can tell WHERE the wedge lives: USB transport, HCI command path, kernel
# scan flags, actual controller scan state, or RX delivery.
#
# Usage:  sudo tools/wedge-probe.sh
# Output: logs/wedge-probe-<ts>.txt  (gitignored) + terminal echo.

set -u
OUT_DIR="$(dirname "$0")/../logs"
mkdir -p "$OUT_DIR"
TS="$(date +%Y%m%d-%H%M%S)"
OUT="$OUT_DIR/wedge-probe-$TS.txt"
# Tee: show on terminal AND save (a file-only redirect makes it look like
# "nothing happened").
exec > >(tee "$OUT") 2>&1
echo "=== wedge-probe $TS — host $(hostname), controller 20:0B:CF:34:F1:BD ==="
echo

note() { echo; echo "########## $1 ##########"; }
hdr() { echo; echo "--- $1 ---"; }

note "1. USB transport (is the radio enumerated/awake?)"
hdr "usb power state"
for d in /sys/bus/usb/devices/*; do
	[ -f "$d/idVendor" ] || continue
	vid="$(cat "$d/idVendor" 2>/dev/null)"
	pid="$(cat "$d/idProduct" 2>/dev/null)"
	[ "$vid:$pid" = "8087:0025" ] || continue
	echo "$d: vid:pid=$vid:$pid"
	cat "$d/power/control" 2>/dev/null | sed 's/^/  control=/'
	cat "$d/power/runtime_status" 2>/dev/null | sed 's/^/  runtime_status=/'
	cat "$d/power/runtime_suspended_time" 2>/dev/null | sed 's/^/  runtime_suspended_time=/'
	cat "$d/power/runtime_active_time" 2>/dev/null | sed 's/^/  runtime_active_time=/'
	cat "$d/power/runtime_usage" 2>/dev/null | sed 's/^/  runtime_usage=/'
	cat "$d/bDeviceClass" 2>/dev/null | sed 's/^/  bDeviceClass=/'
done
command -v lsusb >/dev/null && lsusb -d 8087:0025 -v 2>/dev/null | grep -iE 'MaxPower|bInterval|iInterface' | head

note "2. MGMT / kernel flags (what the daemon believes)"
hdr "btmgmt info"
command -v btmgmt >/dev/null && btmgmt info 2>&1
hdr "hciconfig"
command -v hciconfig >/dev/null && hciconfig hci0 2>&1
hdr "bluetoothctl show"
command -v bluetoothctl >/dev/null && timeout 8 bluetoothctl show 2>&1

note "3. HCI command path (is the controller even answering?)"
hdr "Read Local Version (hcitool cmd 0x04 0x0001)"
command -v hcitool >/dev/null && hcitool cmd 0x04 0x0001 2>&1 || echo "(hcitool missing)"
hdr "Read BD ADDR (0x04 0x0009)"
command -v hcitool >/dev/null && hcitool cmd 0x04 0x0009 2>&1 || true

note "4. Controller's ACTUAL scan state (the key discriminator)"
hdr "Read Scan Enable (hcitool cmd 0x03 0x0019) — 0x00=none 0x01=inquiry 0x02=page 0x03=both"
echo "  NOTE: 0x03 0x001a is WRITE Scan Enable (needs param byte); READ is 0x0019 (fixed 2026-09-11)."
command -v hcitool >/dev/null && hcitool cmd 0x03 0x0019 2>&1 || true
hdr "Read Page Scan Activity (0x03 0x001b) — interval/window"
command -v hcitool >/dev/null && hcitool cmd 0x03 0x001b 2>&1 || true
hdr "Read Page Scan Type (0x03 0x0046)"
command -v hcitool >/dev/null && hcitool cmd 0x03 0x0046 2>&1 || true

note "5. RX delivery check — 20s passive btmon (expect: are there ANY events?)"
hdr "passive btmon 20s (ambient LE ads / any HCI event)"
command -v btmon >/dev/null && {
	timeout 20 sudo btmon -w "$OUT_DIR/wedge-probe-$TS.btsnoop" 2>&1 | head -60 || true
	echo "(captured btsnoop: $OUT_DIR/wedge-probe-$TS.btsnoop)"
	hdr "event count in first 20s window:"
	echo "$(btmon -r 2>/dev/null || echo n/a)"
} || echo "(btmon missing)"

note "6. Kernel debugfs bluetooth state (if mounted)"
if [ -d /sys/kernel/debug/bluetooth/hci0 ]; then
	ls -la /sys/kernel/debug/bluetooth/hci0/ 2>&1 | head -40
	for f in /sys/kernel/debug/bluetooth/hci0/*; do
		[ -f "$f" ] && echo "-- $f: $(cat "$f" 2>&1 | tr '\n' ' ')"
	done
else
	echo "debugfs not mounted — try: sudo mount -t debugfs none /sys/kernel/debug"
fi

note "7. Kernel log memory around now (any radio/coex/suspend noise?)"
journalctl -k --since -10min 2>/dev/null | grep -iE 'hci0|bluetooth|btusb|intel|ptt|coex|suspend|firmware' | tail -40 || true

note "8. Live discriminator candidates (do AFTER the above, one at a time)"
cat <<'EOF'
  A. WiFi off (coex):      ip link set wlan0 down   → press controller button
     If it connects → PT/T coex is the wedge cause. Re-enable after.
  B. btmgmt connectable:   sudo btmgmt connectable on → press button
     If it connects → scan-state loss (daemon level).
  C. rfkill soft cycle:    sudo rfkill block bluetooth; sleep 1; sudo rfkill unblock bluetooth
     (rescued 15:5x 2026-09-11; FAILED to reconnect at 23:58 — no longer the
      assumed "always works" control. Untested: modprobe -r btusb && modprobe btusb)
  D. Real write page scan: sudo hcitool cmd 0x03 0x001a 0x02, then re-read 0x0019
     (2026-09-11: btmgmt connectable equivalent was acked Success — did NOT un-wedge)
  E. RESCUE (confirmed 2026-09-12): read 0x0019 (expect 0x00 = wedged), write
     0x001a 0x02, re-read 0x0019 (expect 0x02), then press the controller button
     until it connects. No rfkill needed — see logs/wedge-fix-20260912.txt
EOF

echo
echo "=== wedged-state snapshot saved to $OUT ==="