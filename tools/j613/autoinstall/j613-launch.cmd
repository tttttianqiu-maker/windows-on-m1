@echo off
rem Runs instead of Setup (winpeshl.ini in boot.wim index 2). Finds j613-install.cmd on
rem any volume (the WINSRC partition on the internal SSD) and runs it. Falls back to Setup.
echo J613: waiting for volumes...
ping -n 8 127.0.0.1 >nul
for /l %%t in (1,1,10) do (
  for %%d in (C D E F G H I J K L M N O P Q R T U V Y Z) do (
    if exist %%d:\j613-install.cmd (
      echo J613: found %%d:\j613-install.cmd
      call %%d:\j613-install.cmd %%d:
      exit /b
    )
  )
  ping -n 4 127.0.0.1 >nul
)
echo J613: j613-install.cmd not found on any volume, starting normal Setup
X:\sources\setup.exe
