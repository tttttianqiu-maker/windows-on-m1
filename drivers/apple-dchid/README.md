# apple-dchid (superseded)

This was a Windows-side DockChannel HID protocol core for the J613 (MacBook
Air M3) built-in keyboard and trackpad. It is **superseded** by:

* `m1n1_windows/src/hv_dchid.c` + `dchid_proto.c`: m1n1 owns the MTP
  coprocessor, DockChannel, trackpad firmware upload and SMC GPIO power
  sequencing, and exposes HID reports through the virtual "AVHD" device.
* `drivers/apple-input/windows/AppleMtpInput.inf` (`ACPI\APPL0613`): the
  J613 transport of the shared Apple input driver (VHF keyboard + Windows
  Precision Touchpad), see `src/transport_avhd.c`.

Kept for reference only; nothing builds or installs it.
