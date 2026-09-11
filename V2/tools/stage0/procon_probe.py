#!/usr/bin/env python3
"""Stage-0 disposable Pro Controller probe (read-only subcommands).

Usage:
    sudo python3 procon_probe.py <hidraw-dev> <subcmd-hex> [data-hex]

Examples:
    python3 procon_probe.py /dev/hidraw5 02
    python3 procon_probe.py /dev/hidraw5 05
    python3 procon_probe.py /dev/hidraw5 10 0020000018   # SPI x2000, 24 bytes (addr is 32-bit LE + len)
    python3 procon_probe.py /dev/hidraw5 10 0050000010   # SPI x5000, 16 bytes

Framing (docs/KEY_CONTEXT.md 3.1): over BT the host hidraw normally delivers the
0x21 report with no HIDP header, so ack=buf[13], subcmd=buf[14], data=buf[15:].
The script also accepts the header-prefixed variant (ack=buf[14], subcmd=buf[15],
data=buf[16:]). It prints every 0x21 report seen during the window, so nothing
is silently mis-parsed.

Exit status: 0 if at least one subcommand reply was seen, 1 otherwise.
"""

import os
import select
import sys
import time

OUT_LEN = 49  # OUTPUT report 0x01 = 1 report id + 48 payload bytes


def build(subcmd, data, counter=0):
    buf = bytearray(OUT_LEN)
    buf[0] = 0x01
    buf[1] = counter & 0x0F  # packet counter, low nibble
    # [2..9] rumble data, left zero
    buf[10] = subcmd
    buf[11:11 + len(data)] = data
    return bytes(buf)


def frames(fd, seconds):
    end = time.time() + seconds
    while True:
        left = end - time.time()
        if left <= 0:
            return
        ready, _, _ = select.select([fd], [], [], left)
        if not ready:
            continue
        try:
            raw = os.read(fd, 512)
        except BlockingIOError:
            continue
        if raw:
            yield raw


def offsets(raw):
    # Known non-reply input report IDs: 0x21 = subcommand reply,
    # 0x30 = standard full input, 0x3F = simple HID.
    if len(raw) >= 15 and raw[0] == 0x21:
        return 13, 14, 15
    if (len(raw) >= 16 and raw[0] not in (0x21, 0x30, 0x3F)
            and raw[1] == 0x21):
        return 14, 15, 16
    return None


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    dev = sys.argv[1]
    subcmd = int(sys.argv[2], 16)
    data = bytes.fromhex(sys.argv[3]) if len(sys.argv) > 3 else b""

    fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK)
    print(f"# dev={dev} subcmd=0x{subcmd:02x} data={data.hex() or '-'}")

    framed = False
    for raw in frames(fd, 0.5):
        if offsets(raw):
            print(f"# framing: 0x21 at byte {raw.index(0x21)}, report len={len(raw)}")
            framed = True
            break
    if not framed:
        print("# warning: no 0x21 report seen while listening; continuing anyway")

    hits = 0
    for attempt in range(3):
        try:
            os.write(fd, build(subcmd, data, attempt))
        except OSError as err:
            print(f"# write error: {err}")
        for raw in frames(fd, 0.8):
            off = offsets(raw)
            if not off:
                continue
            i, j, k = off
            payload = raw[k:]
            print(
                f"attempt={attempt} ack=0x{raw[i]:02x} "
                f"subcmd=0x{raw[j]:02x} data={payload.hex()}"
            )
            # SPI read (0x10) reply payload = addr[4 LE] + len[1] + data[len]
            if raw[j] == 0x10 and len(payload) >= 5:
                addr = int.from_bytes(payload[:4], "little")
                n = payload[4]
                print(f"    spi @0x{addr:04x} len={n} = {payload[5:5 + n].hex()}")
            hits += 1

    os.close(fd)
    if not hits:
        print("# NO REPLY")
        sys.exit(1)


if __name__ == "__main__":
    main()
