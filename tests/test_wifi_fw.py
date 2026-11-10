# SPDX-License-Identifier: MIT
"""tools/j613/wifi_fw.py: Asahi firmware naming/selection and brcmfmac NVRAM conversion."""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools", "j613"))
import wifi_fw  # noqa: E402


def token(words):
    return struct.pack("<I", ((~words & 0xffff) << 16) | words)


class NvramBlob(unittest.TestCase):
    def test_basic_defaults_and_padding(self):
        blob = wifi_fw.nvram_blob("a=1\nb=2\n")
        body = b"a=1\0b=2\0boardrev=0xff\0"  # 22 bytes -> roundup(23, 4) = 24
        self.assertEqual(blob, body + b"\0\0" + token(6))

    def test_always_one_extra_nul(self):
        # 20 bytes of entries: brcmfmac pads to roundup(20 + 1, 4) = 24, not 20
        blob = wifi_fw.nvram_blob("boardrev=0x1234\nab=1\n")
        body = b"boardrev=0x1234\0ab=1\0"
        self.assertEqual(len(body), 21)
        blob2 = wifi_fw.nvram_blob("boardrev=0x123\nab=1\n")  # 20 bytes
        self.assertEqual(blob2[:20], b"boardrev=0x123\0ab=1\0")
        self.assertEqual(blob2[20:24], b"\0\0\0\0")
        self.assertEqual(blob2[24:], token(6))
        self.assertEqual(len(blob), 24 + 4)

    def test_comments_raw1_spaces_and_mac(self):
        text = ("# header\n"
                "RAW1=80 32 fe\n"
                "macaddr=00:90:4c:00:00:01\n"
                "bad key=1\n"
                "name=two words # trailing comment\n"
                "\r\n"
                "boardrev=0x1101\r\n")
        blob = wifi_fw.nvram_blob(text, mac=bytes.fromhex("a1b2c3d4e5f6"))
        entries = blob[:-4].rstrip(b"\0").split(b"\0")
        self.assertEqual(entries, [b"name=two words ", b"boardrev=0x1101",
                                   b"macaddr=a1:b2:c3:d4:e5:f6"])
        self.assertEqual(len(blob) % 4, 0)

    def test_macaddr_kept_without_platform_mac(self):
        blob = wifi_fw.nvram_blob("macaddr=00:90:4c:00:00:01\n")
        self.assertIn(b"macaddr=00:90:4c:00:00:01\0", blob)

    def test_multi_device_refused(self):
        with self.assertRaises(ValueError):
            wifi_fw.nvram_blob("devpath0=pcie/1/1/\n0:x=1\n")


class Selection(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        r = self.root = self.tmp.name
        def put(rel, data):
            path = os.path.join(r, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        put("j613-wifi-info.txt", b"== model\nMac15,12\n")
        c0 = "C-4388__s-C0/"
        put(c0 + "dnieper.trx", b"FW-C0")
        put(c0 + "dnieper.clmb", b"CLM")
        put(c0 + "dnieper-X0.txcb", b"TXCAP-X0")
        put(c0 + "P-dnieper-X0_M-HRPN_V-u__m-7.5.txt", b"a=1\nboard=x0\n")
        put(c0 + "P-dnieper-X3_M-HRPN_V-u__m-7.5.txt", b"a=1\nboard=x3\n")
        put(c0 + "P-dnieper-X0_M-HRPN_V-m__m-1.1.txt", b"a=1\nboard=murata\n")
        put(c0 + "P-other-X0_M-ABCD_V-u__m-1.0.txt", b"a=1\nboard=other\n")
        put(c0 + "other.trx", b"FW-OTHER")
        put(c0 + "perf/dnieper.trx", b"PERF")
        put(c0 + "java_gen.trx", b"GEN")
        c2 = "C-4388__s-C2/"
        put(c2 + "dnieper.trx", b"FW-C2")
        put(c2 + "P-dnieper-X0_M-HRPN_V-u__m-7.5.txt", b"a=1\nboard=c2\n")

    def tearDown(self):
        self.tmp.cleanup()

    def data(self, ref):
        with open(ref.path, "rb") as f:
            return f.read()

    def test_installed_names_match_asahi(self):
        names = wifi_fw.Collection(self.root).files()
        self.assertIn("brcmfmac4388c0-pcie.apple,dnieper.bin", names)
        self.assertIn("brcmfmac4388c0-pcie.apple,dnieper.clm_blob", names)
        # the only txcap (antenna X0) is pruned to the bare board: every antenna gets it
        self.assertIn("brcmfmac4388c0-pcie.apple,dnieper.txcap_blob", names)
        self.assertNotIn("brcmfmac4388c0-pcie.apple,dnieper-X0.txcap_blob", names)
        self.assertIn("brcmfmac4388c0-pcie.apple,dnieper-HRPN-u-7.5-X0.txt", names)
        self.assertIn("brcmfmac4388c0-pcie.apple,dnieper-HRPN-u-7.5-X3.txt", names)
        # single file below vendor m: pruned up to board-module-vendor
        self.assertIn("brcmfmac4388c0-pcie.apple,dnieper-HRPN-m.txt", names)
        self.assertNotIn("brcmfmac4388c0-pcie.apple,dnieper-HRPN-m-1.1-X0.txt", names)
        self.assertNotIn("brcmfmac4388c0-pcie.apple,dnieper-HRPN-m-1.1.txt", names)
        self.assertFalse(any("perf" in n or "_gen" in n or "info" in n for n in names))

    def test_linux_order_rev4(self):
        otp = {"module": "HRPN", "vendor": "u", "version": "7.5"}
        sel, notes, col = wifi_fw.select(self.root, 0x4388, 4, otp=otp, antenna="X3")
        self.assertEqual(self.data(sel["bin"]), b"FW-C0")
        self.assertEqual(col.nvram[sel["txt"].path], b"a=1\nboard=x3\n")
        self.assertEqual(self.data(sel["clm_blob"]), b"CLM")
        self.assertEqual(self.data(sel["txcap_blob"]), b"TXCAP-X0")  # via the pruned name

    def test_pruned_fallback_other_antenna(self):
        otp = {"module": "HRPN", "vendor": "m", "version": "1.1"}
        sel, _, col = wifi_fw.select(self.root, 0x4388, 4, otp=otp, antenna="X9")
        self.assertEqual(col.nvram[sel["txt"].path], b"a=1\nboard=murata\n")

    def test_exact_stepping_first(self):
        otp = {"module": "HRPN", "vendor": "u", "version": "7.5"}
        sel, notes, col = wifi_fw.select(self.root, 0x4388, 6, otp=otp, antenna="X0")
        self.assertEqual(self.data(sel["bin"]), b"FW-C2")
        self.assertEqual(col.nvram[sel["txt"].path], b"a=1\nboard=c2\n")
        # rev 5 = C1: no such folder, Linux's 4388c0 name is next
        sel, notes, _ = wifi_fw.select(self.root, 0x4388, 5, otp=otp, antenna="X0")
        self.assertEqual(self.data(sel["bin"]), b"FW-C0")

    def test_no_otp_board_only(self):
        sel, _, _ = wifi_fw.select(self.root, 0x4388, 4, otp=None, antenna="X0")
        self.assertEqual(self.data(sel["bin"]), b"FW-C0")
        self.assertNotIn("txt", sel)  # NVRAMs are all module-specific

    def test_names(self):
        self.assertEqual(wifi_fw.linux_fw_base(0x4388, 4), "4388c0")
        self.assertEqual(wifi_fw.linux_fw_base(0x4388, 9), "4388c0")
        self.assertEqual(wifi_fw.linux_fw_base(0x4388, 1), "4388b0")
        self.assertEqual(wifi_fw.stepping_base(0x4388, 6), "4388c2")
        self.assertEqual(wifi_fw.stepping_base(0x4387, 7), "4387c2")
        self.assertIsNone(wifi_fw.stepping_base(0x4388, 2))

    def test_from_log(self):
        log = os.path.join(self.root, "hv.log")
        with open(log, "w") as f:
            f.write("TTY> wlan: chip 4388 rev 4 (chipid 0x15484388, AXI, window 0x18000000)\n"
                    "TTY> wlan: OTP module=HRPN vendor=u version=7.5\n")
        chip, rev, otp = wifi_fw.from_log(log)
        self.assertEqual((chip, rev), (0x4388, 4))
        self.assertEqual(otp, {"module": "HRPN", "vendor": "u", "version": "7.5"})


if __name__ == "__main__":
    unittest.main()
