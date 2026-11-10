"""Builds and runs the host C test of m1n1's virtio-net device model (hv_vnet.c), driven
the way NetKVM drives a modern virtio-pci device: caps, BAR, negotiation, queues, RX/TX,
ISR/INTx, link status, NEEDS_RESET, reset, BAR moves."""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "m1n1_windows/src"
DSDT = ROOT / "mu/Platform/MacBookAirM3J613Pkg/AcpiTables/DSDT.asl"


@pytest.mark.skipif(shutil.which("cc") is None, reason="no C compiler")
def test_vnet_device_model(tmp_path):
    exe = tmp_path / "test_vnet"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-DVNET_HOST_TEST",
         "-I", str(ROOT / "tests/c"), "-I", str(SRC),
         str(ROOT / "tests/c/test_vnet.c"), str(SRC / "hv_vnet.c"), "-o", str(exe)],
        check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=60)
    assert "all tests passed" in out.stdout


def test_vnet_interrupt_routing_matches_the_dsdt():
    """hv_vnet.c injects HV_IRQ_SYNTHETIC_VNET_VINTID for 00:01.0 INTA; the DSDT _PRT must
    route device 1 INTA to the same GSIV, and it must not collide with the other synthetic
    lines."""
    routes = (SRC / "hv_irq_routes.h").read_text()
    vintid = int(re.search(r"HV_IRQ_SYNTHETIC_VNET_VINTID (\d+)u", routes).group(1))
    others = {int(v) for v in re.findall(r"HV_IRQ_SYNTHETIC_\w+_VINTID (\d+)u", routes)}
    assert sorted(others).count(vintid) == 1
    prt = re.search(r"Name\(_PRT, Package\(\) \{(.*?)\n\s*\}\)", DSDT.read_text(), re.S).group(1)
    entries = re.findall(r"Package\(\) \{ (0x[0-9A-Fa-f]+), (\d+), Zero, (\d+) \}", prt)
    assert ("0x0001FFFF", "0", str(vintid)) in entries
    assert ("0x0000FFFF", "0", "64") in entries
