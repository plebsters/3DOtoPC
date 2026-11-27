# 3DO to PC — 3DO games → native Windows programs

Turns a 3DO disc image you own into a native Windows `.exe`. Everything runs
on the local PC: disc reading, analysis, code generation and compilation.
There is no network code anywhere in the tool or in the games it produces.

> **No Visual Studio needed.** The installer includes everything: its own compiler
> (zig/clang with the MinGW-w64 headers and libraries, plus CMake and Ninja) and the .NET
> runtime for its windows. Install it, then convert games. Nothing else to download, no
> Windows SDK, no Visual C++ Redistributable, no administrator rights. The games it builds
> run on any Windows 10 or 11 PC. (Building 3DO to PC itself from source still uses Visual
> Studio; see [One-time setup](#one-time-setup).)

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

**To use 3DO to PC:** run the installer (`3DO2PC-<version>-Setup.exe`, from the GitHub release
or `Build-Installer.bat`). That's all: it includes the compiler that builds converted games
(`toolchain\`: zig, CMake and Ninja) and the .NET runtime the windows need. Visual Studio,
the Windows SDK, the .NET runtime and the Visual C++ Redistributable are **not** needed, and
nothing asks for administrator rights. The games it builds use only DLLs that come with
Windows 10 and 11. The first conversion takes about three minutes longer, once: zig builds
its copy of the C runtime into `cache\zig`.

**To build 3DO to PC itself from source** (developers): double-click
**`Install-Prerequisites.bat`** (it asks for administrator rights). It installs only what's
missing: Build Tools for Visual Studio 2022 (C++ x64 compiler, Windows SDK, C++ CMake tools)
and the .NET 8 SDK (for the window apps; add `-SkipDotNet` to skip it).

## Building the Windows app

Double-click **`Build-App.bat`**. It finds Visual Studio / Build Tools, builds the C++ core,
then builds the WPF apps (if the .NET 8 SDK is installed, self-contained) into **`dist\`**:
`3DO to PC.exe` (the app), `3DO to PC CLI.exe` (command line), `tdocore.dll`, `runtime\`, and
`toolchain\`, the compiler for converting games, which `toolchain\Get-Toolchain.ps1` downloads
once (pinned versions from PyPI, checked by SHA-256, kept in `build\downloads`). Keep those
together; copy the whole `dist\` folder if you move it.

## Windows installer

Double-click **`Build-Installer.bat`** to make **`installer\Output\3DO2PC-<version>-Setup.exe`**:
one file that installs 3DO to PC like any other Windows program. It runs `Build-App.bat`, then
packs `dist\` with **Inno Setup 6** (free; if it's missing, the script offers to install it
with winget). Give a version as the first argument (`Build-Installer.bat 1.2`), or it uses
today's date. The script is `installer\3DO2PC.iss`.

What Setup does:

- Installs for the current user by default, in `%LOCALAPPDATA%\Programs\3DO to PC` (no
  administrator rights needed); it also offers an all-users install in Program Files.
- Adds Start-menu shortcuts for **3DO to PC**, **3DO Launcher**, the converted-games folder,
  the tool setup and the uninstaller, and optional desktop shortcuts.
- Creates a `games\` folder next to the apps (the launcher looks there first) and a `cache\`
  folder for the compiled runtime, both writable.
- Includes everything needed to convert and play games: the compiler (`toolchain\`) and the
  .NET runtime of the two windows. Nothing else is downloaded or installed.
- Installs `toolchain\` fresh on every upgrade (no leftovers from an older version).
- Upgrades an older install in place. Uninstalling removes the program and its cache but
  keeps the games you converted.

The GitHub build makes the same installer (artifact **3DO2PC-windows-setup**), and a version
tag attaches it to the release next to the zip.

## The 3DO to PC window

The window has two pages, picked at the top: **Single game** and **Batch convert**. The
pill at the top right says which compiler was found (normally the bundled zig/clang).

**Single game**

- **Choose disc…** (or drop a .cue, .iso or .bin onto the window). The game name and
  **Save to** folder fill in as `games\<title>\` in the app's folder (title from the file
  name, tags like "(1994)(EU-US)[!]" left out), which is where the launcher looks. A name or
  folder you type yourself is kept.
- **Cover:** the card shows the picture the game will get as its window and exe icon (see
  *Cover art* below). **Change cover…** picks another picture (or drop a .png/.jpg onto the
  window); **Use the disc's** goes back to the one found.
- A multi-disc game is found by name, as below: choosing any disc converts all of them
  together from disc 1.
- **Advanced** (click Show):
  - **Build the .exe** (on): compile the generated C++ into the game's .exe with the
    compiler shown at the top. Off, the converter stops after writing the project (the
    game's files in `data\`, the generated `src\*.cpp`, `CMakeLists.txt`) without compiling,
    for looking at the code or building it later by hand (`cmake -S out -B out\build`).
  - **Extra function discovery**: also look for code by its function prologues; try it when a
    game reaches code the converter missed (the log and `tdort_report.txt` say so).
  - **Release build** (on): an optimised game. Off: a Debug build, slower, for debugging.
  - **Runtime hints**: the `interp_hits.txt` a converted game writes, so code it had to
    interpret gets compiled next time.
- The progress card shows the five steps (Read disc, Analyze, Generate code, Compile, Done);
  the log under it has **Copy** and **Open output folder**. **Inspect disc** lists the disc's
  files in the log.

**Batch convert**

- **Add discs…**, **Add a folder…** (searched to any depth) or drop discs and folders onto the
  page. Discs of one game become one row and are converted together; a .bin next to its .cue
  counts once. A row can be removed with its ×.
- **Settings for all:** where the games go (`games\` by default), **Skip games already
  converted** (a folder that already has an .exe; delete it to convert again), **Keep going
  after a failure**, and the same Advanced options as the Single game page.
- **Start** converts the waiting rows one after another into `<folder>\<title>\`. **Pause**
  stops after the current game (Start then goes on with the rest); **Stop** stops after the
  current game and leaves the rest out. A conversion in progress is never cut off. At the end
  the log lists what converted, failed or was skipped, and is saved as `batch-log.txt` in that
  folder.

## Cover art

Each converted game gets a cover: `cover.png` in its folder, the icon of its .exe (in
Explorer, the taskbar and the launcher) and of its window. The converter looks, in order, for

1. a picture you chose (**Change cover…**, or `--cover picture.png` on the command line);
2. a picture next to the disc: `<disc name>.png` / `.jpg`, or `cover`, `folder` or `boxart`
   `.png` / `.jpg` in the disc's folder;
3. the title screen stored on the disc (`BannerScreen`, which most discs have; the
   developer-kit sample picture some discs carry is ignored).

With none of these the game keeps the 3DO to PC icon. Nothing is looked up online.
`"3DO to PC CLI" cover game.cue cover.png` saves the cover a conversion would use.

## Multi-disc games

Drop disc 1 on **Convert-3DO.bat**: discs named like `Game (Disc 1 of 2).cue` find their
other discs in the same folder by name (or drop them all at once). Batch-Test.bat groups
them the same way. From the command line: `"3DO to PC CLI" convert disc1.cue games\Game
--disc disc2.cue --disc disc3.cue`.

Disc 1 is converted; the others are unpacked beside it as `data_disc2`, `data_disc3`, ...
Converting again removes `data_disc` folders left over from an earlier conversion that are
not part of this one.
When the game asks for a file that only another disc has (what happens after "insert
disc 2"), that disc goes in by itself. If a game does not notice, press **F4** in the game
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
- It also packs the installer (`3DO2PC-<version>-Setup.exe`, artifact **3DO2PC-windows-setup**).
- Pushing a tag such as `v1.0` (`git tag v1.0 && git push origin v1.0`) also creates a
  GitHub release with `3DO2PC-v1.0-windows.zip` and `3DO2PC-1.0-Setup.exe`.

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
1. `toolchain\` next to the app (what the installer ships): `zig\` (clang with the MinGW-w64
   headers and libraries; `zig-cc.exe`, `zig-c++.exe`, `zig-ar.exe`... are small front ends
   built from `toolchain\zigwrap.cpp`), `cmake\`, `ninja\` and `zig-windows.cmake`. Set
   `TDO_NO_BUNDLED_TOOLCHAIN=1` to skip it.
2. Visual Studio / Build Tools, found through `vswhere.exe`.
3. `cmake` on `PATH`.

## CLI

```
"3DO to PC CLI" inspect  game.iso
"3DO to PC CLI" convert  game.iso out\ [--name N] [--hints interp_hits.txt] [--cover pic.png] [--no-build] [--debug]
"3DO to PC CLI" cover    game.iso cover.png
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
| Next disc | F4 |

An Xbox controller works too (XInput): D-pad or left stick, A / B / X for 3DO A / B / C,
LB / RB for L / R, Start for Play and Back for Stop.

Press **F2** in the game window to see this mapping on screen (keyboard and controller side
by side); press F2 again to hide it.

### Save states

| Key | Does |
|---|---|
| F5 | Save the game exactly where it is, into the current slot |
| F6 | Load the current slot |
| F3 | Choose the slot (1 to 9; the message shows which) |

A save state is the whole machine at that moment (memory, the game's tasks, open files,
sound, timers), so it works in any game at any point, not only where the game lets you save.
States are files in a `savestates` folder next to the game (`slot1.state` ... `slot9.state`,
about 4 MB each). A state only loads into the same converted game: after re-converting a
game, older states are refused ("made by another game or conversion") because the
translated code may differ. The game's own saves (NVRAM) are separate and still work as before.
For testing without a window: `TDORT_STATE_SAVE=<frame>:<slot>` and `TDORT_STATE_LOAD=<frame>:<slot>`.

### Display options (change them while playing)

| Key | Does |
|---|---|
| F1 | Shows the keys (display and save states) on screen |
| F2 | Shows the button mapping (keyboard and controller); F2 again hides it |
| F4 | Puts the next disc of a multi-disc game in the drive |
| F10 | Screenshot: saves the game picture (320x240 times the F12 detail level) as a PNG in a `screenshots` folder next to the game's .exe, named after the game with the date and time |
| F7 | Window size: 2x, 3x, 4x, 5x, 6x (640x480 up to 1920x1440; limited to what fits the screen) |
| F8 | Picture shape: **4:3** (black bars), **Pixel perfect** (4:3 at a whole-number size, so every pixel is the same size), **Stretch** to the window |
| F9 | Look: **Sharp** pixels, **Smooth** (filtered), **Scanlines** (CRT-style dark lines; best at 3x or more) |
| F11 or Alt+Enter | Fullscreen on and off (a borderless window over the whole monitor, so switching is instant) |
| Ctrl+F | Frame counter (top right): frames per second the window shows, how many times a second the game's picture actually changed, and the milliseconds each frame took to make (average, worst, and the budget, 16.7 at 60 Hz), then D3D11 or GDI (how the window is drawn). Over budget means the game slows down and the sound can click |
| F12 | Detail: draws the game at 1x, 2x, 3x or 4x the 3DO's resolution (320x240 up to 1280x960). 3D games, and sprites that turn or shrink, get smooth edges and the full detail of far-away textures. 1x is the picture exactly as on the console. Default 2x |

A short message in a rounded panel at the top left confirms each change (F1 lists every key). The settings are kept in
`%APPDATA%\3DO to PC\display.cfg` and apply to every converted game; delete the file to go
back to the defaults (3x window, 4:3, sharp, 2x detail). You can also drag the window to any size: the
picture keeps its shape.

**How the detail setting works.** The game still draws into its own 320x240 screens exactly
as on the console, so it behaves the same at every level. Each cel is also drawn a second
time into a finer copy of the screen, and that copy is what you see. Pixels the game writes
itself (movies, screen clears, some text) are copied into the finer picture as they are, so
full-motion video and hand-drawn screens look the same as at 1x. The textures themselves are
not changed: a wall close to the camera still shows its original texels. The finer
picture is drawn by worker threads on your other CPU cores while the game runs on, so the
game's own thread does little extra work; on a PC with few cores, 3x and 4x can still slow a
game down (and then the sound clicks, since frames come late). Go back to 2x or 1x if that
happens. With **Sharp** look and a window size that is not a multiple of the detail level, pixel
edges can look uneven; **Smooth** (F9) or a matching size (F7) avoids that.

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
| `TDORT_MEM_TRACE=1` | log allocations and frees of 1 KB and more (`=all`: every size) |
| `TDORT_WATCH=2AFA64` | log when that memory word changes (checked at each OS call); in code, also who first wrote over it |
| `TDORT_BREAK=3C6C8,2C27C` | log the frame, the registers and 8 words at the stack each time code reaches one of those addresses |
| `TDORT_BREAK_DUMP=2ABE20` | with `TDORT_BREAK`, also log 32 words at that address (or `r4`: at the address in that register) |
| `TDORT_NULLREAD=1` | when a disc read goes to address 0, log the caller chain |
| `TDORT_DUMP_SWITCH=<thread>` | log that thread's frame pointer and saved frame each time it resumes |
| `TDORT_DUMPMEM=frame:addr:len` | write guest memory to `mem_<frame>.bin` |
| `TDORT_SLEEP_TRACE=1` | log audio-clock sleeps and the tags of `CreateSample`/`SetAudioItemInfo` |
| `TDORT_AUDIO_STATUS=1` | keep the status line going past 60 s, with the active voices |
| `TDORT_SPEED_TRACE=1` | every 2 s: how many frames used all the CPU, and the average CPU load |
| `TDORT_TIMING=old` | old speed model: one clock per instruction and free drawing (games get about twice the CPU) |
| `TDORT_OS_RESERVE_KB=128` | DRAM left out of the free memory games are told about (default 128) |
| `TDORT_OLD_LOADWAIT=1` | `LoadSample` reads the disc instantly instead of waiting for the drive |
| `TDORT_OLD_STREAM_DECODE=1` | each streamed SDX2/ADPCM buffer starts its decoder from zero |
| `TDORT_OLD_MATH=1` | `multiply.dsp` and the other arithmetic instruments add their inputs, as before |
| `TDORT_PRE0_BGND=1` | a cel's preamble BGND bit also keeps its 000 pixels opaque (old behaviour) |
| `TDORT_OLD_DEFINE_SAMPLE=1` | `DefineSampleHere` plays the sound from the file image instead of a copy (`=2`: copy into the OS's memory, not the game's allocator) |
| `TDORT_OLD_BADSWI=1` | an SWI naming no folio (16 or more) returns 0 in r0 instead of being ignored |
| `TDORT_OLD_WHERE_FRAMES=1` | `WhereAttachment` returns sample frames instead of a byte offset |
| `TDORT_NO_VDL_CLUT=1` | colour words in a display list (VDL) are ignored; the standard ramp is used |
| `TDORT_OLD_FB_MSB=1` | the cel engine leaves frame-buffer bit 15 clear instead of writing the pixel's P bit |
| `TDORT_OLD_ALLOC_LOW=1` | large allocations take the lowest free block instead of preferring memory above 512 KB |
| `TDORT_OLD_CUE_SIGNALS=1` | a deleted audio cue keeps its signal bit allocated (old behaviour) |
| `TDORT_OLD_PRI_CAP=1` | a thread more than 15 above its creator's priority is capped even when the priority is valid (10-199) |
| `TDORT_LOG_MSGDATA=1` | logs every `SendMsg` (with the first data words) and `ReplyMsg` (debugging) |
| `TDORT_OLD_IO_MSG=1` | an I/O completion message is a plain message with result io_Error, whatever OS the disc was made with |
| `TDORT_OLD_ITEM_NUMBERS=1` | game items are numbered right after the runtime's own (they start at 64 otherwise) |
| `TDORT_OLD_SEMAPHORE=1` | `UnlockSemaphore` frees the semaphore and lets the waiters retry instead of giving it to the first waiter |
| `TDORT_OLD_HELD_BUTTONS=1` | a new event listener's update frames include buttons that were already held when it started |
| `TDORT_OLD_CONTROLMEM_SCREENS=1` | a screen group still frees a frame buffer its task gave to another task with `ControlMem` |
| `TDORT_OLD_WAIT_ANY=1` | `WaitSignal` on signals the task never allocated waits instead of failing at once |
| `TDORT_OLD_NVRAM_ROOT=1` | "/NVRAM" (without a file name) is the disc's root, as before |
| `TDORT_OLD_CARET=1` | "^" in a path goes one folder up instead of to the disc's root |
| `TDORT_OLD_SETVDL=1` | `SetVDL` returns 0 for a screen that has no custom VDL yet (instead of the item for its own VDL) |
| `TDORT_OLD_DETACH_LINK=1` | deleting an attachment that has played to its end does not start the attachment linked after it |
| `TDORT_LOG_CHAIN=1` | logs sample attachments that end with nothing linked, or are deleted while playing (debugging) |
| `TDORT_OLD_CEL_SRC_ALIGN=1` | cels read their source data from the exact address, even when it isn't word-aligned |
| `TDORT_OLD_READ_ADDRESS=1` | `SetReadAddress` is ignored: blended cel pixels read the buffer being drawn into |
| `TDORT_WRITE_WATCH=<hex addr>[,<hex len>]` | logs every guest store into that range with its pc (exact with `--interp`; debugging) |
| `TDORT_PIXEL_WHO=<hex addr>` | logs every cel write to that frame-buffer address, with the cel's flags and PLUT (debugging) |
| `TDORT_OLD_FOLIO_INFO=1` | `GetAudioFolioInfo` fills in nothing (sample rate and master volume stay 0) |
| `TDORT_OLD_FSSTAT=1` | the file system query for size and free space (FSSTAT) is not answered |
| `TDORT_OLD_DELETE_ANY=1` | any task may delete any item (the console allows only the owner) |
| `TDORT_LOG_AUDIO_ITEMS=1` | logs every audio item made and every item deleted (debugging) |
| `TDORT_HI_THREADS=n` | worker threads drawing the finer picture (default: CPU threads - 1, at most 6; 0 = on the game's thread) |
| `TDORT_HI_STATS=1` | prints how long the game waited for those workers (debugging) |
| `TDORT_PAL=0/1` | NTSC (60 Hz) or PAL (50 Hz), overriding the game's `game.cfg` (`display=pal`, written for discs tagged "(PAL)") |
| `TDORT_OLD_EXEC_THREAD=1` | ExecuteAsThread reads the name and priority from the stack and starts the module without KernelBase (the old way) |
| `TDORT_OLD_TASK_AIF=1` | CreateItem of a task from a program image in memory is refused (the old way) |
| `TDORT_OLD_NVRAM_DELETE=1` | deleting an open save file leaves it open (fails on Windows) |
| `TDORT_OLD_PORT_SIGNALS=1` | deleted message ports keep their signal bits allocated |
| `TDORT_OLD_CSTARTUP=1` | the kernel's cstartup leaves r4-r6 as they were (the old way) |
| `TDORT_OLD_CHILD_PROGRAMS=1` | programs keep running after the program that started them ends |
| `TDORT_OLD_DELETE_R0=1` | a task deleting itself leaves DeleteItem's result in the next task's r0 |
| `TDORT_OLD_SHORT_DELAY=1` | timer delays under half a frame only yield (the task doesn't sleep) |
| `TDORT_OLD_THREAD_ITEMS=1` | a thread's ports, messages and I/O requests stay after it ends |
| `TDORT_LOG_FILE_IO=1` | logs every file-device command (file, command, offset, sizes) |
| `TDORT_GDI=1` | the window draws with GDI instead of Direct3D 11 |
| `TDORT_HI_DUMP=file` | with `TDORT_HI_THREADS=0`, writes every batch of finer-picture rows to the file (for benchmarking the rasterizer) |
| `TDORT_OLD_PIXC_TABLES=1` | the pixel processor computes every pixel (no lookup tables) |
| `TDORT_OLD_CEL_DECODE=1` | the 1x cel engine decodes every source pixel (no per-cel table) |
| `TDORT_OLD_QUAD_BOX=1` | the 1x cel engine tests the old, wider box around each pixel quad |
| `TDORT_OLD_FILL_QUAD=1` | the 1x cel engine fills polygons pixel by pixel as before |
| `TDORT_SCALE=1..4` | internal resolution, overriding the F12 setting (headless snapshots are taken at that size too) |
| `TDORT_TRACE_FROM=<frame>` | turns on `--trace` logging at that frame (debugging a long session) |
| `TDORT_SMALL_ALLOC_FLOOR_KB=0` | no 32 KB floor for small allocations (the size in KB; 0 turns it off) |
| `TDORT_OLD_SCREEN_DELETE=1` | deleting a screen group keeps its screens, bitmaps and frame buffers |
| `TDORT_OLD_CSG_BITMAPS=1` | `CreateScreenGroup` makes as many bitmaps per screen as asked, instead of one as the OS does |
| `TDORT_OLD_STACK_FREE=1` | a thread's tag 24 value is freed as its stack when it dies, whatever it points to |
| `TDORT_OLD_EOF_ZERO=1` | a disc read past the end of a chunk file reads zeros instead of a `FILL` chunk |
| `TDORT_OLD_STACK_GROW=1` | memory a growing stack reaches is not kept for the task |
| `TDORT_OLD_SAMPLETIME=1` | `SampleSystemTime` returns its two values in the old order |
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
* Road Rash: its own memory manager walks the kernel's free-memory lists (`kb_MemFreeLists`)
  to find which bank an address is in; the lists were empty, so it read through address 0
  and looped at start-up. The kernel's and the task's lists now hold one `MemList` per bank.
  It then took all the memory it was told was free but 36 KB, and its track loader ran out
  ("Game Loading", then a deadlock): the console's OS fills a few hundred KB of DRAM that the
  runtime keeps elsewhere, so 128 KB at the top of DRAM is now left out of the free-page
  bits games read (`TDORT_OS_RESERVE_KB`; still allocatable). `TDORT_OLD_MEMLISTS=1` empties
  the lists again.
* Need for Speed / OverDrivin': the main task lowers itself to priority 80 with `SetItemPri`,
  starts its controller thread at 92 and spins until that thread has run. `SetItemPri`
  changed only the item's copy of the priority, not the scheduler's, so the thread never ran
  (black screen). `TDORT_OLD_SETPRI=1` brings back the old behaviour.
* Lemmings: a program started with `LoadProgram` begins in the folder its file is in, as
  the file folio does (`FILETASK_TAG_CURRENTDIRECTORY`), not in its creator's current
  directory. Lemmings changes to `/CD-ROM/LemmingsData`, then starts its movie players from
  the disc's top folder, which open `LogoEA.str` and `Intro.Film` there by name.
  `TDORT_OLD_PROGRAM_CWD=1` brings back the old behaviour.
* Return Fire: the menu froze after the first button press. The menu program starts its
  music thread's fade-out and waits until the volume reaches zero; the music thread only
  steps the fade when the video field count has moved on since its last pass. Between
  starting the music and fading it, the menu loads its two sound effects with
  `LoadSample`, which read the disc instantly, so everything happened in one field and
  the fade never began. `LoadSample` (and `LoadSampleHere`, `ScanSample`) now make the
  caller wait for the drive like other synchronous disc reads (`TDORT_OLD_LOADWAIT=1`: no
  wait; `TDORT_CD_SYNC=0` also turns it off).
* Need for Speed sound: a streamed delta-coded sound (SDX2, ADPCM) restarted its decoder
  from zero at each linked buffer, which put a step in the wave at every buffer change
  (pops and crackle in the intro movie: 24 clicks in 12 seconds before, 4 after). The
  decoder state now carries on into the linked buffer, as on the DSP, where it belongs to
  the instrument (`TDORT_OLD_STREAM_DECODE=1`: reset). `multiply.dsp`, `timesplus.dsp`,
  `subtract.dsp` and `add.dsp` now compute their output from their `InputA`/`InputB`/
  `InputC` knobs (a connected instrument or a set value) instead of adding their inputs;
  Need for Speed scales each engine voice's left and right level with `multiply.dsp`, so
  the engines played at full level, unpanned (`TDORT_OLD_MATH=1`: the old sum).
* Return Fire: bushes had black boxes around them and the palm trees' shadows were dark
  rectangles. Their cels set the BGND bit in the preamble word, which the runtime took to
  mean "000 pixels are opaque". The cel engine only goes by the CCB's BGND flag (Opera, the
  reference 3DO emulator, never tests the preamble bit), so 000 pixels of these cels are
  now transparent. `TDORT_PRE0_BGND=1` brings back the old behaviour.
* Need for Speed: static and clicks while driving. The game loads each sound family (engine,
  collisions) into a scratch buffer, defines its samples with `DefineSampleHere`, then
  reuses the buffer for car data. The folio copies a sample's sound out of the image (into
  memory from the caller's alloc function); the runtime played it from the image, so once
  the buffer was reused the engine played whatever was written over it (a click at every
  loop, about four a second). `DefineSampleHere` now copies the sound, as `CreateSample`
  with an image already did (`TDORT_OLD_DEFINE_SAMPLE=1`: play it from the image).
* Wing Commander III printed "Clock failed." at start-up: `OwnAudioClock` returned 0, but
  it returns an item (the owner key for `SetAudioRate` and `DisownAudioClock`). It now
  returns one, and the game sets its clock (225 Hz).
* Wing Commander III then stayed black at frame 130. Its movie player waits until
  `WhereAttachment` reaches the end of a 2940-byte sound buffer; the call returned a
  position in sample frames (at most 1470), so the wait never ended. It now returns a byte
  offset, as the folio does (frames times the bytes per frame as the instrument reads the
  data; `TDORT_OLD_WHERE_FRAMES=1`: frames as before). The intro movies now play.
* Wing Commander III's movies looked like coloured noise. Their frames are stored for a
  colour table that the game's display list (VDL) loads, not for the standard ramp, and VDL
  colours were ignored. `SubmitVDL` now reads the list's colour words line by line and the
  screen is shown through that table (a 0 pixel stays black, the background colour);
  lists that load the standard ramp change nothing (`TDORT_NO_VDL_CLUT=1`: ignore VDL
  colours).
* Wing Commander III's movies still had false-colour speckles and outlines (faces, sky,
  the Origin logo). Each frame marks the pixels whose colour is not in the scene's table
  by setting their top bit, and the display shows those pixels' RGB directly instead of
  looking them up. The cel engine dropped that bit when it wrote the frame buffer. It now
  keeps the pixel's P bit (as decoded, before the CCB's P-mode override) in bit 15, and a
  screen shown through a VDL colour table bypasses the table for such pixels
  (`TDORT_OLD_FB_MSB=1`: bit 15 stays clear). The intro movies now look right.
* Twisted stopped making progress after a while: the board stayed on the spinner, or a
  game screen stood still. Every audio cue takes a signal bit from its task, and the bits
  were never given back when the cue was deleted. Twisted makes new cues for each stream,
  so after about 23 of them the task had no bits left and new cues got signal 0. Their sound
  buffers never reported completion, and its streamer printed "A buffer completion signal
  was received out of order!" and stalled. Deleting a cue now frees its bit
  (`TDORT_OLD_CUE_SIGNALS=1`: keep them as before). The game now plays round after round.
* Shock Wave 2 froze a few seconds into the first mission: the cockpit stayed on screen
  but nothing moved and the controls did nothing. The game's sound thread asks for
  priority 119 under its 99 main task, and it must take each instrument and sample chunk
  the main task hands it (one slot) before the next one arrives. The runtime capped the
  thread to its creator's priority, so the main task overwrote chunks it had not taken yet;
  the thread never got all seven, never started the game clock, and time stood still.
  Priorities in the valid user range (10-199) are now kept; only a request above 199 is
  still capped (`TDORT_OLD_PRI_CAP=1`: cap any jump of more than 15 as before). The ship
  now flies and the radio messages stream in. The same fix stops Slam 'n Jam freezing on
  "End of 1st quarter": its sound thread ("snm", priority 175) was capped the same way.
* Quarterback Attack stayed on a black screen. Its buffer thread gets two kinds of
  message on one port: commands from the main program (small messages carrying a number
  from 1 to 24) and disc-read completions, which it recognises as small messages carrying
  the IOReq's item number. Three things were different from the console:
  - The completion message was a plain one. The kernel makes it a small message, with
    io_Error as the result and ioi_User as the size, for discs made with an OS newer than
    1.4. For older discs it sends a plain message with result and size 0 (the kernel
    keeps that for Digital Pictures' earlier titles such as Night Trap). The runtime now
    reads the disc's OS version from its `rom_tags` file (`DiscOsVersion` returns it too)
    and does the same (`TDORT_OLD_IO_MSG=1`: plain message as before).
  - Its IOReq was item 24, so the completion read as command 24. On the console the OS
    has made dozens of items before a game starts, so game items now start at 64
    (`TDORT_OLD_ITEM_NUMBERS=1`: number them as before).
  - Skipping the intro froze it. The display thread unlocks and relocks a semaphore every
    frame, and the main task, waiting for it at a lower priority, never got it: the
    runtime freed the semaphore and let the waiters race for it. The kernel hands a
    released semaphore to the first task waiting for it, and so does the runtime now
    (`TDORT_OLD_SEMAPHORE=1`: race as before).
  The intro, menus and plays now run.
* Olympic Soccer now gets through its menus into a match with the earlier memory fix
  (the 32 KB floor for small allocations).
* Sewer Shark could not leave its pause screen. It makes a new event listener as it
  unpauses, and the runtime's first update to a new listener held the whole pad, so
  Start, still held from the unpause press, paused the game again at once. The console's
  broker only reports changes, so buttons already held when a listener starts are now
  left out of its updates until they are let go (`TDORT_OLD_HELD_BUTTONS=1`: report
  them). Pausing and unpausing now work, and the intro, story and game play run.
* Samurai Shodown's fighters were drawn wrong (dark, broken or missing sprites). The
  character select program makes the two screens, gives their frame buffers to the fight
  program with `ControlMem` and exits. The runtime then deleted its screen group and, with
  it, freed the buffers it had given away; the fight program's next big allocation landed
  on top of the screens, and drawing filled its sprite tables with pixels. A buffer given
  away now belongs to the new task (`TDORT_OLD_CONTROLMEM_SCREENS=1`: free it as before).
* PAL discs: a disc file tagged "(PAL)", or a game whose program says it needs PAL
  ("Unknown screen refresh rate", "European PAL machine"), gets `display=pal` in the game's
  new `game.cfg` (settings beside the game's data folder, read at start; edit it by hand
  for another PAL-only disc). The game then runs at 50
  fields a second and is told it is on a PAL console (QueryGraphics field rate 50, field
  time 20 ms, display type PAL; the vblank timer, timer delays and the audio clock follow).
  The default picture size stays 320x240. Way of the Warrior's European disc showed
  "this version is for a European PAL machine but your machine is U.S. NTSC" and stopped;
  Powers Kingdom quit with "Unknown screen refresh rate 60". `TDORT_PAL=0/1` overrides
  the file.
* ExecuteAsThread took the thread's name and priority from the wrong places (the name is
  the 4th argument, in a register; only the priority is on the stack) and started the
  module without KernelBase, which a module's start-up code takes from r7 (r5/r6: argc,
  argv). F1GP's and Royal Pro Wrestling's modules jumped to address 0 at once
  (`TDORT_OLD_EXEC_THREAD=1`: the old way).
* A program a game loads itself and starts with CreateItem (CREATETASK_TAG_AIF, the image
  size and a command line) now runs where it is, as a task of its own, like LoadProgram.
  Sangokushi IV starts "open.x" this way and quit at once (`TDORT_OLD_TASK_AIF=1`: refused).
* Save memory: deleting a file the game still has open failed on Windows (Windows doesn't
  delete open files); the open handles are now closed first. Psychic Detective writes a
  test file, deletes it while open, and quit with "Error while deleting NVRAM file"
  (`TDORT_OLD_NVRAM_DELETE=1`: the old way).
* Message ports give back the signal they allocated for themselves when deleted, as the
  kernel does. Penthouse Interactive makes ports for each movie; after about 23 its main
  task had no signals left and it quit with "problem sending/receiving a signal"
  (`TDORT_OLD_PORT_SIGNALS=1`: kept).
* The Kanji font of Japanese consoles (/rom2/System/Graphics/Fonts/Kanji16.4) is in the
  console, not on most discs. Some Japanese discs carry a copy (Blue Forest Story, Sword &
  Sorcery); the converter now keeps one it finds in its cache and copies it into later
  games whose programs ask for it. Twinkle Knights quit with "theCCB is NULL" without it:
  convert one of those discs first, then Twinkle Knights again.
* No Visual Studio needed to convert games: the installer now ships its own compiler in
  `toolchain\`: zig 0.17.1 (clang with the MinGW-w64 headers and libraries), CMake 3.31.6 and
  Ninja 1.11.1, with front-end programs so CMake sees an ordinary compiler, and the C library
  sources zig has for other systems left out (about 340 MB unpacked). Unlike the Nov 22 test
  build (clang-cl, which still needed Microsoft's C runtime and Windows SDK files), it needs
  nothing from Microsoft, and the games it builds use only DLLs that come with Windows. The
  converter tries it before Visual Studio. The window apps are self-contained (the .NET runtime
  is included) and tdocore.dll and the CLI carry their C++ runtime, so a PC needs nothing but
  the installer. The GitHub build converts a test disc with the bundled compiler alone and
  runs the result. `xinput1_4` is linked by name with MinGW (MSVC's xinput.lib is the same DLL).
* Cover art: a converted game gets a cover (a picture you choose or put next to the disc, else
  the title screen on the disc) as `cover.png`, its exe's icon and its window's icon. See
  *Cover art*.
* New look for the 3DO to PC window: a dark theme with **Single game** and **Batch convert**
  pages. Single game shows the cover and the five conversion steps, with the build options
  folded under Advanced; Batch convert has a queue (add discs or whole folders, drag and drop,
  remove rows), per-game status and progress, skip/keep-going settings, and Pause and Stop.
  Each front end (zig-c++.exe...) has its tool built in: with spaces in the install path Ninja
  starts them by their short names (ZIG-C_~1.EXE), and the first test build took the tool from
  that name and failed ("unknown command: C_~1"). The installer then left out zig's `lib\std\Build\Cache`
  folder (its "leave out cache\" rule matched any folder named Cache); the rules now apply only
  at the top of the package, and the GitHub build installs the Setup into a folder with spaces
  and converts a test disc with the installed copy.
* A program's start-up code calls the kernel's cstartup, which hands back the argument count
  and list and leaves argc in r4, argv in r5 and 0 in r6; r6 was left holding argv. Winning
  Post's opening program sends LaunchMe a message whose size is that untouched r6, and
  LaunchMe copied 1.8 MB over its own stack and crashed after the opening
  (`TDORT_OLD_CSTARTUP=1`: registers untouched).
* When a program ends, the programs it started end too, unless it gave them to another task
  with SetItemOwner (a child task is one of its items, and its items are deleted with it).
  Furopon World's logo program left its music program running; the title's own music program
  then made a second port with the same name, the title found the old one and waited forever
  on a black screen. Samurai Shodown hands its fight program to LaunchMe first, so it keeps
  running (`TDORT_OLD_CHILD_PROGRAMS=1`: programs always outlive the one that started them).
* A task that deletes itself no longer leaves DeleteItem's result in the next task's r0 (the
  switch happens inside the delete). Ide Yousuke Meijin no Shin Jissen Mahjong's main task
  was waiting on its scheduler signal when a worker deleted itself; the wait became a wait on
  no signals, returned at once, two workers ran together and the game stayed on "Now Loading"
  (`TDORT_OLD_DELETE_R0=1`: as before).
* Timer delays shorter than half a frame add up, and the task sleeps a frame each time they
  make a whole one, so lower-priority tasks get to run as on the console. Mahjong Gokuu
  Tenjiku's timer thread (priority 150) ticks every 3 ms; each delay finished at once, it
  never stopped running and the main program never started (black screen)
  (`TDORT_OLD_SHORT_DELAY=1`: those delays only yield).
* A thread's own items (message ports, messages, I/O requests) are deleted when it ends, as
  the kernel frees every task's resources; only programs' items were before. Royal Pro
  Wrestling's logo thread left its message port behind; its movie thread then made a port
  with the same name, LaunchMe sent its start message to the dead one, and the two waited
  on each other after the logo. The opening movie, title, mode select and matches now run
  (`TDORT_OLD_THREAD_ITEMS=1`: a thread's items outlive it).
* The window now shows the picture through Direct3D 11: the game's picture is uploaded once
  a frame and the graphics card scales it into the window with the chosen look (sharp,
  smooth, scanlines), and the message and frame-counter panels are blended on top. Before,
  every screen pixel was scaled and filtered on the CPU and copied with GDI, which at 1440p
  or 4K fullscreen took a large part of each frame. Nothing new is needed to build or run
  (d3d11.dll and d3dcompiler_47.dll come with Windows 10 and 11 and are loaded at start);
  if Direct3D isn't available or the device fails, the window falls back to the GDI path
  as before. The Ctrl+F counter ends with "D3D11" or "GDI" to show which is in use
  (`TDORT_GDI=1`: always GDI).
* The finer picture's worker threads share the work better: they take turns by stripes of
  8 game lines when a frame's pixel quads are large (16 rows at 2x, 32 at 4x, instead of 8),
  so far fewer quads are set up by two workers; a quad in another worker's stripe is dropped
  before most of its setup, columns of a tall row (a wall) that can't reach a worker's
  stripe are skipped in one step, and quads smaller than a pixel that cover no pixel centre
  are dropped at once. On recorded frames the slowest of 6 workers needs 28% less time in a
  Crash 'n Burn race at 4x and 53% less in Monster Manor's corridors at 3x; one worker alone
  is 45% faster in Monster Manor. The pictures are identical with 1, 2, 4 and 6 workers.
* Faster cel decoding, the same picture: a cel's pixels with 8 bits or fewer are decoded once
  per value per cel (decoding depends only on the cel's flags, preamble and colour table) and
  16-bit pixels reuse the last value; source bits are read from a 4-byte window instead of
  one bit at a time. With the changes below, a Crash 'n Burn race at 1x takes half the
  instructions it did (regression at 1x and 2x, movies and race pictures byte for byte the
  same; `TDORT_OLD_CEL_DECODE=1`: decode every pixel).
* Faster again, with the same picture and sound: a Crash 'n Burn race at 1x now takes 36%
  fewer instructions for the whole emulation. The sound mixer worked out every connection's
  port names, gain knob names and knob values for each output sample (string searches,
  number parsing, map lookups: about 14% of the time); these are now worked out once and
  kept until a knob changes. Each source pixel of a cel is a small quad that usually covers
  less than one screen pixel: the quad is now dropped at once when no pixel centre lies
  within its corners, only pixels whose centres can be inside are tested, a single
  candidate pixel is tested directly, and the blended colour is worked out only when a
  pixel is written. Pictures (1x and 2x regression, movies, Crash 'n Burn at 1x and 4x) and
  the sound of all test games are byte for byte the same (`TDORT_OLD_QUAD_BOX=1`: the old
  quad bounds).
* Faster drawing, the same picture: the pixel processor now looks each control value up in
  a table of all its cases (made once, 9 KB) instead of working out every pixel; the 1x
  cel engine steps its edge tests across a polygon instead of recomputing them per pixel,
  and sets up the blend once per polygon; the finer picture's rows work out their clip
  window and blend modes once per row; and the sound mixer no longer builds a string for
  every knob it reads each sample. A Crash 'n Burn race runs about 11% faster at 1x and at
  4x (the whole emulation, headless). Pictures were compared byte for byte with the old
  code at 1x to 4x in Crash 'n Burn, Monster Manor and Samurai Shodown, and the regression
  and movie tests give the same results (`TDORT_OLD_PIXC_TABLES=1`, `TDORT_OLD_FILL_QUAD=1`:
  the old code).
* Pebble Beach Golf Links: the course (fairways, rough, bunkers) was drawn in shades of
  blue. Each patch of the course is drawn into a spare buffer, then a shading polygon is
  drawn into the display buffer with SetReadAddress pointing at the spare one, so the
  polygon's blend reads the patch and writes the shaded result. The runtime kept the
  SetReadAddress setting but blended against the display buffer, where the hole's sea
  image lay, so the shading landed on the water. Blends now read the buffer
  SetReadAddress names (`TDORT_OLD_READ_ADDRESS=1`: as before); such draws are made at 1x.
  The flyover and the shot screens show green fairways, bunkers and water.
* The Lost Files of Sherlock Holmes: its opening video and scenes were rows of streaks.
  It draws pictures as hundreds of small packed tiles whose source pointers are often only
  2-byte aligned; the cel engine fetches whole 32-bit words and ignores the low two
  address bits, and the runtime read from the exact byte, so every row was shifted. Cel
  source addresses are now rounded down to a word (`TDORT_OLD_CEL_SRC_ALIGN=1`: as before).
  The title scene, the prologue and the first scenes at Baker Street draw correctly.
* Wing Commander III ignored the pad from its first interactive screen (the hangar): the
  cursor never moved. Its event thread waits on every signal it owns, read from its own
  task structure (t_AllocatedSigs), and the runtime left that at 0xFF when the thread made
  its message port, so the port's signal (0x100) never woke it. Allocating a port's or an
  audio cue's signal now updates the task structure too. The hangar, the ship's rooms and
  their hotspots work.
* Japanese discs with Shift-JIS file names failed to convert ("No mapping for the Unicode
  character exists in the target multi-byte code page", seen with The Doraemons demo):
  Windows can't make a file whose name isn't valid UTF-8. In such names every byte from
  0x80 up is now stored as %XX, like the characters Windows refuses, and the runtime finds
  the files under their original names.
* Batch conversion: a .cue and the .iso it points to, both picked, are converted once
  instead of once plus an "already converted" skip; different discs with the same title
  (a sampler's JP and two US releases, say) each get their own folder, named after the
  whole file name, instead of all but the first being skipped; and each game's exe and
  window are named after the game, not the volume label "CD-ROM" most discs carry (with no
  name given, a "CD-ROM" label falls back to the disc file's name). The "CMAKE_C_COMPILER
  not used" CMake warning is gone.
* Wing Commander III's movies went silent after the first minute (and most of the intro
  had no sound). Its streamer chains small sound buffers with LinkAttachments, waits for
  WhereAttachment to reach a buffer's end, then detaches it at once. The runtime noticed
  the end only at the next output frame, so the buffer was deleted first and the link to
  the next one went with it; the chain stopped. A buffer deleted after playing to its end
  now starts the one linked after it, as the DSP does (`TDORT_OLD_DETACH_LINK=1`: as
  before). The intro and the opening scenes play with sound throughout.
* Dragon Lore's load-game screen and menus were covered in colour noise after the intro
  movie. The movie shows through display lists (VDLs) with their own colour table; to go
  back, the game hands SetVDL the item it got back from its first SetVDL, which on the
  console is the VDL the screen was made with. The runtime returned 0 there, so the
  "restore" changed nothing and the movie's colours stayed. A screen's own VDL now has an
  item, and setting it back gives the screen its normal colours (`TDORT_OLD_SETVDL=1`: as
  before). Save states from earlier packages can't be loaded (the format changed).
* Creature Shock was silent: it reads the audio folio's master volume (GetAudioFolioInfo,
  normally 0x7FFF) and scales every volume by it, and the runtime answered without filling
  it in, so everything played at volume 0. The folio's settings are now returned: 44,100 Hz
  and the master volume (`TDORT_OLD_FOLIO_INFO=1`: as before). Intro, music and effects play.
* The file system query for size and free space (FSSTAT) is answered: the save memory is
  32 KB with what the saved files leave free, the disc is full. Creature Shock checked it
  before making its settings file and made none (`TDORT_OLD_FSSTAT=1`: not answered).
* Flying Nightmares (no input in flight) needs no change: its flight commands are button
  combos pressed together, e.g. L+Up / L+Down for the throttle and R+B to release the
  brakes; the D-pad steers. Holding L first and then pressing Up counts as L alone, as on
  the console.
* Cyberia refused to start a game after the difficulty screen ("not enough backup memory...
  there must be 600 bytes available") and opened the storage manager, which listed the disc's
  own files as saves (12 MB used). "/NVRAM" itself was read as the disc's root (only
  "/NVRAM/<file>" went to the save folder). "/NVRAM" now is the save folder
  (`TDORT_OLD_NVRAM_ROOT=1`: as before); the arcade and puzzle levels can be chosen, the game
  makes its save file and the story begins.
* Cyberia then stopped at the first camera scene after the opening movie (black screen). Its
  loader thread asks the sound thread to stop the music and waits for the reply on a signal
  it never allocated; the reply goes to the camera player. On the console such a wait fails
  at once (ILLEGALSIGNAL) and loading goes on; here it waited forever. WaitSignal now
  returns that error for signals the task doesn't own, counting the low bits the console
  would have given the task's audio cues (`TDORT_OLD_WAIT_ANY=1`: wait anyway). The briefing
  and the camera scenes after it play.
* The finer picture is now recorded one cel row at a time (8 bytes per pixel instead of a
  60-byte command per pixel), so the game's own thread spends much less time on it: Slam
  'n Jam's game play took 6.6 ms a frame at 2x and 8.8 at 4x here, against 7.8 and 10.2
  before (4.9 at 1x). Ctrl+F shows a frame counter to check a game on your PC.
* Family Feud ran out of memory a few questions into a game ("FILE Cache is now empty"),
  then froze. DefineSampleHere takes the game's own alloc and free functions: the folio
  copies the sound into memory from the game's alloc, and UnloadSample hands it back through
  the game's free. Family Feud's file cache keeps a sound file locked until that free comes,
  and the runtime never called either, so nothing ever left the cache. The runtime now calls
  the game's functions as the folio does (`TDORT_OLD_DEFINE_SAMPLE=2`: copy into the OS's
  memory as before); 8 minutes of play through the Bullseye rounds and the main board ran with
  no memory errors. Save states from earlier packages can't be loaded (the format changed).
* Higher detail levels slowed games down and made the sound click on your PC: all the extra
  drawing ran on the game's thread. The finer picture is now drawn by worker threads (up to
  6, one fewer than your CPU's threads), and the 3x/4x picture is converted for the window
  on several cores. The picture is identical to drawing it on one thread
  (`TDORT_HI_THREADS=0` does that).
* Iron Angel of the Apocalypse said "Not enough memory" and showed an empty 3D view: its
  files are named "^/IronManData/...", and "^" (the root of the filesystem being searched)
  was read as "one folder up", so once it changed into a floor's folder nothing loaded.
  Now "^" goes to the disc's root (`TDORT_OLD_CARET=1`: as before); the logos, title and
  first floor (corridors, doors, map) work.
* Every game crashed at start-up on Windows in the package with the new detail setting
  (Crash 'n Burn, Killing Time, Road Rash...): until a game shows its first screen, the
  finer picture was empty and the window read from a null pointer. Fixed; the headless
  platform now reads every presented picture too, so tests catch this.
* Sailor Moon S's fights had no sound and stood still (timer stuck at 99). Its music
  thread "unloads" the fight's mixer when a track ends, but the mixer belongs to the main
  task; on the console that DeleteItem fails (only the owner may delete an item), here it
  worked and the mixer was gone. DeleteItem and the audio Unload/Delete calls now refuse
  items the calling task doesn't own (`TDORT_OLD_DELETE_ANY=1`: as before).
* Higher internal resolution (F12, `TDORT_SCALE`): every cel drawn into a screen-sized
  bitmap is also drawn into a 2x-4x copy of it, with the same pixel processor, so 3D games
  (Crash 'n Burn's tracks, for example) show smooth polygon edges and far textures in full.
  The game's own bitmaps are untouched: regression runs at 2x give the same results as at 1x.
* Theme Park's world map was covered in garbage (rows of orange and black stripes). The
  game loads its sprite data, then adds the data's address to each offset in the sprite
  table, but only to offsets smaller than that address. On the console the OS fills low
  memory, so the data lands high and every offset gets moved; here the 480 KB block was
  given out at 0x1230, so all but the first 32 sprites pointed into the wrong place.
  Allocations of 64 KB or more now come from above the first 512 KB of DRAM when there is
  room there, and from lower memory only when there isn't (`TDORT_OLD_ALLOC_LOW=1`: lowest
  fit as before).
* Lost Eden stopped at frame 10. It calls a function by way of two path strings stored in
  front of it ("ED/C/VIRGIN"), whose bytes decode as conditional SWIs with folio numbers
  that don't exist; on the console they don't run. The runtime answered them by zeroing
  r0, which was the function's argument. SWIs whose folio number is 16 or more are now
  ignored and leave the registers alone (`TDORT_OLD_BADSWI=1`: zero r0 as before).
* Olympic Summer Games (and Olympic Soccer, same engine) waited forever at start-up. They
  size a pool from AvailMem and then take whatever is left in 52-byte blocks until
  AllocMem fails, so the SDK's CreateThread couldn't get a 1 KB stack afterwards. The
  first small (< 256 bytes) request that would leave less than 32 KB of DRAM is now
  refused, once, which ends that loop; ordinary requests still get the memory
  (`TDORT_SMALL_ALLOC_FLOOR_KB=0`: no floor).
* Snow Job looped forever at start-up, then ran out of VRAM after the intro. Games walk a
  screen group's `sg_ScreenList`, which was missing; and `DeleteScreenGroup` did nothing,
  so each scene's frame buffers stayed allocated. Screen groups now carry their screen
  list, and deleting a group (DeleteScreenGroup or DeleteItem) deletes its screens and
  bitmaps and frees the frame buffers the OS allocated (`TDORT_OLD_SCREEN_DELETE=1`: keep).
* Star Control II crashed at start-up and then on a black screen; it now reaches gameplay.
  Two causes. First, it asks `CreateScreenGroup` for 4 bitmaps per screen but passes one
  buffer per screen. The OS makes one bitmap per screen whatever the count says (its
  screen code forces it to 1), so screen n takes entry n of the buffer array. The runtime
  honoured the count, gave the second screen garbage addresses, and clearing that "screen"
  through the SPORT device wiped the game's code (`TDORT_OLD_CSG_BITMAPS=1`: honour it).
  Second, its threads are created with tag 24 holding values that aren't stacks (code
  addresses, once its memory manager's handle table). Tag 24 is "free this stack when the
  thread dies", so the table was freed while in use, handed out again, and its memory
  manager fell apart. The tag is now taken as a stack only when it points into the
  thread's stack (`TDORT_OLD_STACK_FREE=1`: any value).
* Shadow - War of Succession froze on the difficulty menu when a button was pressed, and
  other games could get stray writes from the same cause. The math folio's `DivRemSF16`
  and `DivRemUF16` put the quotient in r0 and then took r0 as the address for the
  remainder, so the remainder went to the quotient's address. Shadow divides a timer value
  every frame, and the remainders landed across its own code; once its input queue pointer
  was hit, the joypad task looped forever. The remainder now goes where the caller asked
  (and is also returned in r1, as the OS routine does). Shadow now reaches the fights.
* Cowboy Casino stopped during the first poker hand. Its animation streamer reads a stream
  file 96 KB at a time and walks each buffer by the chunk sizes. The last read went past
  the end of the file, which read as zeros, so the streamer met a chunk of size 0 and
  never moved on. On a real disc that part of the buffer holds the next file's blocks,
  which are chunks as well. For a chunk file ("SHDR", "CCB ", ...) the part past the end
  now holds one `FILL` chunk, which streamers skip (`TDORT_OLD_EOF_ZERO=1`: zeros).
* VR Stalker crashed after the intro: its program's stack grew 32 KB below the program
  into free memory, `AllocMem` then gave that memory out and its zero fill wiped the
  stack. When a stack grows, the memory it reaches is now kept for that task
  (`TDORT_OLD_STACK_GROW=1`: not kept).
* Primal Rage stopped at frame 60: `SampleSystemTime` returned the time in the wrong
  registers (seconds in r0, microseconds in r1, as the OS does), so the game's timing
  loop never saw time pass (`TDORT_OLD_SAMPLETIME=1`: the old order).
* BattleSport could not be converted ("cannot write ...eye.anim\new"): the disc has two
  names under art/anims that differ only in case, which Windows can't store side by side.
  The console uses the first match, so the converter now keeps the first and lists the
  skipped ones in the conversion log.
* Doom: its game screens are 320x200. The display always showed 240 lines from the screen's
  buffer, so the bottom 40 lines were the start of the next buffer in memory (the top of
  the view again, under the status bar). A screen now shows as many lines as its bitmap
  has, at the top, with black below. `TDORT_FULL_DISPLAY=1` brings back the old behaviour.
* Samurai Shodown: the fighter-select program loads the stage background and makes the
  screens, starts the fight program, gives it those blocks with `ControlMem(..., MEMC_GIVE,
  task)` and exits. `ControlMem` did nothing, so the blocks were freed with the select
  program and the fight program's own data was loaded over them: no background, and fighters
  drawn dark. `MEMC_GIVE` now hands the memory to the other task (or, with task 0, back to
  the system). `TDORT_OLD_CONTROLMEM=1` brings back the old behaviour.
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
5. Picture: (done: 2x-4x internal resolution, F12) texture filtering for the finer picture, a Direct3D 11 presenter with
   scaling filters (integer, xBR, CRT, FSR 1), which also lets driver frame generation
   (NVIDIA Smooth Motion, AMD AFMF) work.
