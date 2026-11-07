@echo off
rem Local Group Policy machine startup script (runs as SYSTEM at every boot).
rem Mailbox on the WINSRC partition, writable from macOS:
rem   \j613-jobs\*.cmd          run once each, then moved to \j613-jobs\done\
rem   \j613-jobs\drivers\...    every .inf installed with pnputil, then moved to done\
rem Log: \j613-jobs\jobs.log
setlocal
set J=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if exist %%d:\j613-jobs\ set J=%%d:\j613-jobs
if not defined J exit /b 0
if not exist %J%\done mkdir %J%\done
echo ==== %date% %time% >> %J%\jobs.log
if exist %J%\drivers\ (
  pnputil /add-driver %J%\drivers\*.inf /subdirs /install >> %J%\jobs.log 2>&1
  mkdir %J%\done\drivers 2>nul
  for /d %%x in (%J%\drivers\*) do move "%%x" %J%\done\drivers\ >nul
  move %J%\drivers\*.* %J%\done\drivers\ >nul 2>&1
)
for %%f in (%J%\*.cmd) do (
  echo -- %%~nxf >> %J%\jobs.log
  call "%%f" >> %J%\jobs.log 2>&1
  move "%%f" %J%\done\ >nul
)
exit /b 0
