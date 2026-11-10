#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only probe for the m1n1 Wi-Fi work: the J613 Wi-Fi chip and its PCIe port.

Run against an Air sitting at "Running proxy..." (watcher stopped):
  j613 wifiprobe
Prints the ADT nodes that describe the Wi-Fi/Bluetooth chip (/arm-io/wlan,
/arm-io/bluetooth), the PCIe controller and its ports (/arm-io/apcie* and
children), the PCIe DARTs, the Wi-Fi/BT MAC addresses from /chosen, and every
pmgr device whose name looks PCIe/WLAN related with its power-state register.
Saves the whole ADT next to the report (raw .bin + text) for offline work.
Reads the ADT and pmgr power-state registers only; writes nothing to the
hardware.
"""
import pathlib
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.append(str(ROOT / "m1n1_windows" / "proxyclient"))

# Offline: `probe-wifi.py --adt runs/adt-XXXX.bin` prints the same report from a saved ADT
# (no Air, no pmgr register reads, nothing saved).
OFFLINE = len(sys.argv) == 3 and sys.argv[1] == "--adt"
if OFFLINE:
    from m1n1.adt import ADTNode, ADTNodeStruct
    dt = ADTNode(ADTNodeStruct.parse(pathlib.Path(sys.argv[2]).read_bytes()))
    try:
        dt.pmgr_init()
    except Exception:
        pass
else:
    from m1n1.setup import *  # noqa: E402,F401,F403  (u, p, iface)
    dt = u.adt
OUT = ROOT / "runs"
OUT.mkdir(exist_ok=True)
STAMP = time.strftime("%Y%m%d-%H%M%S")
WANT = ("apcie", "pcie", "wlan", "wifi", "bluetooth", "dart-apcie")


def fmt(v, maxlen=600):
    if isinstance(v, (bytes, bytearray)):
        s = v.hex() if len(v) <= 96 else v[:96].hex() + f"... ({len(v)} bytes)"
    else:
        s = repr(v)
    return s if len(s) <= maxlen else s[:maxlen] + f"... ({len(s)} chars)"


def dump(node, depth=0):
    ind = "  " * depth
    print(f"{ind}--- {node._path}")
    for k, v in node._properties.items():
        print(f"{ind}  {k:34s} {fmt(v)}")
    try:
        for i, (addr, size) in enumerate(node.get_reg(i) for i in range(len(node.reg))):
            print(f"{ind}  reg[{i}] -> {addr:#x} size {size:#x}")
    except Exception:
        pass
    for child in node:
        dump(child, depth + 1)


print("=== /chosen MAC addresses ===")
for k, v in dt["/chosen"]._properties.items():
    if k.startswith("mac-address"):
        print(f"  {k:34s} {fmt(v)}")

print("\n=== ADT nodes (Wi-Fi, Bluetooth, PCIe) ===")
seen = set()
for d in dt["/arm-io"]:
    if any(w in d.name.lower() for w in WANT) and d._path not in seen:
        seen.add(d._path)
        dump(d)

def walk(node):
    yield node
    for child in node:
        yield from walk(child)


print("\n=== nodes elsewhere mentioning wlan/wifi/pcie in a property name ===")
for node in walk(dt):
    path = getattr(node, "_path", "")
    if path in seen or any(path.startswith(s + "/") for s in seen):
        continue
    hits = [k for k in node._properties if any(w in k.lower() for w in ("wlan", "wifi", "pcie"))]
    for k in hits:
        print(f"  {path}: {k} = {fmt(node._properties[k], 300)}")

print("\n=== pmgr devices (PCIe / WLAN related) ===")
if OFFLINE or "arm-io/pmgr" not in dt:
    print("  (offline: skipped)")
    sys.exit(0)
pmgr = dt["/arm-io/pmgr"]
dev_by_id = {dt.pmgr_dev_get_id(dev): dev for dev in pmgr.devices}
pd_by_id = {pd.id: pd for pd in pmgr.power_domains}
users = {}
for d in dt["/arm-io"]:
    for attr, tag in (("clock_gates", "clkgate"), ("power_gates", "pwrgate")):
        for i, pid in enumerate(getattr(d, attr, None) or []):
            users.setdefault(pid, []).append(f"{d._path}.{tag}[{i}]")
for i, dev in enumerate(pmgr.devices):
    did = dt.pmgr_dev_get_id(dev)
    us = users.get(did, [])
    name = dev.name.upper()
    if not (any(k in name for k in ("PCIE", "WLAN", "WIFI")) or
            any(any(w in x for w in ("pcie", "wlan")) for x in us)):
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

raw = u.get_adt()
(OUT / f"adt-{STAMP}.bin").write_bytes(raw)
(OUT / f"adt-{STAMP}.txt").write_text(str(dt))
print(f"\nsaved runs/adt-{STAMP}.bin ({len(raw)} bytes) and runs/adt-{STAMP}.txt")
