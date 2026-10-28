# Instructions for AI coding agents

Goal: rebuild the native Windows port of *Digimon Rumble Arena 2* from the user's own disc image.

1. Ask the user for the path to their own dump of the game (`.iso` or `.xiso`).
2. Check the requirements in `README.md` (`python -c "import capstone"`, `git`, `cmake`, `ninja`,
   `xdvdfs`, a Visual Studio C++ installation found by `vswhere`). Report anything missing to the
   user and let them install it; do not install software or change system settings yourself unless explicitly asked to.
3. Run `python reproduce.py "<image>"`. It is resumable: on failure, read the error, fix the cause
   and run it again, and it continues from the step that failed.
   - "This is not Digimon Rumble Arena 2 (USA)": the user has another release (or another game).
     Stop and point them to `OTHER-RELEASES.md`; that port is a separate project, not a fix.
   - "The extracted files differ from a clean dump": the image is modified or damaged. Ask the
     user for a clean dump of their disc; do not work around the check.
4. When it finishes, tell the user to start `work/dist/DigimonRumbleArena2.exe`. With
   `--installer` and Inno Setup 6 present it also writes a setup program for their own use.

Rules:
- Everything under `work/` contains or is derived from the user's game and stays on their machine.
- Do not change `patches/`, `data/` or `project/` to work around a failure without explaining the
  failure to the user first. Most build failures are a missing tool.
- A Switch 2 Pro controller is invisible to the game while/if Steam runs (Steam Input holds it): if
  the user reports it missing, ask them to exit Steam completely first.

If something goes wrong at run time:
- `RECOMP_SAMPLE_PROFILE=10 RECOMP_SAMPLE_STACKS=1` prints a profile and every thread's stack
  every 10 s.
- A crash prints the guest function it happened in.
- Lines starting `[STUB]` or `[ICALL] Failed to resolve VA` name code the recompiler did not
  recover; `ENGINEERING.md` explains how to recover it.
