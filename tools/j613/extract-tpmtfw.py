#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Produce firmware/tpmtfw-j613.bin (J613 trackpad firmware, "HIDF" container).

m1n1 (hv_dchid.c) uploads this blob to the MTP trackpad interface; without it
only the built-in keyboard works. Apple's firmware cannot live in this repo,
so it is extracted from what is already on the machine:

  * an Asahi-installer stub ESP ("EFI - ..."): vendorfw/firmware.cpio carries
    the converted apple/tpmtfw-j613.bin, asahi/all_firmware.tar.gz the raw
    fud_firmware/j613/Multitouch.im4p;
  * any firmware.cpio, all_firmware.tar.gz, Multitouch.im4p or HIDF .bin.

The im4p -> plist -> HIDF conversion is a port of asahi-installer
asahi_firmware/multitouch.py and img4.py (Copyright The Asahi Linux
Contributors, MIT licence).

usage: extract-tpmtfw.py SOURCE [-o firmware/tpmtfw-j613.bin] [--machine j613]
"""
import argparse
import ctypes
import hashlib
import io
import os
import plistlib
import struct
import sys
import tarfile
import xml.etree.ElementTree as ET

HIDF_MAGIC = b"HIDF"


# ---------------------------------------------------------------- HIDF check

def check_hidf(blob):
    if len(blob) < 32 or blob[:4] != HIDF_MAGIC:
        raise ValueError("not a HIDF container")
    _, version, hdr_len, data_len, iface_off = struct.unpack_from("<4sIIII", blob)
    if version != 1 or hdr_len < 32 or hdr_len + data_len > len(blob) or iface_off >= data_len:
        raise ValueError("malformed HIDF header")
    return blob[:hdr_len + data_len]


# ---------------------------------------------------------------- containers

def cpio_newc_files(data):
    """Yield (name, bytes) from a newc ("070701") cpio archive."""
    off = 0
    while off + 110 <= len(data):
        magic = data[off:off + 6]
        if magic not in (b"070701", b"070702"):
            raise ValueError(f"bad cpio magic at {off:#x}")
        fields = [int(data[off + 6 + 8 * i: off + 14 + 8 * i], 16) for i in range(13)]
        filesize, namesize = fields[6], fields[11]
        name_start = off + 110
        name = data[name_start:name_start + namesize - 1].decode("utf-8", "replace")
        body = (name_start + namesize + 3) & ~3
        end = body + filesize
        if name == "TRAILER!!!":
            return
        yield name, data[body:end]
        off = (end + 3) & ~3


def tar_member_bytes(tar, name, depth=0):
    m = tar.getmember(name)
    if m.issym() and depth < 8:
        target = os.path.normpath(os.path.join(os.path.dirname(name), m.linkname))
        return tar_member_bytes(tar, target, depth + 1)
    f = tar.extractfile(m)
    if f is None:
        raise ValueError(f"{name} is not a regular file")
    return f.read()


# ---------------------------------------------------------------- im4p

def der_read(data, off):
    tag = data[off]
    off += 1
    length = data[off]
    off += 1
    if length & 0x80:
        n = length & 0x7f
        length = int.from_bytes(data[off:off + n], "big")
        off += n
    return tag, data[off:off + length], off + length


def lzfse_decode(cdata, raw_size):
    dest = ctypes.create_string_buffer(raw_size)
    if sys.platform == "darwin":
        lib = ctypes.CDLL("libcompression.dylib")
        n = lib.compression_decode_buffer(dest, raw_size, cdata, len(cdata), None, 0x801)
    else:
        for name in ("liblzfse.so.1", "liblzfse.so"):
            try:
                lib = ctypes.CDLL(name)
                break
            except OSError:
                lib = None
        if lib is None:
            raise RuntimeError("LZFSE-compressed im4p: install liblzfse or run on macOS")
        n = lib.lzfse_decode_buffer(dest, raw_size, cdata, len(cdata), None)
    if n != raw_size:
        raise ValueError("LZFSE decode size mismatch")
    return dest.raw


def img4p_extract(data):
    tag, seq, _ = der_read(data, 0)
    if tag != 0x30:
        raise ValueError("im4p: not a DER sequence")
    items, off = [], 0
    while off < len(seq):
        t, v, off = der_read(seq, off)
        items.append((t, v))
    if len(items) < 4 or items[0] != (0x16, b"IM4P") or items[3][0] != 0x04:
        raise ValueError("im4p: unexpected layout")
    name, payload = items[1][1].decode(), items[3][1]
    if len(items) > 4 and items[4][0] == 0x30:     # compression info
        comp, coff = [], 0
        while coff < len(items[4][1]):
            t, v, coff = der_read(items[4][1], coff)
            comp.append(int.from_bytes(v, "big"))
        if comp[0] != 1:
            raise ValueError(f"im4p: unknown compression {comp[0]}")
        payload = lzfse_decode(payload, comp[1])
    return name, payload


# ---------------------------------------------------------------- plist -> HIDF

def load_plist_xml(d):
    root = ET.fromstring(d.decode("ascii"))

    def unmunge(el, idmap):
        if "ID" in el.attrib:
            idmap[el.attrib["ID"]] = el
        if "IDREF" in el.attrib:
            return idmap[el.attrib["IDREF"]]
        el2 = ET.Element(el.tag)
        el2.text = el.text
        el2.attrib = el.attrib
        for child in el:
            el2.append(unmunge(child, idmap))
        return el2

    pl = ET.Element("plist")
    pl.append(unmunge(root, {}))
    return plistlib.loads(ET.tostring(pl))


def plist_to_bin_trackpad(plist):
    iface_offset = None

    for i in plist:
        if i["Type"] == "Config":
            for j in i["Config"]["Interface Config"]:
                if "HIDRecorder Descriptor" in j.keys():
                    continue
                j["bInterfaceNumber"] = None

    def serialize(o):
        if o is None:
            yield None
        elif o is True:
            yield bytes([0xf5])
        elif isinstance(o, dict):
            if len(o) >= 0x10:
                raise ValueError("unsupported dict size")
            yield bytes([0xa0 + len(o)])
            for k, v in o.items():
                yield from serialize(k)
                yield from serialize(v)
        elif isinstance(o, list):
            if len(o) >= 0x18:
                raise ValueError("unsupported list size")
            yield bytes([0x80 + len(o)])
            for v in o:
                yield from serialize(v)
        elif isinstance(o, str):
            o = o.encode("utf-8") + b"\0"
            if len(o) < 0x18:
                yield bytes([0x60 + len(o)])
            elif len(o) <= 0xff:
                yield bytes([0x78, len(o)])
            else:
                raise ValueError("unsupported string size")
            yield o
        elif isinstance(o, int):
            if o < 0x18:
                yield bytes([o])
            elif o <= 0xff:
                yield bytes([0x18, o])
            elif o <= 0xffff:
                yield bytes([0x19]) + struct.pack(">H", o)
            else:
                yield bytes([0x1a]) + struct.pack(">I", o)
        elif isinstance(o, bytes):
            if len(o) > 0xffff:
                raise ValueError("unsupported data size")
            yield (4, 3)
            yield struct.pack(">BH", 0x59, len(o))
            yield o
        else:
            raise ValueError(f"unsupported type {type(o)}")

    def add_padding(items):
        nonlocal iface_offset
        off = 0
        for b in items:
            if b is None:
                assert iface_offset is None
                iface_offset = off
                b = b"\x00"
            if isinstance(b, tuple):
                align, n = b
                if (off + n) % align:
                    pad = align - ((off + n) % align)
                    off += pad
                    yield b"\xd3" * pad
            else:
                off += len(b)
                yield b

    blob = b"".join(add_padding(serialize(plist)))
    if iface_offset is None:
        raise ValueError("trackpad plist has no interface config")
    hdr = struct.pack("<4sIII", HIDF_MAGIC, 1, 32, len(blob)) + struct.pack("<I12x", iface_offset)
    return hdr + blob


def im4p_to_tpmtfw(im4p):
    name, xml = img4p_extract(im4p)
    if name != "mtfw":
        raise ValueError(f"im4p type {name!r}, expected 'mtfw'")
    plist = load_plist_xml(xml.rstrip(b"\x00"))
    for key, val in plist.items():
        if key.startswith("C1FD"):          # trackpad
            return plist_to_bin_trackpad(val)
    raise ValueError("no trackpad (C1FD*) entry in Multitouch.im4p")


# ---------------------------------------------------------------- search

def from_file(path, machine):
    data = open(path, "rb").read()
    want_bin = f"tpmtfw-{machine}.bin"
    if data[:4] == HIDF_MAGIC:
        return check_hidf(data), path
    if data[:6] in (b"070701", b"070702"):
        for name, body in cpio_newc_files(data):
            if name.endswith(want_bin):
                return check_hidf(body), f"{path}:{name}"
        raise ValueError(f"{path}: no {want_bin} inside")
    if data[:2] == b"\x1f\x8b" or path.endswith((".tar", ".tar.gz", ".tgz")):
        with tarfile.open(fileobj=io.BytesIO(data)) as tar:
            names = tar.getnames()
            for name in names:
                if name.endswith(want_bin):
                    return check_hidf(tar_member_bytes(tar, name)), f"{path}:{name}"
            for name in names:
                parts = name.split("/")
                if len(parts) >= 2 and parts[-1] == "Multitouch.im4p" and parts[-2].startswith(machine):
                    return check_hidf(im4p_to_tpmtfw(tar_member_bytes(tar, name))), f"{path}:{name}"
        raise ValueError(f"{path}: no {want_bin} or {machine}*/Multitouch.im4p inside")
    if data[:1] == b"\x30":
        return check_hidf(im4p_to_tpmtfw(data)), path
    raise ValueError(f"{path}: unrecognised file")


def search(source, machine):
    if os.path.isfile(source):
        return from_file(source, machine)
    candidates = [
        f"vendorfw/firmware.cpio",
        f"asahi/all_firmware.tar.gz",
        f"apple/tpmtfw-{machine}.bin",
        f"firmware.cpio",
        f"all_firmware.tar.gz",
    ]
    errors = []
    for rel in candidates:
        path = os.path.join(source, rel)
        if os.path.isfile(path):
            try:
                return from_file(path, machine)
            except (ValueError, RuntimeError, KeyError) as e:
                errors.append(str(e))
    for root, dirs, files in os.walk(source):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        if "Multitouch.im4p" in files and os.path.basename(root).startswith(machine):
            return from_file(os.path.join(root, "Multitouch.im4p"), machine)
    raise SystemExit("no trackpad firmware found under %s%s" % (
        source, "".join("\n  " + e for e in errors)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("source", help="ESP root, firmware.cpio, all_firmware.tar.gz, Multitouch.im4p or HIDF .bin")
    ap.add_argument("-o", "--output", default=os.path.join("firmware", "tpmtfw-j613.bin"))
    ap.add_argument("--machine", default="j613")
    args = ap.parse_args()

    blob, origin = search(args.source, args.machine)
    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(blob)
    print(f"{args.output}: {len(blob)} bytes, sha256 {hashlib.sha256(blob).hexdigest()}")
    print(f"  from {origin}")


if __name__ == "__main__":
    main()
