@echo off
rem J613 keyboard/trackpad diagnostics + repair, staged by scripts/j613-windows-diag.sh.
rem Runs ONCE at boot as SYSTEM from the j613-jobs mailbox (j613-jobs.cmd), before
rem the sign-in screen. Never shuts Windows down. Output: <WINSRC>\j613-jobs\diag-input.txt
rem   1. state before (device, driver package, services, events, AppleInputDiag)
rem   2. repair unless j613-jobs\no-repair.flag: trust the test certificate of the
rem      staged AppleMtpInput package, pnputil /add-driver /install, rescan
rem   3. state after
rem   4. boot task "J613InputSnapshot": 2 min after every boot, lib\j613-input-snapshot.cmd
rem      writes j613-jobs\input-snapshot.txt (AppleInputDiag + HID devices)
setlocal
set J=%~dp0
set J=%J:~0,-1%
set W=%~d0
set O=%J%\diag-input.txt
echo ==== J613 input diag %date% %time% > "%O%"
ver >> "%O%"
bcdedit /enum {current} | findstr /i "testsigning nointegritychecks description" >> "%O%" 2>&1

set PHASE=before
call "%J%\lib\j613-input-state.cmd" "%O%" %PHASE% "%W%"

if exist "%J%\no-repair.flag" (
  echo ---- repair skipped ^(no-repair.flag^) >> "%O%"
  goto snapshot
)
if not exist "%W%\j613-drivers\AppleMtpInput.inf" (
  echo ---- repair skipped: %W%\j613-drivers\AppleMtpInput.inf missing >> "%O%"
  goto snapshot
)
echo ---- repair: trust the package's test certificate >> "%O%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$f=Get-ChildItem '%W%\j613-drivers' -Filter *.cat | Select-Object -First 1; if(!$f){'no .cat in j613-drivers'; exit}; $c=(Get-AuthenticodeSignature $f.FullName).SignerCertificate; if(!$c){'catalog has no signer certificate'; exit}; foreach($n in 'Root','TrustedPublisher'){ $s=New-Object System.Security.Cryptography.X509Certificates.X509Store($n,'LocalMachine'); $s.Open('ReadWrite'); $s.Add($c); $s.Close() }; 'trusted: ' + $c.Subject + ' ' + $c.Thumbprint" >> "%O%" 2>&1
echo ---- repair: pnputil /add-driver /install >> "%O%"
pnputil /add-driver "%W%\j613-drivers\AppleMtpInput.inf" /install >> "%O%" 2>&1
echo ---- repair: rescan >> "%O%"
pnputil /scan-devices >> "%O%" 2>&1
rem 15 s for PnP to start the device (timeout.exe needs a console; startup scripts have none)
ping -n 16 127.0.0.1 >nul
set PHASE=after
call "%J%\lib\j613-input-state.cmd" "%O%" %PHASE% "%W%"

:snapshot
echo ---- boot snapshot task >> "%O%"
copy /y "%J%\lib\j613-input-snapshot.cmd" "%SystemRoot%\j613-input-snapshot.cmd" >> "%O%" 2>&1
schtasks /create /tn J613InputSnapshot /sc onstart /delay 0002:00 /ru SYSTEM /rl HIGHEST /f /tr "%SystemRoot%\j613-input-snapshot.cmd" >> "%O%" 2>&1
echo ==== done %date% %time% >> "%O%"
exit /b 0
