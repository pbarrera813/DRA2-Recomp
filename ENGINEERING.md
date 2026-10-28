# How the port was made

This is the engineering record behind `patches/` and `data/`: what was wrong, how it was found, and
what fixed it. Each defect is described from its symptom to its root cause.

## The approach

[XboxRecomp](https://github.com/sp00nznet/xboxrecomp) lifts an Xbox XBE to C. It disassembles the
executable, finds functions, infers calling conventions, translates every x86 instruction into C
that operates on an emulated register file and a flat 4 GB guest address space, and links it all
against a runtime that replaces the Xbox kernel and models the hardware (NV2A GPU, MCPX APU, OHCI
USB). The game's statically linked Microsoft libraries (Direct3D, DirectSound, XAPI) are recompiled
along with the game and drive the modelled hardware exactly as on a console.

The work loop was empirical. Run the recompiled game, find where it stops or misbehaves, trace that
to the guest code, and fix the cause in the right layer:
- the **lifter**, when an instruction was translated wrongly;
- the **function detector**, when code was never recovered as a function;
- the **runtime**, when a kernel call or hardware behaviour was missing.

No game code is patched.

Instruments added along the way (in `project/src/main.c` and the runtime):
- a watchdog report of every guest thread's last kernel call;
- a 1 ms sampling profiler over all threads, resolved through the PDB;
- native stack snapshots of every thread;
- crash reports naming the guest function;
- recompiler stubs that announce themselves when reached.

### Comparing with the real console

Late in the work the question changed from "does it run" to "does it match". The reference was
[xemu](https://xemu.app) with its GDB stub: pause both at the same moment and compare by content.
Static addresses (`.data`, the save buffer, the rumble report) are the same in both; heap
addresses are not, so heap objects were matched by what they contain. Two small parsers read the
GPU command ring: one lists every draw with its texture, vertex buffer, index count and the box its
vertices span; the other dumps the full state behind one draw (vertex program, constants, vertex
bytes). Development builds also had hardware watchpoints (debug registers on every guest thread)
to name the code that writes or reads a given address. A finding counted only when it held in
both directions: the console does X, ours does Y, and the fix makes ours do X.

## The hardest bugs

Five defects where the symptom pointed somewhere else entirely.

**A black cliff that was an index cap.** On RubberTree Falls a cliff beside the waterfall was a
black hole. The first theory was the game's own scene culling: the meshes seemed never to reach the
render list. Comparing with xemu disproved it. All 39 large scenery objects had identical world
boxes in both, and the game's portal traversal and frustum test, lifted, matched the machine code
instruction for instruction. Counting indices per draw over the objects' own command blocks did
find it: the terrain mesh sends 12,391 indices in one draw (two others 7,517 and 6,196), and the
executor kept at most 4,096 per batch and dropped the rest without a word. Two thirds of the cliff
never reached the GPU. The cap is now 65,536, and a one-time warning names any title that exceeds
it.

**Missing wedges from one unhandled method.** Hexagon platforms lacked a wedge of each top, and
menu badge frames had a gap on one side. Both were the last triangle of a triangle strip. Direct3D
sends an odd-length index list as pairs (`ARRAY_ELEMENT16`) plus one final 32-bit index
(`ARRAY_ELEMENT32`, method `0x1808`), which the executor had no case for. Our own GPU log already
listed `0x1808` among unhandled methods, 29,629 times in one run. 47 of RubberTree's mesh blocks end
with it.

**White badges from an unclamped colour.** The selected menu badge washed out to white. Its
combiner was the same as the unselected ones (texture × diffuse × 2), but a light placed near it
drove the vertex program's diffuse output to 73.6 where the others were below 1. The console's
vertex unit clamps its colour outputs to [0, 1]; the interpreter passed them through. The clamp now
happens at the program's output, with a test that fails without it (`tests/nv2a_vsh`).

**Melting bricks and back-face culling.** When fire hits a floating brick it melts in stages. Here
it broke into ragged pieces with flat plates floating above. Captured mid-melt from xemu, the stage
meshes were identical to ours byte for byte: vertex data, the 12-instruction vertex program, and
every constant except the camera. So the geometry was right, and the difference had to be in how
it was drawn. The render state at that draw had culling on (back faces), and our Direct3D 11
rasteriser always drew both sides. A melting mesh keeps a flat cap over its original footprint;
where the top sags below it, the cap's back face, invisible on the console, covered the shape. The
renderer now applies the title's cull registers, with strip winding alternated per triangle.
The first build had the front-face sense inverted, and everything turned inside out: a quick
reminder to check the sense on more than the scene being fixed.

**Floors one too high from a missing carry.** Under the game thread's round-toward-zero x87 mode,
every negative non-integer floored one too high. MSVC's floor idiom compares with `fcomip` and then
subtracts the carry with `sbb`; the lifter routed the compare to branches but never set the carry.
Details in the lifter section below (75 sites).

## Defects found and fixed

### Lifter (translation of x86 to C)

- **Narrow multiply/divide.** One-operand `mul/imul/div/idiv` with 8- or 16-bit operands were
  emitted in their 32-bit form: `imul cl` wrote EDX. DirectSound's cipher reads DL right after
  `imul cl`, so its output was wrong. All widths are now exact (AX, DX:AX, AL/AH remainders).
  Test: `tools/recomp/test_lifter_narrow_muldiv.py` compiles the emitted C and compares it with
  x86 semantics across value sweeps, with a negative control.
- **x87 conditional moves.** `fcmovb/fcmove/fcmovbe/fcmovu` and their negations were emitted as
  comments. Every float clamp written as `fucomi` + `fcmov` returned its input unclamped. The
  controller deadzone turned a centred stick into a constant deflection, so the main menu spun by
  itself and ignored confirm. 275 sites in this game. They are now conditional moves on the
  tracked compare result. Test: `tools/recomp/test_lifter_fcmov.py`.
- **x87 rounding direction under single precision.** The game thread runs with control word
  `0x0C3F`, single precision with rounding toward zero (a common speed trick for float-to-int
  casts). The x87 applies that rounding to every add, multiply and divide. The runtime already
  narrowed results to single precision, but always to the nearest value, so the game's
  arithmetic could differ from the hardware's in the last bit. The narrowing now follows the
  rounding bits. Found by logging each guest thread's control word at kernel calls; checked
  against the host's own single-precision multiply under all four rounding modes
  (`tests/fp_precision`, 800,000 products, bit for bit).
- **Carry from `fcomi`/`fcomip`.** These compares set the CPU flags. The lifter sent branches and
  `fcmov` to the tracked compare result, but left the carry flag alone, so an `sbb` after the
  compare read a stale carry. MSVC's floor-to-int idiom is exactly that: `fld x; fld st0; frndint;
  fxch; fcomip; fistp [m]; sbb [m], eax` subtracts "x < round(x)". Under the game thread's
  round-toward-zero, every negative non-integer floored one too high (75 sites in this game).
  The lifter now sets the carry from the same compare where a function reads it. Test:
  `tools/recomp/test_lifter_fcomi_carry.py`.

### Function recovery (`data/seeds.json`, `tools/`)

About 300 function entries are not found by the toolkit's detector. Each kind showed up as a
different symptom:
- **Indirect-call targets** (vtables, thread entry points) stop the game with
  `[ICALL] Failed to resolve VA`. The save system's worker thread and the battle's stage code were
  among them.
- **Direct tail-jump targets** silently link to a no-op stub. The save manager's per-mode
  handlers were all stubs, so the "Loading. Please don't remove memory units" overlay never
  cleared.

The seeds come from three sources:
- unresolved indirect calls seen at run time;
- `tools/stub_seeds.py`, for stub targets that are real entries;
- `tools/pointer_seeds.py`, for entries reached through data pointers.

The rules these scripts apply were learned from failures:
- A target reached only by branches inside its own function is a label. Seeding a loop head
  (`0xE7200`) split a function and turned its epilogue into a stub. The battle froze in a linked
  list whose update had been skipped.
- Jump tables live inside `.text`, and hand-written CRT assembly pads and aligns its case labels
  exactly like function starts. Taking "pointers" from code bytes split `memcpy`.
- Real function pointers are words in `.rdata`/`.data` and `push`/`mov` immediates. A vtable slot
  that points into bytes the decoder ran through means the decode was wrong there, not that the
  code is data.
- Run logs from a build with a bad split report that split's own labels as missing entries. They
  must not be used as seed sources.
- The opposite error also occurs: the detector split a function from its own cold chunk (code the
  compiler placed after the `ret`). Its jumps back into the owner's epilogue became stubs that
  skipped the stack and SEH restore. `data/coalesce.json` merges such chunks back with the
  toolkit's `--coalesce-functions`, which validates the extent before accepting it.

### Runtime (kernel and hardware models)

- **Pushbuffer packets cut at `DMA_PUT`.** The title can advance `PUT` in the middle of a command
  packet; this game does it for vertex program uploads and long inline vertex streams. The NV2A
  pusher keeps the method and the remaining count until the words arrive. The executor stopped
  the packet at the segment end and read the next segment's first data words as headers. A
  21-instruction vertex program then had its first two slots overwritten by a fragment, and the
  meshes using it collapsed to one point. The walker now holds a cut packet and continues it
  when the next segment starts where it stopped. Test: `tests/pb_scan`.
- **Pushbuffer ring wrap.** When `PUT` comes back to the start of the ring, the title has ended
  the lap with a `JUMP`. The executor scanned only up to the highest `PUT` it had seen, which
  dropped everything written after the last `PUT` of each lap. It now walks to the `JUMP` and
  resumes at its target (`nv2a_pb_scan_wrap`, same test).
- **GPU semaphore release.** Direct3D waits for pushbuffer space on a semaphore that the Kelvin
  class releases. The executor now resolves the semaphore's DMA object through RAMHT and writes the
  release.
- **DSP command acknowledgement.** DirectSound uploads its DSP program and waits for the DSP to
  clear a command word in RAM. The runtime acknowledges it at the measured address.
- **`NtSignalAndWaitForSingleObjectEx`** was an empty stub returning success. The save worker
  span holding its job mutex, and the game thread blocked on that mutex. It is now the host's
  `SignalObjectAndWait`.
- **DISPATCH_LEVEL exclusion.** On the single-CPU console, code at raised IRQL cannot be
  interrupted by a DPC. Here the APU's DPC ran concurrently with the game's own list updates and
  corrupted them, and the timer thread that drains DPCs then starved USB. Raising IRQL now takes a
  lock that DPCs and ISRs also take.
- **Save location.** The game links its save drive straight to `partition1\UDATA`, which the path
  layer mapped into the game's data folder. Partition 1 is the console's user-data partition and
  now maps to the save directory.
- **Controller ports.** The Xbox front panel wires controller ports 1–4 to OHCI0 ports 3, 4, 1
  and 2. Pads now arrive in that order, so host controller *n* is player *n*.
- **Controllers.** The host side of the pad model reads SDL3's gamepad API instead of XInput,
  so DualSense and Switch Pro controllers work alongside Xbox ones. Face buttons follow the
  printed letter, so the game's "A" prompt means the button marked A: Nintendo pads put A on the
  right where Xbox has B. Pads printed with shapes (PlayStation) keep the position, so the bottom
  one is A. Hotplugged pads keep stable player slots.
- **Switch 2 Pro.** A real one was not detected at all. SDL builds its Switch 2 driver only with
  libusb, which needs the controller's second USB interface (it sends the controller's start-up
  commands there). The official Windows `SDL3.dll` is built without libusb: its strings carry every
  other HIDAPI driver's hint, but not `SDL_JOYSTICK_HIDAPI_SWITCH2`. Windows binds that interface
  to its in-box WinUSB driver by itself, so no driver install is needed. The kit therefore builds
  the same SDL3 version from source with libusb. The game then sees the controller.
- **Rumble.** The game drives its motors, but the USB host model dropped every OUT transfer. The
  pad's output report (6 bytes: `00 06`, then the left and right motor speeds) now goes to
  `SDL_RumbleGamepad` for that player, whether XAPI sends it on interrupt endpoint 2 or as a
  `SET_REPORT` on endpoint 0.
- **Player lights.** SDL numbers pads itself when they connect: an XInput pad takes its XInput slot
  and SDL moves the pad holding that number aside. A DualSense then showed another player's lights.
  After any pad connects, every pad is put back at its own slot's number, and a pad that reconnects
  into a player's slot shows that player at once (past the title nobody presses START again).
- **Lights on exit.** A DualSense keeps the last colour it was given. On exit the port sends SDL's
  "reset LED" effect and waits briefly for SDL's sender thread, so the controller returns to its
  own battery display.
- **Switch 2 Pro and Steam.** With Steam running, Steam Input holds the controller's WinUSB
  interface, which allows one opener, so SDL cannot open it and the controller never appears.
  Nothing is logged; libusb's own debug output (`LIBUSB_DEBUG`) shows it.

### Audio

The title's own DirectSound drives the modelled APU, so every defect below sat between the
voice processor and the speakers. Each was located by measuring the signal at each stage
(an opt-in per-stage level log, per-voice state, and a raw capture of one voice compared with
the source data on disc) before anything was changed.

1. **Silence.** The output stage cleared the buffer the DSP stage had just filled, then sent
   1,024 samples for every 256 produced. It now sends exactly the frame that was produced.
2. **No menu music.** The music streamer issues an overlapped read, polls it once at once,
   and only then marks the read as in flight. On hardware that first poll always says
   "pending". Reads here completed instantly, the completion handler ran before the flag was
   set, and the stream waited forever. An asynchronous read now stays pending until the
   issuing thread blocks, or for at least 2 ms.
3. **Music as noise.** The music buffer is ordinary heap memory, and
   `MmGetPhysicalAddress` returned its virtual address as the physical one. Every bus
   master resolves a low address into the contiguous-memory window, so the APU read
   unrelated bytes. Ordinary memory now gets an unambiguous physical address (its first
   RAM mirror). A capture of the music voice now matches the track sample for sample.
4. **Cuts and pauses.** The APU produced 5 s of audio per 5.8 s of wall time: its frame
   pacing restarted its schedule on every timer oversleep instead of catching up. The host
   queue also had no cushion. Now: 0 underruns.
5. **Pitch.** Voices ignored their sample rate, so 44.1 kHz material played 8.8% fast. The
   voice processor now resamples with linear interpolation.

### The port layer: a controller menu and a title-screen exit

The console game has no quit option and no display settings. The port adds a small menu, opened with
the controller's system button (PS / Xbox / Home) or by holding Back + Start for a second (Windows
Game Bar and Steam can take the system button):

- Resume;
- Resolution;
- Widescreen;
- Fullscreen;
- Monitor;
- Quit, with a confirmation that defaults to No.

While it is open, the presenter holds the game at its frame flip, so a match does not continue behind
it. Audio is muted, and the game reads neutral pads; after the menu closes, each pad stays neutral to
the game until released. The menu reads SDL itself, because a frozen game stops the emulated USB
polling. Settings are saved to `settings.ini`.

Esc quits, but only on the title screen. The game has no "which screen" flag the port could ask for,
so it was found by diffing RAM snapshots:

- snapshots on the title, main menu, character select, VS screen, battle and the attract-mode demos;
- keep the words that are the same on every title snapshot and different on every other screen.

A first candidate (`0x2C3B38`, the screen being entered) failed: after an attract demo the title comes
back while it still says "battle". The rule that holds, in three runs and across demos, combines two
`.data` words:
- `0x2F9698`, the size of the loaded menu resource: 2188 is the title's;
- `0x2F88C4`, a screen id: 3 on the title.

Both must match. The game project passes them to the runtime as `RECOMP_TITLE_STATE`, so the runtime
stays game-neutral.

A crash found on the way: left idle, the game plays attract demos, and one in two such runs died
inside `d3d11.dll`. The crash report was extended to name the module and offset of each code address
on the native stack. It showed our draw path, and an instrumented build showed the texture cache:
- the cache was direct-mapped;
- in one draw, stage 1's texture hashed to the slot stage 0 had just filled, with a different size;
- re-creating it freed the D3D texture stage 0 still pointed to.

The cache now probes 8 slots and never evicts an entry the current draw uses.

### Rendering and performance

The toolkit's software NV2A executor reached gameplay at about 4 fps.

1. **Batched software rasterization.** Each draw's triangles are queued and rasterized on all
   cores by interleaved rows, preserving per-pixel order: 10–13 fps.
2. **A Direct3D 11 backend** (`src/kernel/nv2a_gpu_d3d11.c`):
   - The CPU front end (pushbuffer parsing, vertex programs, state) stays as it was.
   - Triangles go to the GPU as clip-space vertices, so the hardware clips and interpolates.
   - One HLSL pixel shader is a line-for-line port of the C register-combiner interpreter, so the
     C code remains the behavioural reference.
   - Textures are decoded once by the existing format code and cached by content hash.
   - The game renders off screen at the internal resolution. Presentation letterboxes the 4:3
     image into the window or fullscreen and syncs to vblank.
3. **Executor work removed from the hot path:**
   - a per-word method survey (half of the executor's time);
   - environment lookups per draw;
   - the software triangle setup.

   Vertex programs now run once per distinct index, in parallel. Result: 60 fps in menus and
   50–60 fps in battle, at 1440p and at 2160p alike.
4. **Correctness against the console:** the index cap, `ARRAY_ELEMENT32`, the colour clamp and
   culling, described under "The hardest bugs" above. Measured afterwards: 59.4–60 fps in a
   Twisted Toytown match.

One known difference remains by design: the game culls with its own 4:3 view frustum, so in the
widened 16:9 view, objects that are only in the side margins are not drawn.

## Evidence classes

| Claim | Evidence |
|-------|----------|
| Translator fixes | Unit tests in `tools/recomp/` |
| Title-screen detection | RAM snapshots across screens and attract demos; Esc tried on the intro, main menu, title, battle and the title after a demo |
| Controller slots and the join lightbar | `src/input/test_input_slots.c`: four SDL virtual gamepads become players 1–4; START lights only that pad, in its player colour; an unplugged pad frees only its own slot, and a new pad takes it; a Switch Pro's buttons map by letter; rumble reaches only the addressed pad and stops at zero; after SDL renumbers a pad every pad is back at its slot, and a joined player who reconnects is relit at once (fails 3 checks without the fix) |
| Vertex colour clamp | `tests/nv2a_vsh`: a colour output above 1 or below 0 is clamped (fails without the fix) |
| Rendering against the console | Draw-by-draw comparisons with xemu over its GDB stub: index counts, vertex bytes, vertex programs, constants and render state |
| Controllers on hardware | DualSense and Xbox Series X\|S over USB and Bluetooth, Switch 2 Pro over USB: detection, rumble on hits and knock-outs, player lights |
| Pushbuffer packets | `tests/pb_scan`: split increasing and non-increasing packets continue in the next segment; a segment elsewhere drops the cut packet; a ring wrap reads up to the `JUMP` and resumes at its target |
| Everything else | Runtime runs of the recompiled game: logs, thread reports, profiles and frame captures |