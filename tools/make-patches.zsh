#!/usr/bin/env zsh
# make-patches.zsh — regenerate src/patches/ from the reviewed files in src/.
#
# One direction only: `src/` holds the full patched files (source of truth),
# `src/patches/*.patch` is generated here. Never hand-edit a generated patch.
#
# Each patch is prefixed with a header (description, license, authorship,
# credits). Both `patch -p1` and `git apply` skip leading text, so the header
# does not affect applying the patch.
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

KERNEL_HDR='# ProBlue - hid-nintendo USB passivity (kernel 6.18.46)
#
# On the USB transport only, hid-nintendo binds and exposes hidraw but sends
# nothing: no 0x80 02/03/04 session, no subcommands, no LED/battery init, and no
# /dev/input device. Bluetooth is untouched. This frees the hidraw for
# bluetoothd Pro Controller cable pairing, and because nothing sends 0x80 04
# ("pin to USB") the controller reverts to Bluetooth when unplugged.
# Full rationale: README.md.
#
# Apply: patch -p1, from a kernel source root.
#
# Copyright (C) 2026 Tim Van Dyke <tim.vandyke123@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
# Signed-off-by: Tim Van Dyke <tim.vandyke123@gmail.com>
#
# Credits: the patched file is upstream drivers/hid/hid-nintendo.c (Daniel J.
# Ogorchock and contributors, GPL-2.0-or-later). Protocol background: dekuNukem,
# Nintendo_Switch_Reverse_Engineering.
'

BLUEZ_HDR='# ProBlue - BlueZ Pro Controller cable pairing (BlueZ 5.86)
#
# Adds a Nintendo Pro Controller path to the sixaxis input plugin. On USB
# plug-in it runs the wired UART session over the controller hidraw, reads the
# controller identity and its stored pairing record, reuses that record when it
# is already paired to this host (read-then-decide) or else runs the wired
# 3-step, then stores the link key and marks the device Paired+Bonded so it
# reconnects over Bluetooth after unplugging. Full rationale: README.md.
#
# Apply: patch -p1, from a BlueZ source root.
#
# Copyright (C) 2026 Tim Van Dyke <tim.vandyke123@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
# Signed-off-by: Tim Van Dyke <tim.vandyke123@gmail.com>
#
# Credits: new files profiles/input/procon.{c,h} carry GPL-2.0-or-later SPDX
# headers and the copyright above; modified files keep their upstream BlueZ
# headers. Protocol from dekuNukem Nintendo_Switch_Reverse_Engineering
# (bluetooth_hid_subcommands_notes.md, spi_flash_notes.md, USB-HID-Notes.md) and
# the nxbt "Example Pairing Session" capture (Brikwerk).
'

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
{
  print -r -- "$BLUEZ_HDR"
  (cd "$WORK/bluez" && diff -ruN a b) || true
} > "$OUT/bluez-procon-cable-pairing-5.86.patch"

# --- kernel ------------------------------------------------------------------
# Upstream layout (drivers/hid/hid-nintendo.c) so the patch applies with -p1 at
# a kernel source root (e.g. nixos-config kernelPatches).
rm -rf "$WORK/kernel" && mkdir -p "$WORK/kernel/a/drivers/hid" "$WORK/kernel/b/drivers/hid"
cp "$PRIS/kernel/drivers/hid/hid-nintendo.c" "$WORK/kernel/a/drivers/hid/hid-nintendo.c"
cp "$SRC/kernel/drivers/hid/hid-nintendo.c"  "$WORK/kernel/b/drivers/hid/hid-nintendo.c"
{
  print -r -- "$KERNEL_HDR"
  (cd "$WORK/kernel" && diff -ruN a b) || true
} > "$OUT/kernel-hid-nintendo-usb-passive-6.18.46.patch"

print "wrote:"
print "  $OUT/bluez-procon-cable-pairing-5.86.patch"
print "  $OUT/kernel-hid-nintendo-usb-passive-6.18.46.patch"
