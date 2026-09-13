# The Pro Controller reconnect wedge — full investigation record

**Status: fingerprint RETRACTED (probe bug 2026-09-11); re-investigating — host-RX-deaf vs controller-stuck; rescue unverified.**

Controller `20:0B:CF:34:F1:BD` (Nintendo Pro Controller), host `50:28:4A:0D:54:7A`
(Intel Wireless-AC 9260, USB `8087:0025`, node `1-4`), NixOS kernel 6.18.46,
BlueZ 5.86. Investigation 2026-09-10→11.

> **CORRECTION (2026-09-11, supersedes fingerprint sections below)**: the
> "Read Scan Enable truncated plen 4" fingerprint everywhere in this doc is
> INVALID — `hcitool cmd 0x03 0x001a` is WRITE Scan Enable (OCF 0x001A,
> needs a 1-byte param); READ is `0x03 0x0019`. The 23:58 session also showed
> a real page-scan write AND a full rfkill reinit BOTH failing to reconnect
> the controller. Authoritative record: KEY_CONTEXT §3.7; probe tool fixed.

---

## 1. The symptom (user-visible reality)

- Controller works *perfectly* right after any full Bluetooth-stack reset.
- Intermittently enters a "wedged" state: it strobes its LEDs (i.e. it IS
  paging the host), but the host never hears it. Pressing buttons does
  nothing. Appears random (not time/count based).
- Wedged state persists indefinitely until the Bluetooth stack is hard-reset.
- `bluetoothctl power off && power on`, `btmgmt power off/on`, and an HCI
  `Reset` command do **NOT** clear it. Only a **rfkill hard cycle** (block +
  unblock) clears it — which is exactly what BlueJay's UI "Toggle Bluetooth"
  does under the hood.
- Since this reset "unwedges" it, the wedge is a HOST-radio state, not a
  controller defect. The controller is fine; the 9260 radio silently stops
  page-scanning.

## 2. Ideas tried and falsified (do NOT retry)

| Idea | Result | Status |
|---|---|---|
| ERTM on/off (`disable_ertm`) | no change | falsified — targets a different failure |
| The BT-side `0x08 00` arm (V1 theory) | x5000 already 0xFF, wake works unarmed | falsified (ledger R3 deleted) |
| Host page-scan duty (R5) | default duty hears pages | falsified |
| Discoverable/connectable keep-alive (R1) | host page-scans, still deaf | falsified |
| Accept-list re-add (R2) | accept list is populated & page still unheard; connect works w/o accept-list entry after power cycle | **demoted to hardening** (not the mechanism) |
| USB autosuspend (`runtime_status=active`, `control=on`) | radio listed `active`, still wedged | **falsified as the live-wedge cause** (still a plausible *trigger* study gotcha) |
| `bluetoothctl power off && power on` | no clear | falsified as rescue (mgmt power is deferred/raced) |
| `hcitool cmd 0x03 0x0003` HCI Reset | no clear | falsified as rescue |
| `btmgmt power off/on` | no clear | falsified as rescue |
| **`rfkill block; unblock`** | **clears it every time** | **THE WORKING RESCUE** |

## 3. The measured wedge fingerprint (probe of 2026-09-11 15:51, wedged)

`tools/wedge-probe.sh` captured the wedged state. Findings, layer by layer:

1. **USB radio: awake & healthy** — `1-4` control=on, runtime_status=active,
   runtime_usage=1. Not suspended, not re-enumerated. (Autosuspend did NOT
   cause this wedge instance.)
2. **Kernel believes: page scan ON** — `hciconfig` shows `UP RUNNING PSCAN`;
   `btmgmt info` shows powered + bonded; controller on kernel device list;
   link key present (`3f 80 86 07 d5 73 87 dc 16 4f bc 0b 70 a0 90 f9`).
3. **Controller answers HCI commands** — `Read Local Version` and `Read BD
   ADDR` reply instantly with correct values. Not crashed, on the bus.
4. **THE KEY MEASUREMENT — the controller's OWN scan state says broken:**
   `Read Scan Enable` (HCI `0x03 0x001a`) replies with **the `scan_enable`
   parameter byte MISSING** (event `0x0e` with `plen 4`: `02 1A 0C 00` — only
   ncmd + opcode echo + status, no `scan_enable` value). Meanwhile `Read
   Page Scan Activity` (0x1b) returns a full valid reply (`00 08` interval,
   `12 00` window) — the controller *knows* how to scan; it just is not
   enabled, and cannot even report its enable state. Compare healthy state:
   this command returns `plen 5` with `0x02` (Page Scan).
5. **RX is completely dead** — 20s passive btmon btsnoop (278 bytes)
   contains **zero events**: no LE advertisements, no Connect Requests,
   nothing. The radio is receiving nothing at all while the controller
   strobes beside it.

**Conclusion:** the Intel 9260 firmware silently reverted its scan-enable
register to "No Scans" — with no HCI event, no error, no disconnect — and the
host trusts its cached `PSCAN` flag. That is why nothing host-side can see
it, and why only a reset that forces the register back works.

## 4. Why rfkill works and HCI/mgmt resets don't (kernel 6.18.46)

- `mgmt.c` (`set_powered` / `__mgmt_power_off`): power-off is **deferred to
  `hdev->power_off` work** ("power off is deferred to hdev->power_off work
  which does call hci_dev_do_close" — mgmt.c comment). Back-to-back
  `power off && power on` races: the second command can see the state as
  already matching ("already powered" no-op) or hit `BUSY`
  (`pending_find(MGMT_OP_SET_POWERED)` / `HCI_POWERING_DOWN`), so the HCI
  reset never fires. This is why `bluetoothctl power off && power on` did
  nothing (and reproduced the same `Busy (0x0a)` seen in the 2026-09-11
  btmon: "Set Powered: Disabled → Status: Busy").
- `HCI_OP_RESET` alone (`hcitool cmd 0x03 0x0003`): resets registers but
  does **not** re-run the section 5 init sequence that rewrites scan
  enable; and the kernel's `hci_update_scan_sync()` **skips re-asserting
  page scan when `HCI_PSCAN` already matches intent**
  (`hci_sync.c`: `if (test_bit(HCI_PSCAN, ...) == !!(scan & SCAN_PAGE)) return 0;`)
  — so nothing re-writes the corrupt register.
- **`rfkill` (`hci_rfkill_set_block` in hci_core.c)**: on block, calls
  `hci_dev_do_poweroff(hdev)` **synchronously** (constantly inlined close,
  not deferred); on unblock, `hdev` reopens through the **full section-5
  init** (local version read, `HCI_OP_RESET` at `hci_init0_sync`, scan
  enable rewrite). That full re-init is the thing that repairs the register.
- BlueJay's "Toggle Bluetooth" is `BluezQt::Manager::setBluetoothBlocked()`
  = rfkill block/unblock (`src/manager.cpp:59`), then `Powered` toggles —
  confirmed by source (vendored: `external_docs/bluez-qt`, HEAD
  `e8e49aba360d8229275b3501c149d6acfe1a7316` v6.12.0; bluejay HEAD
  `a556b2fae50cdb94021b38a669c74f550b69333a` v1.0.3).

## 5. The working rescue (documented, CLI, hotkey-able)

```sh
sudo rfkill block bluetooth; sleep 1; sudo rfkill unblock bluetooth
```

(mirrors BlueJay exactly; `sleep 1` gives the close time to land). Then the
controller reconnects on its own (it is still paging). This is the
**recommended documented rescue** — verified repeatedly, including a
live-wedged test on 2026-09-11 where rungs 1 (HCI Reset) and 2 (mgmt power)
failed and rfkill succeeded.

### The raw rung-ladder evidence (terminal log, 2026-09-11, still wedged)

```
sudo hcitool cmd 0x03 0x0003                     # rung 1: HCI Reset
> HCI Event: 0x0e plen 4  => 02 03 0C 00          (Reset acked)
sudo hcitool cmd 0x03 0x001a                     # Read Scan Enable
> HCI Event: 0x0e plen 4  => 02 1A 0C 00          still truncated, no fix

sudo btmgmt power off; sleep 1; sudo btmgmt power on   # rung 2: mgmt power
hci0 Set Powered complete ... powered connectable ...   (power cycled OK)
sudo hcitool cmd 0x03 0x001a
> HCI Event: 0x0e plen 4  => 02 1A 0C 00          still truncated, no fix

sudo rfkill block bluetooth; sleep 1; sudo rfkill unblock bluetooth  # rung 3
Device open failed: No such device               # radio actually came DOWN
FIXED                                            # reconnected, no button press
```

Linux has native Bluetooth keybinding plumbing on NixOS: the user binds
Waybar/Sway to run this. A desktop-level convenience (e.g. a script keybind
or a systemd-timer watchdog) is a later nicety; the CLI command itself is the
unit of truth.

## 6. The remaining question: WHAT TRIGGERS the wedge?

The wedge = scan-enable register reverts silently. Root cause of the TRIGGER
is still open. Strong candidates, in order:

1. **Intel 9260 PTT/coex switch.** The earlier full btmon shows `Intel PTT
   Switch Notification` (vendor event 0x26) — Intel coex state machine.
   Test: when wedged, `ip link set wlan0 down` and press the button. If it
   connects → coex is the trigger. (Did not get to run before unwedge; still
   queued.)
2. **Firmware bug.** 9260 FW `201-12.24`; linux-firmware `ibt-19-0-4`. Check
   for updated FW.
3. **Kernel flag vs register drift** after some disconnect/connect pattern
   (e.g. controller's own disconnect at ~96s, or page-timeout during an
   unanswered page).
4. **USB autosuspend AS A TRIGGER** (not the live-wedge cause): a
   suspend/resume cycle that leaves the firmware in the busted state. The
   runtime-time counters in the probe: suspended 12.7M ms vs active 5.8M ms
   (i.e. it autosuspends a LOT). This is the most testable: keep
   `power/control=on` permanently and see if wedges still appear.

Next probe to run **while wedged** (never fixed, measure first):

```sh
sudo tools/wedge-probe.sh          # → logs/wedge-probe-<ts>.txt + .btsnoop
```

then the three live discriminators from the probe's section 8 (WiFi off,
`btmgmt connectable on`, rfkill) — rfkill is the control that must always
work. Every wedge should produce a probe file before rescue.

## 7. Probe tool

`V2/tools/wedge-probe.sh` — sudo, run while wedged. Writes everything under
`V2/logs/` (gitignored). Note: it captures to a file; check the file for
full output.

## 8. What is NOT the wedge (project scope note)

- Not a BlueZ userspace bug — no BlueZ patch can fix a radio that went deaf
  while reporting healthy. The stage-3/5 cable-pairing port (kernel
  passivity + wired 3-step) is a *separate feature*, unaffected, still
  build-verified. The reconnect-keeper R-rows (R1/R2/R3/R5) are all either
  falsified or demoted to hardening; no reconnect patch direction survives.
- The `power/control=on` udev rule was added to `~/nixos-config
  /modules/hardware-generic.nix` then removed (did not prevent the wedge),
  per user.