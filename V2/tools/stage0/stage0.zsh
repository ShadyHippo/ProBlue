#!/usr/bin/env zsh
#
# ProBlue V2 - Stage 0 (stock forensics) helper.
#
# Philosophy: automate the tedious and the steady-state; leave anything that
# must be timed by hand (pairing, connecting, pressing a button) to the human.
# The step-by-step procedure lives in README.md; this file only provides:
#
#   preflight   snapshot machine / adapter / controller / hidraw
#   probes      read-only controller subcommands over hidraw (sudo)
#   cadence     capture delta= / timeout-waiting over N seconds, summarize
#   alias       cadence A/B with the adapter alias set to "Nintendo Switch"
#   monitor     start a timed btmon capture in the background for a wake test
#   stop        stop a monitor started by this script
#   collect     gather every artifact under the run dir into one pasteable file
#
# Every bluetoothctl call is bounded (one-shot with /dev/null stdin, or a piped
# session under `timeout`).  No phase edits the controller: probes are read-only.
#
emulate -L zsh
setopt err_return pipe_fail no_nomatch

SCRIPT_NAME=${0:A}
SCRIPT_DIR=${SCRIPT_NAME:h}
REPO_DIR=${SCRIPT_DIR:h:h}
PROCON=${PROCON:-20:0B:CF:34:F1:BD}
OUT=${OUT:-$REPO_DIR/logs}
RAW=${RAW:-$OUT/stage0}
PROBE=$SCRIPT_DIR/procon_probe.py
BCTL_T=${BCTL_T:-25}

log()  { print -r -- "$*" }
warn() { print -r -- "!! $*" >&2 }
die()  { print -r -- "error: $*" >&2; exit 1 }

need() { command -v "$1" >/dev/null 2>&1 || die "missing command: $1" }
ensure_dirs() { mkdir -p "$RAW" }

# One-shot bluetoothctl that is guaranteed to exit.
bctl() { timeout "$BCTL_T" bluetoothctl "$@" </dev/null 2>&1 || true }

find_hidraw() {
  local h props
  for h in /dev/hidraw*(N); do
    props=$(udevadm info -q property -n "$h" 2>/dev/null) || true
    if print -r -- "$props" | grep -qiE 'HID_NAME=.*(Pro Controller|Nintendo)|ID_VENDOR_ID=057e|DEVPATH=.*:057E:'; then
      print -r -- "$h"
      return 0
    fi
  done
  return 1
}

hidraw_map() {
  local h
  for h in /dev/hidraw*(N); do
    print -r -- "== $h"
    udevadm info -q property -n "$h" 2>/dev/null \
      | grep -E '^(ID_VENDOR_ID|ID_MODEL_ID|HID_NAME|ID_SERIAL_SHORT|DEVPATH)=' || true
  done
}

summarize_cadence() {
  local f=$1
  if [[ ! -f $f ]]; then
    warn "no file $f"
    return 1
  fi
  awk '/delta=/ {
      for (i = 1; i <= NF; i++) if ($i ~ /^delta=/) {
        split($i, a, "="); v = a[2] + 0
        if (n == 0 || v < min) min = v
        if (v > max) max = v
        s += v; n++
      }
    }
    END { printf "delta: n=%d min=%d avg=%.1f max=%d\n", n, min, s / n, max }' "$f"
  print -r -- "timeout-waiting: $(grep -c 'timeout waiting' "$f" || true)"
  grep -m3 'imu_report' "$f" || true
}

# ---------------------------------------------------------------------------

phase_preflight() {
  need bluetoothctl
  need udevadm
  ensure_dirs
  local f=$RAW/00-preflight.log
  {
    print -r -- "date:         $(date -Is)"
    print -r -- "kernel:       $(uname -r)"
    print -r -- "btmon:        $(btmon --version 2>&1 | head -1)"
    print -r -- "bluetoothctl: $(bluetoothctl --version 2>&1)"
    print -r -- "powerOnBoot:  $(grep -rh powerOnBoot "$HOME/nixos-config" 2>/dev/null | tr -d ' ')"
    print -r -- "adapter:      $(bctl show | awk '/^Controller/{print $2; exit}')"
    print -r -- ""
    print -r -- "===== bluetoothctl show ====="
    bctl show
    print -r -- ""
    print -r -- "===== bluetoothctl info $PROCON ====="
    bctl info "$PROCON"
    print -r -- ""
    print -r -- "===== hidraw nodes ====="
    ls -l /dev/hidraw* 2>&1
    print -r -- ""
    print -r -- "===== hidraw map ====="
    hidraw_map
    print -r -- ""
    print -r -- "===== bluetooth UIs / scanners running ====="
    ps -ef | grep -iE 'blueman|gnome-bluetooth|bluetoothctl' | grep -v grep || true
  } > "$f" 2>&1
  cat "$f"
  log ""
  log "wrote $f"
}

probe_one() {
  local dev=$1 sub=$2 data=$3 file=$4
  local out
  # the uhid hidraw node is usually granted to the session user via uaccess
  local runner=(python3)
  if [[ ! -w $dev ]]; then runner=(sudo python3); fi
  if [[ -n $data ]]; then
    out=$($runner "$PROBE" "$dev" "$sub" "$data" 2>&1) || true
  else
    out=$($runner "$PROBE" "$dev" "$sub" 2>&1) || true
  fi
  print -r -- "$out" | tee "$RAW/$file"
  print -r -- "$out" | grep -q '^attempt='
}

phase_probes() {
  ensure_dirs
  local d=${1:-}
  if [[ -z $d ]]; then
    d=$(find_hidraw) || die "no controller hidraw found - connect the controller first"
  fi
  log "using hidraw: $d"
  local ok=0
  if probe_one "$d" 02 ""          probe-02-devinfo.txt;  then ok=1; fi
  if probe_one "$d" 05 ""          probe-05-pagelist.txt; then ok=1; fi
  if probe_one "$d" 10 0020000018  probe-10-2000.txt;     then ok=1; fi
  if probe_one "$d" 10 1820000018  probe-10-2018.txt;     then ok=1; fi
  if probe_one "$d" 10 0050000010  probe-10-5000.txt;     then ok=1; fi
  if (( ok )); then
    log ""
    log "replies received - see README.md for how to read them."
  else
    warn "no subcommand replies. The hid_nintendo driver may be swallowing raw output reports."
    warn "Fallback:  sudo modprobe -r hid_nintendo"
    warn "           stage0.zsh probes"
    warn "           sudo modprobe hid_nintendo"
    return 1
  fi
}

phase_cadence() {
  ensure_dirs
  local secs=${1:-300} tag=${2:-stock}
  sudo sh -c 'echo "module hid_nintendo +p" > /sys/kernel/debug/dynamic_debug/control' 2>/dev/null \
    || sudo sh -c 'echo "file hid-nintendo.c +p" > /sys/kernel/debug/dynamic_debug/control' 2>/dev/null \
    || warn "could not enable dynamic debug; delta= lines may be absent"
  sudo dmesg -C 2>/dev/null || true
  log ">>> controller must be connected and idle. Capturing ${secs}s of kernel log..."
  timeout "$secs" sudo dmesg -w > "$RAW/cadence-$tag.log" 2>&1 || true
  log "--- cadence summary ($tag) ---"
  summarize_cadence "$RAW/cadence-$tag.log" || true
}

phase_alias() {
  local secs=${1:-300} orig
  orig=$(bctl show | awk '/Alias:/{sub(/^.*Alias: */, ""); print; exit}')
  if [[ -z $orig ]]; then orig=hippo-xps; fi
  ensure_dirs
  log "adapter alias: '$orig' -> 'Nintendo Switch'"
  bctl system-alias 'Nintendo Switch'
  sleep 2
  phase_cadence "$secs" nintendo
  bctl system-alias "$orig"
  log "alias restored to '$orig'"
}

phase_monitor() {
  ensure_dirs
  local secs=${1:-120} tag=${2:-monitor}
  if ! sudo -n true 2>/dev/null; then
    log "sudo credential not cached; asking now (foreground, so it can prompt)"
    sudo -v || die "sudo failed"
  fi
  sudo timeout "$secs" btmon -t > "$RAW/$tag.btmon.log" 2>&1 &
  local pid=$!
  sleep 2
  if ! kill -0 "$pid" 2>/dev/null; then
    warn "btmon did not start; output was:"
    cat "$RAW/$tag.btmon.log" >&2 || true
    return 1
  fi
  log "btmon -> $RAW/$tag.btmon.log  (auto-stops in ${secs}s)"
  log "stop early: $SCRIPT_NAME stop"
}

phase_stop() {
  sudo pkill -f 'btmon -t' 2>/dev/null || true
  log "btmon stopped"
}

phase_map() {
  ensure_dirs
  hidraw_map | tee "$RAW/hidraw-map.txt"
}

phase_collect() {
  ensure_dirs
  local f=$RAW/collect.txt
  {
    for item in "$RAW"/00-preflight.log \
                "$RAW"/pair-scan.txt "$RAW"/pair-pair.txt "$RAW"/pairing.btmon.log \
                "$RAW"/*-info-connected.txt "$RAW"/hidraw-map.txt; do
      [[ -f $item ]] || continue
      if [[ $item == *.btmon.log ]]; then continue; fi
      print -r -- ""
      print -r -- "########## $item"
      cat "$item"
    done
    for item in "$RAW"/probe-*.txt; do
      [[ -f $item ]] || continue
      print -r -- ""
      print -r -- "########## $item"
      cat "$item"
    done
    print -r -- ""
    print -r -- "########## cadence summaries"
    for item in "$RAW"/cadence-*.log; do
      [[ -f $item ]] || continue
      print -r -- ""
      print -r -- "-- $item"
      summarize_cadence "$item" || true
    done
    print -r -- ""
    print -r -- "########## pairing evidence (grep of pairing.btmon.log)"
    grep -E 'IO Capability|User Confirmation|Simple Pairing Complete|Link Key|Authentication|Pairing' \
      "$RAW"/pairing.btmon.log 2>/dev/null | head -30 || true
    for item in "$RAW"/wake-*.info.txt "$RAW"/smoke-connect.txt; do
      [[ -f $item ]] || continue
      print -r -- ""
      print -r -- "########## $item"
      cat "$item"
    done
  } > "$f" 2>&1
  cat "$f"
  log ""
  log "paste bundle written to: $f"
}

usage() {
  cat <<'EOF'
ProBlue V2 - stage-0 helper (automates capture + summarise only)

  stage0.zsh preflight            snapshot machine / adapter / controller / hidraw
  stage0.zsh probes [hidraw]      read-only subcmd probes (0x02, 0x05, 0x10)
  stage0.zsh cadence [secs] [tag] capture delta= / timeout-waiting, then summarise
  stage0.zsh alias [secs]         cadence A/B with adapter alias "Nintendo Switch"
  stage0.zsh map                  list hidraw nodes with their udev properties
  stage0.zsh monitor [secs] [tag] start a timed btmon capture (for wake tests)
  stage0.zsh stop                 stop that btmon capture
  stage0.zsh collect              gather all artifacts into one pasteable file

Pairing, connecting and button-press wake tests are done by hand - see README.md.

Run dir: V2/logs/stage0/  (V2/logs/ is gitignored)
EOF
}

main() {
  local phase=${1:-help}
  shift 2>/dev/null || true
  case $phase in
    preflight) phase_preflight "$@" ;;
    probes)    phase_probes "$@" ;;
    cadence)   phase_cadence "$@" ;;
    alias)     phase_alias "$@" ;;
    map)       phase_map "$@" ;;
    monitor)   phase_monitor "$@" ;;
    stop)      phase_stop "$@" ;;
    collect)   phase_collect "$@" ;;
    help|--help|-h) usage ;;
    *) usage; die "unknown phase: $phase" ;;
  esac
}

main "$@"
