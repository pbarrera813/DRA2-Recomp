# Porting another release (European or Japanese)

This port was built against the USA release only, and `reproduce.py` refuses any other. This page
explains why, what would carry over, and what would have to be redone. It is a reading guide, not
a script: porting another release is a project of its own, and nobody has done it yet.

## Why the USA port does not just work

A static recompilation is tied to one exact executable. Another release has a different
`default.xbe`, and even small changes (other text, another build of the same code) move functions
to other addresses. Everything in this repository that names an address is therefore USA-only.

| Part | Carries over? | Why |
|------|---------------|-----|
| `patches/` (runtime, lifter, renderer, tests) | **Yes** | It fixes XboxRecomp itself: the x86 translation, the Xbox kernel and hardware models, the D3D11 renderer and the controller layer. None of it names a game address. |
| `tools/` (seed recovery scripts) | **Yes** | They work on any XBE; their rules are in [`ENGINEERING.md`](ENGINEERING.md). |
| `data/seeds.json`, `data/coalesce.json` | No | Function addresses in the USA executable. |
| `data/reference.json` | No | Hashes of the USA files. |
| `project/src/main.c` | Partly | Most of it is game-neutral. Release-specific: `DRA2_ENTRY_POINT` (from the XBE header), `RECOMP_TITLE_STATE` (two `.data` addresses that identify the title screen, for Esc to quit) and `RECOMP_APU_DSP_ACK` (a measured DirectSound address). |

## The work, in order

1. **Dump your disc and extract it** (xdvdfs). Note the hash of your `default.xbe`.
2. **Start from this repository's patch.** Clone XboxRecomp at `193e2995` and apply
   `patches/xboxrecomp-193e2995-dra2.patch`, so you begin with every fix found here.
3. **Run the pipeline without the USA data.** Follow `reproduce.py` step by step, but drop
   `--seed-functions` and `--coalesce-functions`, and set `DRA2_ENTRY_POINT` from your XBE
   header (`python -m tools.xbe_parser default.xbe`).
4. **Recover the missing functions.** Run the game and collect what stops it:
   - `[ICALL] Failed to resolve VA` lines are indirect-call targets the detector missed;
   - `[STUB]` lines are direct jumps into code that was not recovered as a function.

   `tools/stub_seeds.py` and `tools/pointer_seeds.py` find most of these statically. Read the
   "Function recovery" section of `ENGINEERING.md` first: seeding a label instead of an entry
   splits a function, and the result looks like an unrelated bug.
5. **Re-measure the release-specific values.**
   - `RECOMP_APU_DSP_ACK`: the guest dword DirectSound polls after uploading its DSP program,
     waiting for the DSP to clear it. If the game hangs at start-up or stays silent, this is the
     first suspect: find the dword that start-up loop reads and set it here.
   - `RECOMP_TITLE_STATE`: diff RAM snapshots of the title screen against every other screen
     (`RECOMP_RAM_SNAPSHOT`). You can also leave it out: only Esc-to-quit on the title needs it.
6. **Compare with the real console.** [xemu](https://xemu.app) with its GDB stub (`-s`) is the
   reference used here. When something looks or sounds wrong, capture the same moment in both
   and compare by content: static addresses match between the two, heap addresses do not.

## Things that may differ in other releases

Unverified, listed so you check them rather than assume:
- PAL releases sometimes offer 50 Hz modes or other timing.
- Text and menu resources differ in size, which moves `.data` addresses even where the code is
  the same.
- A later revision of the game may also contain code fixes of its own.

If you get another release running, the method and the defects in `ENGINEERING.md` are the map.
