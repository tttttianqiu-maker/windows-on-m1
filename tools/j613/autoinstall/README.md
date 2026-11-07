# J613 hands-free install (needs working USB inside Windows)

Copy these three files to the **root of the WINBOOT partition** of the install stick:

- `autounattend.xml`: Setup reads it; the windowsPE pass runs only `j613-install.cmd`.
- `j613-install.cmd`: finds `sources\install.wim` (WINDATA), picks the one disk with
  >= 100 GB unallocated, creates MSR + 512 MB ESP + NTFS **inside that free extent only**,
  applies Windows 11 Pro, runs bcdboot, copies the fallback BOOTAA64.EFI and
  `oobe-unattend.xml`, logs to the stick, reboots. On any doubt it stops and opens the log.
- `oobe-unattend.xml`: first boot without clicks, local admin "User" without password,
  German keyboard/locale, English UI.

No `clean`, `delete` or format of an existing partition is ever issued.
