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

## State (2026-11-08)

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
1. **SError after trackpad bring-up** (builds 35/36): EL2 SError, FAR at the
   debug UART 0x2a1200020 (just where m1n1 was), L2C_ERR_ADR points to RAM
   near m1n1 (0x103fbbdc140, 0x103fc1f4600). Not seen with keyboard only
   (builds 33/34). Hypotheses: MTP DMA (oslog buffer size: Linux treats the
   OSLOG size field as bytes, we allocate 16 KiB; trackpad firmware buffer;
   DAPF ranges vs m1n1 heap). Trackpad firmware is disabled for now by renaming
   `firmware/tpmtfw-j613.bin` -> `.bin.off` (runner skips a missing file).
2. Windows reinstall in progress (SYSTEM hive corrupt 0xc000014c after hard
   power cuts): `scripts/j613-postcfg-prep.sh --reinstall` on the Air arms a
   one-shot flag; WinPE reinstalls and applies settings, driver, test signing,
   no automatic repair, then powers off.
3. After that: verify the keyboard in Windows (AppleInputDiag.exe on WINSRC),
   then re-enable the trackpad once item 1 is fixed.
4. USB under Windows never sees a port connect (long-standing; see ledger).

## References in this repo / scratch

- Asahi `t8122.dtsi` (MTP, DART, DockChannel), Linux `drivers/soc/apple/rtkit.c`,
  `rtkit-helper.c`, `drivers/hid/dockchannel-hid/`, aurora J613 overlay
  (`power-method 2`, firmware-version specific; this Air's MTP firmware
  rejects it with 0xe00002c2).
