# Handoff: SWE1Racer on Apple Silicon

Last updated: 2026-10-16. Branch `main`, not pushed.

## Where we are

The N64 recomp of *Star Wars Episode I: Racer* builds and runs natively on macOS (M1 Max, Metal via RT64). The game runs the intro, the title/hangar menu and a race loop for minutes without crashing. ESC quits cleanly.

Visually confirmed: intro and hangar menu render correctly (models, textures, lighting).

**Aspect fixed (step 1 done):** intro, cutscenes and title fill the window at 4:3, upscaled. Two causes, both in how RT64 handles the game's 640-wide serrate VI mode (`status=0x13052`): `removeBlackBorders` sized the virtual screen from a 320x476 framebuffer (narrow column), and `VI::fbAddress()` subtracted a 640-pixel row from the origin so RT64 never found the game's 320-wide framebuffer and fell back to a low-res RDRAM copy. Details in `docs/STEP1_ASPECT.md`. Races not checked visually yet.

**Race reached (2026-10-16, evening):** the menus work end to end. Start, Start, then A repeatedly creates pilot "AAA" (Anakin) and starts Amateur race 1, The Boonta Training Course, with HUD and timer. Holding throttle for 4 minutes ran without crashes. The blocker was the game's 64-bit timer `func_800811DC`: two game threads interleaving inside it added a spurious osGetCount wrap, so time jumped 91.6 s (or 183 s). The animation update (`func_80006848` <-> `func_80006790`) then recursed once per loop period of that delta and the main thread's stack ran down through .data/.bss: random SIGBUS in `func_80006C00`/`func_80013DC0`, hangs. Fixed by redirecting it to `swe1r_get_time64_recomp` (same algorithm under a mutex). A full 3-lap race still needs someone to steer.

Unattended route to the race (about 75 s):

```bash
SWE1R_AUTOINPUT="31:start,37:start,43:a,48:a,53:a,58:a,63:a,68:a,74-300:a" ./build/SWE1Racer
```

## Setup on this Mac

| What | Where |
|---|---|
| ROM | `~/Downloads/sw racer.z64`, copied to `baserom.z64` and `swe1racer.z64` (both gitignored) |
| N64Recomp (built) | `../N64Recomp/build/N64Recomp` |
| N64ModernRuntime | `../N64ModernRuntime` (upstream main, unmodified) |
| RT64 | `../rt64` (upstream main `de343a5`) plus `patches/rt64-*.patch`, applied automatically by CMake at configure time |
| Save file | `saves/swe1racer.bin` (16 kbit EEPROM, 2 KB) |

## Build and run

```bash
# after any change to symbols.toml / recomp.toml / tools:
../N64Recomp/build/N64Recomp recomp.toml
python3 tools/fix_fallthroughs.py
python3 tools/post_recomp_patches.py
python3 tools/gen_lookup_table.py

cmake --build build --target SWE1Racer -j10
./build/SWE1Racer
```

Keys: Space = Start, Enter = A, Shift = B, Z, arrows = D-pad, WASD = stick, ESC = quit.

Developer env vars:

| Variable | Effect |
|---|---|
| `SWE1R_AUTOINPUT="20:start,30:a"` | Scripted button presses (seconds:button) |
| `SWE1R_QUIT_AFTER=N` | Quit after N seconds through the normal ESC path |
| `SWE1R_RES=original\|original2x\|auto` | RT64 resolution mode |
| `SWE1R_MSAA=0\|2\|4\|8` | Antialiasing |
| `SWE1R_ASPECT=original\|expand` | Aspect ratio |
| `SWE1R_NO_SPECIAL=1` | Ignore `osViSetSpecialFeatures` |
| `SWE1R_AUTOINPUT` ranges/stick | `"74-300:a"` holds A; `sl sr su sd` push the stick |
| `SWE1R_CRASH_DUMP=path` | Write the 8 MB RDRAM to `path` when the game crashes |
| `SWE1R_HWWATCH="0xADDR:SECS"` | Arm an ARM64 hardware write watchpoint on an N64 word at SECS; the writer's backtrace is printed. `SWE1R_HWWATCH2` adds up to 2 more words |
| `SWE1R_LOG_DT=1` | Log frame delta times over 0.1 s (double at 0x80120BF0) |
| `SWE1R_OVERSCAN=0.03` | Hide that fraction per side like a CRT (default 0, whole framebuffer) |
| `SWE1R_DEV=1` | RT64 developer mode: F1 inspector, F4 texture replacement panel |
| `SWE1R_DUMP_TEXTURES=tex_dump` | Dump every texture the game uploads as `<hash>.v5.png` from boot |
| `SWE1R_TEXTURE_PACKS=dir` | Where texture packs load from (default `texture_packs/`): every `*.rtz` or folder with `rt64.json` |
| `SWE1R_RBB=1` | Turn RT64's `removeBlackBorders` back on (default off; on breaks the serrate mode) |

## Debugging recipe

- Run unattended with a timeout: `perl -e 'alarm 60; exec @ARGV' ./build/SWE1Racer > run.log 2>&1`.
- Where are threads blocked: `sample $(pgrep -nx SWE1Racer) 2 -file out.txt`.
- lldb: use Xcode's, `/Applications/Xcode.app/Contents/Developer/usr/bin/lldb` (the one on PATH crashes). Registers of a recompiled function: `*(uint64_t*)($x1+8*N)` is MIPS register N.
- Silent deaths: read `~/Library/Logs/DiagnosticReports/SWE1Racer-*.ips`.
- Crashes print the faulting N64 address and a symbolized backtrace.
- A display list running for more than 2 s is dumped to `dl_dump.bin` (words are in native order, decode with `<II`).
- ROM helpers: `tools/n64util.py dis|xref|fn`, `tools/find_missing_funcs.py`.
- `tools/capture_window.sh PREFIX "T1 T2" [VAR=val]` captures the game window at given seconds (needs an unlocked screen).
- Screenshot of the game window even when covered: get its window id with a small Swift `CGWindowListCopyWindowInfo` helper, then `screencapture -x -l <id> out.png`. A full-screen capture shows whatever is on top, and nothing useful if the Mac is locked.

## Key lessons (do not re-learn these)

- Memory corruption hunting: dump RDRAM on crash, diff the data segment against the ROM (rom = vram - 0x80000000 + 0xC00), then trap the writer with `SWE1R_HWWATCH` on a read-only word the corruption hits. lldb and `mprotect` changed timing enough to hide the bug. Recompiled code does not maintain `$ra`, so return addresses on the MIPS stack are useless.
- Game threads can run concurrently here. Any game routine that assumes it can't be interrupted (read-modify-write of shared globals) is a suspect.

- SP completion must reach the game **after** RT64 parses the display list. The game rewrites its double-buffered list as soon as the RSP reports done.
- EEPROM is 16 kbit. `Eep4k` silently overflows librecomp's save buffer.
- The game's SP scheduler needs `osSpTaskYielded` to return 1 after a yield request, or it deadlocks.
- `osViBlack` is one-shot on hardware; ultramodern keeps it until cleared.
- The game polls the pad through raw PIF blocks (`__osSiRawStartDma`), not through `osContGetReadData`.
- Function boundaries in the libultra region (0x8008xxxx+) were often wrong or stubbed. When a crash smells like empty output, check `recomp.toml` stubs first.

All of these are implemented host-side in `src/main.cpp`, `src/rt64_render_context.cpp` and `tools/post_recomp_patches.py`. The README "Phase 8" section has the full list with addresses.

## Texture packs (HD textures project)

Phase 1 done: dump with `SWE1R_DUMP_TEXTURES`, edit or upscale the PNGs keeping their names, then `python3 tools/make_texture_pack.py SRC texture_packs/name.rtz`. Verified with a red/blue-swapped 4x pack on the title screen. Next: phase 2 pilot on The Boonta Training Course (dump while playing it, AI upscale with wrap-aware padding and alpha handling, compare), then all tracks. louriccia/SWE1R-Mods has no usable HD textures (PC `.bin` packs).

## Next steps, in order

1. **Gamepad via SDL**: game controller API, analog stick mapping, rumble.
2. **Audio**: recompile the game's audio microcode (`ucode=0x80099450`, `data=0x800AE5F0`) with RSPRecomp, feed SDL audio. Revisit the yield emulation then, since it re-runs resumed audio tasks.
3. **Play-test races** on several tracks to catch remaining game-logic crashes.
4. **RT64 enhancements**: widescreen (note: RT64's source aspect in the serrate mode is still 320/476, so `Expand` will need a look), high resolution, interpolated 60 fps.
5. **macOS `.app` bundle**: icon, ROM import on first launch, saves in Application Support.
6. **Vision Pro stereo** (see the `avp-game-port` skill).

## Known loose ends

- Display-list hang (RT64 executing zeros as no-ops forever) is mitigated: `send_dl` pre-walks the F3DEX2 tree and drops a frame whose list runs into zeros, logging `[dl] list at ... runs into zeros`. Seen once: a sub-list call from model data at 0x801BFE58 landing at ~0x806D0000 (just below a framebuffer), so probably a stale segment register on a scene change. Root cause still open. Graphics tasks also wait until RT64 has parsed them (`swe1r_osSpTaskStartGo_recomp`), keeping hardware ordering.

- Windows build: Win32 paths kept under `_WIN32` but not compiled since the port. The KSEG1 RDRAM mirror needs a `MapViewOfFile` equivalent there.
- `on_game_init` still writes hacks (PI handle at 0x800A7BC0/0x800A7FC0, fake VI context at 0x800A7F00) that predate the real VI functions. 0x800A7BC0 is actually `__osRunningThread`. Worth revisiting.
- Lots of diagnostic logging (`[si]`, `[pi]`, `[vi]`, `[perf]`). Trim once things are stable.
