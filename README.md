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

## Multi-disc games

Drop disc 1 on **Convert-3DO.bat**: discs named like `Game (Disc 1 of 2).cue` find their
other discs in the same folder by name (or drop them all at once). Batch-Test.bat groups
them the same way. From the command line: `"3DO to PC CLI" convert disc1.cue games\Game
--disc disc2.cue --disc disc3.cue`.

Disc 1 is converted; the others are unpacked beside it as `data_disc2`, `data_disc3`, ...
Converting again removes `data_disc` folders left over from an earlier conversion that are
not part of this one.
When the game asks for a file that only another disc has (what happens after "insert
disc 2"), that disc goes in by itself. If a game does not notice, press **F2** in the game
window to put in the next disc. (The 3DO to PC window converts single discs for now.)

## Batch testing a whole library

Drag the folder that holds your 3DO disc images (`.iso` / `.cue` / `.bin`) or extracted
game folders onto **Batch-Test.bat** and leave it running (overnight works well).
For each game it:

1. converts it into `games\<Name>\` (same as Convert-3DO.bat, so the launcher shows it),
2. runs it without a window for about 90 seconds of game time: 30 s untouched for logos and
   intros, then pressing Start / A / C / directions every 2.5 s to get through menus,
3. saves its log and five screenshots to `batch-results\<Name>\`.

At the end it writes `batch-results\report.html` (every game with its result,
diagnosis and screenshots), `summary.csv`, `problems.txt` (missing OS features, most common
first, then the diagnosis of every game that crashed, stalled or stopped) and
**`batch-results-<date>.zip`** to send back. No game data goes into the zip.

Each game's folder in `batch-results\` also holds its `tdort_report.txt` (see
[Diagnosis report](#diagnosis-report)), and its one-line verdict goes into the
**Diagnosis** column of `summary.csv`.

Copies of the same disc (an `.iso` and a `.cue` of one game, two releases, or a folder from
an earlier extract) count as one disc: the batch tester keeps the best copy (`.cue`, then
`.iso`, then a folder, then `.bin`) and lists the skipped ones. Only a different disc
number (`(Disc 2 of 2)`) makes another disc.

Results: **Running** (still going and drawing at the end), **Screen stuck** (last two
screenshots identical), **Exited**, **Crashed**, **Timed out**, **Conversion failed**,
**Conversion timed out** (still converting after 60 minutes; `-ConvertTimeoutMinutes 90`
allows longer).

Games already tested are skipped, so you can close the window and start again later.
Options (from a PowerShell prompt): `tools\batch-test.ps1 <folder> -Rerun` tests everything
again, `-Only "Gex*"` picks games by name, `-Frames 9000` runs longer, `-TimeoutMinutes 20`.

## OS coverage report

`tools/api_coverage.py` lists, folio by folio, which 3DO OS calls the runtime
implements, which are placeholders and which are missing, by comparing the runtime
with the function tables of the Portfolio OS sources (only call names and numbers
are read from them; get a copy with `git clone https://github.com/trapexit/portfolio_os`):

    set TDORT_DUMP_API=api.tsv & set TDORT_FRAMES=1 & games\Gex\Gex.exe --headless
    python tools\api_coverage.py path\to\portfolio_os api.tsv --usage batch-results

`--usage` ranks the gaps by how many games in a batch test called them.

Current result: **all 330 calls** of the eight folios have an implementation.

| Folio | Calls |
|---|---|
| Kernel (tasks, memory, items, signals, messages, I/O) | 78 |
| Graphics | 79 |
| Audio | 87 |
| File | 29 |
| Operamath | 27 |
| JString (Japanese text) | 16 |
| Compression | 8 |
| International | 6 |

Nine of them deliberately answer "not supported" because what they need doesn't exist on a
PC: overlay screens (`DisplayOverlay`), RAM fonts (`OpenRAMFont`), links on the read-only
disc (`CreateLink`), overlays (`LoadOverlay`), supervisor code (`callbacksuper`), driver-only
`CompleteIO`, and two internal file-system entry points. About 25 more accept the call and
do the minimum because nothing here needs more (DSP resource accounting, master tuning,
audio input, `Forbid`/`Permit`, `ControlMem`). SWIs 0x100–0x1FF (the debugger folio, absent
on retail consoles) are ignored and leave the registers alone, as the ROM does.

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

## GitHub builds

`.github/workflows/build.yml` builds the project on GitHub on every push and pull request:

- **Tests (Linux)** builds the converter and runtime and runs the automated tests.
- **Windows package** builds everything `Build-App.bat` builds (converter app, launcher,
  CLI, `tdocore.dll`, runtime) and attaches it to the run as **3DO2PC-windows**
  (Actions tab → the run → Artifacts).
- Pushing a tag such as `v1.0` (`git tag v1.0 && git push origin v1.0`) also creates a
  GitHub release with `3DO2PC-v1.0-windows.zip`.

`.gitignore` keeps build output, converted games and disc images out of the repository.

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
* **Every program on the disc.** Besides the LaunchMe, the converter translates the
  other programs it finds on the disc(s) (front ends, movie players, game parts started
  with `LoadProgram` or `LoadCode`; the disc's `System` folder is skipped, since the
  runtime replaces the OS). These programs are loaded at an address chosen at run time,
  so their code is written relative to the load address. When the game starts one, the
  runtime recognises the file by its hash and, once the program has relocated itself and
  reaches its entry point, switches on each translated block whose bytes still match.
  The log says `program "TF": 5960 of 5974 translated blocks match and run natively`.
* **CPU timing.** The ARM60 runs at 12.5 MHz with no cache, so instructions
  take different times: a register move 1 clock, a load 4, a branch 4, a
  multiple-register store about 1 per register plus 3. `runtime/include/tdort/arm_timing.h`
  holds the table (from the ARM6 datasheet: S = 1 clock, N = 2, I = 1). The
  translator adds it up per block when it writes the C++, and the interpreter
  adds it per instruction, so both paths run at the same speed. `DrawCels` also
  costs CPU time, because the CPU waits while the cel engine draws: about half
  a CPU clock per pixel written (the engine runs on the 25 MHz system clock) plus
  40 per cel. The cost of a non-sequential memory access is a run-time setting
  (`TDORT_N_CLOCKS`), so it can be tuned without converting the game again.
  Each OS call costs 200 clocks, about what Portfolio spends entering the kernel and
  checking the call (`TDORT_OS_CYCLES`). Disc reads move about 300 KB a second, as a
  2x drive does: an asynchronous read finishes when the drive would be done, and a
  synchronous one (`DoIO`) makes its task wait for the drive. Several small reads can
  finish in the same frame. SPORT copies (VRAM fills and page copies) finish at the
  next vertical blank, as on the console. Each frame gets 12.5 MHz / 60 Hz of
  clocks; a game that needs more slows down, as on the console.
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

### Faster conversions: the runtime library cache

The runtime library (the Portfolio replacement, about 60% of the compile work) is the same
for every game, so it is compiled once per runtime version and kept in `cache/` next to the
converter (or in the temp folder if that one isn't writable). Each conversion then compiles
only the game's own translated code: a small game takes about 9 s instead of 26 s on a
2-core machine. A new package changes the runtime, so its first conversion builds the cache
again, once. The build also uses every CPU core (`--parallel`), with Ninja or the Visual
Studio generator. Set `TDO_NO_RUNTIME_CACHE=1` to build the runtime with each game as
before; delete `cache/` to clear it.

Programs the game loads from the disc are translated so they run wherever they land,
and each block is written relative to its own first instruction. Blocks with the same
instructions, in one program or several, are compiled once and shared. Samurai Shodown
has a separate program for every pair of fighters, each carrying the same engine, and
used to compile the same blocks about 90 times (over 3,600 files). The conversion log
shows how much was shared ("Other programs: N blocks, M different").

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
To re-convert a game after updating 3DO to PC, convert its own `data` folder into the same
folder: `"3DO to PC CLI" convert out\data out\ --name N` (add `--disc out\data_disc2` and so
on for multi-disc games). The game files are reused in place, not copied again.
Converted game flags: `--interp` (interpreter only), `--trace` (log HLE calls), `--headless`.
Each run writes `tdort.log`. Every unimplemented SWI or folio call is logged
once with its arguments, so the log is the to-do list for that game.

## Playing: keyboard and controller

| 3DO button | Keyboard |
|---|---|
| D-pad | Arrow keys |
| A / B / C | Z / X / C |
| L / R | A / S |
| Play/Pause (Start) | Enter |
| X (Stop) | Backspace |
| Next disc | F2 |

An Xbox controller works too (XInput).

## Diagnosis report

Every run writes **`tdort_report.txt`** next to the game's `.exe` when it ends (window
closed, frame limit, exit or crash). Its first line is a **VERDICT**:

| Verdict | Meaning |
|---|---|
| `RUNNING` | the picture was still changing at the end |
| `WAITING FOR INPUT?` | the picture stopped changing but the game is still reading the controller (usually a menu) |
| `STALLED` | the picture stopped changing; names the main thread that is stuck and what it waits for, or, if none is, what each long-waiting thread waits for |
| `DEADLOCK` | every thread is waiting and nothing can wake them |
| `CRASH` | the reason, address and thread |
| `EXITED` | the game ended itself |

What a thread waits for is spelled out: a message port and how many messages are unread,
an audio cue, a timer or I/O completion, a signal from another thread, or the reply to a
message (and whether that message is still queued or another thread took it without
replying). Below the verdict come every thread's state and last ten OS calls, ports with
unread messages, the last disc reads (file, block, destination address), free memory, and
the problems the log reported (files not found, out of memory, unsupported calls). When a
game overwrites its own code, `tdort.log` says who wrote it: game code near an address, or
an OS call such as a disc read. Most stalls can be explained from this file alone, without the
game's disc.

## Debugging a converted game

`tdort.log` (next to the game's .exe) records the first OS calls, every unimplemented call,
a task status line every 2 s (each task's state, pending/waited signals and a stack
backtrace), crash reports, the diagnosis verdict, and at exit the most-called OS functions.

Converted game flags: `--headless` (no window), `--trace` (log every OS call and task
switch), `--interp` (interpreter only).

Environment variables:

| Variable | Effect |
|---|---|
| `TDORT_FRAMES=n` | headless: stop after n frames (default 600) |
| `TDORT_SNAP=300,600` | headless: save `snap_<frame>.bmp` of the screen at those frames |
| `TDORT_PRESS=1900-1906:P,2100:A` | headless: hold buttons on frame ranges (U D L R A B C P=Play X=Stop l r) |
| `TDORT_WAV=file.wav` | headless: record the sound |
| `TDORT_TRACE_ALL=1` | with `--trace`: also log repeated identical calls |
| `TDORT_TRACE_READS=1` | log every file read |
| `TDORT_MEM_TRACE=1` | log every allocation and free |
| `TDORT_WATCH=2AFA64` | log when that memory word changes (checked at each OS call) |
| `TDORT_BREAK=3C6C8,2C27C` | log the frame, the registers and 8 words at the stack each time code reaches one of those addresses |
| `TDORT_BREAK_DUMP=2ABE20` | with `TDORT_BREAK`, also log 32 words at that address (or `r4`: at the address in that register) |
| `TDORT_NULLREAD=1` | when a disc read goes to address 0, log the caller chain |
| `TDORT_DUMP_SWITCH=<thread>` | log that thread's frame pointer and saved frame each time it resumes |
| `TDORT_DUMPMEM=frame:addr:len` | write guest memory to `mem_<frame>.bin` |
| `TDORT_SLEEP_TRACE=1` | log audio-clock sleeps and the tags of `CreateSample`/`SetAudioItemInfo` |
| `TDORT_AUDIO_STATUS=1` | keep the status line going past 60 s, with the active voices |
| `TDORT_SPEED_TRACE=1` | every 2 s: how many frames used all the CPU, and the average CPU load |
| `TDORT_TIMING=old` | old speed model: one clock per instruction and free drawing (games get about twice the CPU) |
| `TDORT_CEL_TRACE=1` | log every cel drawn, each `DrawCels` target (bitmap, size, clip) and each clip change, to stderr |
| `TDORT_CEL_CYCLES=0.5` | CPU clocks lost per pixel the cel engine draws (default 0.5; 0 = drawing is free) |
| `TDORT_N_CLOCKS=2` | clocks per non-sequential memory access (default 2, from the datasheet; 1 = faster memory, games get more CPU) |
| `TDORT_CPI=2` | divides the clocks per frame (default 1); higher gives games less CPU per frame |
| `TDORT_OLD_IO=1` | old I/O behaviour (timer `SendIO` sleeps the caller, `WaitIO` returns at once), as a fallback |
| `TDORT_DEBUG_IO=1` | log deferred I/O completions and `AbortIO` |
| `TDORT_CD_SPEED=5120` | disc read speed in bytes per frame for asynchronous reads (default 5120, a 2x drive; 0 = instant) |
| `TDORT_CD_SYNC=0` | synchronous disc reads (`DoIO`) finish at once instead of waiting for the drive |
| `TDORT_OS_CYCLES=200` | CPU clocks charged for each OS call (default 200; 0 = OS calls are free) |
| `TDORT_OLD_BITMAP=1` | `CreateItem` of a bitmap makes a bare item, as before (fallback) |
| `TDORT_DEBUG_DISPLAY=1` | log `DisplayScreen` and `DrawCels` targets |
| `TDORT_DUMP_API=api.tsv` | write the table of implemented OS calls (for `api_coverage.py`) |

## Status

**Milestone 1 (done):** Opera FS (2048/2352-byte sectors, nested dirs), AIF parsing,
ARMv3 decode/disassembly/execution, analysis, C++ emission, local build,
hybrid dispatch, write invalidation, hint feedback, Win32 window/input.

**Milestone 2 (done): Portfolio OS replacement** (`runtime/src/hle_*.cpp`). Numbers come
from the public Portfolio SDK headers (structure offsets measured by compiling them for
big-endian ARM, SWI numbers and vector indices read from them). The leaked Portfolio
sources are used only for interface facts (call names, numbers, documented behaviour); no
code is taken from them. All of it is in `runtime/include/tdort/portfolio_abi.h` as plain
numbers.

* **Kernel:** items, tasks and threads (priority scheduling with time slices; the creator
  gets `SIGF_DEADTASK`, or the exit message it asked for, when a program or thread ends), signals,
  message ports, semaphores, memory (first-fit DRAM then VRAM, `MEMTYPE_FILL`, `FROMTOP`,
  `STARTPAGE`/`SYSTEMPAGESIZE` page alignment, `INPAGE`, free-page bitmaps), lists
  including `UniversalInsertNode` (runs the game's compare function), `SetFunction`,
  `SetItemOwner`, tag lists, errors.
* **I/O:** `SendIO` is asynchronous as on the console: timer delays complete on their due
  frame with `SIGF_IODONE` or a reply message, SPORT transfers at the next vertical blank.
  `DoIO` waits; `WaitIO` blocks until done and takes the completion; `AbortIO` ends a
  pending request with `ER_Aborted`; each request starts with a clear error. Reads past the
  end of a file return the full length, as the drive does.
* **Devices:** timer (VBL and microsecond units, delays and time reads), SPORT (VRAM
  clear/copy), CD-ROM status, file devices.
* **File folio:** files, directories, streams (`OpenDiskStream`), aliases, NVRAM saves,
  `LoadProgram`, code modules (`LoadCode`, `ExecuteAsSubroutine`, `ExecuteAsThread`,
  `UnloadCode`), multi-disc lookup.
* **Graphics folio:** screen groups, bitmaps (also `CreateItem` of a bitmap over the game's
  own buffer), `DisplayScreen`, VDLs (basic), cel drawing,
  `MapCel`, pens, `MoveTo`/`DrawTo` lines, `FillRect`, `WritePixel`/`ReadPixel`, text with
  a built-in 8x8 font (`DrawText8`/`DrawText16`/`DrawChar`), `QueryGraphics`.
* **Event broker:** controller events, focus, pod descriptions; a listener that isn't
  reading keeps at most 4 unread events, so it can't starve the others.
* **Audio folio** (host mixer, `waveOut` output): sample players by template (`sampler`,
  `varmono*`, `fixedmono/stereo*`, `halfmono*`, SDX2 and ADP4 decoders), mixers, envelopes
  (also on `envelope.dsp` instruments, ending with their cue), attachments, loops, cues,
  `LinkAttachments`, the audio clock at the rate the game sets (`SetAudioRate`,
  `SetAudioDuration`), `SleepUntilTime`, samples made from an AIFF image in memory
  (`CreateSample` with `AF_TAG_IMAGE_ADDRESS`, left in place or copied), samples attached to
  an instrument template (given to every instrument made from it), `AbandonInstrument` and
  `AdoptInstrument` (hands back only an abandoned instrument, as the music library expects).
* **C library output:** `printf` goes to the log; `sprintf` writes through the output
  routine the C library passes, so names built with it (`CAM000`) are right.
* **Operamath, International** (English/US locale, number and date formatting, string
  compare/convert), **Compression** (LZSS compressor and decompressor), **JString**
  (Shift-JIS, Unicode, ASCII, romaji, hiragana, katakana).

**Milestone 3 (done): CEL engine, input, threads.** Software rasterizer for CCB chains
(packed/unpacked, all bit depths, PLUTs, pixel processor, corner engine, clipping),
controller input from keyboard and Xbox pad, NVRAM saves, debug console, crash reports.

**Milestone 4 (done): real games.** Compatibility is tracked in the "3DO to PC —
Compatibility Status" sheet. Playable: Real Pinball, Slam 'n Jam '95, Gex, Burning Soldier,
Crash 'n Burn, Demolition Man, John Madden Football, FIFA International Soccer. Movies play in
Sewer Shark, Corpse Killer, Night Trap, Drug Wars (menu) and Dragon's Lair. Partly working: Brain Dead 13 (video
scenes play), Creature Shock (intro plays), Doom (reaches E1M1), Captain Quazar, Escape from
Monster Manor, Alone in the Dark, Jurassic Park Interactive, Another World, Ballz, Cannon
Fodder, Hell, Cyberia (reaches the name entry), Dragon Lore (intro with sound and the main
menu).

Fixes found by running games (each a difference from the real OS):
* EZFlix movies (Zhadnost, Killing Time): the library decodes its video on the sound chip:
  `ezflix225.dsp` (and the older `vpp4.dsp`) turn each 16-bit word of the compressed Y, Y and
  UV planes into four delta pairs from two 16-entry tables (loaded from a fourth FIFO), and
  the ARM code adds them up into the picture. The runtime does that translation natively,
  so the movies play (they showed a flat grey box). Their sound is ordinary SDX2 stereo
  through the streaming library and plays as before. `TDORT_NO_VPP=1` turns it off.
* Twisted: The Game Show: disc reads finished at once, so its own streaming code reached a
  "wait" chunk before the stream's subscribers had answered its start command; the chunk
  clears the pending answers, the start ran twice and failed, and the opening was dropped.
  Asynchronous disc reads now take as long as on a 2x CD drive (about 5 KB a frame, one
  request at a time); `TDORT_CD_SPEED=<bytes per frame>` changes it, `0` makes reads instant.
* Wolfenstein 3D: the game calls `SetClipWidth(bitmap, 0)` before drawing its 3D view. The
  graphics folio refuses a clip window that doesn't fit the bitmap (`GRAFERR_BADCLIP`) and
  leaves the clip as it was; the runtime clamped it to 1 pixel, so only the leftmost column
  of the view was drawn. `SetClipWidth`, `SetClipHeight` and `SetClipOrigin` now accept and
  refuse exactly what the folio does (`SetClipOrigin` also rounds Y down to an even line).
  `TDORT_OLD_CLIP=1` brings back the old behaviour.
* The Coven, Plumbers Don't Wear Ties: with one task, `WaitPort` on an empty port gave up
  with an error, since nothing could ever send there. Now that disc reads take drive time,
  the reply to a read still on its way arrives there a few frames later: the games reported
  "Failure in WaitReadDoneBlockFile" and quit. `WaitPort` and `WaitSignal` now wait while
  an I/O request of the task is still in progress.
* Corpse Killer: event broker "button pressed" and "button released" frames carried the whole
  pad, where the console's pad driver puts only the buttons that just went down or came up.
  The game clears held buttons from released frames, so its gun fired on its own and the
  crosshair kept sliding to the screen edges. (`TDORT_OLD_PAD_EVENTS=1` restores the old way.)
* Sewer Shark, Corpse Killer, Night Trap (Digital Pictures' movie engine): messages created
  with `CREATEMSG_TAG_MSG_IS_SMALL` didn't get the `MESSAGE_SMALL` flag, so the video thread
  rejected every command ("Video: Unknown message!") and all threads waited forever. The
  intros now play.
* Night Trap: a missing file returned the kernel's generic "not found" instead of the file
  folio's `ER_Fs_NoFile` error (`0xD556F101`), so the game never created its save file and
  retried forever. Sewer Shark's boot file is called `Launchme`; it is now found in any case.
* FIFA International Soccer: its movie player makes a bitmap with `CreateItem` (width,
  height, buffer tags) and decodes the Cinepak frames into `bm_Buffer`. The runtime made a
  bare item with no bitmap fields, so the frames went to address 0 and the screen stayed
  blank. The intro movies now play and the game reaches a full match.
* Super Street Fighter II Turbo: its music thread (the highest priority) sleeps with
  `SleepUntilTime` until a time already past, in a loop. The folio still waits for the next
  audio-timer tick there; the runtime returned at once, so no other thread ever ran. Now a
  task gets at most one such return per audio tick (4 a frame) and then waits for the frame.
* Putt-Putt Goes to the Moon asks for `$boot/HEGAMES/$boot/HEGAMES/puttmoon.he0`. The file
  folio expands an alias in any part of a path, and `$boot` (`/cd-rom`) goes back to the root,
  so this is `/cd-rom/HEGAMES/puttmoon.he0`. The runtime only expanded a leading alias.
* FIFA International Soccer: `LookupItem(0)` gave 0 instead of KernelBase (item 0 is the
  kernel). The 1.x `WaitIO` looks up `io_MsgItem`, which is 0 without a reply port, and read
  garbage, so its timer task polled forever at the highest priority.
* Zhadnost and Killing Time now get past their logos, and their EZFlix movies play (above).
* Myst, 3DO Multi Game Sampler 3: the discs have file names Windows can't store (`?`, `<`,
  `>`), so conversion failed. Such characters are now stored as `%XX` and the runtime reads
  the names back exactly (`tdort/host_name.h`).
* PO'ed, Cyberia's joystick check, Creature Shock: messages created with a data area
  (`CREATEMSG_TAG_DATA_SIZE`) didn't get one, so the event broker couldn't answer
  `EB_DescribePods` and PO'ed quit with "could not connect to event broker". Creature Shock
  now skips its intro with Start and reaches the mission menu.
* Immercenary: `ExecuteAsSubroutine` started code modules without argc, argv and KernelBase
  in r5-r7 (and with r0 wrong), so its Cinepak module crashed at once.
* Super Street Fighter II Turbo, 3DO Magazine 2: threads with tiny stacks (256 bytes) run past
  their limit; the C library then calls the kernel to grow the stack (vector -31), which
  wasn't there. The stack limit is now the kernel's (base + 128) and the stack grows.
* `printf`/`sprintf` text now goes through the C library's own output routine (the fifth
  argument of the kernel call), as on the console, whatever the library version.
* Dragon Lore: `CreateSample` ignored an AIFF image in memory, so its sound buffer had no
  address and the next movie read its first block over address 0 and crashed; samples
  attached to an instrument template were refused; `AdoptInstrument` made a new instrument
  instead of returning an abandoned one, which the music library reports as an error.
* Cyberia: `sprintf` output was lost (the C library passes its own output routine), so the
  camera file names came out as `CAM00` and the game retried a missing file forever, taking
  minutes per frame. Its other programs (TF, 3DOCAM, SELECTMENU) now run translated:
  3000 frames take 6 seconds instead of more than 8 minutes.
* Cyberia: threads ending sent no `SIGF_DEADTASK`, so its movie player waited forever for
  its streaming threads after the intro.
* Creature Shock: an envelope on an `envelope.dsp` instrument never ended, so its end cue
  never fired; `SendIO` returned a stale `ER_Aborted` after `AbortIO`.
* Brain Dead 13: `MEMTYPE_STARTPAGE` was ignored; the game clears whole pages, which wiped
  a thread stack sharing the page.
* Crash 'n Burn: the audio clock ignored `SetAudioRate` (128 Hz), so the race music played
  almost twice too fast.
* John Madden Football: the boot program's unread controller events used up the event
  pool, so Game Setup got no input; debugger SWIs zeroed a register.
* Doom: microsecond timer delays ended at once; Captain Quazar: items of ended programs
  were never freed; Jurassic Park Interactive: the clock didn't advance while idle.

**Testing:** `tests/` holds unit tests and end-to-end tests on synthetic discs. The guest
program `tests/guest/api.c` (compiled for the 3DO CPU) checks 20 groups of OS behaviour,
including callbacks into game code, compression round trips, Japanese text conversion,
page-aligned memory, asynchronous timers, `WaitIO` and `AbortIO`. `tests/guest/audio.c` checks the
audio folio the same way, including samples from an AIFF image, template attachments and
`AdoptInstrument`. Every change is also run
against the local game builds before release.

**Not done yet:**
* Sound DSP effects (reverb, filters, echo) pass the sound through unchanged.
* Light gun, mouse and two-player input are untested.
* CPU timing counts the ARM60's clocks per instruction, the cel engine's drawing time, a
  flat cost per OS call and the CD drive's speed. Not modelled: DRAM page timing (the
  memory cost is a single datasheet figure), how much longer some OS calls take than
  others, disc seek time, and load time for programs started with `LoadCode` /
  `LoadProgram` (they load at once).

## Roadmap

1. Speed model: per-call OS costs, disc seek time (instruction, drawing, OS call and drive timing are done).
2. Cinepak / streaming-movie library, DSP effects, more input devices.
3. Recompiler optimizations: direct block chaining, register caching, jump-table recovery.
4. Per-game patch files (widescreen, 60 fps, input remaps) and an Xbox UWP platform layer.
