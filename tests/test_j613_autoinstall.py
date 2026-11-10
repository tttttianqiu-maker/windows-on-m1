"""Contracts for the J613 WinPE install/postcfg tooling (scripts/j613-postcfg-prep.sh,
tools/j613/autoinstall/). Regression tests for the 2026-11-09 review:
the prep never staged the first-boot answer file, so a reinstall could format the
Windows volume and then stop at OOBE pages that need input."""
import re
import unittest
import xml.dom.minidom
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PREP = ROOT / "scripts" / "j613-postcfg-prep.sh"
AUTO = ROOT / "tools" / "j613" / "autoinstall"


class PrepContract(unittest.TestCase):
    def setUp(self):
        self.text = PREP.read_text(encoding="utf-8")

    def test_pin_is_one_full_commit(self):
        pins = re.findall(r"^PIN=(\S+)$", self.text, re.M)
        self.assertEqual(len(pins), 1)
        self.assertRegex(pins[0], r"^[0-9a-f]{40}$")
        # every raw.githubusercontent download goes through the one pin
        self.assertNotRegex(self.text, r"windows-on-m1/[0-9a-f]{7,40}/tools")

    def test_stages_answer_file_with_the_scripts(self):
        self.assertIn("for f in j613-install.cmd j613-jobs.cmd oobe-unattend.xml", self.text)
        self.assertIn("<LocalAccount ", self.text)
        self.assertIn("oobe-unattend.xml missing", self.text)

    def test_no_driver_option_skips_staging(self):
        self.assertIn("--no-driver) DRIVER=0", self.text)
        # stale drivers from an earlier prep must not be injected
        before = self.text.index('if [ "$DRIVER" = 1 ]')
        self.assertIn("rm -rf /Volumes/WINSRC/j613-drivers", self.text[:before])

    def test_driver_staging_queues_the_install_job(self):
        # DISM only stages the package; the first boot must install it (EXP-053)
        driver = self.text.index('if [ "$DRIVER" = 1 ]')
        job = self.text.index("j613-input-diag.cmd from the mailbox")
        self.assertLess(driver, job)
        for f in ("j613-input-diag.cmd", "j613-input-state.cmd", "j613-input-snapshot.cmd"):
            self.assertIn(f, self.text)
            self.assertTrue((AUTO / f).exists(), f)
        self.assertIn("rm -f /Volumes/WINSRC/j613-jobs/no-repair.flag", self.text)

    def test_flags_are_one_shot_and_reset(self):
        self.assertIn("rm -f /Volumes/WINSRC/j613-reinstall.flag /Volumes/WINSRC/j613-fixsetup.flag",
                      self.text)


class InstallScriptContract(unittest.TestCase):
    def setUp(self):
        self.text = (AUTO / "j613-install.cmd").read_text(encoding="ascii")

    def test_reinstall_refuses_without_answer_file_before_format(self):
        guard = self.text.index("oobe-unattend.xml missing, refusing")
        fmt = self.text.index("format fs=ntfs quick label=Windows")
        consume = self.text.index("ren %STICK%\\j613-reinstall.flag")
        self.assertLess(guard, consume)
        self.assertLess(guard, fmt)

    def test_answer_file_copy_is_verified(self):
        self.assertIn("if not exist %TGT%\\Windows\\Panther\\unattend.xml", self.text)

    def test_never_cleans_or_deletes_partitions(self):
        lower = self.text.lower()
        self.assertNotIn("echo clean", lower)
        self.assertNotIn("delete partition", lower)


class AnswerFileContract(unittest.TestCase):
    def setUp(self):
        self.dom = xml.dom.minidom.parse(str(AUTO / "oobe-unattend.xml"))

    def oobe_components(self):
        for s in self.dom.getElementsByTagName("settings"):
            if s.getAttribute("pass") == "oobeSystem":
                return s.getElementsByTagName("component")
        self.fail("no oobeSystem pass")

    def test_local_account_and_autologon(self):
        names = [n.firstChild.data for n in self.dom.getElementsByTagName("Name")]
        self.assertIn("User", names)
        self.assertTrue(self.dom.getElementsByTagName("AutoLogon"))
        for tag in ("HideOnlineAccountScreens", "HideLocalAccountScreen", "HideEULAPage"):
            self.assertEqual(self.dom.getElementsByTagName(tag)[0].firstChild.data, "true")

    def test_no_ui_language_forced_in_oobe(self):
        for c in self.oobe_components():
            if c.getAttribute("name") == "Microsoft-Windows-International-Core":
                self.assertFalse(c.getElementsByTagName("UILanguage"))
                self.assertTrue(c.getElementsByTagName("InputLocale"))


if __name__ == "__main__":
    unittest.main()
