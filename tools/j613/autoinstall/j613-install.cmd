@echo off
rem J613 hands-free install, called by autounattend.xml with the stick's drive letter.
rem Touches ONLY the one unallocated extent of >= 100 GB. Stops (and shuts nothing
rem down) on any doubt, leaving a log in X:\j613-install.log and on the stick.
setlocal enabledelayedexpansion
set STICK=%1
set LOG=X:\j613-install.log
echo J613 install %date% %time% stick=%STICK% > %LOG%

echo J613 STEP 1 find install.wim& echo J613 STEP 1 find install.wim >> %LOG%
rem --- install.wim may sit on the stick's second partition (WINDATA)
set WIM=
for %%d in (C D E F G H I J K L M N O P Q R T U V Y Z) do if exist %%d:\sources\install.wim set WIM=%%d:\sources\install.wim
if "%WIM%"=="" (echo no install.wim found >> %LOG% & goto fail)
echo wim=%WIM% >> %LOG%

echo J613 STEP 2 look for existing volumes& echo J613 STEP 2 look for existing volumes >> %LOG%
rem --- resume: partitions from an earlier run (labels WINESP + Windows) are reused.
rem     WinPE gives an EFI partition no drive letter, so assign S: / W: via diskpart.
echo list volume > X:\lv.txt
diskpart /s X:\lv.txt > X:\vols.txt
set VESP=
set VWIN=
rem (no find/findstr: Setup's WinPE may not have them; cmd built-ins only)
for /f "usebackq tokens=*" %%L in (`type X:\vols.txt`) do (
  set "LINE=%%L"
  if not "!LINE:WINESP=!"=="!LINE!" for /f "tokens=2" %%n in ("!LINE!") do set VESP=%%n
  if not "!LINE: Windows =!"=="!LINE!" if not "!LINE:NTFS=!"=="!LINE!" for /f "tokens=2" %%n in ("!LINE!") do set VWIN=%%n
)
if defined VESP if defined VWIN (
  type X:\vols.txt >> %LOG%
  echo resuming on existing volumes ESP=%VESP% Windows=%VWIN% >> %LOG%
  (
  echo select volume %VESP%
  echo remove noerr
  echo assign letter=S noerr
  echo select volume %VWIN%
  echo remove noerr
  echo assign letter=W noerr
  ) > X:\as.txt
  diskpart /s X:\as.txt >> %LOG% 2>&1
  set TGT=W:
  set ESP=S:
  goto haveparts
)

echo J613 STEP 3 find free disk space& echo J613 STEP 3 find free disk space >> %LOG%
rem --- find the one disk with >= 100 GB free
echo list disk > X:\ld.txt
diskpart /s X:\ld.txt > X:\disks.txt
type X:\disks.txt >> %LOG%
set DISK=
set COUNT=0
for /f "usebackq tokens=1,2,3,4,5,6,7" %%a in (`type X:\disks.txt`) do (
  if "%%a"=="Disk" if "%%c"=="Online" if "%%g"=="GB" if %%f GEQ 100 (set DISK=%%b& set /a COUNT+=1)
)
if not "%COUNT%"=="1" (echo expected exactly one disk with ^>=100 GB free, found %COUNT% >> %LOG% & goto fail)
echo target disk %DISK% >> %LOG%

echo J613 STEP 4 create partitions& echo J613 STEP 4 create partitions >> %LOG%
rem --- partitions inside the free extent only (no clean, no delete, no format of existing)
(
echo select disk %DISK%
echo create partition msr size=16
echo create partition efi size=512
echo format quick fs=fat32 label=WINESP
echo assign letter=S
echo create partition primary
echo format quick fs=ntfs label=Windows
echo assign letter=W
echo list partition
echo list volume
) > X:\dp.txt
diskpart /s X:\dp.txt >> %LOG% 2>&1
set TGT=W:
set ESP=S:

:haveparts
if not exist %TGT%\ (echo %TGT% missing >> %LOG% & goto fail)
if not exist %ESP%\ (echo %ESP% missing >> %LOG% & goto fail)
rem --- owner-requested reinstall (2026-11-08: SYSTEM hive corrupt after hard
rem     power cuts). One-shot: the flag on WINSRC is consumed before formatting,
rem     so a later run can never wipe again. Only the "Windows" NTFS volume.
if not exist %STICK%\j613-reinstall.flag goto noreinstall
if /i "%TGT%"=="%STICK%" (echo reinstall: target is the stick, refusing >> %LOG% & goto fail)
rem without the answer file the first boot stops at OOBE pages that need input
if not exist %STICK%\oobe-unattend.xml (echo reinstall: %STICK%\oobe-unattend.xml missing, refusing >> %LOG% & goto fail)
echo J613 STEP R reinstall: formatting %TGT% (Windows volume)& echo J613 STEP R reinstall: formatting %TGT% >> %LOG%
if exist %STICK%\j613-reinstall.used del /f %STICK%\j613-reinstall.used
ren %STICK%\j613-reinstall.flag j613-reinstall.used
if exist %STICK%\j613-reinstall.flag (echo reinstall: cannot consume the flag, refusing >> %LOG% & goto fail)
rem diskpart, not format.com: format may stop and ask for the volume label
(echo select volume %TGT:~0,1%& echo format fs=ntfs quick label=Windows& echo assign letter=%TGT:~0,1% noerr) > X:\fmt.txt
diskpart /s X:\fmt.txt >> %LOG% 2>&1
if exist %TGT%\Windows\System32\winload.efi (echo reinstall: format failed >> %LOG% & goto fail)
:noreinstall
if exist %TGT%\Windows\System32\winload.efi goto postcfg
echo J613 STEP 5 check target size& echo J613 STEP 5 check target size >> %LOG%
rem an empty root makes dir print only "File Not Found", so give it one file
echo j613> %TGT%\j613.tmp
set FREE=0
for /f "usebackq tokens=1,2,3,4,5" %%a in (`dir /-c %TGT%\`) do if "%%e"=="free" set FREE=%%c
echo %TGT% free bytes %FREE% >> %LOG%
set FREEGB=%FREE:~0,-9%
if "%FREEGB%"=="" set FREEGB=0
if %FREEGB% LSS 90 (echo %TGT% is under 90 GB, wrong volume? >> %LOG% & goto fail)

echo J613 STEP 6 apply Windows 11 Pro (10-20 min)& echo J613 STEP 6 apply Windows 11 Pro (10-20 min) >> %LOG%
rem --- Windows 11 Pro
dism /Apply-Image /ImageFile:%WIM% /Name:"Windows 11 Pro" /ApplyDir:%TGT%\ >> %LOG% 2>&1
if errorlevel 1 (echo dism by name failed, trying index 6 >> %LOG% & dism /Apply-Image /ImageFile:%WIM% /Index:6 /ApplyDir:%TGT%\ >> %LOG% 2>&1)
if not exist %TGT%\Windows\System32\winload.efi (echo apply failed >> %LOG% & goto fail)

echo J613 STEP 7 boot files& echo J613 STEP 7 boot files >> %LOG%
rem --- boot files
bcdboot %TGT%\Windows /s %ESP% /f UEFI /v >> %LOG% 2>&1
mkdir %ESP%\EFI\BOOT
copy /y %ESP%\EFI\Microsoft\Boot\bootmgfw.efi %ESP%\EFI\BOOT\BOOTAA64.EFI >> %LOG%
if not exist %ESP%\EFI\Microsoft\Boot\BCD (echo BCD missing >> %LOG% & goto fail)

echo J613 STEP 8 first-boot answer file& echo J613 STEP 8 first-boot answer file >> %LOG%
rem --- click-free first boot
mkdir %TGT%\Windows\Panther
copy /y %STICK%\oobe-unattend.xml %TGT%\Windows\Panther\unattend.xml >> %LOG%
if not exist %TGT%\Windows\Panther\unattend.xml (echo answer file missing: OOBE would need input >> %LOG% & goto fail)

del %TGT%\j613.tmp 2>nul
echo J613 install DONE >> %LOG%
copy /y %LOG% %STICK%\ >nul
copy /y %LOG% %ESP%\ >nul
rem straight on to the offline settings / driver / boot policy on the fresh,
rem clean hives; postcfg shuts down when it is done
goto postcfg

rem ===== installed Windows found: offline settings for a machine without input =====
:postcfg
echo J613 STEP P1 offline settings: no sleep, hibernate, screen off, disk idle, lock screen& echo J613 STEP P1 offline settings >> %LOG%
rem Hard power cuts leave the volume/hives dirty (EXP-20261108-046: "registry
rem database is corrupt"). Owner-approved repair: chkdsk /f once, as Windows'
rem own autochk does, then retry. If the hives still do not load, skip only
rem the registry settings; the mailbox files and drivers do not need them.
set REGOK=1
fsutil dirty query %TGT% >> %LOG% 2>&1
dir /a %TGT%\Windows\System32\config\SYSTEM* %TGT%\Windows\System32\config\SOFTWARE* %TGT%\hiberfil.sys >> %LOG% 2>&1
reg load HKLM\J613SYS %TGT%\Windows\System32\config\SYSTEM >> %LOG% 2>&1
if not errorlevel 1 goto sysloaded
echo reg load SYSTEM failed, running chkdsk /f %TGT%& echo reg load SYSTEM failed, running chkdsk /f %TGT% >> %LOG%
chkdsk %TGT% /f /x >> %LOG% 2>&1
reg load HKLM\J613SYS %TGT%\Windows\System32\config\SYSTEM >> %LOG% 2>&1
if not errorlevel 1 goto sysloaded
echo WARNING reg load SYSTEM failed after chkdsk, skipping registry settings >> %LOG%
set REGOK=0
goto noreg
:sysloaded
reg load HKLM\J613SW %TGT%\Windows\System32\config\SOFTWARE >> %LOG% 2>&1
if not errorlevel 1 goto swloaded
reg unload HKLM\J613SYS >nul 2>&1
echo WARNING reg load SOFTWARE failed, skipping registry settings >> %LOG%
set REGOK=0
goto noreg
:swloaded
set PW=HKLM\J613SYS\ControlSet001\Control\Power
reg add %PW% /v HibernateEnabled /t REG_DWORD /d 0 /f >> %LOG% 2>&1
reg add %PW% /v HibernateEnabledDefault /t REG_DWORD /d 0 /f >> %LOG% 2>&1
reg add %PW% /v PlatformAoAcOverride /t REG_DWORD /d 0 /f >> %LOG% 2>&1
reg add %PW%\User\PowerSchemes /v ActivePowerScheme /t REG_SZ /d 381b4222-f694-41f0-9685-ff5bb260df2e /f >> %LOG% 2>&1
rem display off, console-lock display off, sleep after, hibernate after, disk off: all 0 = never
for %%k in (7516b95f-f776-4464-8c53-06167f40cc99\3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e 7516b95f-f776-4464-8c53-06167f40cc99\8ec4b3a5-6868-48c2-be75-4f3044be88a7 238c9fa8-0aad-41ed-83f4-97be242c8f20\29f6c1db-86da-48c5-9fdb-f2b67b1f44da 238c9fa8-0aad-41ed-83f4-97be242c8f20\9d7815a6-7ee4-497e-8888-515a05f02364 0012ee47-9041-4b5d-9b77-535fba8b1442\6738e2c4-e8a5-4a42-b16a-e040e769756e) do (
  reg add "%PW%\User\PowerSchemes\381b4222-f694-41f0-9685-ff5bb260df2e\%%k" /v ACSettingIndex /t REG_DWORD /d 0 /f >> %LOG% 2>&1
  reg add "%PW%\User\PowerSchemes\381b4222-f694-41f0-9685-ff5bb260df2e\%%k" /v DCSettingIndex /t REG_DWORD /d 0 /f >> %LOG% 2>&1
)
rem --- opt-in (prep --fix-setup, owner-approved 2026-11-08): first-boot Setup
rem     died with "The computer restarted unexpectedly"; ChildCompletion
rem     setup.exe=3 lets Windows Setup continue. One-shot flag on WINSRC.
if not exist %STICK%\j613-fixsetup.flag goto nofixsetup
echo J613 STEP P1b setup resume (ChildCompletion setup.exe=3)& echo J613 STEP P1b setup resume >> %LOG%
reg add HKLM\J613SYS\Setup\Status\ChildCompletion /v setup.exe /t REG_DWORD /d 3 /f >> %LOG% 2>&1
reg query HKLM\J613SYS\Setup\Status\ChildCompletion >> %LOG% 2>&1
if exist %STICK%\j613-fixsetup.used del /f %STICK%\j613-fixsetup.used
ren %STICK%\j613-fixsetup.flag j613-fixsetup.used
:nofixsetup
goto regpart2
:noreg
:regpart2
rem mailbox: j613-jobs.cmd as a local Group Policy machine startup script (SYSTEM)
copy /y %STICK%\j613-jobs.cmd %TGT%\Windows\j613-jobs.cmd >> %LOG% 2>&1
mkdir %TGT%\Windows\System32\GroupPolicy\Machine\Scripts\Startup 2>nul
copy /y %STICK%\j613-jobs.cmd %TGT%\Windows\System32\GroupPolicy\Machine\Scripts\Startup\j613-jobs.cmd >> %LOG% 2>&1
(echo [Startup]& echo 0CmdLine=j613-jobs.cmd& echo 0Parameters=) > %TGT%\Windows\System32\GroupPolicy\Machine\Scripts\scripts.ini
attrib +h %TGT%\Windows\System32\GroupPolicy\Machine\Scripts\scripts.ini
(echo [General]& echo gPCMachineExtensionNames=[{42B5FAAE-6536-11D2-AE5A-0000F87571E3}{40B6664F-4972-11D1-A7CA-0000F87571E3}]& echo Version=1) > %TGT%\Windows\System32\GroupPolicy\gpt.ini
if not exist %STICK%\j613-jobs mkdir %STICK%\j613-jobs
if "%REGOK%"=="0" goto skipreg2
reg add HKLM\J613SW\Policies\Microsoft\Windows\Personalization /v NoLockScreen /t REG_DWORD /d 1 /f >> %LOG% 2>&1
reg add HKLM\J613SW\Microsoft\Windows\CurrentVersion\Run /v J613Power /t REG_SZ /d "cmd /c powercfg /change monitor-timeout-ac 0 & powercfg /change monitor-timeout-dc 0 & powercfg /change standby-timeout-ac 0 & powercfg /change standby-timeout-dc 0 & powercfg /change disk-timeout-ac 0 & powercfg /change disk-timeout-dc 0 & powercfg /h off" /f >> %LOG% 2>&1
reg query "%PW%\User\PowerSchemes\381b4222-f694-41f0-9685-ff5bb260df2e\238c9fa8-0aad-41ed-83f4-97be242c8f20\29f6c1db-86da-48c5-9fdb-f2b67b1f44da" >> %LOG% 2>&1
reg unload HKLM\J613SW >> %LOG% 2>&1
reg unload HKLM\J613SYS >> %LOG% 2>&1
:skipreg2
rem ===== boot policy: hypervisor crashes are not Windows boot failures =====
rem Without input, WinRE's "Choose an option" is a dead end (2026-11-08), so
rem never enter automatic repair. Owner-approved.
echo J613 STEP P3 boot policy: no automatic repair& echo J613 STEP P3 boot policy >> %LOG%
bcdedit /store %ESP%\EFI\Microsoft\Boot\BCD /set {default} bootstatuspolicy IgnoreAllFailures >> %LOG% 2>&1
bcdedit /store %ESP%\EFI\Microsoft\Boot\BCD /set {default} recoveryenabled No >> %LOG% 2>&1
if exist %ESP%\EFI\Microsoft\Boot\bootstat.dat del /f %ESP%\EFI\Microsoft\Boot\bootstat.dat >> %LOG% 2>&1

rem ===== optional: drivers from WINSRC\j613-drivers (CI test builds) =====
rem Owner-approved (2026-11-08): test signing so the WDK test-signed
rem AppleMtpInput driver loads; Mu has Secure Boot off. Injected offline like
rem OEM drivers. Undo later with: bcdedit /set {default} testsigning off
if not exist %STICK%\j613-drivers\ goto nodrivers
echo J613 STEP P2 offline drivers + test signing& echo J613 STEP P2 offline drivers + test signing >> %LOG%
dism /image:%TGT%\ /add-driver /driver:%STICK%\j613-drivers /recurse /forceunsigned >> %LOG% 2>&1
if errorlevel 1 echo WARNING dism add-driver failed >> %LOG%
bcdedit /store %ESP%\EFI\Microsoft\Boot\BCD /set {default} testsigning on >> %LOG% 2>&1
if errorlevel 1 echo WARNING bcdedit testsigning failed >> %LOG%
bcdedit /store %ESP%\EFI\Microsoft\Boot\BCD /enum {default} >> %LOG% 2>&1
dism /image:%TGT%\ /get-drivers >> %LOG% 2>&1
if exist %STICK%\j613-drivers-done rmdir /s /q %STICK%\j613-drivers-done
move %STICK%\j613-drivers %STICK%\j613-drivers-done >> %LOG% 2>&1
:nodrivers
call :restoreboot
echo J613 postcfg DONE >> %LOG%
copy /y %LOG% %STICK%\j613-postcfg.log >nul
copy /y %LOG% %ESP%\j613-postcfg.log >nul
echo J613 settings written, boot files restored. Shutting down: pull the stick, then boot normally.
ping -n 6 127.0.0.1 >nul
wpeutil shutdown
exit /b 0

rem boot files renamed in macOS so the firmware boots the stick; put them back
:restoreboot
if not defined ESP exit /b 0
if exist %ESP%\EFI\Microsoft\Boot\bootmgfw.efi.j613off ren %ESP%\EFI\Microsoft\Boot\bootmgfw.efi.j613off bootmgfw.efi
if exist %ESP%\EFI\BOOT\BOOTAA64.EFI.j613off ren %ESP%\EFI\BOOT\BOOTAA64.EFI.j613off BOOTAA64.EFI
if exist %ESP%\EFI\Microsoft\Boot\bootmgfw.efi (echo boot files restored >> %LOG%) else (echo WARNING bootmgfw.efi missing on %ESP% >> %LOG%)
exit /b 0

:fail
call :restoreboot
echo J613 install STOPPED, nothing else was done >> %LOG%
copy /y %LOG% %STICK%\ >nul
start notepad %LOG%
exit /b 1
