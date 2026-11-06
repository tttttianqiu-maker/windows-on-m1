@echo -off
# J613 bring-up: boot Windows Setup from the WIN11 USB stick without a keyboard.
# Copy to the root of the stick as startup.nsh. The Shell runs it automatically.
# Setup stops at its first screen and waits for input; it writes nothing on its own.
for %a in fs0 fs1 fs2 fs3 fs4 fs5 fs6 fs7 fs8 fs9
  if exist %a:\sources\boot.wim then
    if exist %a:\efi\boot\bootaa64.efi then
      echo "J613: Windows installer found on %a, starting bootaa64.efi"
      %a:
      \efi\boot\bootaa64.efi
      echo "J613: bootaa64.efi returned"
    endif
  endif
endfor
echo "J613: no Windows installer found"
