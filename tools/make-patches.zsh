#!/usr/bin/env zsh
# make-patches.zsh — regenerate src/patches/ from the reviewed files in src/.
#
# One direction only: `src/` holds the full patched files (source of truth),
# `src/patches/*.patch` is generated here. Never hand-edit a generated patch.
#
# Inputs:  build/pristine/  (materialized by tools/fetch-pristine.zsh)
#          src/kernel/, src/bluez/  (the reviewed full files)
# Outputs: src/patches/kernel-hid-nintendo-usb-passive-6.18.46.patch
#          src/patches/bluez-procon-cable-pairing-5.86.patch
#
# Usage: tools/make-patches.zsh

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PRIS="$ROOT/build/pristine"
SRC="$ROOT/src"
OUT="$SRC/patches"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

[[ -d "$PRIS/bluez" && -f "$PRIS/kernel/drivers/hid/hid-nintendo.c" ]] ||
  { print -u2 "make-patches: run tools/fetch-pristine.zsh first"; exit 1; }

mkdir -p "$OUT"

# --- BlueZ -------------------------------------------------------------------
BLUEZ_FILES=(
  Makefile.plugins
  plugins/sixaxis.c
  profiles/input/procon.c
  profiles/input/procon.h
  profiles/input/server.c
  profiles/input/sixaxis.h
  src/adapter.c
  src/adapter.h
)

rm -rf "$WORK/bluez" && mkdir -p "$WORK/bluez/a" "$WORK/bluez/b"
for rel in "${BLUEZ_FILES[@]}"; do
  mkdir -p "$WORK/bluez/a/${rel:h}" "$WORK/bluez/b/${rel:h}"
  [[ -f "$PRIS/bluez/$rel" ]] && cp "$PRIS/bluez/$rel" "$WORK/bluez/a/$rel"
  cp "$SRC/bluez/$rel" "$WORK/bluez/b/$rel"
done
(cd "$WORK/bluez" && diff -ruN a b) > "$OUT/bluez-procon-cable-pairing-5.86.patch" || true

# --- kernel ------------------------------------------------------------------
# Upstream layout (drivers/hid/hid-nintendo.c) so the patch applies with -p1 at
# a kernel source root (e.g. nixos-config kernelPatches).
rm -rf "$WORK/kernel" && mkdir -p "$WORK/kernel/a/drivers/hid" "$WORK/kernel/b/drivers/hid"
cp "$PRIS/kernel/drivers/hid/hid-nintendo.c" "$WORK/kernel/a/drivers/hid/hid-nintendo.c"
cp "$SRC/kernel/drivers/hid/hid-nintendo.c"  "$WORK/kernel/b/drivers/hid/hid-nintendo.c"
(cd "$WORK/kernel" && diff -ruN a b) > "$OUT/kernel-hid-nintendo-usb-passive-6.18.46.patch" || true

print "wrote:"
print "  $OUT/bluez-procon-cable-pairing-5.86.patch"
print "  $OUT/kernel-hid-nintendo-usb-passive-6.18.46.patch"
