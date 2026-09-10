#!/usr/bin/env bash
# get_pristine_bluez.sh [outdir] — fetch + verify pristine BlueZ 5.84 from
# kernel.org and apply the ProBlue patch, so you can build the patched
# daemon for YOUR system (README §7.2). Exits non-zero with a message on
# ANY failure (never prints "OK" unverified).
set -euo pipefail

VER="5.84"
OUT="${1:-.}"
URL="https://www.kernel.org/pub/linux/bluetooth/bluez-${VER}.tar.xz"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/problue-pristine"
TARBALL="$CACHE/bluez-${VER}.tar.xz"
PATCH="$(dirname "$0")/../patches/bluez-${VER}-procon.patch"

mkdir -p "$OUT" "$CACHE"

# --- 1. download the released tarball (cached) --------------------------------
if [ ! -s "$TARBALL" ]; then
  echo "==> downloading $URL"
  curl -fSL -o "$TARBALL.part" "$URL"
  mv "$TARBALL.part" "$TARBALL"
else
  echo "==> cache hit: $TARBALL"
fi

# --- 2. extract -----------------------------------------------------------------
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
tar -xJf "$TARBALL" -C "$TMP"
SRC="$TMP/bluez-$VER"
[ -d "$SRC" ] || { echo "ERROR: expected dir bluez-$VER not found in tarball" >&2; exit 1; }

# --- 3. verify it is really the released 5.84 (not a renamed other version) ----
if ! grep -q "AC_INIT(bluez, $VER)" "$SRC/configure.ac"; then
  echo "ERROR: configure.ac does not say AC_INIT(bluez, $VER) — wrong tarball?" >&2
  exit 1
fi

# --- 4. apply the ProBlue patch (dry-run first) ---------------------------------
if ! patch -p1 --dry-run -d "$SRC" < "$PATCH" >/dev/null 2>&1; then
  echo "ERROR: ProBlue patch does not apply cleanly to bluez-$VER" >&2
  echo "      (wrong base? re-check the tarball — see README §7.2 B3)" >&2
  exit 1
fi
patch -p1 -d "$SRC" < "$PATCH" >/dev/null

# --- 5. done ---------------------------------------------------------------------
cp -r "$SRC" "$OUT/bluez-$VER"
echo "OK: pristine bluez-$VER verified (AC_INIT) + ProBlue patch applied"
echo "    -> $OUT/bluez-$VER"
echo "Build with:  cd $OUT/bluez-$VER && autoreconf -fi && ./configure --enable-sixaxis && make"
