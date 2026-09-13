# Intel 9260 reconnect wedge — hardware notes

**Secondary, and unrelated to ProBlue.** This is a host-radio firmware problem
observed on the author's machine. It is documented here because it can be
mistaken for a cable-pairing failure and can make ProBlue's acceptance tests
false-fail. Nothing in `src/` causes or fixes it.

Status as of 2026-09-12: mechanism confirmed, rescue known, trigger still unknown.

## Hardware

- Host adapter: Intel Wireless-AC 9260, BT USB `8087:0025`, ACPI node `1-4`,
  controller `20:0B:CF:34:F1:BD`, host `50:28:4A:0D:54:7A`.
- Kernel 6.18.46, BlueZ 5.86. WiFi side of the same card is `8086:2526` (iwlwifi).

## Symptom

The controller is already paired and works after any full Bluetooth-stack reset.
Randomly, it enters a "wedged" state: it strobes its LEDs (it *is* paging) but
the host never hears it, and pressing buttons does nothing. The state persists
until the host scan register is re-armed.

## Confirmed mechanism (2026-09-12)

While wedged, the truthful HCI read **`hcitool cmd 0x03 0x0019`** (Read Scan
Enable) returns `02 19 0C 00 00` — a well-formed reply with `scan_enable`
genuinely `0x00` (No Scans). The radio firmware has silently dropped scan-enable
to zero; the kernel trusts its cached `HCI_CONNECTABLE`/`HCI_PSCAN` flags and
never re-asserts it.

> Earlier "truncated Read Scan Enable (`plen 4`, `02 1A 0C 00`)" fingerprints
> were an instrumentation bug: `hcitool cmd 0x03 0x001a` is **WRITE** Scan Enable
> (needs a parameter byte), so a zero-length write returns a malformed reply in
> *every* state. The read is `0x0019`. Anything based on `0x001a` reads is void.

The kernel's cached flag is inconsistent across wedges (page-scan bit was set in
one capture, clear in another), so "firmware dropped it" vs "kernel cleared it and
forgot" is not fully settled — what is certain is that the register ends at `0x00`
and nothing re-writes it.

## Rescue

Re-arm scan enable directly, then let the controller's ongoing paging be heard:

```sh
sudo hcitool cmd 0x03 0x001a 0x02     # write Scan Enable = Page Scan
sudo hcitool cmd 0x03 0x0019          # verify: reply must end in 0x02
# then press the controller until it connects
```

`tools/unwedge.sh` wraps this (read → write → re-read → 20 s connect watch); the
author binds it to a hotkey with a scoped NOPASSWD sudoers rule.

- **rfkill is not reliable.** `rfkill block; unblock` re-runs the full init and
  rewrites scan enable, but on 2026-09-11 23:58 the firmware dropped it again and
  the controller had stopped paging — i.e. it needed the re-arm *and* button
  presses, not a stack reset. Earlier "rfkill always fixes it" is downgraded.
- **Non-rescues (measured):** `bluetoothctl power off/on`, `btmgmt power off/on`
  (mgmt power-off is deferred to a work item and can race/`Busy`), and a bare HCI
  `Reset` (`hcitool cmd 0x03 0x0003`) which does not re-run the init that rewrites
  scan enable.
- `systemctl restart bluetooth` is *not* a rescue on a machine with
  `powerOnBoot=false` — the adapter comes back off.

## Ruled out as the cause (do not retry)

ERTM, the Bluetooth-side arm `0x08 00`, the page-scan accept list, USB
autosuspend as a *live* cause (`power/control=on` did not prevent a wedge), and
WiFi-off (did not cure an active wedge). Discoverable/connectable keep-alives do
not help. No BlueZ/userspace change can fix a radio that stopped delivering pages
while reporting healthy.

Trigger candidates still open: Intel PTT/WiFi-BT coexistence (vendor event `0x26`
appears in wedge traces), a suspend/resume autoclear quirk, or an idle transition.

## Firmware status

The installed firmware is current: `ibt-18-16-1.sfi` build `201-12.24` (week 12,
2024), identical to the linux-firmware-20260810 blob. The 2025 blob update was
bugged and reverted upstream (linux-bluetooth bug 220306), so **there is no
upgrade path**. A fix would have to come from trigger prevention or a kernel-side
self-heal.

## Upstream work in this area (not merged)

Sean Rhodes' series *"Bluetooth: btusb: clear remote wake on idle Intel ACPI
paths"* (v1 2026-03, v2 2026-05, ~900-line rework 2026-08) is the most relevant
upstream work. It lets the USB port power off when the BT device is idle, gated on
page/inquiry scan, discovery, LE scan/adv, and connections. It is **unmerged** and
depends on an usb-core commit not yet in Linus' tree.

- `btusb-remote-wake-v2.patch` (beside this file) is the backported v2
  single-file btusb patch; it applies to 6.18.46 with offsets.
- **Honest expectation:** the author's machine shows the port already
  `power/control=on` and `runtime_status=active` during wedges, i.e. the series'
  premise (port power-off when idle) may never engage here. Treat it as an
  experiment, not a fix. The full rework's remote-wake retry logic is more likely
  to matter when it lands.

## Where to report (in usefulness order)

1. **linux-bluetooth** `linux-bluetooth@vger.kernel.org` (archives:
   lore.kernel.org/linux-bluetooth) — include kernel + firmware version, the
   `0x0019 → 0x00` evidence, and the `unwedge.sh` rescue.
2. **kernel bugzilla** — Product: Drivers, Component: Bluetooth. Cite bg 220306
   (FW reverted after reports) and LP 1835449 (9260 + BT 5.0 HID, fixed by FW).
3. **bluez/bluez issues** — precedent: issue #1263 collected maintainer attention.
4. **Intel community forum** — consumer channel; corroborating 9260 "adapter
   wedges" threads exist. Realistic outcome is a kernel self-heal, not new FW.
5. **Comment on the btusb series itself** — the most direct line to the people
   already working in this exact code.

## Impact on ProBlue testing

ProBlue's acceptance tests (cable pair → unplug → wake → input; sleep/wake soak)
**false-fail** whenever the wedge strikes. When running them: keep `unwedge.sh`
available, record every wedge occurrence, count only wedge-free cycles toward
acceptance, and read `0x0019` before blaming a pairing or key failure.
