# Test 02 — listening (stage 1a)

**Stage:** 1a · **Change:** bluez `adapter.c` connectable keep (R2 kept as
hardening) · **Status:** done

## Hypothesis

With the arm held constant (present, or proven unneeded by test 01), the
controller wake page is not *heard* or not *accepted* by the host when the GUI is
closed / discoverable is off: page scan off (R1), accept list empty after reboot
(R2), or default page-scan duty missing the brief page (R5).

## A/B — arm held constant

| Trial | Host state | Expected if R1 needed |
|---|---|---|
| A | discoverable OFF, GUI closed | btmon: page arrives from controller, host sends nothing / rejects |
| B | same + connectable keep | page accepted → connect |

btmon distinguishes "controller never pages" (arm problem — test 01) from
"controller pages, host ignores" (listening problem — this stage).

## Result (2026-09-10, stock BlueZ 5.86, arm proven unneeded)

Raw traces in `logs/stage0/`.

- **Trial A** (discoverable OFF, host page-scans `0x02` after disconnect;
  `1a-nodisc.btmon.log`): controller `Connect Request` 15:47:47.55 → connect
  15:47:47.67. Page heard and accepted with page-scan only.
- **Trial B** (discoverable ON, `0x03`; `1a-scan.btmon.log`): same — request
  15:31:52.96 → connect 15:31:53.09, link key reused.
- **SYNC-click sleep** (`syncsleep.btmon.log`): request 15:49:23 → connect;
  x5000 after wake `0xFF`.
- **Canonical successful reconnect** (`wake-a.btmon.log`): one short A press with
  no GUI and no discovery session — controller pages, host accepts as central,
  reuses the stored key, encryption on, input streams at ~67 Hz. The host's only
  requirement is page scan `0x02`.
- **Host-initiated wake is impossible** (`testa-burst`, `testb-awake`): a host
  `Create Connection` into an idle controller page-timeouts; 12 s idle produced
  zero automatic retries. Wake is strictly controller-initiated.

### Verdict

- **R5: deleted (falsified)** — default duty heard the page (`0x02`).
- **R1: kept, low priority** — CLI runs page-scan after a disconnect, so host
  listening works; the daily GUI idle state was never proven to differ.
- **R2: kept as hardening** — the `adapter_start` accept-list re-add protects the
  `disconnected_accept_list_entries` page-scan path. It is **not** a reconnect
  fix: a full failing-period trace showed page scan ON and zero `Connect Request`
  events (a filtered page would still emit one), so the failure is below BlueZ.
- The intermittency originally attributed to the controller was later shown to be
  a host-radio firmware wedge — see
  [`../hardware/intel-9260-reconnect-wedge.md`](../hardware/intel-9260-reconnect-wedge.md).
  No BlueZ reconnect-patch direction survives; this stage's conclusion (the host
  can listen after a disconnect when scan is armed) stands.
