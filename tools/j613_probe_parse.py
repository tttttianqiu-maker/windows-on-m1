#!/usr/bin/env python3
"""Turn a scripts/j613-probe.sh dump into the values the J613 port needs.

Usage:
  python3 tools/j613_probe_parse.py j613-probe-YYYYMMDD-HHMMSS/
  python3 tools/j613_probe_parse.py j613-probe-.../ --apply   # also run j613_layout.py

macOS ioreg prints device-tree data as little-endian hex bytes, e.g.
  "dram-base" = <0000000000010000>   ->  0x10000000000
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# ADT nodes the port depends on, and what we compare them against.
EXPECT = {
    "uart0": "debug UART (firmware PcdAppleUartBase = 0x2a1200000, DSDT COM0)",
    "usb-drd1": "guest xHCI (DSDT XHC1 = 0xb02280000 / AIC 1030)",
    "dart-usb1": "guest xHCI DARTs (hv_vgic.c: 0xb02f00000 / 0xb02f80000)",
    "ans": "NVMe / ANS2 (m1n1 nvme.c uses reg[3])",
    "sart-ans": "SART (expect sart-version 3)",
    "aic": "interrupt controller (expect AIC3)",
    "mtp": "keyboard/trackpad coprocessor",
}


def le_bytes(hexstr: str) -> bytes:
    return bytes.fromhex(hexstr)


def le_u64s(data: bytes):
    return [int.from_bytes(data[i:i + 8], "little") for i in range(0, len(data) - 7, 8)]


def le_u32s(data: bytes):
    return [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data) - 3, 4)]


def prop(text: str, name: str):
    m = re.search(r'"%s" = <([0-9a-fA-F]+)>' % re.escape(name), text)
    return le_bytes(m.group(1)) if m else None


def node_block(text: str, name: str):
    """Return the ioreg property block of the first node called `name`."""
    m = re.search(r"\+-o %s[ @<].*?\n(.*?)(?=\n\s*\+-o |\Z)" % re.escape(name), text, re.S)
    return m.group(1) if m else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("probe_dir", type=Path)
    ap.add_argument("--apply", action="store_true",
                    help="regenerate the J613 guest layout from the measured dram-base")
    ap.add_argument("--mu", type=Path, help="Project Mu checkout (passed to j613_layout.py)")
    a = ap.parse_args()

    chosen = (a.probe_dir / "chosen.txt").read_text(errors="replace")
    armio_path = a.probe_dir / "arm-io.txt"
    armio = armio_path.read_text(errors="replace") if armio_path.exists() else ""
    ident = (a.probe_dir / "identity.txt").read_text(errors="replace") \
        if (a.probe_dir / "identity.txt").exists() else ""

    ok = True
    if "j613" not in ident.lower() and "mac15,12" not in ident.lower():
        print("WARNING: identity does not look like a J613 (MacBook Air 13\", M3). Check identity.txt")
        ok = False

    base = prop(chosen, "dram-base")
    size = prop(chosen, "dram-size")
    if base is None:
        print("ERROR: no dram-base in chosen.txt")
        return 1
    dram_base = le_u64s(base)[0]
    dram_size = le_u64s(size)[0] if size else None
    print(f"dram-base = {dram_base:#x}")
    print(f"dram-size = {dram_size:#x} ({dram_size / 2**30:.1f} GiB)" if dram_size else "dram-size = ?")
    if dram_base == 0x800000000:
        print("NOTE: dram-base equals the M1 value; the J613 layout generator will reject a 16 GB "
              "guest there (it overlaps 0xb02280000). Send the probe for review.")

    print()
    for name, what in EXPECT.items():
        blk = node_block(armio, name)
        print(f"== {name}: {what}")
        if blk is None:
            print("   NOT FOUND under arm-io (name may differ on M3, see arm-io.txt)")
            ok = False
            continue
        for p in ("compatible", "reg", "interrupts", "sart-version", "device_type"):
            m = re.search(r'"%s" = (<[0-9a-fA-F]+>|"[^"]*"|\([^)]*\))' % p, blk)
            if not m:
                continue
            v = m.group(1)
            if v.startswith("<"):
                data = le_bytes(v[1:-1])
                if p == "reg":
                    v = " ".join(f"{x:#x}" for x in le_u64s(data)[:12])
                    v += "   (ADT reg is relative to /arm-io ranges)"
                else:
                    v = " ".join(str(x) for x in le_u32s(data)[:12])
            print(f"   {p}: {v}")

    if a.apply:
        cmd = [sys.executable, "-I", str(ROOT / "tools/j613_layout.py"),
               "--dram-base", hex(dram_base)]
        if dram_size:
            cmd += ["--dram-size", hex(dram_size)]
        if a.mu:
            cmd += ["--mu", str(a.mu)]
        print("\n$ " + " ".join(cmd))
        return subprocess.call(cmd)
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
