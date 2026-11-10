@echo off
rem J613 built-in Wi-Fi in Windows: installs Red Hat's NetKVM (virtio-win, NetKVM\w11\ARM64)
rem for m1n1's virtio-net device (PCI\VEN_1AF4&DEV_1041, build 52+ with `j613 wifi`).
rem Staged by scripts/j613-netkvm.sh (macOS on the Air) together with the driver in
rem <WINSRC>\j613-netkvm\. Runs ONCE at boot as SYSTEM from the j613-jobs mailbox
rem (j613-jobs.cmd), before the sign-in screen. Never shuts Windows down.
rem Output: <WINSRC>\j613-jobs\netkvm.txt
rem   1. trust the catalog signer, only if Windows says the signature is valid and the
rem      signer is Red Hat (the virtio-win drivers carry Red Hat's vendor signature)
rem   2. pnputil /add-driver /install (driver store: also binds later, once the device shows)
rem   3. boot task "J613NetSnapshot": 2 min after every boot, lib\j613-net-snapshot.cmd
rem      writes j613-jobs\net-snapshot.txt (adapter, IP, DNS, ping)
setlocal
set J=%~dp0
set J=%J:~0,-1%
set W=%~d0
set D=%W%\j613-netkvm
set O=%J%\netkvm.txt
echo ==== J613 NetKVM %date% %time% > "%O%"
ver >> "%O%"
bcdedit /enum {current} | findstr /i "testsigning nointegritychecks" >> "%O%" 2>&1
if not exist "%D%\netkvm.inf" (
  echo ---- %D%\netkvm.inf missing: run scripts/j613-netkvm.sh in macOS again >> "%O%"
  goto snapshot
)
echo ---- trust the catalog signer (Red Hat only) >> "%O%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$f=Get-ChildItem '%D%' -Filter *.cat | Select-Object -First 1; if(!$f){'no .cat in j613-netkvm'; exit}; $s=Get-AuthenticodeSignature $f.FullName; 'catalog ' + $f.Name + ': ' + $s.Status + ' ' + $s.SignerCertificate.Subject; if($s.Status -ne 'Valid' -or $s.SignerCertificate.Subject -notmatch 'Red Hat'){'not trusting this signer'; exit}; $st=New-Object System.Security.Cryptography.X509Certificates.X509Store('TrustedPublisher','LocalMachine'); $st.Open('ReadWrite'); $st.Add($s.SignerCertificate); $st.Close(); 'trusted publisher: ' + $s.SignerCertificate.Thumbprint" >> "%O%" 2>&1
echo ---- pnputil /add-driver /install >> "%O%"
pnputil /add-driver "%D%\netkvm.inf" /install >> "%O%" 2>&1
pnputil /scan-devices >> "%O%" 2>&1
rem 20 s for PnP to start the adapter (timeout.exe needs a console; startup scripts have none)
ping -n 21 127.0.0.1 >nul
echo ---- virtio-net device >> "%O%"
pnputil /enum-devices /deviceid "PCI\VEN_1AF4&DEV_1041" /drivers >> "%O%" 2>&1
powershell -NoProfile -Command "Get-NetAdapter | Format-List Name,InterfaceDescription,Status,LinkSpeed,MacAddress" >> "%O%" 2>&1

:snapshot
echo ---- boot snapshot task >> "%O%"
copy /y "%J%\lib\j613-net-snapshot.cmd" "%SystemRoot%\j613-net-snapshot.cmd" >> "%O%" 2>&1
schtasks /create /tn J613NetSnapshot /sc onstart /delay 0002:00 /ru SYSTEM /rl HIGHEST /f /tr "%SystemRoot%\j613-net-snapshot.cmd" >> "%O%" 2>&1
echo ==== done %date% %time% >> "%O%"
exit /b 0
