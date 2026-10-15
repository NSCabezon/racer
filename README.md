# Star Wars Episode I: Racer - N64 Static Recompilation

```
    ____________________________
   /  ________________________  \
  /  /  ___________________  \  \
 /  /  / ___ _    _ ____ _ \ \  \
|  |  | / __| |  | |  __/ | |  |  |
|  |  | \__ \ |/\| | |__  | |  |  |     "Now THIS is podracing!"
|  |  | |___/\    /|____|_| |  |  |          - Anakin Skywalker
|  |  |  ___ _  _ ___ __   |  |  |
|  |  | | __| \/ | _ /  \  |  |  |
|  |  | | _||  . |  /  __|_|  |  |
|  |  | |___|_/\_|_| \___| |  |  |
 \  \  \___________________/  /  /
  \  \________________________/  /
   \____________________________/
```

## What is this?

This is a **static recompilation** project for *Star Wars Episode I: Racer* on the Nintendo 64, using [N64Recomp](https://github.com/N64Recomp/N64Recomp). The goal: take the original N64 binary and recompile it into native PC code, so you can podrace at modern resolutions and framerates without emulation.

No existing N64 decompilation exists for this game. We're flying blind through Beggar's Canyon here, folks.

## Status

| Milestone | Status |
|-----------|--------|
| ROM Analysis | COMPLETE |
| Function Discovery (878+ functions) | COMPLETE |
| Symbol File Generation | COMPLETE |
| N64Recomp Config | COMPLETE |
| Debug Tooling (analyzer, differ, strings, progress) | COMPLETE |
| Build System (Make + CMake) | COMPLETE |
| libultra Stubbing (~120 functions) | COMPLETE |
| N64Recomp Integration | COMPLETE |
| **Native Compilation (MSVC)** | **COMPLETE** |
| **N64ModernRuntime Integration** | **COMPLETE** |
| **libultra Function Identification (25 reimplemented)** | **COMPLETE** |
| **Game Boot + Stable Execution** | **COMPLETE** |
| **RSP Task Routing (osSpTaskLoad/StartGo)** | **COMPLETE** |
| **osRecvMesg Thread Blocking Fix** | **COMPLETE** |
| **Event System Wiring (VI, SI, Timer)** | **COMPLETE** |
| **Thread Scheduling Fixes** | **COMPLETE** |
| **Game Thread Boot + Controller Init** | **COMPLETE** |
| **Split-Function Fallthrough Auto-Fix (143 funcs)** | **COMPLETE** |
| **Full Boot → Stable Main Loop (no crashes)** | **COMPLETE** |
| **PI DMA / Asset Loading** | **COMPLETE** |
| **RSP Task Submission (audio + graphics)** | **COMPLETE** |
| **Display Lists Reach RT64 (processed)** | **COMPLETE** |
| **macOS / Apple Silicon Build (clang + Metal)** | **COMPLETE** |
| **Thread Scheduling Without Runtime Patches** | **COMPLETE** |
| **KSEG1 / MMIO Address Windows** | **COMPLETE** |
| **RT64 Renderer Integration** | **IN PROGRESS** |
| Visible Frame Output | IN PROGRESS |
| Audio Reimplementation | TODO |
| Playable Build | THE DREAM |

**Current Progress: Phase 8 - Native on Apple Silicon, Same Spine, One Crash Further**

*"Great, kid. Don't get cocky."*

The whole pipeline now builds and runs **natively on macOS / Apple Silicon** with clang, SDL2 from Homebrew and RT64's **Metal** backend, against upstream N64Recomp / N64ModernRuntime / RT64 (no runtime patches). The game boots to the same point it reached on Windows — threads, events, SI, RSP audio tasks, **display lists reaching RT64** — and then one step further: the first asset-loading crash on Windows was a read through a KSEG1 (uncached) RDRAM pointer, which we now alias; the next one is a NULL buffer inside the ROM asset reader (`func_80093180` via `dma_read_rom`), which is where the hunt continues.

Things that had to be rediscovered because they lived as uncommitted patches on the old Windows box (see `src/main.cpp` and `tools/post_recomp_patches.py` for the self-contained versions):

- **`osSetEventMesg(OS_EVENT_VI)` → `osViSetEvent`**: ultramodern's `osSetEventMesg` only records SP/DP/AI/SI, and its VI thread only delivers to the queue set via `osViSetEvent`. Call sites are rewritten to `swe1r_osSetEventMesg_recomp`, which forwards VI and passes the rest through.
- **VI warm-up before `start_game`**: ultramodern only swaps its double-buffered VI state once a VI mode exists, and before the game starts only the dummy-VI path sets one. Starting the game immediately from `gfx_init_callback` meant the VI manager's queue never became current. We now wait four retraces on a helper thread.
- **`osSetThreadPri` identified (0x8008BFB0)**: the init thread calls `osSetThreadPri(NULL, 0)` right before its idle loop. Unidentified, it stayed at priority 10, equal to the game thread, and ultramodern's scheduler only yields to strictly higher priority — the game thread never ran. This is what the old "pause_self yields to equal-priority threads" runtime patch was papering over.
- **MMIO window**: the boot trampoline pokes `VI_H_START` through `0xA4400024`. librecomp protects everything above 512 MB, so `on_game_init` unprotects `0xA3F00000-0xA4FFFFFF` as zero-filled scratch.
- **KSEG1 RDRAM mirror**: DMA buffers are addressed through `0xA0000000 | phys`. On macOS the 8 MB of RDRAM are re-mapped at the KSEG1 offset with `mach_vm_remap` (shared pages). Windows/Linux equivalents are TODO.
- **Implicit declarations**: `__osSiRawStartDma` is in N64Recomp's `ignored_funcs`, so callers reference `__osSiRawStartDma_recomp` with no prototype; MSVC tolerated it, clang does not. `post_recomp_patches.py` appends the missing prototypes to `funcs.h`.
- **Runtime API drift** (upstream vs. the May 2026 code): `recomp::start_game` takes a game-mode id, `GameEntry::display_name` is mandatory, `RendererContext::send_dummy_workload` is a new pure virtual, and the Apple `WindowHandle` is `{NSWindow*, CAMetalLayer*}`.

---

**Previous milestone: Phase 7 - Display Lists Are Reaching the Renderer**

*"Now THIS is podracing!"*

The game now boots, **loads its assets from ROM via DMA**, spins up the RSP task pipeline, and **submits graphics display lists that RT64 accepts and processes**. Getting here meant bypassing the stubbed PI-manager thread (routing the game's `osPiStartDma` to the runtime's synchronous DMA), no-op'ing audio RSP tasks so the game proceeds to graphics, splitting a function that was being called through a pointer, un-stubbing a wrongly-stubbed string routine, and — the final piece — calling RT64's `loadUCodeGBI` before `processDisplayLists` so the renderer knows the microcode. The game thread now runs deep into per-frame logic. Still chasing the next game-logic crash and confirming pixels actually hit the screen, but the whole boot-to-render spine is connected.

---

**Previous milestone: Phase 6 - The Game Boots and Runs (no crashes!)**

*"It's working! IT'S WORKING!" - Anakin, after the game finally booted clean*

The game now **boots all the way to a stable main loop and runs indefinitely without crashing** - all four OS threads, the full event system (SP/DP/VI/SI/PI/PRENMI), and controller init come up, and VI retraces tick steadily at 60Hz. The breakthrough was an automated fix for **143 split-function fallthroughs**: where N64Recomp splits one MIPS function in two, the first half ran off its end without calling the second half, leaving globals uninitialized and crashing the game later. `tools/fix_fallthroughs.py` chains every such function to its continuation in one pass (re-run it after each recompile). Next up: getting display lists to RT64 so there's a picture to go with the podracing.

The **game thread now boots and initializes**. All three major threads are running: the scheduler (priority 254), the init thread (priority 10), and the game thread (priority 10). The game's custom event system has been fully decoded and wired to ultramodern's event delivery. Controller initialization passes the SI DMA phase but crashes reading uninitialized PIF data — next up is implementing proper controller response data.

- **1,378 functions** in the lookup table (878 symbols + ~500 auto-detected statics)
- **25 libultra functions** identified and reimplemented via ultramodern:
  - **Phase 5 NEW**: `osSetEventMesg` (0x8008AE30), `osViSetEvent` (0x8008C090), `osSetTimer` (0x800906F0), `__osSiRawStartDma` (0x800907D0)
  - Phase 4: `osSpTaskLoad`, `osSpTaskStartGo`, `osRecvMesg` (game's real copy at 0x80087E80)
  - Plus: osCreateThread, osCreateMesgQueue, osSendMesg, osViSetMode, osPiStartDma, and more
- **Event system fully decoded**: game uses custom `osSetEventMesg` with standard event IDs (VI=7, SP=4, DP=9, SI=5, PI=8, PRENMI=14). Forwarded to ultramodern's event delivery
- **VI event bootstrapping**: `osSetEventMesg(OS_EVENT_VI)` forwards to `osViSetEvent` for ultramodern's VI thread to deliver retraces
- **VI state initialization**: scheduler reads from uninitialized struct at 0x800A7F50 (osCreateViManager is stubbed). Manually initialized with sink queue to prevent feedback loop
- **Thread scheduling fixes** (N64ModernRuntime patches):
  - `pause_self` now yields to equal-priority threads (was only yielding to higher)
  - `run_next_thread_and_wait` idles on external messages when running queue is empty (was crashing)
- **SI DMA stub**: `__osSiRawStartDma` sends immediate SI completion event (needs actual PIF response data)
- **SWE1Racer.exe** links against N64ModernRuntime (librecomp + ultramodern) + RT64
- RT64 renderer initialized (D3D12 backend, RTX 5070 detected)
- SDL2 window (1280x960), keyboard input, and event loop running
- Built with MSVC (Visual Studio 2022) and CMake

## ROM Info

| Field | Value |
|-------|-------|
| Title | STAR WARS EP1 RACER |
| Game Code | NEPE |
| Region | USA (NTSC) |
| Entry Point | 0x80000400 |
| CRC1 | 0x72F70398 |
| CRC2 | 0x6556A98B |
| ROM Size | 32 MB |
| Code Size | ~608 KB (0x1000 - 0x99000) |
| Functions | 878 symbols + ~540 auto-detected |

## Getting Started

### Prerequisites

- Python 3.x
- GNU Make (or compatible)
- CMake ≥ 3.20 and Ninja
- [N64Recomp](https://github.com/N64Recomp/N64Recomp), [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) and [RT64](https://github.com/rt64/rt64), cloned **as siblings of this repo** (`../N64Recomp`, `../N64ModernRuntime`, `../rt64`, all with `--recurse-submodules`)
- Windows: Visual Studio 2022 + vcpkg with SDL2. macOS: Xcode command line tools + `brew install cmake ninja sdl2`
- Your own legally obtained ROM: `Star Wars Episode I - Racer (U) [!].z64`

### Setup

```bash
# Clone the repo
git clone https://github.com/sp00nznet/racer.git
cd racer

# Place your ROM in the project root
cp /path/to/your/rom.z64 baserom.z64

# Run initial analysis
make

# Check progress
make progress
```

### Debug Tools

We've got a full pit crew of analysis tools, inspired by the DKR decompilation project:

```bash
# Analyze the ROM structure and regenerate symbols
make analyze

# Disassemble any function
make diff FUNC=func_80000470

# Dump all strings from the ROM
make strings

# Just the debug strings (source files, asserts, etc.)
make strings-debug

# Game text (menus, dialogue, taunts from Sebulba)
make strings-game

# File paths referenced in the ROM
make strings-files

# View recompilation progress
make progress
```

### Building

```bash
# 1. Build N64Recomp (the recompiler tool)
cd /path/to/N64Recomp
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release --target N64RecompCLI

# 2. Recompile the game (MIPS -> C)
cd /path/to/racer
make recomp N64RECOMP=/path/to/N64Recomp/build/Release/N64Recomp.exe

# 3. Post-process the generated code and build the lookup table
#    (re-run all three after every recompile)
python tools/fix_fallthroughs.py
python tools/post_recomp_patches.py
python tools/gen_lookup_table.py

# 4. Build the native executable
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release

# 5. Run it! (librecomp looks for the ROM as <game_id>.z64 in the working dir)
copy baserom.z64 swe1racer.z64
build/Release/SWE1Racer.exe
```

#### macOS (Apple Silicon)

```bash
# 1. Toolchain, as siblings of this repo
cd /path/to
git clone --recurse-submodules https://github.com/N64Recomp/N64Recomp.git
git clone --recurse-submodules https://github.com/N64Recomp/N64ModernRuntime.git
git clone --recurse-submodules https://github.com/rt64/rt64.git
brew install cmake ninja sdl2
cmake -S N64Recomp -B N64Recomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build N64Recomp/build --target N64Recomp

# 2. Recompile + post-process
cd racer
cp /path/to/rom.z64 baserom.z64
../N64Recomp/build/N64Recomp recomp.toml
python3 tools/fix_fallthroughs.py
python3 tools/post_recomp_patches.py
python3 tools/gen_lookup_table.py

# 3. Build (RT64 compiles its Metal shaders with xcrun; first build takes a few minutes)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target SWE1Racer

# 4. Run
cp baserom.z64 swe1racer.z64
./build/SWE1Racer
```

Crashes print the faulting N64 address and a symbolized native backtrace (recompiled `func_XXXXXXXX` names included). `sample SWE1Racer 2` is handy for seeing where the game threads are blocked.

## Project Structure

```
racer/
├── README.md              # You are here
├── Makefile               # Analysis & debug build system
├── CMakeLists.txt         # CMake build for native executable
├── recomp.toml            # N64Recomp configuration
├── symbols.toml           # Function symbols (878 functions, 17 libultra identified)
├── baserom.z64            # Your ROM goes here (not tracked)
├── tools/
│   ├── rom_analyzer.py    # ROM analysis & symbol generation
│   ├── func_differ.py     # Function disassembler
│   ├── string_dumper.py   # String extraction & categorization
│   ├── progress.py        # Progress tracker
│   ├── identify_libultra.py # libultra function pattern matcher (MIPS instruction matching)
│   ├── fix_statics.py     # Auto-fix static_0_ sub-function errors
│   ├── fix_fallthroughs.py # Chain split functions to their continuation
│   ├── post_recomp_patches.py # Call rewrites, redirects, missing prototypes
│   └── gen_lookup_table.py # Generate function lookup table
├── src/
│   ├── main.cpp           # Entry point, SDL window, callbacks, host-side init hacks, crash handlers
│   ├── section_table.cpp  # VRAM->function pointer lookup table
│   ├── rt64_render_context.cpp  # RT64 renderer bridge (D3D12 on Windows, Metal on macOS)
│   ├── support_apple.mm   # macOS: keep SDL from resizing RT64's CAMetalLayer
│   └── audio.cpp          # Audio stubs (SDL playback TODO)
├── RecompiledFuncs/       # N64Recomp output (generated)
│   ├── funcs.h            # Function declarations
│   └── funcs_*.c          # Recompiled C code (29 files)
├── include/               # Project headers
└── build/                 # CMake build output
```

## Fun Stuff Found in the ROM

The developers at LucasArts left some gems in the binary:

- **Developer control schemes**: "---Jonk---", "---Jake---", "---Steve---", "---Alex---", "---Brett Sett---" - each dev had their own controller config!
- **Debug menu**: `Debug Level`, `Edit Vehicle Stats`, `AI Level x10` - there's a full debug menu hiding in there
- **Sebulba trash talk**: *"I could run faster than your podracer"*, *"Watto sell you that podracer?"*
- **Cheat codes**: `All Pods, tracks unlocked!!!`
- **Engine fire**: `ENGINE FIRE` - every podracer's nightmare
- **Low memory warning**: `Low Memory: %d Racers` - when the N64 couldn't handle all those pods

## How N64 Static Recompilation Works

```
  N64 ROM (MIPS)
       |
       v
  [N64Recomp] -----> Reads symbols.toml for function boundaries
       |
       v
  Recompiled C code (one file per ~50 functions)
       |
       v
  [C Compiler] + [N64ModernRuntime] (reimplements libultra)
       |
       v
  Native PC executable!
```

Unlike emulation, static recompilation converts every MIPS instruction into equivalent C code at build time. The result runs natively on your CPU - no instruction-by-instruction interpretation needed. This means better performance, easier modding, and native resolution/widescreen support.

## References & Credits

- [N64Recomp](https://github.com/N64Recomp/N64Recomp) - The static recompiler that makes this possible
- [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) - The project that proved N64 recomp works
- [Diddy Kong Racing Decomp](https://github.com/DavidSM64/Diddy-Kong-Racing) - Debug tooling inspiration
- [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) - Runtime library for recompiled games
- [OpenSWE1R](https://github.com/OpenSWE1R) - PC version reverse engineering (different codebase, but useful reference)
- [SW_RACER_RE](https://github.com/tim-tim707/SW_RACER_RE) - PC version decompilation
- [HarvestMoon64Recomp](https://github.com/HarvestMoon64Recomp/HarvestMoon64Recomp) (GPL-3.0) - reference for the `RECOMP_PATCH` mod framework, libultra-level controller polling, game-spin-loop yielding, and the `n_aspMain` RSP audio recompile workflow. Techniques referenced; no code copied (their project is decomp-backed and GPL-3.0).

## Legal

This project does not include any copyrighted material. You must provide your own legally obtained ROM. Star Wars Episode I: Racer is a trademark of Lucasfilm Ltd.

---

*"I have a bad feeling about this." - Everyone in Star Wars, at some point*

*"It's working! IT'S WORKING!" - What we said when the game thread finally booted (Phase 5)*

*"Your focus determines your reality." - Qui-Gon Jinn (also good advice for debugging recompiled MIPS)*
