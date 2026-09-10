# Test 05 — edge cases (stage 7)

**Stage:** 7 · **Policy:** no work here until a *reproduced* failure with
journal/btmon evidence · **Status:** not started

## Candidate items (from PLAN.md)

- Charging while playing: BT drop + no USB input under passivity (product
  decision).
- Docked LED/battery absence under passivity.
- Multi-controller / accept-list across bluetoothd restarts.
- Session re-init after a quiet gap (protocol detail from stage 5 logs).
- x2024 capability byte behavior (0x08 vs 0x68) — probe before touching the
  alias question.
- Link-supervision zombie window (~20 s).
- Any disconnect/redock anomaly seen during stages 3–6.

## Method per item

1. Write the one-line hypothesis + the minimal reproduction steps.
2. Reproduce on the current checkpoint; capture btmon/journal.
3. Only then consider a change, with a ledger row in PLAN.md.

## Result

_(table of items: reproduced / not / fixed-by / intentionally ignored)_