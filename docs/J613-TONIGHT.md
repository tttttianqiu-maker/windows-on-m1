# J613: keyboard + trackpad run, then battery (EXP-20261110-054, -055)

Two runs, about 10 minutes each. Everything happens on the Hackintosh except
where it says "on the Air".

## 0. Build and repo

1. Download the newest build into `~/Downloads` and delete older
   `j613-*.tar.gz` there: the watcher always runs the newest tarball.
   - `j613-safe-48.tar.gz` (m1n1 9602b57, Mu 511d7e0): use this one for
     every run below. Same as 47 plus an emergency off switch (below).
   - Already started run 1 with 44 or 45? Fine, finish it; use 47 after.
2. `j613 pull` (brings `j613 battery`, `j613 kbdlight`, `j613 input`, the
   clock hand-over and this file). If it complains about divergent branches:
   `git fetch && git reset --hard origin/j613-bringup && git submodule update
   --init` (firmware/ and runs/ are untracked and stay).

What is in it, compared with build 42 (EXP-053):
- The keyboard/trackpad ACPI device has a new `_UID`, so Windows sees new
  hardware and installs the AppleMtpInput driver it already has in its driver
  store (EXP-053: Windows never started the driver on the old device, which
  Setup had marked "no driver"). No WinPE pass, no macOS boot.
- Fn + top row = media keys.
- Windows shows the real date and time (it booted at January 2019 before).
- Switches, all off by default: battery % (`j613 battery on`), keyboard
  backlight (`j613 kbdlight on`), USB reset (`j613 usb reset`).

## 1. Run 1: keyboard + trackpad

```
cd ~/src/windows-on-m1
mv firmware/tpmtfw-j613.bin.off firmware/tpmtfw-j613.bin   # if it is still .off
j613 battery off
j613 status
```

Expected: next build `j613-safe-48`, MTP on, trackpad fw present, Air
screen on, USB reset off, battery off, kbdlight off, watcher running (`j613
watch` in its own Terminal if not).

`j613 again`, then put the Air into proxy mode as usual.

If you staged `diag-appl0613.cmd` on WINSRC earlier, the first Windows boot
runs it once and Windows shuts itself down about a minute after the desktop
("j613 diag done"). That is expected: `j613 again` once more.

### Try

Windows installs the driver for the "new" device in the background after boot:
give it about 2 minutes at the desktop (or the sign-in screen) before you judge
the keyboard. A "setting up a device" notification is a good sign.

- Keyboard: Cmd (⌘) is the Windows key, Option is Alt. Type into the Start
  search. Fn+Backspace = Delete, Fn+Enter = Insert, Fn+arrows = PgUp/PgDn/
  Home/End. On a German (ISO) keyboard `^` and `<` are where the keycaps say.
- Media keys (hold Fn): F7/F8/F9 previous/play-pause/next, F10/F11/F12
  mute/volume down/up (the volume pop-up shows even without a sound device),
  F4 search. F1/F2 brightness probably does nothing (virtual display).
  Without Fn the top row is F1-F12.
- Trackpad: move, click (press or tap), two-finger scroll, two-finger click =
  right click.
- Clock: the taskbar should show the real time (the Hackintosh's).

### Verdict

```
j613 input
```

Paste the output to Claude whatever happened.

## 2. Run 2: battery (only after run 1)

```
j613 battery on
j613 status          # battery: on (experimental)
j613 again
```

Proxy mode as usual. In Windows: a battery icon in the taskbar with the same
percentage macOS shows. Plug and unplug the charger once, wait a minute or two
each time (no instant notifications yet: Windows polls). Then `j613 input`
and paste it: the battery section says whether Windows found the battery and
how often it polls.

m1n1 reads the battery from the SMC (the Mac's power controller) read-only
every 60 s, the same values Linux reads. It never changes charging. If Windows
stutters every minute, or anything odd happens: `j613 battery off`.

## 3. Run 3: keyboard backlight (after run 1 works)

```
j613 battery off      # one thing at a time
j613 kbdlight on      # start level 3 of 8; `j613 kbdlight on 6` = brighter start
j613 again
```

The keys light up when Windows starts. Fn+F5 = darker, Fn+F6 = brighter (m1n1
handles these two, Windows never sees them while this is on). `j613 input`
shows the last level. If the Air resets or the log shows an SError right after
a `kbdlight:` line: `j613 kbdlight off` and paste the output.

## If something fails

| `j613 input` says | Do |
|---|---|
| `[FAIL] SError` with trackpad firmware on | `mv firmware/tpmtfw-j613.bin firmware/tpmtfw-j613.bin.off`, `j613 again` (keyboard only) |
| `keyboard not ready in m1n1` / `trackpad did not come up` | paste the output |
| `the Windows driver never touched the device` | Windows side: one macOS visit, below |
| everything `[ok]` but Windows ignores keys or the cursor | Windows HID side: one macOS visit, below |
| battery `[FAIL]` | `j613 battery off`, paste the output |
| Windows blue-screens or restarts over and over once the keyboard driver starts | `j613 mtp off`, `j613 again`: hides the keyboard device so the driver does not load (build 48+), then paste `j613 input` |

One macOS visit (on the Air, in macOS; prints what Windows recorded, then stages
a boot-time diagnose + repair job that never shuts Windows down):

```
curl -fsSL https://raw.githubusercontent.com/tttttianqiu-maker/windows-on-m1/09d68699a2f9b446e0ac7b11a5e36de6b9430e52/scripts/j613-windows-diag.sh -o /tmp/diag.sh && bash /tmp/diag.sh
```

Then shut down, `j613 again`, proxy mode. The job runs before the sign-in
screen, so if the repair works the keyboard works in that same boot. It also
installs a boot task that writes `j613-jobs/input-snapshot.txt` 2 minutes after
every boot. Next macOS visit: `bash /tmp/diag.sh --read` (or the curl line
again) shows the results.

## 4. Run 4: USB (EXP-20261109-052)

Separate run, one variable: `j613 kbdlight off`, `j613 usb reset`, plug a USB
mouse or a USB stick that is NOT the Windows install stick into the Air before
boot, `j613 again`. Expect the worst case to be a sudden Air reset (known
failure mode, no damage). Then `grep -a -E "xHCI|AIC 1030|usb" hv.log | tail
-60` and paste it. Build 47 adds the M3-only step Linux does when it powers the
USB2 PHY on, and logs the PHY's power state for the next fix.
