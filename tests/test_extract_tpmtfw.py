"""tools/j613/extract-tpmtfw.py: im4p/cpio/tar/ESP inputs give one HIDF blob."""
import importlib.util
import io
import os
import plistlib
import struct
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("extract_tpmtfw", ROOT / "tools" / "j613" / "extract-tpmtfw.py")
ex = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ex)


def der(tag, body):
    n = len(body)
    if n < 0x80:
        return bytes([tag, n]) + body
    nb = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([tag, 0x80 | len(nb)]) + nb + body


def make_im4p(payload):
    return der(0x30, der(0x16, b"IM4P") + der(0x16, b"mtfw") + der(0x16, b"test") + der(0x04, payload))


def make_cpio(files):
    out = b""
    for i, (name, data) in enumerate(list(files) + [("TRAILER!!!", b"")]):
        nm = name.encode() + b"\0"
        hdr = b"070701" + b"".join(b"%08X" % v for v in (i, 0o100644, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(nm), 0))
        out += hdr + nm
        out += b"\0" * (-len(out) % 4) + data
        out += b"\0" * (-len(out) % 4)
    return out


TRACKPAD = [
    {"Type": "Config", "Config": {"Interface Config": [
        {"bInterfaceNumber": 1, "Name": "multi-touch"},
        {"HIDRecorder Descriptor": b"\x05\x01"},
    ]}},
    {"Type": "Binary", "Address": 0x1000, "Payload": b"\x11" * 300, "Enabled": True},
]


def plist_xml():
    return plistlib.dumps({"C1FD0001": TRACKPAD, "C1FB0002": [{"Type": "Config"}]}, fmt=plistlib.FMT_XML) + b"\0\0"


class ExtractTpmtfwTests(unittest.TestCase):
    def test_im4p_conversion_and_containers_agree(self):
        im4p = make_im4p(plist_xml())
        blob = ex.im4p_to_tpmtfw(im4p)
        magic, ver, hlen, dlen, ioff = struct.unpack_from("<4sIII I", blob)
        self.assertEqual((magic, ver, hlen, dlen), (b"HIDF", 1, 32, len(blob) - 32))
        self.assertEqual(blob[32 + ioff], 0)       # bInterfaceNumber placeholder
        self.assertEqual(ex.check_hidf(blob), blob)

        with tempfile.TemporaryDirectory() as tmp:
            esp = Path(tmp) / "esp"
            (esp / "vendorfw").mkdir(parents=True)
            (esp / "vendorfw" / "firmware.cpio").write_bytes(
                make_cpio([("apple/other.bin", b"x" * 5), ("apple/tpmtfw-j613.bin", blob)]))
            got, origin = ex.search(str(esp), "j613")
            self.assertEqual(got, blob)
            self.assertIn("firmware.cpio", origin)

            # all_firmware.tar.gz with the installer's symlink layout
            tgz = Path(tmp) / "all_firmware.tar.gz"
            with tarfile.open(tgz, "w:gz") as tar:
                def add(name, data=None, link=None):
                    info = tarfile.TarInfo(name)
                    if link:
                        info.type, info.linkname = tarfile.SYMTYPE, link
                        tar.addfile(info)
                    else:
                        info.size = len(data)
                        tar.addfile(info, io.BytesIO(data))
                add("fud_firmware/j613/../../blob/mt.im4p".replace("j613/../../", ""), im4p)
                add("fud_firmware/j613/Multitouch.im4p", link="../blob/mt.im4p")
            got, origin = ex.from_file(str(tgz), "j613")
            self.assertEqual(got, blob)

            raw = Path(tmp) / "Multitouch.im4p"
            raw.write_bytes(im4p)
            self.assertEqual(ex.from_file(str(raw), "j613")[0], blob)

    def test_rejects_garbage(self):
        with self.assertRaises(ValueError):
            ex.check_hidf(b"HIDF" + b"\0" * 10)
        with self.assertRaises(ValueError):
            ex.im4p_to_tpmtfw(make_im4p(plistlib.dumps({"C1FB": []}, fmt=plistlib.FMT_XML)))


if __name__ == "__main__":
    unittest.main()
