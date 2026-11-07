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
if exist %TGT%\Windows\System32\winload.efi (echo Windows is already installed on %TGT%, nothing to do. Remove the stick. >> %LOG% & goto fail)
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

del %TGT%\j613.tmp 2>nul
echo J613 install DONE >> %LOG%
copy /y %LOG% %STICK%\ >nul
copy /y %LOG% %ESP%\ >nul
wpeutil reboot
exit /b 0

:fail
echo J613 install STOPPED, nothing else was done >> %LOG%
copy /y %LOG% %STICK%\ >nul
start notepad %LOG%
exit /b 1
