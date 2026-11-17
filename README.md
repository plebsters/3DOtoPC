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
screenshots identical), **Exited**, **Crashed**, **Timed out**, **Conversion failed**.

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
unread messages, free memory, and the problems the log reported (files not found, out of
memory, unsupported calls). Most stalls can be explained from this file alone, without the
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
| `TDORT_DUMP_SWITCH=<thread>` | log that thread's frame pointer and saved frame each time it resumes |
| `TDORT_DUMPMEM=frame:addr:len` | write guest memory to `mem_<frame>.bin` |
| `TDORT_SLEEP_TRACE=1` | log audio-clock sleeps |
| `TDORT_AUDIO_STATUS=1` | keep the status line going past 60 s, with the active voices |
| `TDORT_SPEED_TRACE=1` | every 2 s: how many frames used all the CPU |
| `TDORT_CPI=2` | cycles per instruction (default 1); higher gives games less CPU per frame |
| `TDORT_OLD_IO=1` | old I/O behaviour (timer `SendIO` sleeps the caller, `WaitIO` returns at once), as a fallback |
| `TDORT_DEBUG_IO=1` | log deferred I/O completions and `AbortIO` |
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

* **Kernel:** items, tasks and threads (priority scheduling with time slices), signals,
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
* **Graphics folio:** screen groups, bitmaps, `DisplayScreen`, VDLs (basic), cel drawing,
  `MapCel`, pens, `MoveTo`/`DrawTo` lines, `FillRect`, `WritePixel`/`ReadPixel`, text with
  a built-in 8x8 font (`DrawText8`/`DrawText16`/`DrawChar`), `QueryGraphics`.
* **Event broker:** controller events, focus, pod descriptions; a listener that isn't
  reading keeps at most 4 unread events, so it can't starve the others.
* **Audio folio** (host mixer, `waveOut` output): sample players by template (`sampler`,
  `varmono*`, `fixedmono/stereo*`, `halfmono*`, SDX2 and ADP4 decoders), mixers, envelopes
  (also on `envelope.dsp` instruments, ending with their cue), attachments, loops, cues,
  `LinkAttachments`, the audio clock at the rate the game sets (`SetAudioRate`,
  `SetAudioDuration`), `SleepUntilTime`.
* **Operamath, International** (English/US locale, number and date formatting, string
  compare/convert), **Compression** (LZSS compressor and decompressor), **JString**
  (Shift-JIS, Unicode, ASCII, romaji, hiragana, katakana).

**Milestone 3 (done): CEL engine, input, threads.** Software rasterizer for CCB chains
(packed/unpacked, all bit depths, PLUTs, pixel processor, corner engine, clipping),
controller input from keyboard and Xbox pad, NVRAM saves, debug console, crash reports.

**Milestone 4 (done): real games.** Compatibility is tracked in the "3DO to PC —
Compatibility Status" sheet. Playable: Real Pinball, Slam 'n Jam '95, Gex, Burning Soldier,
Crash 'n Burn, Demolition Man, John Madden Football. Partly working: Brain Dead 13 (video
scenes play), Creature Shock (intro plays), Doom (reaches E1M1), Captain Quazar, Escape from
Monster Manor, Alone in the Dark, Jurassic Park Interactive, Another World, Ballz, Cannon
Fodder, Hell. Still stuck: Cyberia, Dragon Lore.

Fixes found by running games (each a difference from the real OS):
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
page-aligned memory, asynchronous timers, `WaitIO` and `AbortIO`. Every change is also run
against the local game builds before release.

**Not done yet:**
* Sound DSP effects (reverb, filters, echo) pass the sound through unchanged.
* Cinepak and the 3DO streaming-movie library are untested.
* Light gun, mouse and two-player input are untested.
* Separately loaded programs (Madden's front end, Gex's movie player) run in the
  interpreter rather than being recompiled.
* CPU timing counts one cycle per instruction and drawing costs no CPU time, so games that
  run as fast as the CPU allows can run faster than on a console (`TDORT_CPI` adjusts it).

## Roadmap

1. Recompile separately loaded programs at conversion time.
2. A more accurate speed model (cycles per instruction, cel engine time).
3. Cinepak / streaming-movie library, DSP effects, more input devices.
4. Recompiler optimizations: direct block chaining, register caching, jump-table recovery.
5. Per-game patch files (widescreen, 60 fps, input remaps) and an Xbox UWP platform layer.
