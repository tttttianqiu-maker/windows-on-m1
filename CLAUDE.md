# Agent handoff: Windows 11 on the MacBook Air M3 (J613, T8122)

Read `AGENTS.md` first (source-first rules, experiment ledger, change ledger),
then **`docs/J613-HANDOFF.md`** (the complete history and technical detail).
This file is the operational state for an agent running **on the Hackintosh**
(Intel macOS, repo at `~/src/windows-on-m1`, branch `j613-bringup`).

## Owner rules

- **Ask before any disk or boot change on the Air** (partitions, formatting,
  boot files, BCD, firmware). Builds and assisted runs are fine.
- Commits: no assistant attribution trailers (AGENTS.md). Every hardware run
  gets an `investigation/EXPERIMENTS.md` entry before and after; every change a
  row in `investigation/CHANGES.csv` (`python3 -m pytest -q tests/test_change_ledger.py`).
- The owner wants short instructions and minimal manual steps.

## Physical setup

- Air boots its "Windows 11" boot-picker entry = Asahi stub + m1n1 -> standalone
  image fails preflight -> "Running proxy..." on USB (usb-drd0 port).
- After an m1n1 crash the Air reboots by itself back into proxy mode.
- Hackintosh sees two `/dev/cu.usbmodem*` (proxy, vuart).
- USB install stick (WinPE) is only for offline Windows work; WINSRC (exFAT on
  the Air's SSD) holds scripts, `install.wim`, and the `j613-jobs` mailbox.

## Commands (`scripts/j613`)

- `j613 watch`  watcher: runs the newest `~/Downloads/j613-*.tar.gz`, follow mode
- `j613 again`  re-run the current build (watcher must be running)
- `j613 log`    MTP/DockChannel lines of `hv.log`; `j613 hv` tail of `hv.log`
- `j613 probe`  read-only MTP power/ADT probe (Air at proxy, watcher stopped)
- `j613 status` next tarball the watcher will run + all switches (check before every run)
- `j613 mtp off|on`, `j613 screen on|off` (Windows on the Air's panel, build 39+),
  `j613 usb reset|normal` (experimental Linux-ordered guest USB reset, build 40+)
- Build (Linux or macOS with the cross toolchain):
  `BOARD=j613 sh scripts/build-development.sh --display virtual --debug uart`
  -> `dist/j613/debug-uart/` (MANIFEST.json, SHA256SUMS); package as
  `tar -czf ~/Downloads/j613-<name>-<n>.tar.gz -C dist/j613 debug-uart`.

## Building on the Hackintosh (Intel macOS) — no Docker

- **Use `j613 build <name>`** (`scripts/j613-build-m1n1.sh`): compiles m1n1
  natively and reuses the installed `J613_EFI.fd`; writes
  `~/Downloads/j613-<name>.tar.gz` for the watcher. Needs
  `brew install llvm lld rustup` (+ `rustup-init`,
  `rustup target add aarch64-unknown-none-softfloat`) and python3.
- **Do not set up Docker.** It is only needed if `mu/` changes (Mu last changed
  in c0cd337); the script refuses with a clear message in that case. All
  current work (trackpad SError) is m1n1-only.
- Commit before building so MANIFEST.json says `source_dirty: false`.
- CI artifacts without login and the trackpad firmware: see docs/J613-HANDOFF.md §3, §6.

## State (2026-11-09)

Working:
- SMP, Windows 11 Pro on the internal SSD (assisted, virtual display only;
  the physical DCP handoff is rejected by M3 DCP firmware, EXP-20261105-006).
- MTP keyboard/trackpad coprocessor in m1n1 (`m1n1_windows/src/hv_mtp.c`,
  `hv_dchid.c`): DAPF init, SRAM 0x2fac00000, OSLOG buffer reply, DockChannel
  HID, all interfaces; keyboard reports flow; trackpad firmware upload with
  power-method-1 fallback; haptic click works. Exposed to Windows as the AVHD
  device (IPA 0x5300000000, GSIV 65, ACPI `APPL0613`).
- Windows driver `drivers/apple-input/windows/AppleMtpInput` (CI job
  build-j613-mtp-arm64; host tests `tests/test_apple_input_j613.py`).

Open, in priority order:
1. **SErrors** (builds 35/36, also EL1 SErrors in WinPE): likely cause found
   2026-11-09 — m1n1 had no T8122 MCC support, so the TrustZone carveouts
   stayed mapped (m1n1 + guest stage 2); L2C_ERR_ADR 0x103fbbdc140 /
   0x103fc1f4600 is in the carveout area just below the top of the 16 GiB DRAM.
   Fixed in m1n1 90dbf4c (build 38), hardware check pending
   (EXP-20261109-048; handoff §7). Keep `j613 mtp off` and the trackpad
   firmware renamed (`firmware/tpmtfw-j613.bin` -> `.bin.off`) until a clean
   run, then re-enable one at a time.
2. Windows Setup is broken ("could not complete the installation" after a
   hard reset mid-Setup). Reinstall: `scripts/j613-postcfg-prep.sh --reinstall`
   on the Air arms a one-shot flag; WinPE reinstalls and applies settings,
   driver, test signing, no automatic repair, then powers off. Do it with
   build 38. The watcher always runs the NEWEST ~/Downloads/j613-*.tar.gz —
   check with `ls -t ~/Downloads/j613-*.tar.gz | head -1` before a run.
3. Keyboard/trackpad in Windows: EXP-049 (build 40) = keys reach m1n1, no
   input in Windows. m1n1 ae2af33 (CHG-20261109-010) sanitizes what the guest
   gets (Linux descriptor fixup, plain guest keyboard descriptor, ISO swap,
   trackpad report trim, queue fixes) and logs guest AVHD use ("dchid: avhd");
   m1n1 5df3acb adds the hid-apple Fn layer. EXP-053 (build 42): m1n1 side
   all OK (keyboard, trackpad 12194x7408, ISO), but Windows never started the
   driver (Setup left ACPI\APPL0613\0 at Code 28; DISM-staged package not
   retried). Next: EXP-054 with build 43 (Mu 4766076: AVHD _UID 0x613 = new
   devnode), docs/J613-TONIGHT.md; fallback scripts/j613-windows-diag.sh.
4. USB under Windows: cause understood (Linux dwc3-apple.c: one connect event
   per PHY+DWC3 init; eUSB2 repeater reset by the PD chip). Build 40 logs the
   AIC level of the xHCI IRQ and has `j613 usb reset` (Linux-ordered reset).
   Full write-up and the run order for everything else (screen, standalone,
   battery, Wi-Fi, audio, GPU): `docs/J613-RESEARCH-2026-11-09.md`.

## References in this repo / scratch

- Asahi `t8122.dtsi` (MTP, DART, DockChannel), Linux `drivers/soc/apple/rtkit.c`,
  `rtkit-helper.c`, `drivers/hid/dockchannel-hid/`, aurora J613 overlay
  (`power-method 2`, firmware-version specific; this Air's MTP firmware
  rejects it with 0xe00002c2).
