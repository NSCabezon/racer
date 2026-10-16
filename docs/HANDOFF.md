# Handoff: SWE1Racer on Apple Silicon

Last updated: 2026-10-16. Branch `main`, last commit `da6de0d`, not pushed.

## Where we are

The N64 recomp of *Star Wars Episode I: Racer* builds and runs natively on macOS (M1 Max, Metal via RT64). The game runs the intro, the title/hangar menu and a race loop for minutes without crashing. ESC quits cleanly.

Visually confirmed: intro and hangar menu render correctly (models, textures, lighting).

**Open visual bug (next task):** the title/main menu (VI 320 mode) fills the window at 4:3, but the intro/cutscenes (VI 640 serrate mode, `status=0x13052`) are presented in a centered, nearly square box (~0.48 x 0.60 of the window). Aspect config, `removeBlackBorders` and V-span tweaks did not fix it; the framebuffer has stride 320 with a ~304x288 picture, and RT64 presents it inside a virtual 640x476 area. Full findings, measurements and next ideas: `docs/STEP1_ASPECT.md`. Races not checked yet.

## Setup on this Mac

| What | Where |
|---|---|
| ROM | `~/Downloads/sw racer.z64`, copied to `baserom.z64` and `swe1racer.z64` (both gitignored) |
| N64Recomp (built) | `../N64Recomp/build/N64Recomp` |
| N64ModernRuntime | `../N64ModernRuntime` (upstream main, unmodified) |
| RT64 | `../rt64` (upstream main, unmodified) |
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

- SP completion must reach the game **after** RT64 parses the display list. The game rewrites its double-buffered list as soon as the RSP reports done.
- EEPROM is 16 kbit. `Eep4k` silently overflows librecomp's save buffer.
- The game's SP scheduler needs `osSpTaskYielded` to return 1 after a yield request, or it deadlocks.
- `osViBlack` is one-shot on hardware; ultramodern keeps it until cleared.
- The game polls the pad through raw PIF blocks (`__osSiRawStartDma`), not through `osContGetReadData`.
- Function boundaries in the libultra region (0x8008xxxx+) were often wrong or stubbed. When a crash smells like empty output, check `recomp.toml` stubs first.

All of these are implemented host-side in `src/main.cpp`, `src/rt64_render_context.cpp` and `tools/post_recomp_patches.py`. The README "Phase 8" section has the full list with addresses.

## Next steps, in order

1. **Fill the window at 4:3 in the 640 serrate mode** (title already OK; see `docs/STEP1_ASPECT.md`). Find how RT64 derives the presented rect from the VI registers. The game uses `h_start 0x006C02EC`, `x_scale 0x200`, `y_scale 0x400`, `VI_WIDTH` 320 (intro) and 640 with serrate (menu/race). Starting points: `../rt64/src/hle/rt64_vi.cpp`, `../rt64/src/render/rt64_vi_renderer.cpp`, ultramodern `update_vi()` in `../N64ModernRuntime/ultramodern/src/events.cpp`. Compare with how Zelda64Recomp configures RT64.
2. **Gamepad via SDL**: game controller API, analog stick mapping, rumble.
3. **Audio**: recompile the game's audio microcode (`ucode=0x80099450`, `data=0x800AE5F0`) with RSPRecomp, feed SDL audio. Revisit the yield emulation then, since it re-runs resumed audio tasks.
4. **Play-test races** on several tracks to catch remaining game-logic crashes.
5. **RT64 enhancements**: widescreen, high resolution, interpolated 60 fps.
6. **macOS `.app` bundle**: icon, ROM import on first launch, saves in Application Support.
7. **Vision Pro stereo** (see the `avp-game-port` skill).

## Known loose ends

- Windows build: Win32 paths kept under `_WIN32` but not compiled since the port. The KSEG1 RDRAM mirror needs a `MapViewOfFile` equivalent there.
- `on_game_init` still writes hacks (PI handle at 0x800A7BC0/0x800A7FC0, fake VI context at 0x800A7F00) that predate the real VI functions. 0x800A7BC0 is actually `__osRunningThread`. Worth revisiting.
- Lots of diagnostic logging (`[si]`, `[pi]`, `[vi]`, `[perf]`). Trim once things are stable.
