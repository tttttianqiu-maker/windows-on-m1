#!/usr/bin/env python3
"""Convert the assisted-mode virtual framebuffer (fb.raw + fb-info.json, B8G8R8X8)
into a half-size PNG using only the standard library.

usage: fb2png.py <fb.raw> <fb-info.json> <out.png>
"""
import json
import struct
import sys
import zlib


def main() -> int:
    raw_path, info_path, out_path = sys.argv[1:4]
    info = json.load(open(info_path))
    w, h, stride = info["width"], info["height"], info["stride"]
    data = open(raw_path, "rb").read()
    if len(data) < stride * h:
        print(f"fb2png: short framebuffer ({len(data)} < {stride * h})", file=sys.stderr)
        return 1
    ow, oh = w // 2, h // 2
    rows = bytearray()
    for y in range(oh):
        line = memoryview(data)[2 * y * stride : 2 * y * stride + w * 4]
        b, g, r = line[0::8], line[1::8], line[2::8]
        px = bytearray(ow * 3)
        px[0::3], px[1::3], px[2::3] = r[:ow], g[:ow], b[:ow]
        rows += b"\x00" + px

    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", ow, oh, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b"")
    open(out_path, "wb").write(png)
    return 0


if __name__ == "__main__":
    sys.exit(main())
