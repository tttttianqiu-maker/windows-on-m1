# Built-in Wi-Fi on the J613 (Broadcom 4388) — how it works and how to run it

Status (2026-11-11): implemented and host-tested, **not run on the Air yet**
(EXP-20261110-059 for the m1n1 side, EXP-20261110-060 for the Windows adapter).

## Why it is built this way

There is no Windows driver for Apple's Broadcom 4388 (Apple never shipped Boot Camp for
Apple Silicon, and Broadcom's Windows drivers are for other parts). So m1n1 runs the chip
itself, the way Linux brcmfmac does, and hands Windows a plain paravirtual NIC that already
has an ARM64 driver: virtio-net, driven by Red Hat's NetKVM (virtio-win). Asahi's own docs
use the same NIC + driver for Windows 11 VMs on Apple Silicon.

```
Windows: NetKVM (virtio-win NetKVM\w11\ARM64)
   |  virtio 1.x over PCI, INTA -> DSDT _PRT -> vINTID 67
m1n1: hv_vnet.c  (virtio-net 00:01.0 on hv_pci.c's emulated PCIe root)
   |  802.3 frames
m1n1: brcm_msgbuf.c (msgbuf rings, ioctls, events, RX/TX) + brcm_fwil.c (dongle init, WPA2 join)
   |  PCIe DMA through the pcie0 DART (stream 1)
m1n1: brcm.c (attach, OTP, firmware boot) + wlan.c (PCIe port 0 power/link)
   |
BCM4388 running Apple's firmware (copied from the Air's own macOS)
```

The chip associates with the network itself (firmware supplicant, WPA2-Personal). Windows
only sees an Ethernet adapter with the Wi-Fi MAC; its link goes up when the Wi-Fi is
associated and keyed.

## Running it (owner)

1. Hackintosh: `j613 pull`.
2. One visit in macOS on the Air:
   - `curl -fsSL https://raw.githubusercontent.com/tttttianqiu-maker/windows-on-m1/j613-bringup/scripts/j613-wifi-fw.sh -o /tmp/wifi-fw.sh && bash /tmp/wifi-fw.sh`
     (packs the Wi-Fi firmware to the Desktop; AirDrop it to the Hackintosh's ~/Downloads)
   - `curl -fsSL https://raw.githubusercontent.com/tttttianqiu-maker/windows-on-m1/j613-bringup/scripts/j613-netkvm.sh -o /tmp/netkvm.sh && bash /tmp/netkvm.sh`
     (downloads virtio-win, puts NetKVM on WINSRC, stages a one-shot install job)
3. Hackintosh: `j613 wififw`, then `j613 wifi "<network name>"` (asks for the password;
   stored in firmware/wifi-net, mode 0600, never logged; `j613 wifi forget` removes it).
4. Run the newest build as usual; afterwards `j613 input` has a "built-in Wi-Fi" and a
   "Wi-Fi adapter in Windows" section.
5. Next macOS visit: `bash /tmp/netkvm.sh --read` prints what Windows logged
   (netkvm.txt from the install job, net-snapshot.txt from every boot: adapter, IP, ping).

Switches: `j613 wifi off` (nothing Wi-Fi runs, no adapter for Windows), `j613 wifi probe`
(PCIe + chip only), `j613 wifi boot` (firmware + init, no join), `j613 wifisel` (offline:
which firmware files the next run picks).

## What the log says (hv.log)

| Line | Meaning |
| --- | --- |
| `wlan: link up after N ms`, `wlan: chip 4388 rev N` | PCIe link, chip id (stage 2) |
| `wlan: firmware started after N ms`, `wlan: fw: ...` | firmware running, its console |
| `wlan: msgbuf up: ...` | DMA rings set up |
| `wlan: MAC ...` | first ioctl answered: the control path works |
| `wlan: interface up: OK` | CLM/TxCap/calibration loaded, radio up |
| `wlan: CONNECTED to <bssid>, RSSI ...` | associated and keyed |
| `wlan: "<name>" was not found` | wrong name, out of range, hidden, 6 GHz-only, or not WPA2 |
| `handshake timeout (wrong password?)` | association OK, keys failed (after the PMK and the passphrase try) |
| `vnet: BAR0 at ...` | UEFI/Windows enumerated the adapter |
| `vnet: guest driver up` | NetKVM started |
| `vnet: guest driver up, link up, rx N ..., tx M ...` (every 60 s) | traffic both ways |
| `vnet: device error (...): NEEDS_RESET` | ring/address problem between NetKVM and m1n1 |
| `wlan: DART fault ...` | the chip DMA'd outside its pool |

## Known limits

- WPA2-Personal only (no WPA3/SAE, no enterprise). WPA2/WPA3 transition networks work
  through their WPA2 side. The join offers CCMP only; a network that still uses TKIP
  will refuse it.
- The network is chosen on the Hackintosh before the run; Windows cannot scan or switch
  networks (it sees a cable). Roaming between APs of the same network is the firmware's.
- Interrupts are polled by m1n1 every 1 ms (the chip has no interrupt wired to m1n1 here),
  so add up to ~1 ms of latency each way.
- No checksum or segmentation offloads; Windows does them in software.
- Country code is the firmware/CLM default (log line `wlan: country ...`).

## Source map

- m1n1: `src/wlan.c` (port power, PERST#, link, RID2SID), `src/brcm.c` (backplane, cores,
  OTP, firmware download + random seed, shared RAM), `src/brcm_msgbuf.c`,
  `src/brcm_fwil.c`, `src/hv_vnet.c`, `src/hv_pci.c` (00:01.0 dispatch),
  `src/hv_irq_routes.h` (vINTID 67).
- Mu: `Platform/MacBookAirM3J613Pkg/AcpiTables/DSDT.asl` (PCI0 `_PRT`).
- Host: `run_uefi.py` (Wi-Fi block), `tools/j613/wifi_fw.py` (firmware choice, NVRAM),
  `tools/j613/wifi_net.py` (network file, PMK, structs), `scripts/j613`,
  `scripts/j613-input.sh`, `scripts/j613-wifi-fw.sh`, `scripts/j613-netkvm.sh`,
  `tools/j613/autoinstall/j613-netkvm.cmd`, `j613-net-snapshot.cmd`.
- Tests: `tests/test_brcm_msgbuf.py` (driver vs a simulated dongle),
  `tests/test_vnet.py` (device model vs a NetKVM-style driver), `tests/test_wifi_net.py`,
  `tests/test_wifi_fw.py`, `tests/test_wifi_windows.py`, `tests/test_j613_input_tools.py`.
