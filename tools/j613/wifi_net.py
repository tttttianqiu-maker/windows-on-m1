#!/usr/bin/env python3
"""J613 built-in Wi-Fi: the network to join and the structs m1n1 (wlan.c, brcm.h) takes.

`j613 wifi "<name>" ["<password>"]` stores the network in firmware/wifi-net (JSON, mode
0600, git-ignored with the rest of firmware/); run_uefi.py hands it to m1n1 as
struct wlan_net (P_HV_WIFI stage 2). The WPA2 PMK is derived here the way wpa_supplicant
does (IEEE 802.11i: PBKDF2-HMAC-SHA1(passphrase, SSID, 4096 rounds, 32 bytes)); the
passphrase goes along so the firmware can derive it itself if the PMK does not key.
Nothing here prints the password.
"""
import hashlib
import json
import os
import struct
import sys

WLAN_NET_MAGIC = 0x54454E57  # "WNET", wlan.h
WLAN_NET_FMT = "<II32s32sI64s"  # magic, ssid_len, ssid[32], pmk[32], pass_len, pass[64]
FW_SET_FIELDS = ("fw", "fw_len", "nvram", "nvram_len", "clm", "clm_len", "txcap",
                 "txcap_len", "sig", "sig_len", "seed", "cal", "cal_len", "mac")  # brcm.h


def wpa2_pmk(ssid, passphrase):
    """32-byte PMK for a WPA2-Personal network; a 64-hex-digit "password" is the PMK."""
    if len(passphrase) == 64 and all(c in "0123456789abcdefABCDEF" for c in passphrase):
        return bytes.fromhex(passphrase)
    return hashlib.pbkdf2_hmac("sha1", passphrase.encode("utf-8"), ssid, 4096, 32)


def check(ssid, passphrase):
    """Raise ValueError when the network cannot be a WPA2-Personal one."""
    if not 1 <= len(ssid) <= 32:
        raise ValueError(f"network name must be 1..32 bytes (is {len(ssid)})")
    raw = passphrase.encode("utf-8")
    hexkey = len(passphrase) == 64 and all(c in "0123456789abcdefABCDEF" for c in passphrase)
    if not hexkey and not 8 <= len(raw) <= 63:
        raise ValueError("WPA2 password must be 8..63 characters (or 64 hex digits)")
    if not hexkey and any(not 32 <= b <= 126 for b in raw):
        raise ValueError("WPA2 passwords are printable ASCII (IEEE 802.11i)")


def pack_wlan_net(ssid, passphrase):
    """struct wlan_net for P_HV_WIFI stage 2 (wlan.h)."""
    check(ssid, passphrase)
    pmk = wpa2_pmk(ssid, passphrase)
    hexkey = len(passphrase) == 64 and pmk == bytes.fromhex(passphrase)
    raw = b"" if hexkey else passphrase.encode("ascii")
    return struct.pack(WLAN_NET_FMT, WLAN_NET_MAGIC, len(ssid), ssid, pmk, len(raw), raw)


def pack_fw_set(**kw):
    """struct brcm_fw_set for P_HV_WIFI stage 1 (brcm.h): 14 little-endian u64."""
    unknown = set(kw) - set(FW_SET_FIELDS)
    if unknown:
        raise ValueError(f"unknown brcm_fw_set fields {sorted(unknown)}")
    return struct.pack("<14Q", *(int(kw.get(k, 0)) for k in FW_SET_FIELDS))


def mac_u64(mac):
    """6 MAC bytes in the low bytes of brcm_fw_set.mac (0 = keep the firmware's)."""
    return int.from_bytes((mac or b"")[:6].ljust(8, b"\0"), "little") if mac else 0


def load(path):
    """(ssid bytes, passphrase) from firmware/wifi-net, or None."""
    try:
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
        ssid = d["ssid"].encode("utf-8")
        check(ssid, d["password"])
        return ssid, d["password"]
    except (OSError, ValueError, KeyError, TypeError):
        return None


def save(path, ssid, passphrase):
    check(ssid.encode("utf-8"), passphrase)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump({"ssid": ssid, "password": passphrase}, f)
    os.chmod(path, 0o600)


def main(argv):
    import argparse
    import getpass
    ap = argparse.ArgumentParser(description="J613 Wi-Fi network for the next runs")
    ap.add_argument("--file", default="firmware/wifi-net")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("set", help="store the network (password prompted when left out)")
    s.add_argument("ssid")
    s.add_argument("password", nargs="?")
    sub.add_parser("show", help="which network the next run joins (no password)")
    sub.add_parser("forget", help="delete the stored network")
    a = ap.parse_args(argv)
    if a.cmd == "set":
        pw = a.password if a.password is not None else getpass.getpass(f"password for {a.ssid}: ")
        try:
            save(a.file, a.ssid, pw)
        except ValueError as e:
            print(f"not saved: {e}")
            return 1
        print(f"saved: the next runs join \"{a.ssid}\" (WPA2-Personal)")
    elif a.cmd == "show":
        net = load(a.file)
        print(f"Wi-Fi net  : \"{net[0].decode('utf-8', 'replace')}\"" if net else "Wi-Fi net  : none")
    elif a.cmd == "forget":
        try:
            os.remove(a.file)
        except FileNotFoundError:
            pass
        print("Wi-Fi network forgotten")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
