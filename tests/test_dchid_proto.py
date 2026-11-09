"""Builds and runs the host C test of the J613 DockChannel HID protocol core."""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.skipif(shutil.which("cc") is None, reason="no C compiler")
def test_dchid_proto(tmp_path):
    exe = tmp_path / "test_dchid_proto"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-DDCHID_HOST_TEST",
         str(ROOT / "tests/c/test_dchid_proto.c"),
         str(ROOT / "m1n1_windows/src/dchid_proto.c"), "-o", str(exe)],
        check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    assert "all tests passed" in out.stdout


@pytest.mark.skipif(shutil.which("cc") is None, reason="no C compiler")
def test_dchid_guest_contract(tmp_path):
    """Guest keyboard descriptor, trackpad sanitizer and report queue vs the Windows decoder."""
    exe = tmp_path / "test_dchid_guest"
    proto = ROOT / "drivers/apple-input/protocol"
    srcs = [ROOT / "tests/c/test_dchid_guest.c", ROOT / "m1n1_windows/src/dchid_proto.c"]
    srcs += sorted((proto / "src").glob("*.c"))
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-DDCHID_HOST_TEST",
         "-I", str(proto / "include")] + [str(s) for s in srcs] + ["-o", str(exe)],
        check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    assert "all tests passed" in out.stdout
