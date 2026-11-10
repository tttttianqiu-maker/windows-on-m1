# Windows 11 ARM64 on the M3 MacBook Air

Experimental native Windows 11 boot for the 2024 13" M3 MacBook Air (`j613`, Apple T8122),
built from a Project Mu UEFI firmware fork and an m1n1 EL2 hypervisor fork.

This is a fork of [arditspark/windows-on-m1](https://github.com/arditspark/windows-on-m1),
which got Windows 11 running on the M1 MacBook Air (`j313`). This fork ports that work to the M3.
The J313 sources are kept as close to upstream as possible.

> **Active work is on the [`j613-bringup`](https://github.com/tttttianqiu-maker/windows-on-m1/tree/j613-bringup) branch.** `main` is still
> the unmodified upstream J313 (M1) code plus this README.

> **Warning:** This project runs experimental firmware against the internal SSD. A wrong
> `diskpart` command can destroy macOS, Recovery, or the Asahi boot environment. Keep a full
> backup. This is not a general Apple Silicon Windows installer.

## Current status (2026-11-10)

Status labels follow the upstream rules: **validated** = seen working on the development Air,
**implemented** = in source and host-tested but not yet confirmed on hardware,
**planned** = not implemented yet.

### Validated on the J613

- Windows 11 Pro ARM64 installed to the internal SSD and booting to the desktop
  (assisted mode only, see below). OOBE runs unattended.
- All eight CPU cores (4 efficiency + 4 performance) brought up under the hypervisor.
- Display: Windows on a virtual framebuffer streamed to the host, and mirrored onto the
  Air's own panel (`j613 screen on`, build 39+). The M3 display coprocessor rejects a new
  surface, so there is no real display driver, no brightness control, and no GPU acceleration.
- Built-in keyboard and trackpad **on the m1n1 side**: the MTP coprocessor boots, keyboard
  reports arrive, the trackpad firmware loads and the haptic click works. ISO layout is detected.

### Implemented, hardware check pending

- Windows keyboard/trackpad driver (`AppleMtpInput`, ACPI `APPL0613`). The driver package
  installs, but Windows has not started it yet (Code 28 on the old devnode). Build 43+ gives
  the device a new `_UID` so PnP installs it fresh; that run is in progress.
- Fn layer and top-row media keys (build 44+).
- Battery and charger status in Windows from the SMC, read-only (`j613 battery on`, build 44+),
  with change events through an ACPI GED (build 49+).
- Correct Windows clock taken from the host (build 47+).
- Keyboard backlight with Fn+F5/F6 (`j613 kbdlight on`, build 47+).
- Fix for the SErrors seen with the trackpad: m1n1 now unmaps the T8122 TrustZone carveouts.

### Not working yet

- USB ports under Windows (they work in UEFI/Setup). Root cause understood, an ordered reset
  is implemented as `j613 usb reset`.
- Standalone boot without the host Mac (the on-disk image still falls back to proxy mode).
- Sleep (disabled, it shut the machine down), Wi-Fi (hardware probe and PCIe
  bring-up written, not run yet), Bluetooth, audio, webcam, GPU.

## How it runs (assisted mode)

iBoot → Asahi stub m1n1 (proxy over USB) → the host's `run_uefi.py` chainloads this m1n1
build → m1n1 hypervisor → Project Mu UEFI (`J613_EFI.fd`) → Windows Boot Manager → Windows.

A second Mac (here an Intel Hackintosh) sits on the Air's USB-C port, sends each build,
and captures UART, hypervisor logs, the framebuffer and Windows KD traffic.

## Building

```sh
git clone --recurse-submodules -b j613-bringup https://github.com/tttttianqiu-maker/windows-on-m1
cd windows-on-m1

# Full build (Linux, or macOS with the cross toolchain; Mu needs Docker on macOS)
BOARD=j613 sh scripts/build-development.sh --display virtual --debug uart
# -> dist/j613/debug-uart/ (MANIFEST.json, SHA256SUMS)

# m1n1-only build on macOS without Docker (reuses the installed Mu firmware)
sh scripts/j613 build <name>
```

The `scripts/j613` helper covers the day-to-day loop: `watch` (run the newest build),
`again`, `status`, `log`, `input`, and the experimental switches (`mtp`, `screen`, `usb`,
`battery`, `kbdlight`, `wifi`). See [`CLAUDE.md`](https://github.com/tttttianqiu-maker/windows-on-m1/blob/j613-bringup/CLAUDE.md).

## Documentation (on `j613-bringup`)

- [J613 handoff: full history and technical detail](https://github.com/tttttianqiu-maker/windows-on-m1/blob/j613-bringup/docs/J613-HANDOFF.md)
- [J613 research notes: USB, display, standalone, wishlist](https://github.com/tttttianqiu-maker/windows-on-m1/blob/j613-bringup/docs/J613-RESEARCH-2026-11-09.md)
- [Experiment ledger](https://github.com/tttttianqiu-maker/windows-on-m1/blob/j613-bringup/investigation/EXPERIMENTS.md) and
  [change ledger](https://github.com/tttttianqiu-maker/windows-on-m1/blob/j613-bringup/investigation/CHANGES.csv): every hardware run
  and every change, with EXP/CHG ids
- Upstream J313 docs: [`documentation/`](documentation/)

## Repositories

| Repo | Role | Branch |
| --- | --- | --- |
| [windows-on-m1](https://github.com/tttttianqiu-maker/windows-on-m1) | Build scripts, Windows drivers, host tools, docs | `j613-bringup` |
| [m1n1_windows](https://github.com/tttttianqiu-maker/m1n1_windows) | Hypervisor/bootloader (MTP, DockChannel HID, battery, mirror) | `j613-bringup` |
| [apple_silicon_platforms_mu](https://github.com/tttttianqiu-maker/apple_silicon_platforms_mu) | Project Mu UEFI firmware with the J613 platform | `j613-bringup` |

## Credits

Built on [arditspark/windows-on-m1](https://github.com/arditspark/windows-on-m1) (the M1 port),
[Asahi Linux](https://asahilinux.org/) (hardware documentation, `t8122` device trees, drivers),
[m1n1](https://github.com/AsahiLinux/m1n1), [Project Mu](https://github.com/microsoft/mu) and
[NT-for-ASi](https://github.com/NT-for-ASi). See the submodule histories and licenses for
their copyrights and terms.
