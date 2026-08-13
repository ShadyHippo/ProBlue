#!/usr/bin/env bash
# get_pristine_hid_nintendo.sh [outdir] — fetch + VERIFY the exact
# hid-nintendo.c/hid-ids.h the running Ubuntu/Debian kernel was built from.
# Exits non-zero with a message on ANY failure (never prints "OK" unverified).
set -euo pipefail

KVER="$(uname -r)"
KVERNO="${KVER%%-*}"                     # 6.8.0  (strip -ABI-flavor)
OUT="${1:-docs}"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/problue-pristine"
mkdir -p "$OUT" "$CACHE"

# --- 0. the oracle: locate the stock module ---------------------------------
STOCK=""
for p in /lib/modules/"$KVER"/kernel/drivers/hid/hid-nintendo.ko* \
         /usr/lib/modules/"$KVER"/kernel/drivers/hid/hid-nintendo.ko*; do
  [ -e "$p" ] && STOCK="$p"
done
[ -n "$STOCK" ] || { echo "ERROR: no stock hid-nintendo module for $KVER" >&2; exit 1; }

# --- 1. exact source version from the installed image package ----------------
PKG="linux-image-$KVER"
DEBV="$(dpkg-query -W -f='${Version}' "$PKG" 2>/dev/null || true)"
if [ -z "$DEBV" ]; then
  ABI="${KVER#*-}"; ABI="${ABI%%-*}"
  DEBV="$KVERNO-$ABI.$ABI"
  echo "WARN: $PKG not installed; guessing $DEBV (the oracle will catch a wrong guess)" >&2
fi

# --- 2. download the source package (cached) ----------------------------------
SRCPKG="linux-source-$KVERNO"
DEB="$CACHE/${SRCPKG}_${DEBV}_all.deb"
if [ ! -s "$DEB" ]; then
  URL="http://archive.ubuntu.com/ubuntu/pool/main/l/linux/${SRCPKG}_${DEBV}_all.deb"
  echo "==> downloading $URL"
  curl -fS -o "$DEB.part" "$URL" || { echo "ERROR: download failed (try a mirror or apt-get source)" >&2; exit 1; }
  mv "$DEB.part" "$DEB"
else
  echo "==> cache hit: $DEB"
fi

# --- 3. extract the two driver files -------------------------------------------
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
dpkg-deb -x "$DEB" "$TMP/x"
TARBALL="$(find "$TMP/x/usr/src" -name "linux-source-$KVERNO.tar.bz2" | head -1)"
[ -n "$TARBALL" ] || { echo "ERROR: no linux-source tarball in $DEB" >&2; exit 1; }
tar -xjf "$TARBALL" -C "$TMP" --strip-components=1 \
  "linux-source-$KVERNO/drivers/hid/hid-nintendo.c" \
  "linux-source-$KVERNO/drivers/hid/hid-ids.h"
for f in "$TMP/drivers/hid/hid-nintendo.c" "$TMP/drivers/hid/hid-ids.h"; do
  t="$OUT/$(basename "$f")"
  [ -e "$t" ] && chmod u+w "$t"   # canonical files are 0444 — allow overwrite
  install -m 0444 "$f" "$t"
done

# --- 4. build the candidate + oracle compare ------------------------------------
[ -d "/lib/modules/$KVER/build" ] || { echo "ERROR: install linux-headers-$KVER first" >&2; exit 1; }
B="$CACHE/build-$KVERNO-$DEBV"
mkdir -p "$B"
cp "$OUT/hid-nintendo.c" "$OUT/hid-ids.h" "$B/"
printf 'obj-m += hid-nintendo.o\nKDIR ?= /lib/modules/$(shell uname -r)/build\nall:\n\tmake -C $(KDIR) M=$(CURDIR) modules\nclean:\n\tmake -C $(KDIR) M=$(CURDIR) clean\n' > "$B/Makefile"
make -C "$B" >/dev/null 2>&1 || { echo "ERROR: build failed (see $B)" >&2; exit 1; }

STOCK_KO="$TMP/stock.ko"
case "$STOCK" in
  *.zst) zstd -d -c "$STOCK" > "$STOCK_KO" ;;
  *.xz)  xz -d -c "$STOCK" > "$STOCK_KO" ;;
  *.gz)  gzip -d -c "$STOCK" > "$STOCK_KO" ;;
  *)     cp "$STOCK" "$STOCK_KO" ;;
esac
SV_STOCK="$(modinfo "$STOCK_KO"  | awk '/^srcversion:/{print $2}')"
SV_BUILD="$(modinfo "$B/hid-nintendo.ko" | awk '/^srcversion:/{print $2}')"
[ -n "$SV_STOCK" ] && [ "$SV_STOCK" = "$SV_BUILD" ] || {
  echo "FAIL: srcversion mismatch (stock=$SV_STOCK candidate=$SV_BUILD)" >&2
  echo "      candidate is NOT the source of $KVER — wrong version or distro patchset" >&2
  exit 1
}

echo "OK: verified $KVER source (srcversion $SV_BUILD == stock $STOCK)"
echo "    -> $OUT/hid-nintendo.c (canonical pristine, 0444)"
echo "    -> $OUT/hid-ids.h"
echo "(the old staging dir 'pristine-hid-nintendo/' was retired 2026-08-10 —"
echo " docs/ is the canonical pristine; delete any stale copy if present)"
