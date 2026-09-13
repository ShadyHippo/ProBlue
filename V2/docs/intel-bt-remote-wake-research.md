# Intel BT idle-power / remote-wake research (upstream patches) + reporting channels

Status: RESEARCH NOTES, 2026-09-12. Not a normative conclusion — the series is
UNMERGED upstream and NOT verified to fix the ProBlue wedge. See KEY_CONTEXT §3.7
for the wedge itself.

## The upstream series we found

Title: "Bluetooth: btusb: clear remote wake on idle Intel ACPI paths"
Author: Sean Rhodes <sean@starlabs.systems>, reviewed with Luiz von Dentz (Intel).

- **v1** 2026-03-18 (lore msg id `4863e9725130166fdfcea77298f70500dcc03b02.1773863497.git.sean@starlabs.systems`)
- **v2** 2026-05-13, single-file btusb.c change, +48/-2:
  - gated on `BTUSB_INTEL_COMBINED` devices on ACPI-managed hard-wired USB ports
  - replaces the unconditional `needs_remote_wakeup = 1` in `btusb_open()` with a
    computed value updated on every received event:
    `btusb_needs_runtime_remote_wakeup()` returns true while there are links,
    discovery, LE scan/adv, or **BR/EDR page/inquiry scan**
    (`test_bit(HCI_PSCAN, &hdev->flags)` etc.)
  - intent: let the USB PM core power the port off when the BT device is truly
    idle; the next host command resumes it. Full diff captured from
    spinics.net/lists/kernel/msg6204521.html.
- **Rework** 2026-08-04 (lkml.iu.edu 2608.0/06813): ~900 lines, adds
  `btusb_remote_wakeup_set_active()`, retry work, HCI_NOTIFY_* plumbing in
  hci_sync.c/mgmt.c/hci_sock.c, coredump/diag/DUT handling. Big series; depends
  on usb-core commit `6953217d9f44` (which Paul Menzel reports is ALSO not in
  Linus' master or bluetooth-next as of 2026-08-04).

## Status: NOT MERGED (as of 2026-09-12)

- v2 was revised into the Aug 2026 rework; still under review.
- Paul Menzel, 2026-08-04 (lkml.iu.edu hypermail 2608.0/05785): dependency
  commit not findable in Linus' master or bluetooth-next.
- Consequence: **a plain kernel update will NOT include this fix yet.**

## Backport feasibility for 6.18.46 (checked 2026-09-12)

- Dependency `usb_acpi_power_manageable()` (include/linux/usb.h +
  drivers/usb/core/usb-acpi.c) **IS present in v6.18** (verified against
  raw.githubusercontent.com/torvalds/linux v6.18).
- Local kernel source has `BTUSB_INTEL_COMBINED` + the `8087:0025` entry, so the
  v2 hunk context applies to this card.
- v2 applies as a **single-file btusb.c patch** → fits the existing nixos-config
  `kernelPatches` mechanism (stage-3 hid-nintendo precedent). Low risk: gated to
  Intel-combined ACPI devices, only affects runtime PM.
- **ARTIFACT (2026-09-12)**: `V2/stages/btusb-remote-wake-v2.patch` — extracted
  from lkml.iu.edu hypermail 2605.1/11558 (lossy HTML: blank context lines are
  single-space lines; trailing-footer stripped). Verified: `patch -p1 --dry-run`
  against 6.18.46 `btusb.c` → all 6 hunks succeed (offsets -17..-44 only).
- **Expected effect — be honest**: our wedge measurements show USB
  `power/control=on` and `runtime_status=active` during wedges, i.e. the port is
  NOT being power-managed — the *premise* of this series (let the port power off
  when idle) may not even engage on this machine. Treat as related-experiment,
  NOT a proven fix; the fuller Aug-2026 rework (remote-wake retry, HCI_NOTIFY)
  is the more likely real fix when it lands upstream.

## Relevance honesty check

- The series targets USB-port power-off when the device is idle — a *power
  management* mechanism. Our measured wedge is scan_enable silently reading
  0x00 while the kernel trusts cached flags; at 23:58-2026-09-11 the kernel's
  own `HCI_PSCAN` was already cleared (hciconfig: UP RUNNING, no PSCAN) — a
  state this series explicitly treats as "idle, safe to power off".
- So this is the most relevant upstream work in the area, but it is **NOT
  proven to fix the wedge**. Treat as an experiment: backport v2, observe the
  usual 2-week window with `tools/unwedge.sh` as the canary.
- Related but different (documented elsewhere): bg 220306 (reverted 2025 FW),
  LP 1835449 (9260+BT5.0 HID, fixed by FW), Ubuntu 1909450 (LE scan arbitration,
  not ours), bluez/bluez #1263 (HCI reset -19 death, not ours — but shows the
  same area collecting reports).

## Where to report (who to complain to), in order of usefulness

1. **linux-bluetooth mailing list** — linux-bluetooth@vger.kernel.org
   (subscribe: vger.kernel.org/vger-lists.html#linux-bluetooth; archives:
   lore.kernel.org/linux-bluetooth). Where Intel's BT maintainers (Luiz von
   Dentz) work. Include: kernel version, FW version (ibt-18-16-1.sfi 201-12.24),
   btmon + `hcitool cmd 0x03 0x0019` → 0x00 evidence, unwedge.sh rescue demo.
2. **kernel bugzilla** — bugzilla.kernel.org, Product Drivers, component
   Bluetooth. Reference bg 220306 (FW reverted after report = the loop works)
   and LP 1835449 (9260 + BT 5.0 HID fixed by FW after reports).
3. **bluez/bluez GitHub issues** — github.com/bluez/bluez/issues. Precedent:
   issue #1263 got maintainer attention.
4. **Intel community forum** — community.intel.com, Wireless board (consumer
   channel; Windows-oriented). Corroborating 9260 threads: "Intel AC 9260
   Bluetooth fails after restart" (BTHUSB Code 43) — same card family, similar
   "adapter wedges" story on Windows. Note: this card is 2018-era and its FW
   line looks frozen (last non-reverted blob is 201-12.24); a kernel-side fix is
   the more realistic outcome than a new FW build.
5. **Intel's Linux maintainers via the series itself** — the remote-wake series
   author is actively contributing; commenting on it with our measured evidence
   (scan_enable 0x00 white-wedged) is the most direct channel to the people who
   already touched this exact code area.

Should we report? Yes — concrete, reproducible, better-documented than most.
Expectation management: 9260 is frozen-era hardware; outcome likely = kernel
self-heal or acknowledgment, possibly nothing.