@echo off
rem Boot task J613InputSnapshot (installed by j613-input-diag.cmd), 2 min after
rem every boot as SYSTEM: AppleInputDiag + HID device state into
rem <WINSRC>\j613-jobs\input-snapshot.txt (overwritten each boot). Read-only.
setlocal
set J=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if exist %%d:\j613-jobs\ set J=%%d:\j613-jobs
if not defined J exit /b 0
set W=%J:~0,2%
set O=%J%\input-snapshot.txt
echo ==== J613 input snapshot %date% %time% > "%O%"
if exist "%W%\AppleInputDiag.exe" ("%W%\AppleInputDiag.exe" status >> "%O%" 2>&1) else (echo AppleInputDiag.exe not on WINSRC >> "%O%")
echo ---- APPL0613 >> "%O%"
pnputil /enum-devices /deviceid "ACPI\APPL0613" /drivers >> "%O%" 2>&1
echo ---- HID / keyboard / mouse devices >> "%O%"
pnputil /enum-devices /class HIDClass >> "%O%" 2>&1
pnputil /enum-devices /class Keyboard >> "%O%" 2>&1
pnputil /enum-devices /class Mouse >> "%O%" 2>&1
pnputil /enum-devices /problem >> "%O%" 2>&1
exit /b 0
