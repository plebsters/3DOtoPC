# 3DO to PC — 3DO games → native Windows programs

Turns a 3DO disc image you own into a native Windows `.exe`. Everything runs
on the local PC: disc reading, analysis, code generation and compilation.
There is no network code anywhere in the tool or in the games it produces.

```
 disc.iso/.bin ──► Opera FS reader ──► data\ (extracted files)
                         │
                    LaunchMe (AIF)
                         │
                  control-flow analysis ──► listing.txt
                         │
                 C++ emitter (src\recomp_*.cpp)
                         │
         local CMake + MSVC/clang-cl  ──►  Game.exe  +  data\
                                              │
                         tdort runtime: hybrid dispatcher
                          ├─ recompiled block found & unmodified → native code
                          ├─ otherwise → ARM60 interpreter (same semantics)
                          ├─ SWIs / folio vectors → Portfolio HLE
                          └─ Win32 window, XInput, frame pacing
```

## One-time setup

Double-click **`Install-Prerequisites.bat`** (it asks for administrator rights). It installs
only what's missing: Build Tools for Visual Studio 2022 (C++ x64 compiler, Windows SDK,
C++ CMake tools) and the .NET 8 SDK (needed only for the `3DO to PC.exe` window app; add
`-SkipDotNet` to skip it). This is the only step that uses the internet.

## Building the Windows app

Double-click **`Build-App.bat`**. It finds Visual Studio / Build Tools, builds the C++ core,
then builds the WPF app (if the .NET 8 SDK is installed) into **`dist\`**:
`3DO to PC.exe` (the app), `3DO to PC CLI.exe` (command line), `tdocore.dll` and `runtime\`.
Keep those together; copy the whole `dist\` folder if you move it.

## Game launcher

`dist\"3DO Launcher.exe"` shows every converted game as a tile and starts it with a
double-click, Enter, or the A button on an Xbox controller.

- It scans the `games\` folder next to `dist\` (where the converter puts games).
  **Games folder…** points it at any other folder; the choice is remembered.
  A game is any folder holding an `.exe` and a `data` folder.
- Search box (Ctrl+F) and sorting by name, recently played or most played.
- Box art: **Get box art** downloads a cover for every game that has none, from
  the free [libretro thumbnails](https://github.com/libretro-thumbnails/The_3DO_Company_-_3DO)
  collection (USA covers preferred). Right-click a tile → **Choose box art online…**
  to search the collection with a preview and pick a different cover, or
  **Set cover image from file…** to use your own. Covers are saved as `cover.png`
  in the game's folder; games without one get a placeholder tile.
- Controller: D-pad or left stick to move, A or Start to play, Y to rescan,
  B to clear the search. The launcher minimises while a game runs and comes back
  when it exits.
- Play count and last-played time are kept in `%AppData%\3DO to PC\launcher.json`.
  Nothing goes online except box art, and only when you ask for it.

## Quick test (drag and drop)

1. Install **Build Tools for Visual Studio 2022** (free) with the **Desktop development with C++**
   workload. That's the only requirement; nothing else is downloaded.
2. Unzip this folder anywhere.
3. Drag a disc image (`.iso`, `.bin`, `.cue`) or a folder of extracted game files (the one
   containing `LaunchMe`) onto **`Convert-3DO.bat`**.

The script builds the converter (first time only), converts the game into `games\<Name>\`,
compiles `<Name>.exe`, runs it, then shows the end of `tdort.log` and highlights it in
Explorer. That log lists every OS call the game needed that isn't implemented yet.
Drop the same game again and the code it had to interpret is recompiled (`interp_hits.txt`).

## How the hybrid design works

* **One definition of ARM semantics.** `runtime/include/tdort/arm_exec.h`
  is used by both the interpreter and the recompiled code. Generated code is
  `exec(c, <insn constant>, <pc constant>)` per instruction; `exec` is
  force-inlined so the compiler folds the decode away. The two paths can't
  disagree, and recompiling bytes that turn out to be data does no harm.
* **Indirect jumps** (`mov pc, rX`, `ldr pc, …`, jump tables, returns) go
  back to the dispatcher. If the target has no recompiled block, it is
  interpreted.
* **Feedback loop.** On exit, the game writes `interp_hits.txt`: the
  addresses it had to interpret. Pass that file back as **Runtime hints**
  (GUI) or `--hints` (CLI) and those blocks get recompiled on the next build.
* **Self-modifying code / overlays.** Writes into recompiled code are tracked
  in 64-byte granules. A block whose bytes change falls back to the
  interpreter permanently. A LaunchMe hash check turns on interpreter-only
  mode if the generated code doesn't match the disc.

## Layout

| Path | What |
|---|---|
| `core/` | Converter: Opera FS, disassembler, analysis, C++ emitter, toolchain discovery, C API (`tdo/api.h`) |
| `runtime/` | `tdort`, linked into every converted game: CPU, memory map, interpreter, dispatcher, HLE, gfx, platform |
| `cli/` | `3DO to PC CLI` command line |
| `gui/TdoConverter/` | C# WPF front end (P/Invoke into `tdocore.dll`) |
| `tests/` | Unit tests; end-to-end tests on synthetic discs: a hand-assembled ARM program, and a C program (`tests/guest/`) compiled for the 3DO CPU that exercises the OS replacement |

## Building (Windows)

1. Install **Visual Studio 2022 or Build Tools** with the *Desktop development with C++* workload, and the **.NET 8 SDK**.
2. Native core:
   ```
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release
   ctest --test-dir build -C Release
   ```
3. GUI: `dotnet build gui\TdoConverter -c Release`. This copies `tdocore.dll` and `runtime\` next to the app.
   (`Build-App.bat` does steps 2-3 for you.)

### Compiler used for converted games (all local)
1. `toolchain\` next to the app: portable `cmake\`, `ninja\`, `llvm\` (optional `windows-sdk.cmake`). Lets you ship a self-contained app.
2. Visual Studio / Build Tools, found through `vswhere.exe`.
3. `cmake` on `PATH`.

## CLI

```
"3DO to PC CLI" inspect  game.iso
"3DO to PC CLI" convert  game.iso out\ [--name N] [--hints interp_hits.txt] [--no-build] [--debug]
"3DO to PC CLI" toolchain
```
Converted game flags: `--interp` (interpreter only), `--trace` (log HLE calls), `--headless`.
Each run writes `tdort.log`. Every unimplemented SWI or folio call is logged
once with its arguments, so the log is the to-do list for that game.

## Debugging a converted game

`tdort.log` (next to the game's .exe) records the first OS calls, every unimplemented call,
a task status line every 2 s (each task's state, pending/waited signals and a stack
backtrace), crash reports, and at exit the most-called OS functions.

Environment variables for headless runs (`Game.exe --headless`):

| Variable | Effect |
|---|---|
| `TDORT_FRAMES=n` | stop after n frames (default 600) |
| `TDORT_SNAP=300,600` | save `snap_<frame>.bmp` of the screen at those frames |
| `TDORT_PRESS=1900-1906:P,2100:A` | hold buttons on frame ranges (U D L R A B C P=start X=stop l r) |
| `TDORT_TRACE_READS=1` | log every file read |

`--trace` logs every OS call and task switch.

## Status

**Milestone 1 (done):** Opera FS (2048/2352-byte sectors, nested dirs), AIF parsing,
ARMv3 decode/disassembly/execution, analysis, C++ emission, local build,
hybrid dispatch, write invalidation, hint feedback, Win32 window/input.

**Milestone 2 (done): Portfolio kernel + file folio replacement** (`runtime/src/hle_*.cpp`)

The numbers come from the public Portfolio 2.5 SDK. Structure offsets were
measured by compiling its headers for big-endian ARM, SWI numbers and vector
indices were read from them, and the task start-up convention came from
disassembling the SDK's `cstartup` object. All of it is in
`runtime/include/tdort/portfolio_abi.h` as plain numbers.

* **Task start-up:** r5=argc, r6=argv, r7=KernelBase, sp/sl set; vectors -30 (init) and -37 (exit).
* **KernelBase vectors:** list ops (Exec-style anchors, priority insert), AllocMem/FreeMem
  (first-fit DRAM then VRAM, `MEMTYPE_FILL`), memset/memcpy, LookupItem/CheckItem,
  FindTagArg/NextTagArg, GetSysErr, debug printf to the log.
* **Kernel SWIs:** items (Create/Delete/Find/FindAndOpen/Open/Close/Lock), signals
  (Alloc/Free/Send/Wait; a wait ends the frame), message ports (Send/Get/GetThis/Reply/Wait),
  IO (SendIO/DoIO/WaitIO/AbortIO, IO_DONE, SIGF_IODONE, reply-port messages),
  SampleSystemTime, random numbers, AllocMemBlocks.
* **Devices:** `timer` (VBL delay = WaitVBL frame pacing, time reads). Any other device is created
  on demand and its commands are logged.
* **File folio:** OpenDiskFile / OpenDiskFileInDir / CloseDiskFile / ChangeDirectory / GetDirectory;
  block-based CMD_READ, CMD_STATUS (FileStatus), FILECMD_READDIR, FILECMD_GETPATH. `/cd-rom/…`,
  `/<anything>/…`, `$boot/…` and relative paths all resolve into `data\`, case-insensitively.
  Data read from disc invalidates any recompiled code it overwrites, so overlays work.
* **Graphics folio (basic):** CreateScreenGroup (screens/bitmaps in VRAM, honours count/size/buffer
  tags), Add/Remove/DeleteScreenGroup, DisplayScreen (scan-out + vblank), DrawCels/DrawScreenCels
  entry points (SWI and vector), common setters accepted.
* **Other folios** (`audio`, `Operamath`, `International`, `Compression`, `JString`) open
  successfully. Their calls are logged by name (all 34 known audio SWIs are named).

Tested by `tests/guest/`: a C program compiled with clang for big-endian ARM. It
checks start-up, folios, memory, lists, signals, time, file reads, paths,
directories and GETPATH. It passes recompiled (100% native blocks) and
interpreted, and a one-byte change to the disc file makes it fail.

**Milestone 3 (done): CEL engine, input, threads** — tested on synthetic programs and run against Gex's executable

* **CEL engine** (`runtime/src/gfx.cpp`): software rasterizer for CCB chains with sequential
  field loading, absolute/relative pointers, preambles in the CCB or the source, unpacked and
  packed rows, 1/2/4/6/8/16 bpp coded (PLUT, PLUTA) and uncoded (REP8), the pixel processor
  (primary/secondary sources, multiply/divide, add/subtract/XOR, average, clamp/wrap), full
  corner-engine projection (scaling, rotation, HDDX/HDDY warps), face culling and clipping into
  LR-form bitmaps.
* **Event broker** (`hle_input.cpp`): games register as listeners and get control-pad pressed /
  released / update events each frame from the keyboard or an Xbox pad. Blocking waits work.
* **Tasks and threads** (`hle_tasks.cpp`): CreateThread / CreateItem(Task), per-task registers and
  signals, priority scheduling with time slices, semaphores, VBL timer sleeps, thread exit.
* **NVRAM saves**: `/NVRAM/...` maps to a writable `nvram` folder next to the game's `data` folder.
* **Debug console**: `printf`/`putc` output from the game is written to `tdort.log`.
* **Crash report**: bad jumps stop the game and log registers, the recent block trail and code.

**Milestone 4 (in progress): first real game.** Gex (1994) now converts and runs to its title
screen, save-slot screen and level-select hub with correct graphics and controller input. Fixes
found by running it:
* the 1.x-SDK kernel vector −24 is `WaitPort`; `WaitPort(0, msg)` waits for that message's reply;
* IO completion matches the real kernel: new IOReqs start `IO_DONE`, each IOReq owns a message
  item, quick completions set `IO_QUICK` and send nothing, `msg_MsgPort` is cleared when a
  message is taken off a port;
* `WaitSignal(0)` returns at once; idle threads may wait forever (no watchdog wake-ups once
  more than one task exists);
* File folio vector −6 `LoadProgramPrio`: programs that can't be run (Gex's movie player) act
  like a program that starts and exits a few frames later;
* the audio clock runs (~240 ticks/s): `GetAudioTime`, `SleepUntilTime`, `SleepAudioTicks`,
  `GetCueSignal`, `SignalAtTime`;
* the OS allocates its own structures from the top of memory (`MEMTYPE_FROMTOP`), so a game
  can free and re-allocate one big block;
* memory-bank headers (`FindMH`), Graphics folio fields games read directly, SPORT fill.

Where Gex stops: after the hub, the main loop waits for the restarted player thread
(`thread_player`) to finish, and that thread waits on the IO thread.

**Not done yet:**
* **Audio**: effects that need a real DSP (reverb, filters, delay feedback) pass sound through unchanged.
* **Display list (VDL) effects**: accepted but not emulated (the SPORT device for VRAM clears and copies is done).
* **Loading separate programs** as tasks (CreateItem(Task) without a PC tag).
* Folio vector slots that the public headers don't document (File folio vectors, -31) are logged stubs.
* The analyzer treats `mov lr, pc` + `ldr pc, …` as a call. Jump tables still go through the interpreter until hints are fed back.

### Audio

The audio folio is implemented on top of a host mixer (`runtime/src/audio.cpp`), output
through `waveOut` on Windows (no extra libraries):

* instruments are recognised by template name: sample players (`sampler`, `varmono*`,
  `fixedmono/stereo*`, `halfmono*`, SDX2 / ADP4 decoders), mixers (`mixerNx1/x2`, with
  `LeftGainN` / `RightGainN` / `GainN` knobs) and simple oscillators; other DSP programs pass
  their input through;
* samples: `LoadSample` (AIFF / AIFC with loops), `DefineSampleHere`, `MakeSample`,
  `CreateSample`, `ScanSample`, `SetAudioItemInfo` / `GetAudioItemInfo`. Sample data is read
  from guest memory as it plays, so streaming players that refill buffers work;
* attachments with sustain / release loops, `LinkAttachments`, `MonitorAttachment` cues,
  start / release / stop, envelopes as gain curves; `StartInstrument` rate, pitch, frequency,
  amplitude and velocity tags; `BendInstrumentPitch`; pause / resume;
* `TDORT_WAV=file.wav` records the sound on headless runs (for testing).

## Roadmap
3. VDL basics, audio folio → XAudio2
4. Audio folio → XAudio2
5. Recompiler optimizations: direct block chaining, register caching, jump-table recovery
6. Per-game patch files (widescreen, 60 fps, input remaps) and an Xbox UWP platform layer
