@echo off
rem J613 hands-free install, called by autounattend.xml with the stick's drive letter.
rem Touches ONLY the one unallocated extent of >= 100 GB. Stops (and shuts nothing
rem down) on any doubt, leaving a log in X:\j613-install.log and on the stick.
setlocal enabledelayedexpansion
set STICK=%1
set LOG=X:\j613-install.log
echo J613 install %date% %time% stick=%STICK% > %LOG%

rem --- install.wim may sit on the stick's second partition (WINDATA)
set WIM=
for %%d in (C D E F G H I J K L M N O P Q R T U V Y Z) do if exist %%d:\sources\install.wim set WIM=%%d:\sources\install.wim
if "%WIM%"=="" (echo no install.wim found >> %LOG% & goto fail)
echo wim=%WIM% >> %LOG%

rem --- find the one disk with >= 100 GB free
echo list disk > X:\ld.txt
diskpart /s X:\ld.txt > X:\disks.txt
type X:\disks.txt >> %LOG%
set DISK=
set COUNT=0
for /f "tokens=2,6,7" %%a in ('findstr /r /c:"Disk [0-9]" X:\disks.txt') do (
  if "%%c"=="GB" if %%b GEQ 100 (set DISK=%%a& set /a COUNT+=1)
)
if not "%COUNT%"=="1" (echo expected exactly one disk with ^>=100 GB free, found %COUNT% >> %LOG% & goto fail)
echo target disk %DISK% >> %LOG%

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
if not exist W:\ (echo W: missing after diskpart >> %LOG% & goto fail)
if not exist S:\ (echo S: missing after diskpart >> %LOG% & goto fail)
for /f "tokens=3" %%f in ('dir /-c W:\ ^| find "bytes free"') do set FREE=%%f
echo W: free bytes %FREE% >> %LOG%
if "%FREE:~0,-9%"=="" (echo W: is too small >> %LOG% & goto fail)
if %FREE:~0,-9% LSS 90 (echo W: is under 90 GB, wrong extent? >> %LOG% & goto fail)

rem --- Windows 11 Pro
dism /Apply-Image /ImageFile:%WIM% /Name:"Windows 11 Pro" /ApplyDir:W:\ >> %LOG% 2>&1
if errorlevel 1 (echo dism by name failed, trying index 6 >> %LOG% & dism /Apply-Image /ImageFile:%WIM% /Index:6 /ApplyDir:W:\ >> %LOG% 2>&1)
if not exist W:\Windows\System32\winload.efi (echo apply failed >> %LOG% & goto fail)

rem --- boot files
bcdboot W:\Windows /s S: /f UEFI /v >> %LOG% 2>&1
mkdir S:\EFI\BOOT
copy /y S:\EFI\Microsoft\Boot\bootmgfw.efi S:\EFI\BOOT\BOOTAA64.EFI >> %LOG%
if not exist S:\EFI\Microsoft\Boot\BCD (echo BCD missing >> %LOG% & goto fail)

rem --- click-free first boot
mkdir W:\Windows\Panther
copy /y %STICK%\oobe-unattend.xml W:\Windows\Panther\unattend.xml >> %LOG%

echo J613 install DONE >> %LOG%
copy /y %LOG% %STICK%\ >nul
copy /y %LOG% S:\ >nul
wpeutil reboot
exit /b 0

:fail
echo J613 install STOPPED, nothing else was done >> %LOG%
copy /y %LOG% %STICK%\ >nul
start notepad %LOG%
exit /b 1
