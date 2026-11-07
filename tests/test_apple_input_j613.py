"""Host tests for the J613 (MacBook Air M3) input path.

* apple_mtp_test: MTP 0x75 report <-> J313 SPI framing and 0xd9 dimensions.
* avhd_sim_test: the real Windows AVHD transport, trackpad pipeline and VHF
  frontend against stand-in WDK headers and a model of the m1n1 AVHD page.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INPUT = ROOT / "drivers" / "apple-input"
PROTOCOL = INPUT / "protocol"
WINDOWS = INPUT / "windows"
CC = os.environ.get("CC", "cc")
FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function"]


def build_and_run(test, binary_name, sources, extra):
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / binary_name
        subprocess.run([CC, *FLAGS, *extra, str(test), *map(str, sources), "-o", str(binary)],
                       check=True, cwd=ROOT)
        return subprocess.run([str(binary)], check=True, cwd=ROOT,
                              capture_output=True, text=True).stdout


class AppleInputJ613Tests(unittest.TestCase):
    def test_mtp_adapter(self):
        out = build_and_run(
            PROTOCOL / "tests" / "apple_mtp_test.c", "apple_mtp_test",
            sorted((PROTOCOL / "src").glob("*.c")),
            ["-I", str(PROTOCOL / "include"), "-I", str(PROTOCOL / "tests")])
        self.assertIn("all tests passed", out)

    def test_avhd_transport_simulation(self):
        sources = [WINDOWS / "src" / name for name in (
            "transport_avhd.c", "trackpad_pipeline.c", "vhf_frontend.c",
            "vhf_keyboard.c", "vhf_trackpad.c")]
        sources += sorted((PROTOCOL / "src").glob("*.c"))
        out = build_and_run(
            WINDOWS / "tests" / "avhd_sim_test.c", "avhd_sim_test", sources,
            ["-DAI_KERNEL_MODE", "-include", str(WINDOWS / "tests" / "host" / "ntddk.h"),
             "-I", str(WINDOWS / "tests" / "host"), "-I", str(WINDOWS / "include"),
             "-I", str(PROTOCOL / "include")])
        self.assertIn("all tests passed", out)


if __name__ == "__main__":
    unittest.main()
