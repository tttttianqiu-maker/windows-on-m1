"""J613 Wi-Fi host side: WPA2 PMK, the network file, and the structs m1n1 takes
(tools/j613/wifi_net.py against m1n1_windows/src/brcm.h and wlan.h)."""
import re
import stat
import struct
from pathlib import Path

import pytest

from tools.j613 import wifi_net

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "m1n1_windows/src"


def c_struct_fields(header, name):
    text = (SRC / header).read_text()
    body = re.search(r"struct %s \{(.*?)\n\};" % name, text, re.S).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    fields = []
    for decl in body.split(";"):
        decl = decl.strip()
        if not decl:
            continue
        ctype, names = decl.split(None, 1)
        for n in names.split(","):
            n = n.strip()
            m = re.match(r"(\w+)(?:\[(\d+)\])?$", n)
            fields.append((ctype, m.group(1), int(m.group(2) or 1)))
    return fields


def test_pmk_ieee_80211i_vectors():
    # IEEE 802.11i-2004 Annex H.4 test vectors
    assert wifi_net.wpa2_pmk(b"IEEE", "password").hex() == \
        "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"
    assert wifi_net.wpa2_pmk(b"ThisIsASSID", "ThisIsAPassword").hex() == \
        "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af"
    hexkey = "00112233445566778899aabbccddeeff" * 2
    assert wifi_net.wpa2_pmk(b"x", hexkey) == bytes.fromhex(hexkey)


def test_fw_set_matches_brcm_h():
    fields = c_struct_fields("brcm.h", "brcm_fw_set")
    assert all(t == "u64" and n == 1 for t, _, n in fields)
    assert tuple(f for _, f, _ in fields) == wifi_net.FW_SET_FIELDS
    blob = wifi_net.pack_fw_set(fw=1, fw_len=2, cal=3, cal_len=4, mac=wifi_net.mac_u64(b"\x11\x22\x33\x44\x55\x66"))
    vals = struct.unpack("<14Q", blob)
    assert vals[0:2] == (1, 2) and vals[11:13] == (3, 4)
    assert vals[13].to_bytes(8, "little") == b"\x11\x22\x33\x44\x55\x66\0\0"
    with pytest.raises(ValueError):
        wifi_net.pack_fw_set(bogus=1)


def test_wlan_net_matches_wlan_h():
    fields = c_struct_fields("wlan.h", "wlan_net")
    assert [(t, f, n) for t, f, n in fields] == [
        ("u32", "magic", 1), ("u32", "ssid_len", 1), ("u8", "ssid", 32), ("u8", "pmk", 32),
        ("u32", "pass_len", 1), ("char", "pass", 64)]
    assert struct.calcsize(wifi_net.WLAN_NET_FMT) == 140
    assert "0x54454e57" in (SRC / "wlan.h").read_text().lower()
    blob = wifi_net.pack_wlan_net(b"IEEE", "password")
    magic, slen, ssid, pmk, plen, pw = struct.unpack(wifi_net.WLAN_NET_FMT, blob)
    assert magic == wifi_net.WLAN_NET_MAGIC == 0x54454E57
    assert (slen, ssid[:4], plen, pw[:8]) == (4, b"IEEE", 8, b"password")
    assert pmk.hex().startswith("f42c6fc5")
    # a 64-hex-digit key is the PMK itself; no passphrase goes along
    blob = wifi_net.pack_wlan_net(b"net", "ab" * 32)
    _, _, _, pmk, plen, _ = struct.unpack(wifi_net.WLAN_NET_FMT, blob)
    assert pmk == b"\xab" * 32 and plen == 0


@pytest.mark.parametrize("ssid,pw", [(b"", "password"), (b"x" * 33, "password"),
                                     (b"net", "short"), (b"net", "p" * 64),
                                     (b"net", "pässwörd123")])
def test_rejects_what_wpa2_personal_cannot_be(ssid, pw):
    with pytest.raises(ValueError):
        wifi_net.pack_wlan_net(ssid, pw)


def test_network_file_is_private(tmp_path):
    f = tmp_path / "firmware" / "wifi-net"
    wifi_net.save(str(f), "Café WLAN", "correct horse battery")
    assert stat.S_IMODE(f.stat().st_mode) == 0o600
    assert wifi_net.load(str(f)) == ("Café WLAN".encode(), "correct horse battery")
    assert wifi_net.main(["--file", str(f), "forget"]) == 0
    assert wifi_net.load(str(f)) is None


def test_show_never_prints_the_password(tmp_path, capsys):
    f = tmp_path / "wifi-net"
    wifi_net.save(str(f), "Home", "s3cret-passphrase")
    wifi_net.main(["--file", str(f), "show"])
    out = capsys.readouterr().out
    assert "Home" in out and "s3cret" not in out
