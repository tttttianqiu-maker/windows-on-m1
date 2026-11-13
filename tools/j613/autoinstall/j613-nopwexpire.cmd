@echo off
rem J613: local passwords never expire. Windows asked for a new password at sign-in
rem ("password has expired or been changed"), which breaks the autologon of the
rem passwordless "User" account while there is no working keyboard in Windows.
rem Runs ONCE at boot as SYSTEM from the j613-jobs mailbox, before sign-in.
rem Output: <WINSRC>\j613-jobs\nopwexpire.txt
setlocal
set O=%~dp0nopwexpire.txt
echo ==== J613 nopwexpire %date% %time% > "%O%"
net accounts /maxpwage:unlimited >> "%O%" 2>&1
net user User /logonpasswordchg:no >> "%O%" 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-LocalUser | Where-Object Enabled | ForEach-Object { Set-LocalUser -Name $_.Name -PasswordNeverExpires $true; $_.Name + ': password never expires' }" >> "%O%" 2>&1
net accounts >> "%O%" 2>&1
net user User >> "%O%" 2>&1
echo ==== done %date% %time% >> "%O%"
exit /b 0
