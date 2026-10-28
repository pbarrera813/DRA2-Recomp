# Digimon Rumble Arena 2: a native Windows port by static recompilation

> **You need your own, legally owned copy of the game.** This repository contains no game code
> or data. It rebuilds the port on your machine from your own dump of the disc (`.iso` or
> `.xiso`), and nothing it produces may be shared.

The original Xbox release of *Digimon Rumble Arena 2* (2004), turned into a native 64-bit Windows
program: the game's own x86 code, lifted to C and compiled with MSVC, running on a reimplemented
Xbox kernel, with the NV2A GPU drawn through Direct3D 11 at modern resolutions and the original USB
controllers fed by SDL3.

## Just want to play?

1. Install the requirements below.
2. Get this repository:
   ```
   git clone https://github.com/pbarrera813/DRA2-Recomp.git
   cd DRA2-Recomp
   ```
3. Run one command with your own dump of the **USA release**:
   ```
   python reproduce.py "Digimon Rumble Arena 2 (USA).iso"
   ```
4. Start `work/dist/DigimonRumbleArena2.exe`.

**Faster with an AI coding assistant:** open this folder with one (Claude Code, Codex, Cursor,
Copilot and others) and ask it to build the game. It reads [`AGENTS.md`](AGENTS.md), checks your
tools, runs the script and explains any error. It will ask you for the path to your own dump.

Have the European or Japanese release? This port is for the USA release only. See
[`OTHER-RELEASES.md`](OTHER-RELEASES.md).

## What you get

- The game, natively: menus, battles and four-player local multiplayer, with its music, voices
  and effects.
- Borderless fullscreen at 1080p, 1440p or 2160p, locked at 60 fps. Widened to fill 16:9
  screens without stretching (or the original 4:3); F11 or Alt+Enter switches to a window;
  Alt+F4 quits. Settings live in `settings.ini` beside the saves.
- No keyboard needed: the PS / Xbox / Home button (or Back + Start held for a second) opens a menu
  with Resolution, Widescreen, Fullscreen, Monitor and Quit. On the title screen, Esc also quits.
- Up to four controllers. Controller *n* is player *n*, with rumble, and a DualSense's light bar
  shows its player's colour.
- Saves under `%LOCALAPPDATA%\DigimonRumbleArena2`.

Controllers tested on real hardware:

| Controller | USB | Bluetooth |
|------------|-----|-----------|
| DualSense | yes | yes |
| Xbox Series X\|S | yes | yes |
| Switch 2 Pro | yes | not supported by SDL 3.4 |

Other controllers SDL3 supports (DualShock 4, Switch Pro, Xbox One and others) should work but
were not tested. **Switch 2 Pro: close Steam completely first.** Steam Input holds the controller,
and the game then cannot see it.

## Requirements

All free:
- Windows 10 or 11, x64.
- Python 3.10+ with `pip install capstone`.
- Git.
- Visual Studio 2022 or newer with "Desktop development with C++" and a Windows SDK.
- CMake 3.21+ and Ninja on `PATH`.
- [xdvdfs](https://github.com/antangelo/xdvdfs) on `PATH` (or pass `--xdvdfs`).
- Optional: Inno Setup 6, for a setup `.exe`.

## What the script does

```
python reproduce.py "<your dump>.iso" [--work work] [--xdvdfs path] [--installer] [--version 1.0.0]
```

1. Extracts your image with xdvdfs (a full `.iso` and a trimmed `.xiso` hold the same files).
2. Checks the files: `default.xbe` must be the USA release's, and all 2,863 files must match a
   clean dump (`data/reference.json`).
3. Fetches [XboxRecomp](https://github.com/sp00nznet/xboxrecomp) at commit `193e2995` and applies
   `patches/xboxrecomp-193e2995-dra2.patch`.
4. Recompiles the XBE with the function seeds in `data/seeds.json`.
5. Downloads SDL3 3.4.18 and libusb 1.0.30 from their official releases and checks their hashes.
6. Builds SDL3 with libusb (the official `SDL3.dll` has no Switch 2 controller driver), then the
   game, with MSVC.

The result is `work/dist/`. Steps already done are skipped on a re-run. `--installer` also writes a
per-user setup program to `installer/output/`, for your own machines only (`--version` sets the
version it shows). The first build takes a while: about 20,000
functions are lifted to C and compiled.

## How it was made

[`ENGINEERING.md`](ENGINEERING.md) is the engineering record: every defect found between the
recompiled game and the real console, from symptom to root cause, and how it was proven. It starts
with the hardest ones: a black wall that turned out to be a 4,096-index cap, melting bricks that
came down to back-face culling, and a floor-to-int idiom broken by one missing carry flag.

## What is in here

| Path | What it is |
|------|------------|
| `reproduce.py` | The whole rebuild, one script, standard library only |
| `patches/` | Every change made to XboxRecomp for this game: runtime, lifter, D3D11 renderer, tests |
| `data/seeds.json` | Function entry points recovered beyond the toolkit's own detection |
| `data/coalesce.json` | Cold code chunks merged back into the functions they belong to |
| `data/reference.json` | Hashes of the supported release's files; the toolkit commit; the library releases |
| `project/` | The game project: entry point, product defaults, profiling and crash tooling |
| `tools/` | The scripts that recovered the seeds, kept as evidence of method |
| `installer/` | Inno Setup script for the optional setup program |
| `ENGINEERING.md` | How the reverse engineering was done, defect by defect |
| `OTHER-RELEASES.md` | What porting the European or Japanese release would take |
| `AGENTS.md` | Instructions for an AI coding assistant to reproduce the build |

## License and credits

The patch, scripts, data and documents in this repository are under the [MIT license](LICENSE).
It covers this project's own work only: no game code or data is included or licensed.

Built on [XboxRecomp](https://github.com/sp00nznet/xboxrecomp) (MIT), which includes code derived
from [xemu](https://github.com/xemu-project/xemu) under its own licenses (see the toolkit's
`LICENSES/`). Controllers through [SDL3](https://github.com/libsdl-org/SDL) (zlib license) and
[libusb](https://github.com/libusb/libusb) (LGPL 2.1, dynamically loaded).
*Digimon Rumble Arena 2* is the property of its rights holders; this project is not
affiliated with or endorsed by them.
