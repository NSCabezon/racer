# Handoff: SWE1Racer on Apple Silicon

Last updated: 2026-10-17. Branch `main`, not pushed.

## Where we are

The N64 recomp of *Star Wars Episode I: Racer* builds and runs natively on macOS (M1 Max, Metal via RT64) and is **playable**: intro, title, menus (pilot creation, vehicle, tournament, track) and races with HUD. A 17-minute play session by keyboard had no crashes or hangs. ESC quits cleanly. Sound works (music, menu effects, engines; see Audio below). **Not verified:** the finish line and results screen (nobody has completed 3 laps yet).

What was fixed on 2026-10-16 (details in commit messages and `docs/STEP1_ASPECT.md`):

| Problem | Cause | Fix |
|---|---|---|
| Intro/menus in a narrow box, then pixelated | RT64 `removeBlackBorders` sized the screen from a 320x476 estimate in the 640-wide serrate VI mode; `VI::fbAddress()` stepped back a 640-pixel row, so the real 320-wide framebuffer was never found | `removeBlackBorders` off; RT64 patch `rt64-serrate-fbaddress.patch` |
| Random SIGBUS / hangs on the player-select menu | Game's 64-bit timer `func_800811DC` raced between game threads, added a fake osGetCount wrap (+91.6 s); animation update `func_80006848` <-> `func_80006790` recursed once per loop period and the main stack overwrote .data/.bss | Redirected to `swe1r_get_time64_recomp` (same algorithm under a mutex) |
| Magenta strip on screen edges (title, under the pods), flickering | New RT64 render targets hold uninitialized Metal memory; the game never draws the overscan edges | RT64 patch `rt64-workload-clear-new-targets.patch` clears them |
| Freeze on scene changes ("display list running for >2 s") | A sub-list pointing into zeroed memory; RT64 ran zeros as G_NOOP forever | `send_dl` pre-walks the list tree and drops such a frame (`[dl]` log); gfx tasks wait until RT64 parsed them |

Unattended route to a race (about 75 s), holding throttle:

```bash
SWE1R_AUTOINPUT="31:start,37:start,43:a,48:a,53:a,58:a,63:a,68:a,74-300:a" ./build/SWE1Racer
```

The save file already holds pilot "AAA" (Anakin) from testing.

**Heads-up:** this Mac is often loaded by other sessions' visionOS simulators and VMs (load average in the hundreds). That makes the game stutter and shakes out timing bugs. Never launch a test instance while someone is playing: it competes for CPU/GPU, and `tools/capture_window.sh` grabs the first game window it finds.

## Setup on this Mac

| What | Where |
|---|---|
| ROM | `~/Downloads/sw racer.z64`, copied to `baserom.z64` and `swe1racer.z64` (both gitignored) |
| N64Recomp (built) | `../N64Recomp/build/N64Recomp` |
| N64ModernRuntime | `../N64ModernRuntime` (upstream main, unmodified) |
| RT64 | `../rt64` (upstream main `de343a5`) plus `patches/rt64-*.patch` (serrate fbAddress, overscan crop option, clear new targets), applied by CMake at configure time; already-applied patches are skipped |
| Real-ESRGAN (texture upscaler) | `../tools_ext/realesrgan/realesrgan-ncnn-vulkan` (v0.2.5.0 macOS zip, universal binary, runs on the M1 GPU) |
| Save file | `saves/swe1racer.bin` (16 kbit EEPROM, 2 KB) |

## Build and run

```bash
# after any change to symbols.toml / recomp.toml / tools:
../N64Recomp/build/N64Recomp recomp.toml
python3 tools/fix_fallthroughs.py
python3 tools/post_recomp_patches.py
python3 tools/gen_lookup_table.py
# audio microcode (needs baserom.z64; output rsp/aspMain.cpp is not versioned):
../N64Recomp/build/RSPRecomp rsp/aspMain.toml

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
| `SWE1R_MUTE=1` | No audio device; a wall-clock drain stands in so the game's audio pacing is unchanged |
| `SWE1R_AUDIO_LOG=1` | Once per second: frequency, frames received/s, queue depth (min/max), peak sample, AI FIFO-full answers, underruns, aspMain task count |
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
- Function boundaries in the libultra region (0x8008xxxx+) were often wrong or stubbed. When a crash smells like empty output, check `recomp.toml` stubs first. Names there are unreliable too: check a "libultra" name against its disassembly (`tools/n64util.py dis`) before trusting it (hardware functions use cop0 or 0xA4xx registers; libaudio uses neither).

All of these are implemented host-side in `src/main.cpp`, `src/rt64_render_context.cpp` and `tools/post_recomp_patches.py`. The README "Phase 8" section has the full list with addresses.

## Audio

Done 2026-10-17. Pieces:

- **Microcode**: the game's audio ucode is the standard libultra `aspMain` (ROM 0x9A050, 0xE20 bytes at IMEM 0x1080, data at 0x800AE5F0). `rsp/aspMain.toml` drives RSPRecomp; it lists the 16 command-table handlers (DMEM 0x10) as `extra_indirect_branch_targets`, otherwise the first `jr` dispatch fails. `get_rsp_microcode` (src/main.cpp) runs it for `M_AUDTASK` and then delivers the SP message.
- **libaudio was stubbed**: everything from 0x8008C000 had been stubbed as "OS", including libaudio, and `alSynNew` (0x800887D0) was misnamed `cache_writeback`. Result: empty command lists, silence. recomp.toml now leaves libaudio compiled (list in its comment). Three libaudio functions were misnamed `osSetIntMask` / `osAiSetFrequency` / `osAiSetNextBuffer` (0x80090170, 0x800903D0, 0x80090450), so voice calls went to the runtime's AI and interrupt functions; the real osSetIntMask is 0x80090500. A filter param handler (0x8008E9F0) had been split into three pieces, one stubbed.
- **Real AI functions** (0x80088150 osAiSetFrequency, 0x800882B0 osAiSetNextBuffer, 0x800883E0 osAiGetLength, 0x8008ADA0 osAiGetStatus) poke registers directly; `tools/post_recomp_patches.py` routes them to the runtime / host.
- **Pacing**: the game's audio scheduler (0x8002E134) makes a ~1088-frame buffer at 22050 Hz when its clock passes a deadline **and** `osAiGetStatus` says the FIFO isn't full. `swe1r_osAiGetStatus_recomp` (src/audio.cpp) reports full while more than 1.5 buffers are queued. Without it production ran ~1.5% fast and the queue grew without bound.
- **Output**: SDL queued audio, S16 stereo at the game's rate, channel pairs swapped (RDRAM word order).
- **Yields**: `osSpTaskYielded` answering 1 also sets `OS_TASK_YIELDED` on the task; a resubmitted task carrying that flag is completed as a no-op (it already ran in full) and the flag is cleared. In testing the game never resubmitted one (`resumes_skipped=0`).

## Texture packs (HD textures project)

Goal: higher-quality textures through RT64 replacement packs (`.rtz` = zip with `rt64.json` mapping texture hashes to images). louriccia/SWE1R-Mods has nothing usable for this (PC `.bin` packs and joke recolors).

- **Phase 1, runtime support: done** (commit `005ebdf`). Verified with a red/blue-swapped, 4x pack on the title screen.
- **Phase 2, upscaling: working.** `tools/upscale_textures.py` runs Real-ESRGAN (`realesrgan-x4plus`) at 4x with 3x3 wrap tiling so repeating textures don't show seams, and upscales alpha separately (thresholded back to 0/255 for cut-outs). Coverage so far: boot, title, menus, vehicle select and The Boonta Training Course (902 textures, ~7 min to upscale, 45 MB pack). Checked in game: menu text and banners clearly sharper, HUD digits clean (no need to exclude fonts so far).
- **Seams fixed (2026-10-16).** The first pack showed a grid on the title background. Two causes: (1) every texture was padded as if it wrapped; now padding follows each axis's real mode from `.tile.json` (wrap / mirror / clamp). (2) Big images are drawn as many clamped tiles (title: 75 tiles of 64x32 at 0x212b30, 10 per row, each stored vertically flipped); upscaling tiles alone can't match their neighbours. The upscaler now finds such mosaics (clamped, same size, consecutive addresses; columns and flips picked by edge continuity against a random-pair baseline), upscales the reassembled image and cuts it back. `--plan` prints the mosaics without upscaling. Found 3 so far (title, a ring, a menu panel). Remaining: the title's last partial row (5 tiles) is placed elsewhere by the game and still upscales alone, so a faint line can show at the very bottom.

Policy: **no generated texture is versioned** (they derive from the ROM, and are reproducible). `tex_dump/`, `hd_out/` and `texture_packs/` are gitignored; only the recipe lives in git: the scripts and `textures/exclude.txt` (hashes to keep original). If textures ever get hand-retouched, those go in a separate private sibling repo (`../swe1r-hd-textures`, Git LFS), never a public remote.

Workflow:

```bash
SWE1R_DUMP_TEXTURES=tex_dump ./build/SWE1Racer   # play what you want covered; new textures add up
tools/build_hd_pack.sh                           # incremental upscale -> texture_packs/hd.rtz
./build/SWE1Racer                                # loads texture_packs/*.rtz
```

Next: dump the other 24 tracks (needs playing them by hand; autoinput only reaches the first), look for textures the AI spoils (I4/IA ones may look plastic) and list them in `textures/exclude.txt`.

## PC version: same assets, not worth porting (checked 2026-10-17)

Question was whether the 1999 Windows release would look better (more polygons, sharper textures) and deserve the same treatment. Measured against the user's PC CD image (`~/Downloads/SW racer PC.zip`, MODE1/2352 bin; data in `gnome/data/lev01/out_*block.bin`, big-endian, same formats as the N64 ROM):

| Models | N64 triangles | PC triangles |
|---|---|---|
| Tracks (25) | 241,257 | 241,212 |
| Pods (25) | 18,901 | 18,901 |
| Pupp, characters (38) | 12,995 | 12,995 |
| Part (183) | 38,684 | 38,684 |
| Scen, menu scenery | 2 models, 910 | 18 models, 22,134 |

- Geometry is identical model for model; tracks differ by under 1%.
- Textures: 95% of the 3,154 texture-block chunks are byte-identical, same total size. The PC release reused the N64 assets.
- Only real extra: 16 3D menu scenes on PC. Its "better looks" back then came from rendering resolution, which RT64 already gives us.

Decision: stay on the N64 recomp. ROM layout found on the way: model block at ROM `0x141e200` (307 models; 92 are `Comp`, LZSS, see `func_80011940`), texture block at `0x102abb0` (1648 textures). `tools/modelblock.py` parses either version and counts meshes, vertices and triangles; useful if model replacement ever comes up.

## Next steps, in order

1. **Finish a race** by hand and check the results screen and the next race.
2. **Gamepad via SDL**: game controller API, analog stick mapping, rumble.
3. **Audio**: working (see below). First listen by a human on 2026-10-17: sounds right in menus and a race. Keep an ear out for missing effects on untested screens (some audio functions may still be stubbed).
4. **Play-test races** on several tracks to catch remaining game-logic crashes.
5. **RT64 enhancements**: widescreen (note: RT64's source aspect in the serrate mode is still 320/476, so `Expand` will need a look), high resolution, interpolated 60 fps.
6. **macOS `.app` bundle**: icon, ROM import on first launch, saves in Application Support.
7. **Vision Pro stereo** (see the `avp-game-port` skill).

## Known loose ends

- Display-list hang (RT64 executing zeros as no-ops forever) is mitigated: `send_dl` pre-walks the F3DEX2 tree and drops a frame whose list runs into zeros, logging `[dl] list at ... runs into zeros`. Seen once: a sub-list call from model data at 0x801BFE58 landing at ~0x806D0000 (just below a framebuffer), so probably a stale segment register on a scene change. Root cause still open. Graphics tasks also wait until RT64 has parsed them (`swe1r_osSpTaskStartGo_recomp`), keeping hardware ordering.

- Windows build: Win32 paths kept under `_WIN32` but not compiled since the port. The KSEG1 RDRAM mirror needs a `MapViewOfFile` equivalent there.
- `on_game_init` still writes hacks (PI handle at 0x800A7BC0/0x800A7FC0, fake VI context at 0x800A7F00) that predate the real VI functions. 0x800A7BC0 is actually `__osRunningThread`. Worth revisiting.
- Lots of diagnostic logging (`[si]`, `[pi]`, `[vi]`, `[perf]`). Trim once things are stable.
- Why game threads can run concurrently at all (ultramodern should serialize them) is not understood. The timer race and possibly the stale-segment display lists come from it.
