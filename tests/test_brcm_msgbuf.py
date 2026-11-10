"""Builds and runs the host C test of the J613 Wi-Fi driver (brcm_msgbuf.c, brcm_fwil.c)
against a simulated BCM4388 dongle: rings, ioctls, dongle init, WPA2 join, TX/RX."""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "m1n1_windows/src"


@pytest.mark.skipif(shutil.which("cc") is None, reason="no C compiler")
def test_brcm_msgbuf_against_simulated_dongle(tmp_path):
    exe = tmp_path / "test_brcm_msgbuf"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-DBRCM_HOST_TEST",
         "-I", str(ROOT / "tests/c"), "-I", str(SRC),
         str(ROOT / "tests/c/test_brcm_msgbuf.c"), str(SRC / "brcm_msgbuf.c"),
         str(SRC / "brcm_fwil.c"), "-o", str(exe)],
        check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=60)
    assert "all tests passed" in out.stdout
    # the driver's own log lines the hardware run is judged by
    assert "wlan: msgbuf up: 64 rx" in out.stdout
    assert "wlan: interface up: OK" in out.stdout
    assert "wlan: CONNECTED to a0:b1:c2:d3:e4:f5" in out.stdout
