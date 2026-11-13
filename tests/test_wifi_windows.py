"""Windows side of the J613 built-in Wi-Fi: the macOS staging script for Red Hat's NetKVM
(scripts/j613-netkvm.sh) and the one-shot mailbox job + boot snapshot it stages
(tools/j613/autoinstall/j613-netkvm.cmd, j613-net-snapshot.cmd)."""
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AUTO = ROOT / "tools" / "j613" / "autoinstall"
SCRIPT = ROOT / "scripts" / "j613-netkvm.sh"
JOBS = ("j613-netkvm.cmd", "j613-net-snapshot.cmd", "j613-nopwexpire.cmd")


def test_jobs_never_stop_windows_and_need_no_console():
    for f in JOBS:
        raw = (AUTO / f).read_bytes()
        raw.decode("ascii")  # cmd.exe code page safety
        text = raw.decode().lower()
        assert "shutdown" not in text, f
        assert "timeout /t" not in text, f  # timeout.exe fails without a console
        assert "\npause" not in text, f


def test_job_trusts_only_a_valid_red_hat_signature():
    job = (AUTO / "j613-netkvm.cmd").read_text()
    trust = next(l for l in job.splitlines() if "Get-AuthenticodeSignature" in l)
    assert "$s.Status -ne 'Valid'" in trust and "-notmatch 'Red Hat'" in trust
    assert trust.index("exit") < trust.index("TrustedPublisher")
    assert "'Root'" not in trust  # never adds a root certificate
    assert 'pnputil /add-driver "%D%\\netkvm.inf" /install' in job
    assert 'set D=%W%\\j613-netkvm' in job
    # helpers live in lib\, which j613-jobs.cmd does not execute (top-level *.cmd only)
    assert '"%J%\\lib\\j613-net-snapshot.cmd"' in job
    assert '/deviceid "PCI\\VEN_1AF4&DEV_1041"' in job


def test_staging_script_pins_existing_job_files():
    text = SCRIPT.read_text()
    pins = re.findall(r"^PIN=(\S+)$", text, re.M)
    assert len(pins) == 1 and re.fullmatch(r"[0-9a-f]{40}", pins[0])
    assert not re.search(r"windows-on-m1/[0-9a-f]{7,40}/tools", text)
    have_pin = subprocess.run(["git", "-C", str(ROOT), "cat-file", "-e", pins[0] + "^{commit}"],
                              capture_output=True).returncode == 0  # shallow clones may lack it
    for f in JOBS:
        assert f"fetch {f} " in text
        if have_pin:  # the pinned commit has exactly the reviewed job files
            pinned = subprocess.run(
                ["git", "-C", str(ROOT), "show", f"{pins[0]}:tools/j613/autoinstall/{f}"],
                capture_output=True, check=True).stdout
            assert pinned == (AUTO / f).read_bytes(), f
    assert "NetKVM/w11/ARM64" in text
    assert "fedorapeople.org/groups/virt/virtio-win/direct-downloads/$CH/virtio-win.iso" in text
    subprocess.run(["bash", "-n", str(SCRIPT)], check=True)
