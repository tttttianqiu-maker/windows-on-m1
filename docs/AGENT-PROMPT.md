# Prompt for the local agent (Cherry Studio agent mode / Claude Code)

Set the work directory to `~/src/windows-on-m1`, permission mode
"Approve for Me" (not "Full Access"), then paste:

---

You are taking over the Windows-11-on-MacBook-Air-M3 (J613) project on this
Hackintosh. Work autonomously; the owner only does physical steps.

1. Setup: `git pull && git submodule update --init`. Read `AGENTS.md`,
   `CLAUDE.md` and `docs/J613-HANDOFF.md` completely before doing anything.
   Everything (hardware, commands, history, scripts, open problems) is there.
   Install the native toolchain if missing: `brew install llvm lld rustup`,
   `rustup-init -y`, `rustup target add aarch64-unknown-none-softfloat`.
   **Do not install or use Docker.** Build with `sh scripts/j613 build <name>`.
   Run `sh scripts/j613 setup` once so `j613` works.

2. Current symptom: **black screen**. Diagnose it yourself first: newest
   `runs/*/` (console, `screen.png`), `hv.log` (`j613 hv`, `j613 log`, grep for
   SError/Exception/PSCI/WindowsAutoBootDxe/Recovery), the WinPE logs on WINSRC
   if the owner is in macOS. Known context: the Windows SYSTEM hive was corrupt
   (0xc000014c) and a reinstall was armed (`prep.sh --reinstall`) but may not
   have run yet; the trackpad causes an SError after bring-up (handoff §7), so
   keep `firmware/tpmtfw-j613.bin` renamed to `.bin.off` until that is fixed.

3. Then work the next steps in `docs/J613-HANDOFF.md` §9 in order:
   reinstall -> verify the built-in keyboard in Windows -> fix the trackpad
   SError (m1n1-only, one variable per run) -> re-enable the trackpad.

4. Loop on your own: change -> commit -> `j613 build <name>` -> the watcher
   (`j613 watch`, keep it running in its own terminal) runs it; request reruns
   with `j613 again`; read results with `j613 log` / `j613 hv` / `runs/`;
   decide; repeat. Record every run in `investigation/EXPERIMENTS.md` (before
   and after) and every change in `investigation/CHANGES.csv`; keep
   `python3 -m pytest -q tests/test_change_ledger.py` green. Push to
   `j613-bringup` after each commit. No assistant attribution trailers.

5. Ask the owner only for: physical actions (cold boot into proxy, stick in/out,
   booting macOS on the Air, running a command there) — give one copy-paste
   line each — and **before any disk or boot change on the Air** (partitions,
   formatting, boot files, BCD, firmware, registry of the installed Windows).
   Do not disable UAC or add certificates to trusted root stores without his
   explicit OK. Keep messages short and direct.

6. Use sources, not guesses: Asahi Linux (`t8122.dtsi`, `drivers/soc/apple/`,
   `drivers/hid/dockchannel-hid/`), upstream m1n1, Microsoft docs. Keep costs
   down: Sonnet-level effort for routine build/run/log loops, deeper reasoning
   only for hard bugs; don't paste whole 2000-line logs, grep them.
