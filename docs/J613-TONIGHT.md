# J613: keyboard + trackpad run (EXP-20261109-053)

One run, about 10 minutes. Everything happens on the Hackintosh except where it
says "on the Air".

## 0. Build and repo

1. Download `j613-kbdfix-42.tar.gz` into `~/Downloads`. If `j613-kbdfix-41.tar.gz`
   is there too, delete it: the watcher always runs the newest tarball.
2. `j613 pull` (brings `j613 input` and this file).

Build 42 = m1n1 5df3acb + Mu c0cd337 (same Mu as builds 40/41). The Windows
driver is the one already installed; no WinPE pass, no macOS boot.

## 1. Switches

```
cd ~/src/windows-on-m1
mv firmware/tpmtfw-j613.bin.off firmware/tpmtfw-j613.bin
j613 status
```

Expected: next build `j613-kbdfix-42`, MTP on, trackpad fw present, Air screen on,
USB reset off, watcher running (`j613 watch` in its own Terminal if not).

## 2. Run

`j613 again`, then put the Air into proxy mode as usual.

If you staged `diag-appl0613.cmd` on WINSRC last night, the first Windows boot
runs it once and Windows shuts itself down about a minute after the desktop
("j613 diag done"). That is expected: `j613 again` once more.

## 3. Try

- Keyboard: Cmd (⌘) is the Windows key, Option is Alt. Type into the Start
  search. Fn+Backspace = Delete, Fn+Enter = Insert, Fn+arrows = PgUp/PgDn/
  Home/End. On a German (ISO) keyboard `^` and `<` are where the keycaps say.
- Trackpad: move, click (press or tap), two-finger scroll, two-finger click =
  right click.
- F1-F12 are plain F-keys (no brightness/volume yet).

## 4. Verdict

```
j613 input
```

Paste the output to Claude whatever happened.

## If something fails

| `j613 input` says | Do |
|---|---|
| `[FAIL] SError` with trackpad firmware on | `mv firmware/tpmtfw-j613.bin firmware/tpmtfw-j613.bin.off`, `j613 again` (keyboard only) |
| `keyboard not ready in m1n1` / `trackpad did not come up` | paste the output |
| `the Windows driver never touched the device` | Windows side: one macOS visit, below |
| everything `[ok]` but Windows ignores keys or the cursor | Windows HID side: one macOS visit, below |

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
