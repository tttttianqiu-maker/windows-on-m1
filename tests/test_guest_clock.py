"""Guest wall clock: run_uefi.py hands the host's local time to Mu's RTC through the ADT.

Contract: ADT /chosen "wom-wall-clock" = struct.pack("<QQ", local seconds since 1970,
the Air's CNTPCT at that moment); Mu's AppleSiliconPkg VirtualRealTimeClockLib reads it
once at init and adds the counter delta in GetTime.
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUN = ROOT / "run_uefi.py"
RTC = ROOT / "mu" / "Silicon" / "Apple" / "AppleSiliconPkg" / "Library" / "VirtualRealTimeClockLib"


def test_host_sets_the_property_before_the_adt_upload():
    text = RUN.read_text()
    set_at = text.index('hv.adt["chosen"].wom_wall_clock = _struct.pack("<QQ", _wall, _cnt)')
    assert set_at < text.index("hv.start()")  # hv.start() uploads the ADT
    assert "tm_gmtoff" in text  # local time: Windows keeps the RTC in local time
    assert "align(u.ba.devtree_size)" in text[set_at:text.index("hv.start()")]


def test_mu_reads_the_same_property():
    c = (RTC / "VirtualRealTimeClockLib.c").read_text()
    assert '"wom-wall-clock"' in c and "Size >= 16" in c
    assert c.count("ReadUnaligned64") == 2
    assert "EpochToEfiTime" in c and "EFI_UNSPECIFIED_TIMEZONE" in c
    # runtime path: no ADT/MMIO access in LibGetTime
    get_time = c[c.index("LibGetTime("):c.index("LibSetTime(")]
    assert "dt_" not in get_time and "MmioRead" not in get_time
    inf = (RTC / "VirtualRealTimeClockLib.inf").read_text()
    for lib in ("AppleDTLib", "TimeBaseLib", "BaseLib"):
        assert re.search(rf"^\s+{lib}\s*$", inf, re.M), lib
    assert "AppleSiliconPkg/AppleSiliconPkg.dec" in inf


def test_adt_attribute_name_and_layout():
    sys.path.insert(0, str(ROOT / "m1n1_windows" / "proxyclient"))
    from m1n1.adt import ADTNode, ADTNodeStruct

    def prop(name, val):
        return name.encode().ljust(32, b"\0") + struct.pack("<I", len(val)) + val + b"\0" * (-len(val) % 4)

    def node(props, children):
        return struct.pack("<II", len(props), len(children)) + b"".join(props) + b"".join(children)

    root = node([prop("name", b"device-tree\0")], [node([prop("name", b"chosen\0")], [])])
    adt = ADTNode(ADTNodeStruct.parse(root))
    adt["chosen"].wom_wall_clock = struct.pack("<QQ", 1794000000, 0x123456789A)
    blob = adt.build()
    i = blob.index(b"wom-wall-clock\0")
    assert struct.unpack("<I", blob[i + 32:i + 36]) == (16,)
    assert struct.unpack("<QQ", blob[i + 36:i + 52]) == (1794000000, 0x123456789A)
    del adt["chosen"].wom_wall_clock
    assert adt.build() == root
