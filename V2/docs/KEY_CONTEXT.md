# V2 KEY_CONTEXT — read this file first

This is the canonical briefing for any agent (human or otherwise) working in
V2. It consolidates the findings that were gathered by reasoning about the
V1 patch code and by reading the vendored reference material in
`external_docs/` (gitignored, local-only). Everything here is either grounded
in those docs or **verified against this machine on 2026-09-10**. Distrust any
status claim not in this file; re-verify protocol facts from `external_docs/`
before touching that code.

---

## 1. What this project is

A clean-room, step-by-step rebuild of "Nintendo Switch Pro Controller parity on
Linux", which V1 (the `V1/` directory) attempted as a two-patch bundle:

- **BlueZ patch**: cable ("wired") pairing over the controller's USB hidraw +
  a Bluetooth-side reconnect keeper.
- **Kernel patch** (`hid-nintendo`): makes the driver *passive* on USB —
  binds, creates hidraw, sends nothing, creates no input device — so
  bluetoothd owns the hidraw exclusively and the wire never carries input.

V2's process rule: **no code enters unless a test proves it is needed**
(necessity ledger in `PLAN.md`). V1 is a frozen reference; reuse is an
explicit copy logged in `SALVAGE.md`. NixOS machine plumbing lives in
`~/nixos-config`, **not** in this repo.

---

## 2. The machine (facts, verified)

- OS: NixOS, **kernel 6.18.46**, nixpkgs pinned `nixos-26.05`
  (flake.lock rev `f4f698677b11` → **BlueZ 5.86**).
- `hardware.bluetooth.powerOnBoot = false` **today** — must be `true` for any
  wake/reconnect test. (Config repo change, not V2 change.)
- `boot.kernelModules` already contains `hid_nintendo`.
- The host has **no autoreconf/autotools** on PATH. Build BlueZ inside:
  ```bash
  nix-shell -p autoconf automake libtool gettext glib dbus systemd.dev pkg-config readline \
    --run 'cd <bluez-src> && autoreconf -fi && ./configure --enable-sixaxis \
      --disable-obex --disable-mesh --disable-midi --disable-nfc --disable-health \
      --disable-test --disable-btpclient --disable-manpages && make -j"$(nproc)"'
  ```
  This exact recipe **fully built bluetoothd 5.84** from V1's patched tree on
  2026-09-10 (zero warnings). The sixaxis plugin is enabled by default in
  nixpkgs' bluez build (`lib.enableFeature true "sixaxis"`).
- NixOS kernel changes = full kernel rebuild. Minimize kernel iterations;
  they are the scarcest resource in this project.
- `external_docs/` at repo root holds the vendored references (gitignored).

---

## 3. Protocol digest (grounded in `external_docs/Nintendo_Switch_Reverse_Engineering/` + `external_docs/nxbt/docs/Example Pairing Session.md`)

### 3.1 Report framing (byte-exact, matches V1's code)

OUTPUT report `0x01` (host→controller; every subcommand; same over USB hidraw
and BT PSM19):
```
[0]=0x01 report id, [1]=packet counter (low nibble, 0x0-0xF),
[2..9]=rumble data (zero), [10]=subcmd id, [11..]=subcmd data
```
INPUT report `0x21` (controller→host; subcommand reply):
```
[13]=ack (MSB=1 ack, MSB=0 NACK; 0x80 = simple ack, 0x00 = NACK),
[14]=subcmd id echo, [15..49]=reply data
```
**The BT/USB offset shift**: over the Bluetooth interrupt channel the HIDP
header precedes the report, and bluetoothd's `hidp_recv_intr_data()` hands the
hook the report *without* the header, so the ack byte is at `data[14]`/subcmd
at `data[15]` (with `data[1]==0x21`). Over the USB hidraw there is no header:
ack at `buf[13]`/subcmd at `buf[14]` (with `buf[0]==0x21`). Both are correct in
their own context; do not "fix" one to match the other.

### 3.2 USB session commands (2-byte writes, answered by `0x81 <cmd>`)

| cmd | meaning |
|---|---|
| `80 02` | UART handshake with the Broadcom BT chip. **Only once per session.** |
| `80 03` | switch to 3 Mbit. Must follow `80 02`; **another `80 02` is then required** for the baud switch to take effect. |
| `80 04` | **pin to USB**: force USB-only, no timeout. "Required for the Pro Controller to not time out and revert to Bluetooth." The stock kernel driver sends this — it is what kills wake-while-docked. |
| `80 05` | restore the timeout so the controller reverts to BT (V1 marked this "[DEAD]" as an approach — correct, but for the wrong reason: **default behavior without `80 04` is already time-out-and-revert**, so `80 05` is unnecessary). |
| `80 01` | connection-status query → `81 01 00 <type> <MAC>` (type 2=right Joy-Con, 3=Pro Controller) |

Consequence: **full passivity (send nothing) gives BT-revert-while-docked for
free** — the kernel change is "don't write", not "add 80 05".

### 3.3 Subcommands used by this project

| subcmd | purpose | notes |
|---|---|---|
| `0x01` | manual pairing (3-step, below) | also usable over BT; used to *rewrite* pairing info |
| `0x02` | device info | reply data: `[0..1]` fw, `[2]` controller type (1=L, 2=R, **3=Pro**), `[4..9]` MAC **big-endian** (raw reply: `buf[19..24]`) |
| `0x03` | set input report mode | `0x30` = standard full (60 Hz JC / **120 Hz Pro**), `0x3F` = simple HID |
| `0x05` | get page list state | replies `0x01` if a host list with BD addresses/link keys is in memory — **the "am I already paired?" check** |
| `0x06` | set HCI state | `x00` sleep/page-scan, `x01`/`x04` reboot-and-reconnect (page mode), `x02` reboot to pair mode (discoverable) |
| `0x07` | reset pairing info | zeroes the x2000 section |
| `0x08` | set shipment low power state | see 3.5 |
| `0x10` | SPI flash read | LE address + length (max 0x1D); reply `90 xx` ack + data |
| `0x11`/`0x12` | SPI write / sector erase | write-protection status byte available |

### 3.4 SPI flash layout (the pairs that matter)

**x2000 pairing record** (repeated sections; current section has magic):
```
x2000       magic: 0x95=used, 0x00=unused (if 0x00, next section is current)
x2001       size (0x22)
x2004-09    host BT address, BIG-ENDIAN
x200A-19    128-bit LTK, LITTLE-ENDIAN
x2024       host capability: Switch=0x68, PC=0x08
x2026..     section 2 (repeat layout; 0xFF if unused)
```
**x5000 shipment**: `0x01` = shipment on, `0xFF` = normal. Factory = `0x01`.
**x6000**: factory calibration/config (not pairing).
Reset pairing: subcmd `0x07`. **Read-only access exists: subtcmds `0x05` and
`0x10` → V1's "no read stored central" claim was false.**

### 3.5 Power / wake model (the arm, now mechanical)

- `0x08 0x01` writes `0x01` @ x5000: disables **Triggered Broadcom Fast
  Connect**, LPM→HID OFF, button wake disabled (only the "easy pressable"
  buttons; **a long press can still wake a paired controller** — test-design
  trap).
- `0x08 0x00` resets x5000 to `0xFF`: enables Triggered Broadcom Fast Connect
  (scan-on-button-press), LPM→SLEEP. **"Switch always sends x08 00 after every
  connection"** — the nxbt capture shows it sent **over the BT interrupt
  channel** shortly after connect. So the arm is genuine, documented, and a
  Bluetooth-side behavior; clearing it once persists in flash.
- Factory controllers ship with shipment ON → a never-armed controller may
  never wake from normal button presses.

Verified on this unit 2026-09-10 (see `docs/results/2026-09-10-stage0/SUMMARY.md`):

- x5000 read `0xFF` (normal, fast-connect on) from the first read and stays
  `0xFF` through fresh OTA pairs, host disconnects, and SYNC-click sleeps.
  The old claim "clearing it once persists in flash" therefore **holds** here.
- `0x08 00` on this unit is a no-op (`0xFF`→`0xFF`); the arm is **not** the
  reconnect mechanism (ledger R3 deleted).
- A normal button press makes the controller emit a real HCI
  `Connect Request` (0x04). The host reconnects **iff it is page-scanning**;
  BlueZ enables page scan (0x02) on its own after a disconnect, and
  discoverable (0x03) is not required.
- The 14:01 "controller emitted no page while host listening" episode was
  **re-read on 2026-09-11 as host-side deafness**, not controller-side: a
  deaf radio (page-scan accept list dropped / scan state lost / autosuspend)
  produces zero Connect Requests at HCI regardless of what the controller
  does — see below.
- **REVISED 2026-09-11 — the wedge is host-side listening loss, not the
  controller — and is now PATCHED.** Evidence: (a) every Linux host reset
  (reboot or BlueJay "Toggle Bluetooth" adapter power cycle) reliably
  restores reconnects; failing-period onset is entirely random; (b) while
  failing, the controller LEDs strobe (it IS paging) and resetting the host
  mid-strobe reconnects **with no new button press** — the page was being
  emitted all along; (c) btmon sees NOTHING during failure — no Connect
  Request reaches HCI. **A full btmon trace (2026-09-11) narrows this to the
  controller/radio layer, NOT BlueZ:** during the 22 s dead window the host
  had page scan ON (`Write Scan Enable: Page Scan 0x02`, 46.3→68.5 s), kept
  alive by accept-list entries (kernel `hci_update_scan_sync()`:
  `HCI_CONNECTABLE || disconnected_accept_list_entries`). An accept-list miss
  would still emit a Connect Request + negative reply in btmon — none
  appears. After the UI bounce (`Reset` + re-init) the next Connect Request
  lands **43 ms** later with no button press, proving the page was in flight
  all along and the radio stopped delivering it (the same trace shows
  `Intel PTT Switch Notification` → WiFi/BT coexistence). Only a controller
  reset clears it. R2 (accept-list re-add on `adapter_start`, ported
  2026-09-11) is therefore **hardening, not the fix** — it covers the
  `disconnected_accept_list_entries` page-scan path, which this failure does
  not exercise. See PLAN R2 and testplan 02 REVISION 2.

### 3.6 Pairing model (corrects V1's premise)

- First pairing is ordinary OTA: sync button → pair mode (subcmd 0x06 x02) →
  standard SSP → sockets. **The controller does SSP fine; V1's "it does not
  SSP" claim is inaccurate.** It just doesn't re-SSP on reconnect — it uses the
  stored key, which is why the host must hold the key/accept-list state.
- The `0x01` 3-step is for **rewriting** pairing info (and is how docking
  works); it is parity/convenience, not a necessity:
  1. `0x01 0x01 [+6B host MAC LE]` → reply echoes type; joy-con MAC in reply
  2. `0x01 0x02` → "Acquire the XORed LTK hash": key bytes each XORed with
     `0xAA`, flash (LE) order. BR/EDR link key must be byte-reversed vs the
     flash order. **SETTLED (sources: RE subcommands notes + SPI flash
     notes): step 2 returns the *stored* key — the active x2000 section's
     current LTK ("Acquire", not "generate"; SPI notes: "it keeps the active
     section and the current LTK used with Switch can be acquired"). No
     fresh-key generation. Therefore P8 (re-pair every dock) is deletable
     and GET_LTK == the OTA-stored key on this unit (`25 31 28 85 …`) is the
     stage-5 falsification for P4–P8.**
  3. `0x01 0x03` → commit
- **Re-pair-on-every-dock (V1 P8) is deletable**: `0x05` + SPI read of x2000
  give a read-then-decide path (magic 0x95 + stored MAC == ours → skip).

### 3.7 THE RECONNECT WEDGE — measured 2026-09-11 (READ THIS, don't relitigate)

The controller "won't connect" problem is a **host-radio wedge**, not a
BlueZ bug and not a controller defect. Full record:
`docs/wedge-investigation.md` (normative for this topic).

- **Fingerprint (RETRACTED 2026-09-11)**: the "Read Scan Enable truncated
  plen 4" fingerprint was an INSTRUMENTATION BUG — `hcitool cmd 0x03 0x001a`
  is WRITE Scan Enable (OCF 0x001A, needs a param byte); READ is `0x03
  0x0019`. The `plen 4` `02 1A 0C 00` reply is the constant answer to a
  malformed zero-length Write — present in EVERY state, proves nothing.
  "Register silently reverts" theory FALSIFIED: a real `Write Scan Enable:
  Page Scan 0x02` (btmgmt connectable) was ACKED Success and did NOT
  un-wedge; kernel PSCAN flag is inconsistent across wedges (23:58 capture:
  `UP RUNNING` without PSCAN).
- **What still stands (2026-09-11 23:58 session)**: while wedged, the
  controller never connects through ANY action — WiFi radio off (no), real
  page-scan write (no), rfkill block/unblock full reinit (NO — rescue failed
  this session), and 20 s passive btmon shows zero HCI events. Open
  question: is the CONTROLLER paging at all (host RX deaf vs controller-side
  stuck)? `hcitool cmd 0x03 0x0019` + `hcitool lescan` are the truthful
  discriminators. Upstream activity + reporting channels:
  docs/intel-bt-remote-wake-research.md (UNMERGED as of 2026-09-12; v2 btusb
  patch backports cleanly to 6.18 — kernelPatches candidate, test only).
- **DEPLOYED 2026-09-12 (works)**: ProBlue live in ~/nixos-config
  (machine/problue.nix; patches/). Boot-freeze bisection removed the btusb v2
  patch + coex/autosuspend params (they hard-froze this Dell ~3s into boot).
  Stage-5 live fixes: device now marked Paired+Bonded after cable-pairing;
  procon_acquire_ltk's extra session re-init removed (V1-identical sequence).
  The wedge itself is unchanged and possible — `tools/unwedge.sh` is the live
  rescue. Full record: memory wedge-final-handoff.md + docs/KEY_CONTEXT.
- **CONFIRMED 2026-09-12 (the corrected mechanism)**: while wedged, the
  truthful read `hcitool cmd 0x03 0x0019` returns `02 19 0C 00 00` —
  scan_enable GENUINELY 0x00 (No Scans), well-formed reply. The register
  really does drop to scan-off; the kernel trusts its cached
  `HCI_CONNECTABLE`/`HCI_PSCAN` and never re-asserts. **Gentle rescue
  (no rfkill): `sudo hcitool cmd 0x03 0x001a 0x02` → re-read 0x0019 → 0x02 →
  press controller button.** Explains 23:58 "rfkill failed": rfkill rewrote
  scan-enable but the firmware dropped it again and the controller had
  stopped paging — needed re-arm + presses, not a stack reset. TRIGGER (what
  drops the register) still unknown — PTT/coex prime suspect.
- **Rescue status (CORRECTED 2026-09-11)**: rfkill block/unblock reopened
  the device through full init (incl. rewriting scan enable) — and on
  2026-09-11 23:58 the controller STILL did not connect afterwards. Old
  "rfkill always fixes it" is downgraded to "fixed on 2026-09-11 15:5x";
  re-verify, and consider the controller side. Untested rescue rung: full
  USB re-enumeration (`modprobe -r btusb && modprobe btusb`).
  `bluetoothctl power off/on` (mgmt, deferred → BUSY/no-op) and a bare HCI
  `Reset` remain non-rescues as measured.
- **Falsified as the wedge**: ERTM, arm `0x08 00`, accept list (R2 = keep as
  hardening only), USB autosuspend as live cause (`control=on` did not
  prevent), discoverable/connectable keep-alives. Do not retry these as the
  wedge fix.
- **Trigger unknown** — candidates: Intel PTT/coex switch (vendor evt 0x26
  seen), disconnect/page-timeout patterns, autosuspend as trigger (not live
  cause). Probe while wedged with `tools/wedge-probe.sh` before any rescue.
- **Host firmware is CURRENT (measured 2026-09-11)**: `ibt-18-16-1.sfi`
  build `201-12.24` (ww 12, 2024) == the linux-firmware-20260810 blob; the
  driver's `Firmware already loaded` line = version-match skip
  (`btintel_firmware_version()` in btintel.c). The 2025 blob update was
  bugged and reverted upstream (linux-bluetooth bug 220306) → **firmware
  upgrade path closed**, no newer non-buggy blob exists. Fix must come from
  trigger-hunt prevention or kernel-level self-heal (Intel vendor Reset on
  truncated `Read Scan Enable`).

---

## 4. V1 autopsy — what to trust, what not

Trust (verified against pristine sources or by build):
- `V1/src/bluez/profiles/input/procon.c` framing/offsets are byte-exact per §3.
- The BT-side arm design (delayed T+1s, retry ×3, 500 ms ack cap) is
  Switch-parity per the nxbt capture.
- `btd_adapter_store_link_key` + full `MGMT_OP_LOAD_LINK_KEYS` reload is the
  only runtime key-add path in 5.84-era BlueZ.
- The passive-USB kernel shape (early-return in `joycon_init`, no USB writes,
  probe exit before input/leds/battery, resume NULL guard).
- BlueZ patch applies 100% clean to pristine 5.84; full build verified.

Do NOT trust / known-wrong:
- **`V1/README.md` (and its PLAN status prose)**: AI-generated against the
  Ubuntu build, with issues — **disavowed 2026-09-11; not authoritative at
  all**. Only `V1/docs/results/` evidence + the RE docs count.
- **`V1/patches/bluez-5.84-procon.patch` is a stale snapshot.** The
  `V1/src/bluez/` tree is the superset (e.g. it contains
  `btd_adapter_has_cable_pairing_devices` at `adapter.c:416`, which the
  `.patch` omits). `device_is_cable_pairing()` / `device_set_cable_pairing()`
  are stock 5.84 `src/device.c` functions — no port needed. Kernel:
  `V1/src/kernel` is empty; the only kernel reference is
  `V1/patches/hid-nintendo-6.8.0-137-generic-usb-passive.patch`.
- "No read stored central" (false: 0x05/0x10).
- "Controller doesn't SSP" (false, §3.6).
- "Re-pair every dock required" (false, §3.4/3.6).
- Hardcoded SDP record necessity — the record was captured *over real SDP*;
  test real SDP browse on 5.86 before keeping it.
- The kernel patch as shipped targets Ubuntu 6.8.0-137; on 6.18.46 **hunk 3
  (joycon_init) fails** because upstream made 3-Mbit baudrate failure
  non-fatal. Fixed 6.8 patch uses a 13-line early-return; port = replace the
  stock USB/charggrip init block with that early return. (6.18 also renamed
  `joycon_hid_resume` → `nintendo_hid_resume`; other anchors unchanged.)

---

## 5. Verification workflow (do not skip)

- **`src/` is the source of truth** (full files); patches in `stages/` are
  generated. One direction. Never hand-edit a patch (V1 bit twice on hunk
  counts).
- **V2 port base (2026-09-11):** copy from the `V1/src/bluez/` tree, NOT from
  `V1/patches/*.patch` (stale snapshots — the tree is the superset; see §4).
  Kernel: `V1/patches/hid-nintendo-6.8.0-137-generic-usb-passive.patch` is
  the only kernel reference (`V1/src/kernel` empty).
- BlueZ: pristine via the kernel.org tarball; patch-test with
  `patch -p1 --dry-run`; build with the nix-shell recipe in §2.
- Kernel: fetch pristine `hid-nintendo.c` for the exact target version;
  patch-test; verify no residual `joycon_send_usb` refs; grep
  `joycon_is_passive` uses (expect 3: is_passive def, joycon_init,
  probe, resume).
- Round-trip test after regenerating any patch: fresh pristine → apply →
  `diff -q` every touched file vs the edited tree.

---

## 6. Review concerns carried forward (do not regress)

- `procon.c` does **synchronous blocking select/read in bluetoothd's mainloop**
  (up to ~2 s per read; several per dock). Acceptable for now, but it blocks
  the whole daemon during dock operations — the first thing to revisit if any
  other device misbehaves during pairing.
- The `property_set_mode()` DISCOVERABLE change affects **all** cable-paired
  devices (PS included), not just Pro Controllers — a global-behavior change.
- `jc_type_is_chrggrip` becomes unused after the kernel hunk-3 port (static
  inline, harmless).
- Session re-init: `0x80 02` is "once per session"; V1 re-inits the session
  before the 3-step (agent gap). Verify empirically that a second session
  init after a quiet gap is acked (harness-in-daemon logging at stage 5).
- Report cadence health: deltas ≈7–17 ms = clean; 30–120 ms = sniff-ish/link
  quality. Measure with numbers, not feel (V1's original sin).
- The Bluetooth name/"Nintendo" hostname workaround has **no support in any
  primary RE source**; it's community-only. Capability byte x2024
  (Switch=0x68 vs PC=0x08) is a candidate alternative mechanism. If a
  power/sniff difference ever reproduces, probe x2024 before renaming anything.