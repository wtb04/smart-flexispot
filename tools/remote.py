#!/usr/bin/env python3
"""Looks at the panel over Wi-Fi: its recent log, a picture of its screen, or
what holds its DMA-capable memory.

Usage: tools/remote.py log [lines] [host]
       tools/remote.py screen [out.png] [host]
       tools/remote.py heap [host]
       tools/remote.py power [seconds] [host]   the pack's draw, averaged, on battery

Only a build made with -DREMOTE_ENABLED=1 answers:
    idf.py -B build_remote -DCMAKE_CXX_FLAGS="-DREMOTE_ENABLED=1" build
The key is the one updates take, read from components/ota/include/ota_secrets.h.
host defaults to smart-flexispot, the name the panel gives the router.
"""
import pathlib
import re
import struct
import sys
import urllib.request
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
SECRETS = ROOT / "components/ota/include/ota_secrets.h"


def key():
    found = re.search(r'#define OTA_KEY "([^"]*)"', SECRETS.read_text())
    if not found or not found.group(1):
        sys.exit(f"no OTA_KEY in {SECRETS}")
    return found.group(1)


def fetch(host, path):
    request = urllib.request.Request(f"http://{host}{path}", headers={"X-Update-Key": key()})
    with urllib.request.urlopen(request, timeout=150) as answer:
        return answer.read()


def png(width, height, rgb):
    raw = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def screen(out, host):
    data = fetch(host, "/screen")
    head, pixels = data.split(b"\n", 1)
    _, width, height = head.split()
    width, height = int(width), int(height)
    rgb = bytearray()
    for (value,) in struct.iter_unpack("<H", pixels[:width * height * 2]):
        rgb += bytes(((value >> 11) << 3, ((value >> 5) & 0x3f) << 2, (value & 0x1f) << 3))
    pathlib.Path(out).write_bytes(png(width, height, bytes(rgb)))
    print(f"wrote {out} ({width}x{height})")


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else ""
    if what == "log":
        lines = sys.argv[2] if len(sys.argv) > 2 else "200"
        host = sys.argv[3] if len(sys.argv) > 3 else "smart-flexispot"
        sys.stdout.write(fetch(host, f"/log?lines={lines}").decode(errors="replace"))
    elif what == "heap":
        host = sys.argv[2] if len(sys.argv) > 2 else "smart-flexispot"
        sys.stdout.write(fetch(host, "/heap").decode(errors="replace"))
    elif what == "power":
        seconds = sys.argv[2] if len(sys.argv) > 2 else "20"
        host = sys.argv[3] if len(sys.argv) > 3 else "smart-flexispot"
        sys.stdout.write(fetch(host, f"/power?seconds={seconds}").decode(errors="replace"))
    elif what == "screen":
        out = sys.argv[2] if len(sys.argv) > 2 else "screen.png"
        host = sys.argv[3] if len(sys.argv) > 3 else "smart-flexispot"
        screen(out, host)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
