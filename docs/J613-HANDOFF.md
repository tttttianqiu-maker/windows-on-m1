# J613 handoff: Windows 11 on the MacBook Air M3 — everything so far

Written 2026-11-08 at the end of a long cloud-agent session, for the next agent
(running on the Hackintosh) and for the owner. Complements `AGENTS.md` (rules),
`CLAUDE.md` (short operational state), `investigation/EXPERIMENTS.md` (every
run, EXP ids) and `investigation/CHANGES.csv` (every change, CHG ids).
When this file and the ledgers disagree, the ledgers win.

---

## 1. Owner, goals, rules

- Owner: Tian (he/him), German/Chinese, casual and direct, wants **short
  instructions, minimal manual steps**, high-energy tone, facts looked up
  (not guessed) with sources.
- **Hard rule: ask before any disk or boot change on the Air** (partitions,
  formatting, boot files, BCD, firmware). Builds and assisted runs are fine.
  Approvals given so far (2026-11-07/08): WINSRC partition (Plan B), boot-file
  rename for WinPE runs, Windows test signing, chkdsk /f, boot policy
  (no automatic repair), **wipe + reinstall of the Windows partition only**.
- Goal list (owner's order of wishes): built-in keyboard + trackpad (now),
  then boot without the hypervisor/Hackintosh, sleep, battery, Wi-Fi,
  Bluetooth, audio, webcam, GPU. Also: install drivers without WinPE each time.
- Commits: no assistant attribution trailers (AGENTS.md). A Stop hook asked
  to rewrite/force-push already-pushed commits to change the author email; that
  was **declined** (would break `git pull` on the Hackintosh). Don't do it
  unless the owner asks.
- Two things the safety classifier blocked as "security weaken"; do not route
  around them, ask the owner explicitly if they become necessary:
  1. disabling UAC (replaced by the GPO startup-script mailbox),
  2. auto-adding the WDK test certificate to LocalMachine Root/TrustedPublisher
     (only needed for silent `pnputil` installs of test-signed drivers).
  Test signing itself was approved by the owner and is set (see §8).

## 2. Machines and disks

- **Air**: MacBook Air 13" M3 (J613, SoC T8122). macOS user `tqiu`.
  Boot picker entry "Windows 11" = Asahi-installer stub ESP ("EFI - WINDO",
  disk0s4, 500 MB) with `m1n1/boot.bin` + `vendorfw/firmware.cpio` +
  `asahi/all_firmware.tar.gz`. That m1n1 has a standalone image that fails
  its preflight (`PREFLIGHT FAIL checkpoint=PRE_HV_INIT field=11 ...`) and
  falls back to **"Running proxy..."** — this is normal and how every run
  starts. After an m1n1 crash the Air reboots back into proxy by itself.
- Air internal disk (disk0): macOS, `EFI - WINDO` (s4), **WINSRC** (exFAT,
  ~9 GB: `install.wim` in `sources/`, `j613-install.cmd`, `j613-jobs.cmd`,
  `oobe-unattend.xml`, `j613-jobs/` mailbox, logs), MSR, **WINESP** (s7,
  FAT32 512 MB, Windows boot files), **Windows** (NTFS ~171 GB).
- **USB stick** (WINBOOT + WINDATA): Windows 11 setup media whose WinPE
  (boot.wim index 2) was patched: `winpeshl.ini` -> `j613-launch.cmd` ->
  runs `j613-install.cmd` from WINSRC (scripts use only cmd built-ins; Setup's
  WinPE has no find/findstr).
- **Hackintosh** (Intel macOS, user `username`): repo `~/src/windows-on-m1`,
  Python venv `proxyenv/`, talks to the Air over USB (two `/dev/cu.usbmodem*`:
  first = proxy, second = vUART). Cable in the Air's usb-drd0 port.
- The cloud agent reached the Air through the Claude desktop "device bridge":
  Mac-side file tools work on granted folders (`/Volumes/WINSRC`, ...), but
  `device_bash` is a **Linux VM** (no diskutil, no /Volumes, no serial).
  The Hackintosh was never linked; a local Claude Code (or Cherry Studio
  agent mode with the work directory set to the repo) is the way to drive it.

## 3. Repositories

- `github.com/tttttianqiu-maker/windows-on-m1` (branch `j613-bringup`), with
  submodules `m1n1_windows` (hypervisor/bootloader) and `mu`
  (`apple_silicon_platforms_mu`, Project Mu UEFI). All on `j613-bringup`.
- J313 (M1 Air) is the original, hardware-validated target of this repo
  (paulsmir/windows-on-m1). Keep J313 sources byte-identical when possible.
- CI: `.github/workflows/apple-input-wdk.yml` builds the Windows input
  drivers (jobs build-arm64 = J313 AppleInput, build-j613-mtp-arm64 =
  AppleMtpInput). Actions had to be enabled on the fork (done 2026-11-07).
  Artifacts without login: `https://nightly.link/tttttianqiu-maker/windows-on-m1/actions/runs/<run>/<artifact>.zip`.
  Last green run: 42415908537.

## 4. Boot chain and architecture (assisted mode)

iBoot -> stub m1n1 (proxy) -> host `run_uefi.py` chainloads our m1n1 build
(`dist/j613/debug-uart/m1n1.macho`) -> m1n1 hypervisor -> Project Mu UEFI
(`J613_EFI.fd`) -> Windows Boot Manager on WINESP -> Windows 11 Pro.

- Display: **virtual only** (framebuffer streamed over USB to
  `extra/viewer.html`, ~0.5 fps). The physical DCP handoff is rejected by the
  M3 DCP firmware (EXP-20261105-006); fixing needs a real display driver.
- SMP: fixed (boot CPU MPIDR 0x10100 first in MADT, PSCI ALREADY_ON guard,
  first-match MPIDR lookup, fast IPI, `cyc_ovrd` so WFI keeps register state).
  T8122 chickens: `SLEEP_NONE, fast_ipi, cyc_ovrd`.
- USB for Windows: guest port usb-drd1 (xHCI 0xb02280000, AIC 1030 -> vINTID
  857), vGIC direct injection. **Still dead under Windows: PORTSC CCS never
  set** (TPS reports plug present/DFP, DWC3 host mode, PHY host bits tried;
  a PHY power cycle crashed the Air). UEFI/Mu USB works (keyboard/mouse in
  Setup). Long-standing open item, see ledger ~EXP-0261107-02x..039.
- Windows clock shows 01.04.2024 (no RTC handed over) — cosmetic.
- Sleep under Windows shut the machine down; postcfg/mailbox disable sleep,
  hibernate, screen-off, disk idle, lock screen.

## 5. Workflow and commands

`scripts/j613` (after `sh scripts/j613 setup`, just `j613`):

| command | what |
|---|---|
| `j613 watch` | watcher (`scripts/j613-autorun.sh`): installs the newest `~/Downloads/j613-*.tar.gz` (checks SHA256SUMS), waits for the Air at proxy, runs it; follow mode re-queues after PSCI reset |
| `j613 again` | request a re-run of the current build (`touch runs/.go`) |
| `j613 log` | MTP/DockChannel/RTKit lines from `hv.log` |
| `j613 hv` | last 60 lines of `hv.log` |
| `j613 probe` | read-only ADT/pmgr/MTP power probe (watcher stopped, Air at proxy) |
| `j613 build N` | **no-Docker build**: m1n1 natively, reuses the installed Mu FD (only if `mu/` unchanged) -> `~/Downloads/j613-N.tar.gz` |
| `j613 fw` | move `~/Downloads/tpmtfw-j613.bin` into `firmware/` |
| `j613 pull` | git pull + submodules |

- Run results: `runs/<timestamp>/` (console, `screen.png`), `hv.log` in the
  repo root. The run summary prints milestones, faults and the tail.
- Builds were made on Linux (cloud) with
  `BOARD=j613 sh scripts/build-development.sh --display virtual --debug uart`
  and shipped as tarballs. On the Hackintosh use `j613 build`; a full build
  including Mu needs Docker (`STANDALONE_BUILD_CONTAINER=always`), only
  necessary if `mu/` changes. Mu was last changed in c0cd337 (AVHD device).
- The runner verifies `MANIFEST.json` (`--require-role m1n1.macho=assisted-chainload`);
  always ship builds from clean commits (`source_dirty: false`).
- Pinned claude.ai artifact "J613 Command Card" lists these commands for the owner.

## 6. Built-in keyboard/trackpad: how the M3 does it

M1 (J313) uses SPI HID; **M3 uses the MTP coprocessor + DockChannel HID**.
Sources: Asahi `t8122.dtsi`, Linux `drivers/soc/apple/rtkit.c`,
`rtkit-helper.c`, `drivers/hid/dockchannel-hid/dockchannel-hid.c`, U-Boot
`apple_mtp_kbd.c`, the "aurora" M3 Linux tree (J613 overlay). Used under MIT.

Hardware (T8122):
- MTP ASC 0x2fa400000 (`CPU_CONTROL` +0x44, RUN = bit 4), mailbox
  0x2fa408000, **SRAM 0x2fac00000 + 1 MiB** (from the DT; the ADT's second
  `reg` 0x2fa050000+0x4000 is NOT the SRAM), MTP DART 0x2fa808000 stream 1
  (`/arm-io/dart-mtp`), DockChannel IRQ block 0x2fab14000, data window
  0x2fab28000 (rmt-config +0, rmt-data +0x4000, config +0x8000, data +0xc000,
  FIFO 0x800).
- pmgr devices MTP_FABRIC..MTP_DMA (ids 336-346) are all **"no ps"**; the
  MTP block is already powered (probe 20261108-101642). No power domain to
  enable from m1n1.

Bring-up sequence that works (m1n1 `src/hv_mtp.c`, `src/hv_dchid.c`,
`src/dchid_proto.c`, `src/rtkit.c`), in the order the problems were found:
1. iovad base aligned to 32 MiB (old silent failure).
2. SRAM window from the DT constant (build 31).
3. **DAPF**: `dapf_init("/arm-io/dart-mtp", 1)` before booting the ASC —
   upstream kboot does `dapf_init_all()` before Linux; without it the MTP never
   sends HELLO (build 33: first "MTP running").
4. **OSLOG buffer request** answered like Linux (type 63:56 = 1, size 55:36
   bytes, iova>>12 in 35:0; allocate + reply when iova 0). Without the reply
   the HID service never starts (build 34: keyboard reports flow).
5. DockChannel HID: INIT events per interface (stm 3, multi-touch 1,
   keyboard 2, actuator 4, tp_accel 5), enable after 50 ms quiet, STM first
   (vendor 0x05ac product 0x0357, keyboard type 2), GPIO requests
   (`stm-reset` #168, `afe-reset` #167).
6. Trackpad firmware `firmware/tpmtfw-j613.bin` (HIDF, 80448 bytes, sha256
   e16d15a7…, extracted from the Air's `vendorfw/firmware.cpio` with
   `tools/j613/extract-tpmtfw.py`; gitignored, never commit Apple firmware),
   staged by `run_uefi.py` (skipped if the file is missing), SEND_FIRMWARE
   0x95 via DART.
7. Reset: **power method 2 (9-byte, aurora "25G83" profile) is rejected by
   this Air's MTP firmware (0xe00002c2)** -> fall back to method 1
   `{0x40, 1, iface, state}` (Linux mainline); the MTP then asks for an
   AFE-reset pulse (GPIO cmd 3, line 8) which m1n1 drives through the SMC
   (key `gP08`, value CMD_OUTPUT|level), servicing GPIO + RTKit while waiting
   for READY (build 35: "trackpad ready", 12194 x 7408 (1/100 mm),
   x -5787..6256, y -157..7102; haptic click works).
8. SMC: open, write, `smc_shutdown()` per GPIO operation (upstream dcp.c
   pattern) — build 36.

Exposure to Windows: the **AVHD** virtual device (m1n1 `hv_dchid.h`):
4 KiB trapped MMIO page at IPA 0x5300000000, edge vSPI **65** (reserved:
AIC IRQ 65 is dropped, cannot be routed), always mapped on T8122 even if the
MTP fails (STATUS 0). Registers: MAGIC 'AVHD', VERSION, STATUS (bit0 kbd,
bit1 tp, bit2 MTP up, bit3 dims), IRQ_ACK, DESC_SEL/DESC_LEN/DESC window
0x400, RX_LEN/RX_IFACE/RX_POP/RX_DROPS, REPORT window 0x800, IDS, DIMS 0x40.
Mu DSDT `Device(AVHD)` `_HID "APPL0613"`.

Windows driver **AppleMtpInput** (`drivers/apple-input/`): reuses the J313
stack unchanged (VHF keyboard, Precision Touchpad VHF, decoder, tracker, PTP
encoder, axis/HID contracts, diagnostics/AppleInputDiag). New: `device_avhd.c`,
`transport_avhd.c` (DIRQL ISR + work item, readiness poll timer, keyboard
length normalization like Linux), `trackpad_pipeline.c` (copy of the J313
pipeline; J313 files untouched), protocol adapter `apple_mtp_adapt.c` (MTP
report 0x75 = J313 SPI report minus its 8-byte prefix; dims 0xd9). INF
`AppleMtpInput.inf` (`ACPI\APPL0613`, Vhf lower filter, Publish*=1).
Host tests: `tests/test_apple_input_j613.py` (adapter + full transport
simulation against a model of the m1n1 page). The PTP device stays silent
until Windows sets input mode 3 — expected.

## 7. Open problem #1: SErrors (likely cause found 2026-11-09)

- **Root-cause candidate (EXP-20261109-048, m1n1 90dbf4c):** `mcc_init()` had no
  `mcc,t8122` case, so on the M3 the TrustZone carveouts were never unmapped —
  neither from m1n1's EL2 page tables nor from the guest's stage 2 (proxyclient
  `unmap_carveouts()` got an empty list). Speculative accesses into a TZ region
  arrive as asynchronous SErrors. Both logged `L2C_ERR_ADR` values (low 42 bits
  0x103fbbdc140, 0x103fc1f4600) lie 64–68 MiB below the top of the 16 GiB DRAM
  (0x10400000000), in the carveout area above `mem_size`, **not** near m1n1's
  heap (~0x1005xxxxxxx) as first assumed below. `FAR 0x2a1200020` is stale
  (Mu's identity-mapped UART access trapped earlier; FAR is not valid for an
  SError). Fixed as upstream does it; the SError dump now says when
  `L2C_ERR_ADR` is inside a carveout. Hardware confirmation pending.
- Builds 35 and 36: bring-up completes, Windows boots for a while, then
  **EL2 SError**, `FAR 0x2a1200020`,
  `L2C_ERR_ADR` 0x3820103fbbdc140 / 0x3835103fc1f4600. Once it hit exactly at
  `WindowsAutoBootDxe: starting \EFI\BOOT\BOOTAA64.EFI`.
- Keyboard-only runs (builds 33/34, or 36 with the firmware file renamed) were
  not seen to crash. Trackpad is disabled for now:
  `firmware/tpmtfw-j613.bin` -> `.bin.off`.
- Older hypotheses (only if the TZ fix does not stop the SErrors): MTP DMA into a buffer it is not allowed to
  write or that is too small — OSLOG buffer (we allocate 16 KiB; check the
  size semantics against Linux carefully), the trackpad firmware buffer
  (memalign'd from the m1n1 heap, DART-mapped; the MTP may write runtime data
  there), DAPF allowed ranges vs. m1n1 heap addresses. Compare the
  L2C_ERR_ADR with the physical addresses of the buffers m1n1 hands out
  (log them), and try one variable at a time.
- These hard crashes repeatedly killed Windows mid-write; they are the likely
  cause of the corrupted SYSTEM hive (§8).

## 8. Windows install, postcfg, mailbox (WinPE side)

Scripts in `tools/j613/autoinstall/`; prep in `scripts/j613-postcfg-prep.sh`.

- **Plan B install** (2026-11-07): `j613-install.cmd` finds `install.wim`,
  resumes on volumes labelled WINESP/Windows (or creates partitions in the
  one free extent ≥100 GB), applies "Windows 11 Pro", `bcdboot`, copies
  `oobe-unattend.xml` (local admin "User", German keyboard/locale, English UI).
- **postcfg** (runs when a Windows install already exists):
  P1 offline registry (power scheme 381b4222… timeouts 0, hibernate off,
  PlatformAoAcOverride 0, NoLockScreen, logon powercfg fallback; on hive load
  failure: `chkdsk /f /x` + retry, else skip registry), mailbox files (GPO
  startup script), P3 boot policy (`bootstatuspolicy IgnoreAllFailures`,
  `recoveryenabled No`, delete bootstat.dat), P2 drivers from
  `WINSRC\j613-drivers` (DISM `/forceunsigned`) + `testsigning on` in the
  offline BCD; restores boot files, copies the log to `WINSRC\j613-postcfg.log`,
  `wpeutil shutdown`.
- **How a WinPE run is triggered**: on the Air in macOS, stick in:
  `curl -fsSL https://raw.githubusercontent.com/tttttianqiu-maker/windows-on-m1/<commit>/scripts/j613-postcfg-prep.sh -o /tmp/prep.sh && bash /tmp/prep.sh [--reinstall]`
  It prints the previous WinPE result, downloads the pinned install/jobs
  scripts to WINSRC, stages the CI driver (sha256-checked), and renames
  `bootmgfw.efi`/`BOOTAA64.EFI` on WINESP to `*.j613off` (sudo) so Mu boots
  the stick once. Then: shut down, stick in, `j613 again`, proxy; WinPE powers
  off when done.
- **Reinstall** (`--reinstall`): one-shot `WINSRC\j613-reinstall.flag`,
  consumed (renamed to `.used`) before anything is formatted; diskpart quick-
  formats only the "Windows" volume (not format.com — it can prompt), installs,
  then continues straight into postcfg on the clean hives. Latest prep pin:
  `d05bdb6`.
- **Mailbox**: `j613-jobs.cmd` as a local GPO machine startup script (SYSTEM,
  every boot): runs `WINSRC\j613-jobs\*.cmd` once (moved to `done\`), installs
  `j613-jobs\drivers\**\*.inf` with pnputil, logs to `j613-jobs\jobs.log`.
  Confirmed running (jobs.log entries 01.04.2024 1:15). Currently staged:
  `10-power-settings.cmd` (online powercfg/reg) and `drivers\AppleMtpInput\`.
  pnputil may refuse the untrusted test signer non-interactively (§1).

What happened (2026-11-08):
- postcfg run 1: `reg load SYSTEM` -> "configuration registry database is
  corrupt"; stopped safely.
- run 2: chkdsk ran, hive still failed (registry skipped), DISM add-driver
  failed 0x800703F1 (same hive), **testsigning Yes**, GPO mailbox files
  written, DONE.
- Then boots ended in Automatic Repair / "Choose an option" (no input), run 3
  added the boot policy, and finally Windows Recovery reported
  `\Windows\system32\config\system` **0xc000014c (STATUS_REGISTRY_CORRUPT)**.
- Owner approved a **wipe + reinstall of the Windows partition**. As of this
  writing the reinstall is armed via prep `d05bdb6 --reinstall` but **not yet
  run**. After it: first boot should show "Test Mode", the driver is injected
  offline, settings applied; then verify the keyboard in Windows.

## 9. Next steps (in order)

1. Run the reinstall with build 38 (TZ carveout fix) and `j613 mtp off`
   (owner: macOS prep `--reinstall`, stick run ~25 min, stick out, boot).
   Keep the trackpad firmware renamed during this. Then `j613 mtp on`
   (keyboard only) as the next single variable.
2. Verify the keyboard in Windows (Device Manager `ACPI\APPL0613`, Test Mode,
   `AppleInputDiag.exe` on WINSRC; m1n1 side: `j613 log`).
3. Fix §7 (m1n1-only, `j613 build`), then re-enable the trackpad.
4. Avoid hard power cuts while Windows runs (they corrupt the registry);
   prefer letting Windows shut down via a mailbox job when possible.
5. Later: USB under Windows (§4), standalone boot without the Hackintosh,
   physical display, power/battery, Wi-Fi/BT, audio, webcam.

## 10. Gotchas learned the hard way

- Never run dependent steps in parallel (a tarball was once uploaded before its
  rebuild finished).
- Proxy `hv_init` has a ~3 s timeout; slow bring-up belongs in `hv_start`.
- Windows' own chkdsk ("Fixing (C:) Stage 1") after hard stops is normal.
- WinPE: no find/findstr; an empty volume makes `dir` print "File Not Found"
  (write a marker file); format.com can prompt — use diskpart.
- `0xe00002c2` = IOKit kIOReturnBadArgument from the MTP firmware.
- The `unknown oslog message` lines after the buffer exists are just log
  traffic (now rate-limited).
- The owner's internet once died because of Proton VPN's kill switch.
- Script Editor notifications on the Hackintosh ("Cold boot the MacBook Air
  into Windows 11 now") come from the watcher.
- API cost reference (Claude Code docs): ~$13 per developer per active day on
  average, <$30 for 90% of users; use Sonnet for routine loops, `/clear` between
  tasks.

## 11. Ledger index (this phase)

EXP-20261107-039 desktop reached · EXP-20261107-040 input build 30 (RTKit
boot failed) · -041 SRAM (no HELLO) · -042 power domain (not applicable) ·
EXP-20261108-043 DAPF (MTP running) · -044 OSLOG (keyboard works, PM2
rejected) · -045 PM1 fallback (trackpad ready, SError) · -046 SMC shutdown
(SError persists) · -046b postcfg runs / registry corrupt / reinstall.
CHG-20261107-006..014, CHG-20261108-001..012.
