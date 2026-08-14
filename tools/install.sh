#!/usr/bin/env bash
# install.sh [builddir] — one-shot PERMANENT install of the patched BlueZ daemon
# (README §1, steps 4-5): `make install` + systemd drop-in + page-scan config.
#
#   bash tools/install.sh ~/ProBlue-build
#
# Idempotent: safe to re-run. Run as your normal user (sudo is invoked as
# needed). Undoes the "manual run" hack (a masked bluetooth.service) if present.
# Exits non-zero with a message on ANY failure.
set -euo pipefail

BUILD="${1:-$HOME/ProBlue-build}"

# Accept the top-level outdir (what tools/get_pristine_bluez.sh creates) OR
# the tree dir directly: resolve outdir/bluez-5.84 -> bluez-5.84
if [ ! -f "$BUILD/src/bluetoothd" ] && [ -f "$BUILD/bluez-5.84/src/bluetoothd" ]; then
  BUILD="$BUILD/bluez-5.84"
fi

die() { echo "ERROR: $*" >&2; exit 1; }
say() { echo "==> $*"; }

# --- 0. sanity ---------------------------------------------------------------
[ -x "$(command -v sudo)" ] || die "sudo not found"
sudo -v || die "sudo required"
[ -f "$BUILD/src/bluetoothd" ] || die "no built daemon at $BUILD/src/bluetoothd — build first (README §1 steps 1-3: tools/get_pristine_bluez.sh + configure + make)"

# --- 1. make install (idempotent) ---------------------------------------------
say "make install from $BUILD"
sudo make -C "$BUILD" install >/dev/null

# --- 2. locate the installed daemon --------------------------------------------
# Default autoconf libexecdir is ${prefix}/libexec, so with a default
# configure the daemon lands in /usr/local/libexec/bluetooth/bluetoothd,
# NOT /usr/local/sbin. Check the usual candidates, prefer /usr/local.
DAEMON=""
for cand in \
    /usr/local/libexec/bluetooth/bluetoothd \
    /usr/local/sbin/bluetoothd \
    /usr/libexec/bluetooth/bluetoothd \
    /usr/sbin/bluetoothd; do
  if [ -x "$cand" ]; then DAEMON="$cand"; break; fi
done
[ -n "$DAEMON" ] || die "installed bluetoothd not found in any expected path"
# NOTE: do not use `grep -q` here — it exits on first match, SIGPIPEs
# `strings`, and with `pipefail` the pipeline reports 141 even on a match.
# `grep -c` reads all input, so it can't race.
strings "$DAEMON" | grep -ci procon >/dev/null 2>&1 || die "$DAEMON has no procon code — is that really the patched build?"
say "daemon: $DAEMON"

# --- 3. undo the manual-run hack (mask) ----------------------------------------
if [ -L /etc/systemd/system/bluetooth.service ]; then
  if [ "$(readlink /etc/systemd/system/bluetooth.service)" = "/dev/null" ]; then
    say "removing bluetooth.service mask (manual-run hack)"
    sudo rm /etc/systemd/system/bluetooth.service
  else
    die "/etc/systemd/system/bluetooth.service exists but is not a /dev/null mask — remove it manually first"
  fi
fi

# --- 4. systemd drop-in (distro updates can't clobber ExecStart) ----------------
DROPDIR=/etc/systemd/system/bluetooth.service.d
sudo mkdir -p "$DROPDIR"
say "writing $DROPDIR/ProBlue.conf"
sudo tee "$DROPDIR/ProBlue.conf" >/dev/null <<EOF
[Service]
ExecStart=
ExecStart=$DAEMON
EOF

# --- 5. page scan: the keys must live in the [BR] section -----------------------
# They ship commented out under [BR] (Ubuntu and BlueZ both). Appending them
# at the end of the file puts them in the last section, where BlueZ ignores
# them — so uncomment in place, and insert under [BR] only if truly absent.
MAIN=/etc/bluetooth/main.conf
if [ ! -f "$MAIN" ]; then
  say "$MAIN missing — creating one with the [BR] block"
  sudo install -d /etc/bluetooth
  sudo tee "$MAIN" >/dev/null <<'EOF'
[General]

[BR]
PageScanType=0x01
PageScanInterval=0x0012
PageScanWindow=0x0012
EOF
else
  say "setting page scan under [BR] in $MAIN"
  sudo sed -i \
    -e 's/^#PageScanType=$/PageScanType=0x01/' \
    -e 's/^#PageScanInterval=$/PageScanInterval=0x0012/' \
    -e 's/^#PageScanWindow=$/PageScanWindow=0x0012/' \
    "$MAIN"
  MISSING=""
  sudo grep -q '^PageScanType='     "$MAIN" || MISSING="$MISSING PageScanType=0x01"
  sudo grep -q '^PageScanInterval=' "$MAIN" || MISSING="$MISSING PageScanInterval=0x0012"
  sudo grep -q '^PageScanWindow='   "$MAIN" || MISSING="$MISSING PageScanWindow=0x0012"
  if [ -n "$MISSING" ]; then
    MISSING="${MISSING# }"
    if sudo grep -q '^\[BR\]$' "$MAIN"; then
      TMP="$(mktemp)"
      sudo awk -v extra="$MISSING" \
        '{ print } /^\[BR\]$/ { n = split(extra, a); for (i = 1; i <= n; i++) print a[i] }' \
        "$MAIN" > "$TMP"
      sudo install -m 0644 "$TMP" "$MAIN"
      rm -f "$TMP"
    else
      printf '[BR]\n%s\n' "$MISSING" | tr ' ' '\n' | sudo tee -a "$MAIN" >/dev/null
    fi
  fi
fi

# --- 6. hand the daemon over to systemd -----------------------------------------
for pid in $(pgrep -x bluetoothd 2>/dev/null || true); do
  if ! grep -q 'bluetooth.service' "/proc/$pid/cgroup" 2>/dev/null; then
    say "stopping manual bluetoothd (pid $pid) running outside systemd"
    sudo kill "$pid" 2>/dev/null || true
  fi
done

sudo systemctl daemon-reload
sudo systemctl enable bluetooth >/dev/null 2>&1 || true
sudo systemctl restart bluetooth

# --- 7. verify -------------------------------------------------------------------
if systemctl is-active --quiet bluetooth; then
  say "bluetooth.service: $(systemctl is-active bluetooth)"
else
  journalctl -u bluetooth -n 30 --no-pager 2>/dev/null | tail -30 >&2 || true
  die "bluetooth.service failed to start — journal above"
fi
systemctl status bluetooth --no-pager | head -6
"$DAEMON" --version
if sudo grep -q '^PageScanType=0x01$' "$MAIN" 2>/dev/null && \
   sudo grep -q '^PageScanInterval=0x0012$' "$MAIN" 2>/dev/null && \
   sudo grep -q '^PageScanWindow=0x0012$' "$MAIN" 2>/dev/null; then
  say "page scan config confirmed in $MAIN"
else
  echo "WARN: page scan keys not confirmed — check the [BR] section of $MAIN" >&2
fi

say "done. Test: plug the controller into USB and watch: journalctl -u bluetooth -f"
say "      (expect procon: usb 0x80 0x02 ack ... 3-step ... done ... link key stored)"
