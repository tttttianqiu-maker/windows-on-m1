@echo off
rem Called by j613-input-diag.cmd: append the J613 input state to %1, labelled %2,
rem WINSRC drive %3. Read-only.
setlocal
set O=%~1
set W=%~3
echo. >> "%O%"
echo ==================== %~2 %time% >> "%O%"
echo ---- APPL0613 device (driver, stack, problem) >> "%O%"
pnputil /enum-devices /deviceid "ACPI\APPL0613" /drivers /stack >> "%O%" 2>&1
pnputil /enum-devices /instanceid "ACPI\APPL0613\0" /drivers /stack >> "%O%" 2>&1
echo ---- HID devices (VHF children: keyboard, touch pad) >> "%O%"
pnputil /enum-devices /class HIDClass >> "%O%" 2>&1
pnputil /enum-devices /class Keyboard >> "%O%" 2>&1
pnputil /enum-devices /class Mouse >> "%O%" 2>&1
echo ---- devices with problems >> "%O%"
pnputil /enum-devices /problem >> "%O%" 2>&1
echo ---- third-party driver packages >> "%O%"
pnputil /enum-drivers >> "%O%" 2>&1
echo ---- services >> "%O%"
sc query AppleMtpInput >> "%O%" 2>&1
sc qc AppleMtpInput >> "%O%" 2>&1
sc query vhf >> "%O%" 2>&1
echo ---- registry >> "%O%"
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\APPL0613" /s >> "%O%" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Services\AppleMtpInput" /s >> "%O%" 2>&1
echo ---- AppleInputDiag >> "%O%"
if exist "%W%\AppleInputDiag.exe" ("%W%\AppleInputDiag.exe" status >> "%O%" 2>&1) else (echo AppleInputDiag.exe not on WINSRC >> "%O%")
echo ---- setupapi.dev.log >> "%O%"
findstr /n /i /c:"APPL0613" /c:"applemtpinput" "%SystemRoot%\INF\setupapi.dev.log" >> "%O%" 2>&1
echo ---- Code Integrity events (newest first) >> "%O%"
wevtutil qe Microsoft-Windows-CodeIntegrity/Operational /c:15 /rd:true /f:text >> "%O%" 2>&1
echo ---- Kernel-PnP configuration events (newest first) >> "%O%"
wevtutil qe Microsoft-Windows-Kernel-PnP/Configuration /c:30 /rd:true /f:text >> "%O%" 2>&1
exit /b 0
