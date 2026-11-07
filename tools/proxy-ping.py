#!/usr/bin/env python3
"""Exit 0 if an m1n1 proxy answers a NOP on the given serial device, else 1.

Used by scripts/j613-run.sh to tell a fresh "Running proxy..." Air from one
that is still running (or hung in) an earlier guest.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "m1n1_windows", "proxyclient"))
os.environ.setdefault("M1N1TIMEOUT", "2")

try:
    from m1n1.proxy import UartInterface

    iface = UartInterface(sys.argv[1])
    iface.nop()
except Exception as e:  # any failure means "not a usable proxy"
    print(f"proxy-ping: {type(e).__name__}: {e}", file=sys.stderr)
    sys.exit(1)
print("proxy-ping: m1n1 proxy is answering")
