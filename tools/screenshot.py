#!/usr/bin/env python3
"""Asks the panel for a picture of its screen and writes a PNG.

Usage: tools/screenshot.py [port] [out.png]

Build with -DSHOT_ENABLED=1 first, and set SHOT_PAGE in ui.cpp to the page
wanted; it is off otherwise because taking one holds the LVGL lock for several
seconds. This restarts the panel, which is what asks for the picture.

The panel writes numbered base64 RGB565 between BEGIN and END on the console.
PNG is assembled here from zlib and struct, so nothing has to be installed
beyond pyserial. A line or two out of five thousand is usually lost -- the
console writes without blocking and discards what will not fit -- and those
rows come out black rather than shifting everything after them.
"""
import base64
import re
import struct
import sys
import time
import zlib

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem202401"
OUT = sys.argv[2] if len(sys.argv) > 2 else "screen.png"


def png(width, height, rgb):
    raw = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def main():
    port = serial.Serial(PORT, 115200, timeout=1)
    # Restart it: the panel offers a picture a little while after it comes up,
    # which is the whole trigger. Cheaper than a button nobody would press.
    port.setDTR(False)
    port.setRTS(True)
    time.sleep(0.1)
    port.setRTS(False)
    time.sleep(0.1)
    port.reset_input_buffer()

    size = None
    body = {}
    deadline = time.time() + 240
    pending = b""

    while time.time() < deadline:
        pending += port.read(4096)
        while b"\n" in pending:
            line, pending = pending.split(b"\n", 1)
            text = line.decode("utf-8", "replace").strip()

            found = re.search(r"shot: BEGIN (\d+) (\d+)", text)
            if found:
                size = (int(found.group(1)), int(found.group(2)))
                body = {}
                continue
            if "shot: END" in text and size:
                width, height = size
                want = (width * height * 2 + 89) // 90
                lost = [i for i in range(want) if i not in body]
                if lost:
                    print(f"{len(lost)} of {want} lines never arrived "
                          f"(first at {lost[0]}); those rows are black",
                          file=sys.stderr)
                data = base64.b64decode(
                    "".join(body.get(i, "A" * 120) for i in range(want)))
                rgb = bytearray()
                # The panel packs each pixel as a native uint16, low byte first.
                for i in range(0, min(len(data), width * height * 2), 2):
                    pixel = data[i] | (data[i + 1] << 8)
                    rgb += bytes(((pixel >> 8) & 0xf8, (pixel >> 3) & 0xfc, (pixel << 3) & 0xf8))
                with open(OUT, "wb") as out:
                    out.write(png(width, height, bytes(rgb)))
                print(f"wrote {OUT} ({width}x{height})")
                return 0
            # Only whole, untouched lines. Under load the console drops a
            # newline now and then and two lines arrive as one; taken as data
            # that corrupts everything after it, quietly.
            whole = re.fullmatch(r"SHOT (\d+) ([A-Za-z0-9+/=]{1,120})", text)
            if whole:
                body[int(whole.group(1))] = whole.group(2)

    print("no picture came back", file=sys.stderr)
    return 1


sys.exit(main())
