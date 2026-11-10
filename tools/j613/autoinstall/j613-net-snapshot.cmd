@echo off
rem Boot task J613NetSnapshot (installed by j613-netkvm.cmd), 2 min after every boot as
rem SYSTEM: the Wi-Fi adapter as Windows sees it into <WINSRC>\j613-jobs\net-snapshot.txt
rem (overwritten each boot). Read-only.
setlocal
set J=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if exist %%d:\j613-jobs\ set J=%%d:\j613-jobs
if not defined J exit /b 0
set O=%J%\net-snapshot.txt
echo ==== J613 net snapshot %date% %time% > "%O%"
echo ---- virtio-net device >> "%O%"
pnputil /enum-devices /deviceid "PCI\VEN_1AF4&DEV_1041" /drivers >> "%O%" 2>&1
powershell -NoProfile -Command "Get-NetAdapter | Format-List Name,InterfaceDescription,Status,LinkSpeed,MacAddress; Get-NetIPConfiguration | Format-List" >> "%O%" 2>&1
ipconfig /all >> "%O%" 2>&1
echo ---- reachability >> "%O%"
ping -n 3 1.1.1.1 >> "%O%" 2>&1
nslookup microsoft.com >> "%O%" 2>&1
powershell -NoProfile -Command "try { (Invoke-WebRequest -UseBasicParsing -TimeoutSec 10 http://www.msftconnecttest.com/connecttest.txt).Content } catch { 'HTTP: ' + $_ }" >> "%O%" 2>&1
exit /b 0
