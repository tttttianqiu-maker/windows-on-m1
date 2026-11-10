# SPDX-License-Identifier: MIT
"""J613 built-in Wi-Fi: pick and prepare the Broadcom firmware set for m1n1 (wlan.c/brcm.c).

The files come from the owner's macOS (scripts/j613-wifi-fw.sh -> `j613 wififw` ->
firmware/wifi/C-4388__s-XX/...). This does what Asahi Linux does with them, in two steps:

1. asahi-installer asahi_firmware/wifi.py (WiFiFWCollection, ported below): every macOS
   file name encodes dimensions C (chip), s (silicon stepping), P (board, "P-<board>-<A>"
   carries the antenna A), M (module), V (vendor), m (module version); the files go into a
   tree keyed [ext, C, s, P, M, V, m, A], which is pruned (a node whose children are all the
   same file becomes that file) and installed as
   brcmfmac<C><s>-pcie.apple,<P>[-<M>[-<V>[-<m>]]][-<A>].<bin|txt|clm_blob|txcap_blob|sig>.
2. Linux brcmfmac (pcie.c brcmf_pcie_prepare_fw_request, firmware.c brcm_alt_fw_path):
   with the OTP module/vendor/version and the antenna SKU known, each file kind is looked
   up as board-module-vendor-version-antenna, board-module-vendor-version,
   board-module-vendor, board-module, board-antenna, board, first hit wins.

Linux names the 4388 firmware "4388c0" for every chip revision >= 4 (pcie.c
brcmf_pcie_fwnames). Asahi never ran on an M3, whose BCM4388 may be a later stepping, so
the stepping that matches the chip revision is tried first and Linux's name second.

nvram_blob() is brcmfmac firmware.c brcmf_fw_nvram_strip for a single-device NVRAM:
key=value entries NUL-separated, RAW1 dropped, "boardrev=0xff" if missing, the platform MAC
replacing macaddr, zero padding to roundup(len + 1, 4), then the length token.
"""
import hashlib
import os
import struct
import sys

EXTMAP = {"trx": "bin", "txt": "txt", "clmb": "clm_blob", "txcb": "txcap_blob", "sig": "sig"}
KINDS = ("bin", "txt", "clm_blob", "txcap_blob", "sig")
DIMS = ["C", "s", "P", "M", "V", "m", "A"]
BOARD_J613 = "apple,dnieper"  # Linux t8122-j613.dts: brcm,board-type
MAX_NVRAM = 64000  # brcmfmac BRCMF_FW_MAX_NVRAM_SIZE

# brcmfmac pcie.c brcmf_pcie_fwnames for the Apple chips: (chip, revmask, name)
LINUX_FWNAMES = [
    (0x4377, 0xFFFFFFFF, "4377b3"),
    (0x4378, 0x0000000F, "4378b1"),
    (0x4378, 0xFFFFFFE0, "4378b3"),
    (0x4387, 0xFFFFFFFF, "4387c2"),
    (0x4388, 0x0000000F, "4388b0"),
    (0x4388, 0xFFFFFFF0, "4388c0"),
]
# revision ID of the first C stepping (pcie.c comments: 4388 rev 4 = C0, 4387 rev 7 = C2)
FIRST_C = {0x4388: 4, 0x4387: 5}


def linux_fw_base(chip, chiprev):
    for c, mask, name in LINUX_FWNAMES:
        if c == chip and mask & (1 << chiprev):
            return name
    return None


def stepping_base(chip, chiprev):
    """The macOS folder stepping for this exact revision, e.g. 4388 rev 6 -> '4388c2'."""
    first = FIRST_C.get(chip)
    if first is None or chiprev < first:
        return None
    return f"{chip:x}c{chiprev - first}"


class FWRef:
    """One macOS file; equality by content like asahi-installer's FWFile."""

    def __init__(self, path, data):
        self.path = path
        self.sha = hashlib.sha256(data).hexdigest()
        self.size = len(data)

    def __eq__(self, other):
        return other is not None and self.sha == other.sha

    def __hash__(self):
        return hash(self.sha)


class _Node:
    def __init__(self):
        self.this = None
        self.leaves = {}

    def __eq__(self, other):
        return self.this == other.this and self.leaves == other.leaves


def process_nvram(data):
    """asahi-installer: drop empty lines, strip whitespace around keys."""
    lines = []
    for line in data.decode("ascii", "replace").split("\n"):
        if not line:
            continue
        if "=" not in line:
            lines.append(line + "\n")
            continue
        key, value = line.split("=", 1)
        lines.append(f"{key.strip()}={value}\n")
    return "".join(lines).encode("ascii", "replace")


def file_dims(subpath, name):
    """macOS file name -> {dim: value}, or None for files that are not firmware."""
    ext = name.rsplit(".", 1)[-1]
    if ext not in EXTMAP or "_gen" in name or subpath in ("", "."):
        return None
    if not name.endswith(".txt"):
        name = "P-" + name
    idpath = os.path.join(subpath, name).rsplit(".", 1)[0]
    props = {}
    for part in idpath.replace(os.sep, "_").split("_"):
        if not part:
            continue
        if "-" not in part:
            return None
        k, v = part.split("-", 1)
        if k == "P" and "-" in v:
            props["P"], props["A"] = v.split("-", 1)
        else:
            props[k] = v
    if set(props) - set(DIMS) or "C" not in props or "s" not in props or "P" not in props:
        return None
    return props


class Collection:
    """asahi-installer WiFiFWCollection over firmware/wifi (macOS /usr/share/firmware/wifi)."""

    def __init__(self, root):
        self.root = _Node()
        self.nvram = {}  # path -> processed text bytes
        for dirpath, dirnames, filenames in os.walk(root):
            for skip in ("perf", "assert"):
                if skip in dirnames:
                    dirnames.remove(skip)
            sub = os.path.relpath(dirpath, root)
            for name in sorted(filenames):
                props = file_dims(sub, name)
                if props is None:
                    continue
                ext = name.rsplit(".", 1)[-1]
                ident = [ext] + [props[d] for d in DIMS if d in props]
                path = os.path.join(dirpath, name)
                with open(path, "rb") as f:
                    data = f.read()
                if ext == "txt":
                    data = process_nvram(data)
                    self.nvram[path] = data
                node = self.root
                for k in ident:
                    node = node.leaves.setdefault(k, _Node())
                node.this = FWRef(path, data)
        self._prune(self.root, 0)

    def _prune(self, node, depth):
        for i in node.leaves.values():
            self._prune(i, depth + 1)
        if node.this is None and node.leaves and depth > 3:
            first = next(iter(node.leaves.values()))
            if all(i == first for i in node.leaves.values()):
                node.this = first.this
        for i in node.leaves.values():
            if not i.this or not node.this or i.this != node.this:
                break
        else:
            node.leaves = {}

    def _walk(self, node, ident):
        if node.this is not None:
            yield ident, node.this
        for k, sub in node.leaves.items():
            yield from self._walk(sub, ident + [k])

    def files(self):
        """{linux file name: FWRef}, the names asahi-installer would install."""
        out = {}
        for ident, ref in self._walk(self.root, []):
            if len(ident) < 3:
                continue
            (ext, chip, rev), rest = ident[:3], ident[3:]
            rest = ("," + "-".join(rest)) if rest else ""
            out[f"brcmfmac{chip}{rev.lower()}-pcie.apple{rest}.{EXTMAP[ext]}"] = ref
        return out

    def steppings(self):
        return sorted({n.split("-pcie", 1)[0][len("brcmfmac"):] for n in self.files()})


def board_types(board, otp, antenna):
    """brcmfmac pcie.c brcmf_pcie_prepare_fw_request: the Apple lookup order."""
    if board and antenna and otp:
        m, v, ver = otp["module"], otp["vendor"], otp["version"]
        return [f"{board}-{m}-{v}-{ver}-{antenna}", f"{board}-{m}-{v}-{ver}",
                f"{board}-{m}-{v}", f"{board}-{m}", f"{board}-{antenna}", board]
    return [board]


def select(root, chip, chiprev, board=BOARD_J613, otp=None, antenna=None, base=None):
    """Return ({kind: FWRef}, notes). otp = {"module", "vendor", "version"} or None."""
    col = Collection(root)
    files = col.files()
    notes = []
    bases = [base] if base else [b for b in (stepping_base(chip, chiprev),
                                             linux_fw_base(chip, chiprev)) if b]
    bases = list(dict.fromkeys(bases))
    bts = board_types(board, otp, antenna)
    notes.append(f"chip {chip:x} rev {chiprev}: firmware names {bases}, boards {bts}")
    notes.append(f"steppings in firmware/wifi: {col.steppings()}")
    found = []
    for b in bases:
        out, picked = {}, []
        for kind in KINDS:
            for bt in bts:
                name = f"brcmfmac{b}-pcie.{bt}.{kind}"
                if name in files:
                    out[kind] = files[name]
                    picked.append(f"{kind}: {name} <- {os.path.relpath(files[name].path, root)}")
                    break
        found.append((b, out, picked))
        if "bin" in out and "txt" in out:  # firmware and NVRAM of one stepping
            return out, notes + picked, col
        notes.append(f"{b}: {'firmware' if 'bin' in out else 'no firmware'}, "
                     f"{'NVRAM' if 'txt' in out else 'no NVRAM'} for {bts[-1]}")
    for b, out, picked in found:
        if "bin" in out:
            notes.append(f"using {b} without NVRAM: the firmware will lack its board calibration")
            return out, notes + picked, col
    return {}, notes, col


def nvram_blob(text, mac=None):
    """brcmfmac firmware.c brcmf_fw_nvram_strip (single device)."""
    if isinstance(text, bytes):
        text = text.decode("ascii", "replace")

    def nvram_char(c):
        return c != "#" and 0x20 <= ord(c) < 0x7f

    out, boardrev, size = [], False, 0
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "#" or c == "\n":  # comment / end of line
            j = text.find("\n", i)
            i = n if j < 0 else j + 1
            continue
        if not nvram_char(c) or c == " ":  # whitespace, NUL, invalid: skip
            i += 1
            continue
        start = i
        while i < n and text[i] != "=" and nvram_char(text[i]) and text[i] != " ":
            i += 1
        if i >= n or text[i] != "=":  # "'=' expected, skip invalid key entry"
            j = text.find("\n", i)
            i = n if j < 0 else j + 1
            continue
        key = text[start:i]
        if key.startswith("devpath") or key.startswith("pcie/"):
            raise ValueError("multi-device NVRAM (devpath/pcie/ keys) is not supported here")
        skip = key.startswith("RAW1") or (mac is not None and key.startswith("macaddr"))
        boardrev |= key.startswith("boardrev")
        i += 1
        while i < n and nvram_char(text[i]):
            i += 1
        if skip:
            continue
        entry = text[start:i]
        if size + len(entry) + 1 >= MAX_NVRAM:
            break
        out.append(entry)
        size += len(entry) + 1
    if not out:
        raise ValueError("empty NVRAM")
    if not boardrev:
        out.append("boardrev=0xff")
    if mac is not None:
        out.append("macaddr=" + ":".join(f"{b:02x}" for b in mac))
    blob = b"".join(e.encode("ascii") + b"\0" for e in out)
    blob += b"\0" * ((len(blob) + 1 + 3) // 4 * 4 - len(blob))
    words = len(blob) // 4
    return blob + struct.pack("<I", ((~words & 0xffff) << 16) | (words & 0xffff))


# ---- ADT helpers (m1n1 proxyclient ADT nodes) ----

def raw_prop(node, name):
    """The property's bytes, whatever type m1n1's adt.py parsed it as."""
    from m1n1.adt import build_prop
    v = node._properties[name]
    if isinstance(v, (bytes, bytearray)):
        return bytes(v)
    t = node._types.get(name, (None, False))[0]
    return build_prop(node._path, name, v, t=t)


def antenna_from_adt(adt):
    """/arm-io/wlan wifi-antenna-sku-info bytes 8..15 (m1n1 kboot.c dt_set_wifi)."""
    try:
        raw = raw_prop(adt["/arm-io/wlan"], "wifi-antenna-sku-info")
    except Exception:
        return None
    if len(raw) < 16:
        return None
    return raw[8:16].split(b"\0", 1)[0].decode("ascii", "replace") or None


def mac_from_adt(adt):
    """/chosen mac-address-wifi0 (m1n1 kboot.c dt_set_mac_addresses)."""
    try:
        raw = raw_prop(adt["chosen"], "mac-address-wifi0")
    except Exception:
        return None
    return bytes(raw[:6]) if len(raw) >= 6 and any(raw[:6]) else None


def calblob_from_adt(adt):
    """/arm-io/wlan wifi-calibration-msf (m1n1 kboot.c -> brcm,cal-blob -> "calload")."""
    try:
        raw = raw_prop(adt["/arm-io/wlan"], "wifi-calibration-msf")
    except Exception:
        return None
    return raw or None


def from_log(path):
    """(chip, rev, otp) from the newest m1n1 'wlan:' lines of an hv.log, or Nones."""
    import re
    chip = rev = otp = None
    try:
        text = open(path, "rb").read().decode("utf-8", "replace")
    except OSError:
        return chip, rev, otp
    for m in re.finditer(r"wlan: chip ([0-9a-f]+) rev (\d+)", text):
        chip, rev = int(m.group(1), 16), int(m.group(2))
    for m in re.finditer(r"wlan: OTP module=(\S*) vendor=(\S*) version=(\S*)", text):
        otp = {"module": m.group(1), "vendor": m.group(2), "version": m.group(3)}
    return chip, rev, otp


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="J613 Wi-Fi firmware selection (offline check)")
    ap.add_argument("root", help="firmware/wifi (the extracted macOS folders)")
    ap.add_argument("--chip", type=lambda s: int(s, 16))
    ap.add_argument("--rev", type=int, help="chip revision (hv.log 'wlan: chip 4388 rev N')")
    ap.add_argument("--board", default=BOARD_J613)
    ap.add_argument("--otp", help="module,vendor,version (hv.log 'wlan: OTP module=...')")
    ap.add_argument("--antenna")
    ap.add_argument("--from-log", help="take chip, revision and OTP from this hv.log")
    ap.add_argument("--adt", help="ADT dump (runs/adt-*.bin from `j613 wifiprobe`): antenna, MAC")
    ap.add_argument("--list", action="store_true", help="list every installable name")
    a = ap.parse_args(argv)
    chip, rev, otp = from_log(a.from_log) if a.from_log else (None, None, None)
    chip = a.chip if a.chip is not None else (chip or 0x4388)
    rev = a.rev if a.rev is not None else rev
    if a.otp:
        m, v, ver = a.otp.split(",")
        otp = {"module": m, "vendor": v, "version": ver}
    antenna, mac = a.antenna, None
    if a.adt:
        here = os.path.dirname(os.path.abspath(__file__))
        sys.path.insert(0, os.path.join(here, "..", "..", "m1n1_windows", "proxyclient"))
        from m1n1.adt import load_adt
        adt = load_adt(open(a.adt, "rb").read())
        antenna = antenna or antenna_from_adt(adt)
        mac = mac_from_adt(adt)
        cal = calblob_from_adt(adt)
        print(f"ADT: antenna {antenna}, MAC {mac.hex(':') if mac else 'none'}, "
              f"calibration {len(cal) if cal else 0} bytes")
    if rev is None:
        print("chip revision unknown: run `j613 wifi probe` once, or pass --rev")
        rev = 4
    print(f"chip {chip:x} rev {rev}, OTP {otp or 'unknown'}, antenna {antenna or 'unknown'}")
    sel, notes, col = select(a.root, chip, rev, a.board, otp, antenna)
    if a.list:
        for name, ref in sorted(col.files().items()):
            print(f"{name}  ({ref.size} bytes)")
    for n in notes:
        print(n)
    if "txt" in sel:
        blob = nvram_blob(col.nvram[sel["txt"].path], mac)
        print(f"NVRAM for the chip: {len(blob)} bytes")
    return 0 if "bin" in sel else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
