"""J613 input tooling: `j613 input` verdicts and the one-visit Windows diag/repair job."""
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIX = ROOT / "tests" / "fixtures" / "j613-input"
AUTO = ROOT / "tools" / "j613" / "autoinstall"
DIAG_SH = ROOT / "scripts" / "j613-windows-diag.sh"


def verdict(name):
    out = subprocess.run(["sh", str(ROOT / "scripts" / "j613-input.sh"), str(FIX / name)],
                         capture_output=True, text=True, check=True).stdout
    return out


def test_exp049_points_at_windows_side():
    out = verdict("exp049.txt")
    assert "[ok]   keyboard up in m1n1" in out
    assert "the Windows driver never touched the device" in out
    assert "j613-windows-diag.sh" in out


def test_success_path():
    out = verdict("success.txt")
    assert "[FAIL]" not in out
    for line in ("driver started", "keyboard descriptor loaded", "driver drains reports",
                 "driver loading the trackpad", "trackpad up"):
        assert line in out
    assert "m1n1 + driver path OK" in out


def test_serror_is_first():
    out = verdict("serror.txt")
    first_fail = out.index("[FAIL]")
    assert "SError" in out[first_fail:first_fail + 80]
    assert "tpmtfw-j613.bin.off" in out


def test_battery_section():
    out = verdict("battery.txt")
    assert "-- battery (experimental" in out
    assert "[ok]   m1n1 reads the SMC: #3 82% battery" in out
    assert "1 SMC reads kept back" in out
    assert "[ok]   Windows found the battery" in out
    assert "[ok]   Windows reads the charge level" in out
    assert "_BST 6 (last 25 s ago)" in out and "_EVT 2 of 2 events sent" in out
    assert "[ok]   instant updates: Windows connected the battery event interrupt" in out
    assert "[FAIL]" not in out
    # battery off: one info line, no section
    assert "-- battery" not in verdict("success.txt")


def test_wifi_connected():
    out = verdict("wifi-connected.txt")
    assert "-- built-in Wi-Fi" in out
    for line in ("PCIe link to the Wi-Fi chip is up", "Wi-Fi firmware is running",
                 "message rings set up", "control channel works", "Wi-Fi interface up",
                 "[ok]   CONNECTED to a0:b1:c2:d3:e4:f5, RSSI -52 dBm",
                 "last status: link up, rx 120 frames"):
        assert line in out
    assert "[FAIL]" not in out.split("-- built-in Wi-Fi")[1].split("\n-- ")[0]


def test_wifi_wrong_password():
    out = verdict("wifi-wrongpass.txt")
    assert "[FAIL] joined but the WPA2 handshake failed: password wrong?" in out
    assert "CONNECTED" not in out


def test_wifi_control_path_dead():
    out = verdict("wifi-noctl.txt")
    assert "[ok]   message rings set up" in out
    assert "[FAIL] the chip hit a DART fault" in out
    assert "[FAIL] no answer to the first ioctl" in out
    assert "joining" not in out


def test_helper_knows_input():
    assert "input) shift; exec sh scripts/j613-input.sh" in (ROOT / "scripts" / "j613").read_text()


def test_job_never_stops_windows_and_has_no_console_tools():
    for f in ("j613-input-diag.cmd", "j613-input-state.cmd", "j613-input-snapshot.cmd"):
        text = (AUTO / f).read_text().lower()
        assert "shutdown" not in text, f
        assert "timeout /t" not in text, f  # timeout.exe fails without a console
        assert "pause" not in text.split("rem")[0], f


def test_job_layout_matches_the_mailbox():
    diag = (AUTO / "j613-input-diag.cmd").read_text()
    # helpers live in lib\, which j613-jobs.cmd does not execute (top-level *.cmd only)
    assert '"%J%\\lib\\j613-input-state.cmd"' in diag
    assert '"%J%\\lib\\j613-input-snapshot.cmd"' in diag
    jobs = (AUTO / "j613-jobs.cmd").read_text()
    assert r"for %%f in (%J%\*.cmd)" in jobs


def test_mac_script_pins_existing_files():
    text = DIAG_SH.read_text()
    pins = re.findall(r"^PIN=(\S+)$", text, re.M)
    assert len(pins) == 1 and re.fullmatch(r"[0-9a-f]{40}", pins[0]), pins
    for f in re.findall(r"fetch (j613-[a-z-]+\.cmd)", text):
        assert (AUTO / f).exists(), f
    # same driver artifact + checksum as the prep
    prep = (ROOT / "scripts" / "j613-postcfg-prep.sh").read_text()
    for token in ("actions/runs/42415908537",
                  "83bc606c5b6de3d832c00c1ecdaa58d856c1c04346a28a58c2dbbe1c7fbf7666"):
        assert token in text and token in prep
    subprocess.run(["bash", "-n", str(DIAG_SH)], check=True)
