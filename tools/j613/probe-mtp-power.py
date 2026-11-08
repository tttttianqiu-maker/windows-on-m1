#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only probe: how is the J613 MTP (keyboard/trackpad coprocessor) powered?

Run against an Air sitting at "Running proxy..." (no guest):
  M1N1DEVICE=/dev/cu.usbmodemXXX proxyenv/bin/python tools/j613/probe-mtp-power.py
Prints the /arm-io/mtp ADT node (+ related nodes) and every pmgr device whose
name or ADT users look MTP/DockChannel related, with live power-state registers.
Writes nothing to the hardware.
"""
import pathlib, sys
sys.path.append(str(pathlib.Path(__file__).resolve().parents[2] / "m1n1_windows" / "proxyclient"))
from m1n1.setup import *  # noqa: E402,F401  (u, p, iface)

dt = u.adt
KEYS = ("MTP", "DOCK", "DCHID", "KBD", "HID", "SIO", "AOP")

def dump_node(path):
    try:
        node = dt[path]
    except KeyError:
        print(f"--- {path}: not present")
        return
    print(f"--- {path}")
    for k, v in node._properties.items():
        s = repr(v)
        print(f"  {k:28s} {s[:200]}")

print("=== ADT nodes ===")
for d in dt["/arm-io"]:
    n = d._path
    if any(k.lower() in n.lower() for k in ("mtp", "dockchannel", "dart-mtp", "mtp-")):
        dump_node(n)
dump_node("/arm-io/mtp")

pmgr = dt["/arm-io/pmgr"]
dev_by_id = {dt.pmgr_dev_get_id(dev): dev for dev in pmgr.devices}
pd_by_id = {pd.id: pd for pd in pmgr.power_domains}
users = {}
for d in dt["/arm-io"]:
    for attr, tag in (("clock_gates", "clkgate"), ("power_gates", "pwrgate")):
        for i, pid in enumerate(getattr(d, attr, None) or []):
            users.setdefault(pid, []).append(f"{d._path}.{tag}[{i}]")

print("\n=== pmgr devices (MTP/DockChannel/HID related) ===")
for i, dev in enumerate(pmgr.devices):
    did = dt.pmgr_dev_get_id(dev)
    us = users.get(did, [])
    if not (any(k in dev.name.upper() for k in KEYS) or any("mtp" in x or "dock" in x for x in us)):
        continue
    s = f" #{i:3d} {dev.name:22s} id {did:3d} psreg {dev.psreg}:{dev.psidx}"
    if not dev.flags.no_ps:
        ps = pmgr.ps_regs[dev.psreg]
        addr = pmgr.get_reg(ps.reg)[0] + ps.offset + dev.psidx * 8
        s += f" @ {addr:#x} = {p.read32(addr):#010x}"
    else:
        s += " (no ps)"
    if dev.pd and dev.pd in pd_by_id:
        s += f" pd {pd_by_id[dev.pd].name}"
    parents = [x for x in dt.pmgr_dev_get_parents(dev) if x]
    if parents:
        s += " parents " + ",".join(dev_by_id[x].name if x in dev_by_id else f"#{x}" for x in parents)
    print(s)
    for x in us:
        print(f"      user {x}")

print("\n=== MTP ASC CPU_CONTROL ===")
try:
    base = dt["/arm-io/mtp"].get_reg(0)[0]
    print(f" asc base {base:#x} CPU_CONTROL(+0x44) = {p.read32(base + 0x44):#010x}")
except Exception as e:  # unpowered blocks can fault; report and carry on
    print(f" read failed: {e}")
