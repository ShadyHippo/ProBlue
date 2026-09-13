# `src/` — the reviewed artifact

This directory is what a reviewer needs: the **full patched files** and the
**generated patches**, for the exact upstream versions pinned in
[`MANIFEST.md`](MANIFEST.md).

```
src/
├── patches/
│   ├── kernel-hid-nintendo-usb-passive-6.18.46.patch
│   └── bluez-procon-cable-pairing-5.86.patch
├── kernel/
│   └── drivers/hid/hid-nintendo.c   # full patched driver
├── bluez/
│   ├── Makefile.plugins
│   ├── plugins/sixaxis.c
│   ├── profiles/input/procon.c # new
│   ├── profiles/input/procon.h # new
│   ├── profiles/input/server.c
│   ├── profiles/input/sixaxis.h
│   └── src/adapter.{c,h}
└── MANIFEST.md                 # pins: nixpkgs rev, versions, store paths, sha256
```

The file paths under `src/bluez/` and `src/kernel/` mirror the upstream trees, so
`diff -u <pristine>/<path> src/<path>` is always a valid comparison.

## Source of truth

- `src/kernel/` and `src/bluez/` are the **source of truth** (full files).
- `src/patches/` is **generated** from them by `tools/make-patches.zsh` and must
  never be hand-edited.
- Pristine upstream is not committed. `tools/fetch-pristine.zsh` materializes it
  under `build/pristine/` (gitignored) for diffing and rebuilding.

## Verification (do this after touching `src/`)

```sh
V2/tools/fetch-pristine.zsh          # if build/pristine/ is absent
V2/tools/make-patches.zsh            # regenerate patches from src/

# round-trip: pristine + patch must reproduce src/ byte-for-byte
cp -a build/pristine /tmp/rt && (cd /tmp/rt/bluez   && patch -p1 < .../bluez-procon-cable-pairing-5.86.patch)
(cd /tmp/rt/kernel && patch -p1 < .../kernel-hid-nintendo-usb-passive-6.18.46.patch)
diff -r /tmp/rt/bluez src/bluez && diff /tmp/rt/kernel/drivers/hid/hid-nintendo.c src/kernel/drivers/hid/hid-nintendo.c
```

Expected: the patches apply clean to pristine and the diffed files match `src/`.
