# AppleMtpInput: J613 (MacBook Air M3) built-in keyboard and trackpad

`AppleMtpInput.inf` binds to `ACPI\APPL0613`, the AVHD virtual device that m1n1
exposes (m1n1_windows/src/hv_dchid.h, Mu J613 DSDT `Device(AVHD)`).

Needs, on the m1n1 side:

* an m1n1/Mu build with hv_dchid (build 30 or later);
* `firmware/tpmtfw-j613.bin` on the host for the trackpad
  (`python3 tools/j613/extract-tpmtfw.py "/Volumes/EFI - WINDO"`);
  without it only the keyboard comes up.

Install (unsigned test build from CI artifact `AppleMtpInput-J613-ARM64-Debug`):

1. Windows test signing on (`bcdedit /set testsigning on`, reboot).
2. Copy `AppleMtpInput.inf`, `.sys`, `.cat` into `WINSRC:\j613-jobs\drivers\`;
   the startup mailbox runs `pnputil /add-driver *.inf /install`.

Check with `AppleInputDiag.exe`: `KeyboardReportAcceptedCount`,
`TrackpadReportSubmittedCount`, `FragmentFailureCount` (= m1n1 queue drops).
Host side, hv.log should show `dchid: bring-up done` and
`dchid: AVHD device at 0x5300000000, GSIV 65`.
