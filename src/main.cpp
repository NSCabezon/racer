// Star Wars Episode I: Racer - N64 Static Recompilation
// Main entry point
//
// "Now THIS is podracing!" - Anakin Skywalker

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#else
#include <csignal>
#include <cinttypes>
#include <execinfo.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/mach_error.h>
#endif

#include <SDL.h>
#include <SDL_syswm.h>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/rsp.hpp"
#include "librecomp/addresses.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/ultra64.h"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/error_handling.hpp"
#include "ultramodern/events.hpp"
#include "ultramodern/input.hpp"
#include "ultramodern/threads.hpp"

// =============================================================================
// External declarations
// =============================================================================

// From section_table.cpp
extern SectionTableEntry section_table[];
extern const size_t num_sections;

// From rt64_render_context.cpp
extern std::unique_ptr<ultramodern::renderer::RendererContext> create_swe1r_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

// The recompiled entrypoint function
extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);

#if defined(__APPLE__)
// support_apple.mm
extern "C" void swe1r_pin_metal_layer_scale(void* layer);
#endif

// Defined below (input callbacks); used by the SI read-button response.
bool get_input(int controller_num, uint16_t* buttons, float* x, float* y);

// From audio.cpp
extern void swe1r_queue_samples(int16_t* samples, size_t num_samples);
extern size_t swe1r_get_frames_remaining();
extern void swe1r_set_frequency(uint32_t freq);

// RDRAM base — captured in on_game_init so the crash handler can translate
// native fault addresses back into N64 virtual addresses.
static uint8_t* g_rdram_base = nullptr;

// SI DMA: the game's __osSiRawStartDma is COP0/MMIO based and can't be
// recompiled. It moves a 64-byte block between DRAM and PIF RAM and raises an
// SI interrupt on completion.
//
//   a0 = direction (1 = DRAM->PIF write, 0 = PIF->DRAM read)
//   a1 = DRAM buffer address
//
// There is no PIF hardware here, so on a read we synthesize the controller
// status block the game's detection code (func_800899E8) expects: 8 bytes per
// channel, 4 channels. Controller 0 is reported present (standard pad, no pak);
// channels 1-3 report "no controller" via the error bits in byte 2.
//   byte0 pad | byte1 txsize | byte2 rxsize/error | byte3 cmd
//   byte4 type_lo | byte5 type_hi | byte6 status | byte7 pad
// The parser checks (byte2 & 0xC0) for an error, reads type from byte4/5 and
// the pak/status flag from byte6.
extern "C" void __osSiRawStartDma_recomp(uint8_t* rdram, recomp_context* ctx) {
    int32_t  dir = (int32_t)ctx->r4;
    uint32_t buf = (uint32_t)ctx->r5;

    if (dir == 0 && buf != 0) {
        // PIF -> DRAM read. The game laid out its command block in this same
        // buffer on the preceding DRAM->PIF write (8 bytes per channel:
        // pad | tx | rx | cmd | 4 response bytes), so inspect the command and
        // fill in the matching response. The block is word-aligned, so
        // big-endian-packed word accesses land the bytes correctly for the
        // recompiled MEM_B (XOR 3) accessors.
        auto r32 = [&](uint32_t addr) -> uint32_t {
            return *(uint32_t*)(rdram + (addr - 0x80000000u));
        };
        auto w32 = [&](uint32_t addr, uint32_t value) {
            *(uint32_t*)(rdram + (addr - 0x80000000u)) = value;
        };
        static uint32_t read_count = 0;
        read_count++;
        for (int ch = 0; ch < 4; ch++) {
            uint32_t c = buf + ch * 8;
            uint32_t hdr = r32(c);
            uint8_t cmd = (uint8_t)(hdr & 0xFF);
            if (ch != 0) {
                w32(c + 0, (hdr & 0xFFFF00FFu) | 0x00008000u); // rx error bit: no controller
                w32(c + 4, 0xFFFFFFFFu);
                continue;
            }
            if (cmd == 0x01) {
                // CONT_CMD_READ_BUTTON: buttons (u16), stick x (s8), stick y (s8)
                uint16_t buttons = 0; float x = 0.0f, y = 0.0f;
                get_input(0, &buttons, &x, &y);
                int8_t sx = (int8_t)std::clamp((int)(x * 80.0f), -80, 80);
                int8_t sy = (int8_t)std::clamp((int)(y * 80.0f), -80, 80);
                w32(c + 0, (hdr & 0xFFFF00FFu) | 0x00000001u); // clear rx error bits, keep cmd
                w32(c + 4, ((uint32_t)buttons << 16) | ((uint32_t)(uint8_t)sx << 8) | (uint8_t)sy);
                if (buttons != 0 && (read_count % 30) == 0) {
                    printf("[si] read_button -> buttons=0x%04X x=%d y=%d\n", buttons, sx, sy);
                }
            } else {
                // CONT_CMD_REQUEST_STATUS (0x00) / RESET (0xFF): standard pad, no pak.
                w32(c + 0, 0xFF010300u); // pad, tx=1, rx=3 (no error), cmd=0
                w32(c + 4, 0x050000FFu); // type=0x0005, status 0, pad
                if (read_count <= 4) printf("[si] status -> standard pad on channel 0 (cmd=0x%02X)\n", cmd);
            }
        }
    } else if (dir != 0) {
        // DRAM -> PIF write: dump each distinct 64-byte command block once so
        // the PIF emulation can be matched to what the game actually sends.
        static uint32_t write_count = 0;
        static std::vector<std::array<uint8_t, 64>> seen;
        write_count++;
        std::array<uint8_t, 64> blk{};
        for (int i = 0; i < 64; i++) blk[i] = rdram[((buf + i) ^ 3) - 0x80000000u];
        bool known = false;
        for (auto& b : seen) if (b == blk) { known = true; break; }
        if (!known && seen.size() < 16) {
            seen.push_back(blk);
            printf("[si] PIF block #%zu (write %u, buf=0x%08X):", seen.size(), write_count, buf);
            for (int i = 0; i < 64; i++) printf("%s%02X", (i % 8) ? " " : " | ", blk[i]);
            printf("\n");
        }
    }

    // No real hardware to wait on — signal SI completion immediately.
    ultramodern::send_si_message();
    ctx->r2 = 0; // success
}

// osSetEventMesg: the game registers every interrupt queue through this call,
// including OS_EVENT_VI. ultramodern's osSetEventMesg only records SP/DP/AI/SI
// and its VI thread delivers retraces solely to the queue set via osViSetEvent,
// so without this forwarder the scheduler (func_8008BC30) blocks forever in
// osRecvMesg waiting for a retrace that never arrives. Call sites are rewritten
// to this function by tools/post_recomp_patches.py.
//
//   a0 = event id, a1 = message queue, a2 = message
// SP completion ordering. ultramodern reports "RSP done" the moment a gfx
// task is dequeued, before RT64 has parsed the display list. On hardware the
// RSP has finished reading the list when that interrupt fires, and this game
// starts rebuilding the same (double-buffered) list right after it, so RT64
// ended up parsing a list being overwritten: random garbage, endless texrect
// loops, reads past 8 MB. We point ultramodern's SP event at a sink queue and
// deliver the real message ourselves: after RT64 parsed the list (send_dl), or
// immediately for audio tasks (no-op microcode).
static constexpr int32_t kSpSinkQueue = 0x800A7E80; // same sink the VI-state hack uses
static std::atomic<int32_t> g_sp_real_mq{0};
static std::atomic<int32_t> g_sp_real_msg{0};

extern "C" void swe1r_signal_sp_done() {
    int32_t mq = g_sp_real_mq.load(std::memory_order_relaxed);
    if (mq != 0) {
        ultramodern::enqueue_external_message(mq, (OSMesg)g_sp_real_msg.load(std::memory_order_relaxed), false, false);
    }
}

extern "C" void swe1r_osSetEventMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    OSEvent  event = (OSEvent)ctx->r4;
    int32_t  mq    = (int32_t)ctx->r5;
    OSMesg   msg   = (OSMesg)ctx->r6;

    if (event == OS_EVENT_SP) {
        printf("[events] osSetEventMesg(SP, mq=0x%08X, msg=0x%08X) -> deferred until the display list is parsed\n",
               (uint32_t)mq, (uint32_t)msg);
        g_sp_real_mq.store(mq, std::memory_order_relaxed);
        g_sp_real_msg.store((int32_t)msg, std::memory_order_relaxed);
        osSetEventMesg(rdram, OS_EVENT_SP, kSpSinkQueue, 0);
        return;
    }

    if (event == OS_EVENT_VI) {
        printf("[events] osSetEventMesg(VI) -> osViSetEvent(mq=0x%08X, msg=0x%08X, retrace=1)\n",
               (uint32_t)mq, (uint32_t)msg);
        osViSetEvent(rdram, mq, msg, 1);
        return;
    }

    printf("[events] osSetEventMesg(event=%u, mq=0x%08X, msg=0x%08X)\n",
           (unsigned)event, (uint32_t)mq, (uint32_t)msg);
    osSetEventMesg(rdram, event, mq, msg);
}

// osViSetSpecialFeatures(func): forwarded to ultramodern, logged. SWE1R_NO_SPECIAL=1
// ignores it (diagnostic: the game enables the VI dither filter and disables
// gamma for its hi-res modes, control 0x13052).
extern "C" void osViSetSpecialFeatures_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void swe1r_osViSetSpecialFeatures_recomp(uint8_t* rdram, recomp_context* ctx) {
    static const bool ignore = std::getenv("SWE1R_NO_SPECIAL") != nullptr;
    printf("[vi] osViSetSpecialFeatures(0x%08X)%s\n", (uint32_t)ctx->r4, ignore ? " ignored" : "");
    if (!ignore) osViSetSpecialFeatures_recomp(rdram, ctx);
    ctx->r2 = 0;
}

// RSP task yield emulation. The game's SP scheduler (func_8002E2FC and friends,
// flags at 0x800A2670..0x800A2694) requests osSpTaskYield when an audio task is
// still "in flight" at frame start, waits for the SP completion, then asks
// osSpTaskYielded: if yielded it parks the task and resumes it after the gfx
// task (0x8002E548); if not, it leaves its audio-pending flag set and the next
// frame requests a yield with nothing running, waiting forever. ultramodern
// runs tasks to completion instantly and librecomp answers "never yielded",
// which is exactly the deadlock. Answering "yielded" after a request keeps the
// game's state machine consistent. The resumed audio task is simply run again;
// harmless while audio is a no-op (revisit when real audio microcode lands).
static std::atomic<bool> g_sp_yield_requested{false};

extern "C" void swe1r_osSpTaskYield_recomp(uint8_t* rdram, recomp_context* ctx) {
    g_sp_yield_requested.store(true, std::memory_order_relaxed);
    ctx->r2 = 0;
}

extern "C" void swe1r_osSpTaskYielded_recomp(uint8_t* rdram, recomp_context* ctx) {
    bool yielded = g_sp_yield_requested.exchange(false, std::memory_order_relaxed);
    static uint32_t count = 0;
    if (++count <= 5) printf("[sp] osSpTaskYielded -> %d\n", yielded ? 1 : 0);
    ctx->r2 = yielded ? 1 : 0;
}

// func_800811DC: the game's 64-bit time, (wraps << 32) + osGetCount(), where
// wraps (0x800A675C) is bumped whenever the count is lower than the last one
// seen (0x800A6760). On hardware only one thread runs at a time; here two game
// threads can interleave inside it, so one sees the count "go backwards", adds
// a wrap and time jumps 91.6 s. The animation code (func_80006848 <->
// func_80006790) then wraps a looping animation once per period of that delta,
// recursing until the main thread's stack runs down through .data/.bss.
// Same algorithm and same globals (the game resets the wrap count itself via
// func_80081260), but read and updated atomically.
extern "C" void swe1r_get_time64_recomp(uint8_t* rdram, recomp_context* ctx) {
    static std::mutex m;
    std::lock_guard lock{ m };
    uint32_t* wraps = (uint32_t*)(rdram + 0xA675C);
    uint32_t* last  = (uint32_t*)(rdram + 0xA6760);
    uint32_t count = osGetCount();
    if (count < *last) {
        (*wraps)++;
        static uint32_t logged = 0;
        if (++logged <= 4) printf("[time] osGetCount wrapped (last 0x%08X now 0x%08X), wraps=%u\n", *last, count, *wraps);
    }
    *last = count;
    uint64_t t = ((uint64_t)*wraps << 32) + count;
    ctx->r2 = (int32_t)(uint32_t)(t >> 32);
    ctx->r3 = (int32_t)(uint32_t)t;
}

// osPiStartDma(mb, pri, dir, devAddr, dramAddr, size, mq): logged pass-through
// to librecomp's synchronous DMA (the game's PI manager thread is stubbed).
extern "C" void osPiStartDma_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void swe1r_osPiStartDma_recomp(uint8_t* rdram, recomp_context* ctx) {
    static uint32_t count = 0;
    uint32_t sp = (uint32_t)ctx->r29;
    auto r32 = [&](uint32_t addr) -> uint32_t { return *(uint32_t*)(rdram + (addr - 0x80000000u)); };
    uint32_t dram = r32(sp + 0x10), size = r32(sp + 0x14), mq = r32(sp + 0x18);
    if (++count <= 40 || (count % 50) == 0) {
        printf("[pi] DMA #%u dir=%u rom=0x%08X -> 0x%08X size=0x%X mq=0x%08X\n",
               count, (uint32_t)ctx->r6, (uint32_t)ctx->r7, dram, size, mq);
    }
    osPiStartDma_recomp(rdram, ctx);
}

// osPiReadIo(devAddr, u32* data): single-word PIO read from the cartridge. The
// game reads its file table and asset headers this way (tools: func_80011BDC
// loops over osPiReadIo per word for reads below 4 bytes alignment, and
// read_rom chunks of up to 2 KB go through osPiStartDma). librecomp only
// provides osEPiReadIo (handle-based), so we serve the word straight from the
// ROM image. Call sites are redirected here by tools/post_recomp_patches.py.
//
//   a0 = cart offset (0x0141E200 style; may or may not carry the 0x10000000 base)
//   a1 = destination pointer (RDRAM)
extern "C" void swe1r_osPiReadIo_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t dev_addr = (uint32_t)ctx->r4;
    uint32_t dst      = (uint32_t)ctx->r5;
    uint32_t phys     = (dev_addr & 0x1FFFFFFFu) | recomp::rom_base; // 0x10000000 cart base
    uint32_t offset   = phys - recomp::rom_base;

    auto rom = recomp::get_rom();
    uint32_t value = 0;
    if (offset + 4 <= rom.size()) {
        value = (uint32_t)rom[offset] << 24 | (uint32_t)rom[offset + 1] << 16 |
                (uint32_t)rom[offset + 2] << 8 | (uint32_t)rom[offset + 3];
    } else {
        fprintf(stderr, "[pi] osPiReadIo out of ROM: dev=0x%08X\n", dev_addr);
    }
    MEM_W(0, (gpr)(int32_t)dst) = value;

    static uint32_t count = 0;
    if (++count <= 8) {
        printf("[pi] osPiReadIo(0x%08X) -> 0x%08X @ 0x%08X\n", dev_addr, value, dst);
    }
    ctx->r2 = 0;
}

// =============================================================================
// ROM Hash: Star Wars Episode I: Racer (US)
// CRC1: 0x72F70398  CRC2: 0x6556A98B
// =============================================================================
constexpr uint64_t SWE1R_ROM_HASH = 0x9B89D0F9EBC12D32ULL; // XXH3_64bits of baserom.z64

// =============================================================================
// Global state
// =============================================================================
static SDL_Window* g_window = nullptr;

// =============================================================================
// RSP Callbacks
// =============================================================================

// No-op RSP microcode. We don't yet have SWE1R's recompiled audio microcode
// (that would come from N64Recomp's RSPRecomp, like pokemonsnap's aspMain).
// Returning nullptr makes ultramodern's run_task abort the whole process, so
// instead we pretend audio tasks completed (RspExitReason::Broke). Result: no
// sound yet, but the game proceeds — and graphics tasks (M_GFXTASK) never reach
// here anyway; submit_rsp_task routes those straight to the RT64 renderer.
static RspExitReason swe1r_noop_ucode(uint8_t* /*rdram*/, uint32_t /*ucode_addr*/) {
    // Audio task "completed": deliver the real SP message now (ultramodern's
    // own completion goes to the sink queue, see swe1r_osSetEventMesg_recomp).
    swe1r_signal_sp_done();
    return RspExitReason::Broke;
}

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    static uint32_t rsp_call_count = 0;
    rsp_call_count++;
    if (rsp_call_count <= 5 || (rsp_call_count % 120 == 0)) {
        fprintf(stderr, "[SWE1R] get_rsp_microcode #%u: type=%u ucode=0x%08X data=0x%08X\n",
                rsp_call_count, task->t.type,
                (uint32_t)task->t.ucode, (uint32_t)task->t.ucode_data);
    }
    // Only non-graphics (audio) tasks reach here; no-op them for now.
    return swe1r_noop_ucode;
}

// =============================================================================
// Renderer Callbacks
// =============================================================================

std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context_callback(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return create_swe1r_render_context(rdram, window_handle, developer_mode);
}

// =============================================================================
// GFX Callbacks (window management)
// =============================================================================

ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
#if defined(_WIN32)
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "Failed to initialize SDL2: %s\n", SDL_GetError());
        std::exit(1);
    }

    printf("[SWE1R] SDL initialized: %s\n", SDL_GetCurrentVideoDriver());
    return nullptr;
}

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    uint32_t window_flags = SDL_WINDOW_RESIZABLE;
#if defined(__APPLE__)
    // RT64's Metal backend renders into a CAMetalLayer owned by an SDL Metal view.
    window_flags |= SDL_WINDOW_METAL;
#endif

    g_window = SDL_CreateWindow(
        "Star Wars Episode I: Racer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 960,
        window_flags
    );

    if (!g_window) {
        fprintf(stderr, "Failed to create window: %s\n", SDL_GetError());
        std::exit(1);
    }

    printf("[SWE1R] Window created (1280x960)\n");

    // Return native window handle
    SDL_SysWMinfo wminfo;
    SDL_VERSION(&wminfo.version);
    SDL_GetWindowWMInfo(g_window, &wminfo);

#if defined(_WIN32)
    ultramodern::renderer::WindowHandle handle{};
    handle.window = wminfo.info.win.window;
    handle.thread_id = GetCurrentThreadId();
    return handle;
#elif defined(__APPLE__)
    // Hand RT64 the NSWindow plus the CAMetalLayer backing the SDL Metal view.
    // The view is intentionally never destroyed: it must outlive the swap chain.
    SDL_MetalView metal_view = SDL_Metal_CreateView(g_window);
    if (!metal_view) {
        fprintf(stderr, "Failed to create Metal view: %s\n", SDL_GetError());
        std::exit(1);
    }
    ultramodern::renderer::WindowHandle handle{};
    handle.window = wminfo.info.cocoa.window;
    handle.view = SDL_Metal_GetLayer(metal_view);
    swe1r_pin_metal_layer_scale(handle.view);
    return handle;
#else
#error "create_window: unsupported platform"
#endif
}

void update_gfx(ultramodern::gfx_callbacks_t::gfx_data_t) {
    // Process SDL events
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                ultramodern::quit();
                break;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    ultramodern::quit();
                }
                break;
            case SDL_WINDOWEVENT:
                printf("[sdl] window event %u (data %d,%d)\n", (unsigned)event.window.event,
                       event.window.data1, event.window.data2);
                break;
        }
    }
}

// =============================================================================
// Input Callbacks (stub)
// =============================================================================

void poll_input() {
    // SDL event polling is done in update_gfx
}

// Scripted input for unattended runs: SWE1R_AUTOINPUT="12:start,30:a" presses
// the named button at that many seconds after start, held for half a second.
// Buttons: start a b z l r up down left right; stick: sl sr su sd.
// A range "40-90:a" holds the button from 40 s to 90 s.
static float g_autoinput_x = 0.0f, g_autoinput_y = 0.0f;
static uint16_t autoinput_buttons_at(double t_seconds) {
    struct Press { double t; double t_end; uint16_t mask; int8_t sx, sy; };
    static std::vector<Press> presses;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* spec = std::getenv("SWE1R_AUTOINPUT")) {
            std::string str(spec);
            size_t pos = 0;
            while (pos < str.size()) {
                size_t comma = str.find(',', pos);
                std::string item = str.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                size_t colon = item.find(':');
                if (colon != std::string::npos) {
                    std::string when = item.substr(0, colon);
                    double t = atof(when.c_str());
                    size_t dash = when.find('-');
                    double t_end = (dash != std::string::npos) ? atof(when.c_str() + dash + 1) : t + 0.5;
                    std::string name = item.substr(colon + 1);
                    uint16_t mask = 0;
                    if (name == "start") mask = 0x1000; else if (name == "a") mask = 0x8000;
                    else if (name == "b") mask = 0x4000; else if (name == "z") mask = 0x2000;
                    else if (name == "l") mask = 0x0020; else if (name == "r") mask = 0x0010;
                    else if (name == "up") mask = 0x0800; else if (name == "down") mask = 0x0400;
                    else if (name == "left") mask = 0x0200; else if (name == "right") mask = 0x0100;
                    int8_t sx = 0, sy = 0;
                    if (name == "sl") sx = -1; else if (name == "sr") sx = 1;
                    else if (name == "su") sy = 1; else if (name == "sd") sy = -1;
                    if (mask || sx || sy) presses.push_back({ t, t_end, mask, sx, sy });
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            printf("[input] SWE1R_AUTOINPUT: %zu scripted presses\n", presses.size());
        }
    }
    uint16_t out = 0;
    g_autoinput_x = 0.0f; g_autoinput_y = 0.0f;
    for (const Press& p : presses) {
        if (t_seconds >= p.t && t_seconds < p.t_end) {
            out |= p.mask;
            if (p.sx) g_autoinput_x = p.sx; if (p.sy) g_autoinput_y = p.sy;
        }
    }
    return out;
}

bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0) return false;

    *buttons = 0;
    *x = 0.0f;
    *y = 0.0f;

    {
        using namespace std::chrono;
        static const auto t0 = steady_clock::now();
        double t = duration<double>(steady_clock::now() - t0).count();
        uint16_t scripted = autoinput_buttons_at(t);
        *x += g_autoinput_x; *y += g_autoinput_y;
        if (scripted) {
            static uint16_t last = 0;
            if (scripted != last) printf("[input] scripted buttons 0x%04X at %.1fs\n", scripted, t);
            last = scripted;
            *buttons |= scripted;
        }
    }

    // Basic keyboard input
    const uint8_t* keys = SDL_GetKeyboardState(nullptr);

    // N64 button mappings
    if (keys[SDL_SCANCODE_RETURN]) *buttons |= 0x8000; // A
    if (keys[SDL_SCANCODE_LSHIFT]) *buttons |= 0x4000; // B
    if (keys[SDL_SCANCODE_Z])      *buttons |= 0x2000; // Z
    if (keys[SDL_SCANCODE_SPACE])  *buttons |= 0x1000; // Start
    if (keys[SDL_SCANCODE_UP])     *buttons |= 0x0800; // D-Up
    if (keys[SDL_SCANCODE_DOWN])   *buttons |= 0x0400; // D-Down
    if (keys[SDL_SCANCODE_LEFT])   *buttons |= 0x0200; // D-Left
    if (keys[SDL_SCANCODE_RIGHT])  *buttons |= 0x0100; // D-Right

    // Analog stick via WASD
    if (keys[SDL_SCANCODE_W]) *y += 1.0f;
    if (keys[SDL_SCANCODE_S]) *y -= 1.0f;
    if (keys[SDL_SCANCODE_A]) *x -= 1.0f;
    if (keys[SDL_SCANCODE_D]) *x += 1.0f;

    return true;
}

void set_rumble(int controller_num, bool rumble) {
    // TODO: SDL haptic feedback
}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (controller_num == 0) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

// =============================================================================
// Event Callbacks
// =============================================================================

// Retraces seen so far. ultramodern's VI thread only swaps its double-buffered
// ViState when the pending state has a VI mode, and before the game starts the
// only thing that sets one is the thread's own dummy VI path. The game's VI
// manager registers its retrace queue through osViSetEvent into the *pending*
// state, so if we start the game before a couple of dummy retraces have run,
// that state never becomes current and the VI manager blocks forever in
// osRecvMesg. We therefore hold start_game until a few retraces have passed.

// osViBlack(active): the game calls osViBlack(1) at boot (func_8002F550) and
// again on scene changes (func_80039D34), and never osViBlack(0). On hardware
// VI_STATE_BLACK is consumed by __osViSwapContext at the next retrace, so the
// screen only stays black for one frame. ultramodern keeps the flag set until
// an explicit osViBlack(0), which left the window black forever. We emulate
// the one-shot semantics: apply the black, then clear it two retraces later
// from the VI thread. Call sites are rewritten to this function by
// tools/post_recomp_patches.py.
static std::atomic<uint32_t> g_vi_count{0};
static std::atomic<uint32_t> g_unblack_at{0};

extern "C" void swe1r_osViBlack_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint8_t active = (uint8_t)ctx->r4;
    printf("[vi] osViBlack(%u) at retrace %u\n", active, g_vi_count.load(std::memory_order_relaxed));
    osViBlack(active);
    if (active) {
        g_unblack_at.store(g_vi_count.load(std::memory_order_relaxed) + 2, std::memory_order_relaxed);
    } else {
        g_unblack_at.store(0, std::memory_order_relaxed);
    }
    ctx->r2 = 0;
}

void swe1r_dl_watchdog_tick(); // rt64_render_context.cpp
#if defined(__APPLE__) && defined(__aarch64__)
static void swe1r_arm_hw_watch(uint8_t* rdram, uint32_t n64_addr);
#endif

void vi_callback() {
    // Called every VI interrupt
    uint32_t n = g_vi_count.fetch_add(1, std::memory_order_relaxed) + 1;
    swe1r_dl_watchdog_tick();

    // Unattended shutdown test: SWE1R_QUIT_AFTER=<seconds> runs the same path as ESC.
    static const int quit_after = std::getenv("SWE1R_QUIT_AFTER") ? atoi(std::getenv("SWE1R_QUIT_AFTER")) : 0;
    if (quit_after > 0 && n == (uint32_t)quit_after * 60) {
        printf("[SWE1R] SWE1R_QUIT_AFTER: requesting quit at retrace %u\n", n);
        ultramodern::quit();
    }

    // Debug: report large frame delta times (double at 0x80120BF0, seconds).
    static const bool log_dt = std::getenv("SWE1R_LOG_DT") != nullptr;
    if (log_dt && g_rdram_base) {
        uint64_t bits = ((uint64_t)*(uint32_t*)(g_rdram_base + 0x120BF0) << 32) | *(uint32_t*)(g_rdram_base + 0x120BF4);
        double dt; memcpy(&dt, &bits, 8);
        static double last = 0;
        if (dt != last && (dt > 0.1 || dt < 0)) printf("[dt] frame delta %.3f s at retrace %u\n", dt, n);
        last = dt;
    }

#if defined(__APPLE__) && defined(__aarch64__)
    // Debug: SWE1R_HWWATCH="0x800A8150:46" arms a hardware write watchpoint at 46 s.
    static const char* hwwatch = std::getenv("SWE1R_HWWATCH");
    if (hwwatch && g_rdram_base) {
        static uint32_t at = 0, addr = 0;
        if (at == 0) { addr = strtoul(hwwatch, nullptr, 16); const char* c = strchr(hwwatch, ':'); at = (c ? atoi(c + 1) : 40) * 60; }
        if (n == at) swe1r_arm_hw_watch(g_rdram_base, addr);
    }
#endif

    uint32_t target = g_unblack_at.load(std::memory_order_relaxed);
    if (target != 0 && n >= target) {
        g_unblack_at.store(0, std::memory_order_relaxed);
        osViBlack(0);
    }
}

void gfx_init_callback() {
    printf("[SWE1R] Graphics initialized\n");
    // Start the game now that the renderer and VI thread are ready.
    // This must happen after start() has begun, not before, so the VI
    // thread gets a chance to set dummy VI state before is_game_started() is true.
    // The VI thread is spawned only after this callback returns, so the wait
    // has to happen on a helper thread rather than here.
    std::thread([]() {
        constexpr uint32_t kWarmupRetraces = 4;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (g_vi_count.load(std::memory_order_relaxed) < kWarmupRetraces &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        printf("[SWE1R] VI warm-up done (%u retraces)\n", g_vi_count.load());
        std::u8string game_id = u8"swe1racer";
        // Second argument is the game-mode mod id; empty selects the base game.
        recomp::start_game(game_id, std::string{});
        printf("[SWE1R] Game started\n");
    }).detach();
}

// =============================================================================
// Error Handling
// =============================================================================

void error_message_box(const char* msg) {
    fprintf(stderr, "[SWE1R ERROR] %s\n", msg);
    if (g_window) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SWE1R Error", msg, g_window);
    }
}

// =============================================================================
// Thread Naming
// =============================================================================

std::string get_game_thread_name(const OSThread* t) {
    return "SWE1R-Thread";
}

// =============================================================================
// Game Init Callback - initialize libultra state that stubbed boot code skips
// =============================================================================

void on_game_init(uint8_t* rdram, recomp_context* ctx) {
    // Capture the RDRAM base so the crash handler can map native fault
    // addresses back to N64 virtual addresses.
    g_rdram_base = rdram;

    // The game's boot code at 0x8008D284 initializes the PI (cartridge ROM) handle
    // and stores it at globals 0x800A7BC0 and 0x800A7FC0. That code is stubbed
    // because it contains COP0 instructions. We replicate the initialization here.

    // Initialize the PI handle struct at the pre-allocated RDRAM address
    int32_t handle_addr = recomp::cart_handle; // 0x80800000
    OSPiHandle* handle = (OSPiHandle*)(rdram + ((uint32_t)handle_addr - 0x80000000u));
    memset(handle, 0, sizeof(OSPiHandle));
    handle->type = 0; // PI_DOMAIN2 (cartridge)
    handle->baseAddress = 0xB0000000; // phys_to_k1(0x10000000) = cartridge ROM base
    handle->domain = 0;

    // Store the handle pointer in the game's global variables (big-endian u32 writes)
    // 0x800A7BC0 = __osCartRomHandle (used by PI access functions)
    // 0x800A7FC0 = __osPiTable (PI device linked list head)
    auto write32 = [&](uint32_t mips_addr, uint32_t value) {
        *(uint32_t*)(rdram + (mips_addr - 0x80000000u)) = value;
    };

    write32(0x800A7BC0, (uint32_t)handle_addr);
    write32(0x800A7FC0, (uint32_t)handle_addr);

    // Self-link the handle's next pointer (linked list of one element)
    // handle_addr = 0x80800000, so this writes to RDRAM offset 0x00800000 (8MB)
    write32((uint32_t)handle_addr, (uint32_t)handle_addr); // handle->unused (next ptr) = self

    printf("[SWE1R] PI handle initialized at 0x%08X\n", (uint32_t)handle_addr);

    // Initialize VI state for the scheduler thread.
    // The scheduler (func_8008BC30) calls static_0_800941D0 which reads a pointer
    // from 0x800A7F50 pointing to the "current VI mode" struct. It uses:
    //   +0x02 (halfword): retrace count — how many VI retraces before sending a response
    //   +0x10 (word): response queue — where to send the "frame done" message
    // osCreateViManager normally sets this up, but it's stubbed in the recomp.
    // Without this, the scheduler sends responses to its own queue (feedback loop)
    // and never yields to the game thread.
    //
    // We create a fake VI state at 0x800A7F00 (unused BSS near the pointer) and a
    // "sink" queue at 0x800A7E80 that absorbs the scheduler's responses.

    // Create the sink queue: osCreateMesgQueue(queue=0x800A7E80, buf=0x800A7E60, count=8)
    // We can't call osCreateMesgQueue_recomp here (no ctx), so write the struct directly.
    // OSMesgQueue layout (big-endian):
    //   +0x00: mtqueue (PTR) = 0 (unused)
    //   +0x04: fullqueue (PTR) = 0 (unused)
    //   +0x08: validCount (s32) = 0
    //   +0x0C: first (s32) = 0
    //   +0x10: msgCount (s32) = 8
    //   +0x14: msg (PTR) = 0x800A7E60 (buffer)
    write32(0x800A7E80 + 0x00, 0);
    write32(0x800A7E80 + 0x04, 0);
    write32(0x800A7E80 + 0x08, 0);
    write32(0x800A7E80 + 0x0C, 0);
    write32(0x800A7E80 + 0x10, 8);
    write32(0x800A7E80 + 0x14, 0x800A7E60);

    // Create fake VI state at 0x800A7F00
    // RDRAM uses native word order with XOR byte-swapping for sub-word access.
    // MEM_W reads/writes uint32_t directly. MEM_HU(0x02, base) reads the lower
    // halfword of the word at base. So to set the halfword at +0x02 = 1,
    // we write the word at +0x00 = 0x00000001 (lower half = 1, upper half = 0).
    memset(rdram + (0x800A7F00 - 0x80000000u), 0, 0x20);
    write32(0x800A7F00, 0x00000001);  // +0x00 word: lower halfword (+0x02) = retrace count 1
    write32(0x800A7F10, 0x800A7E80);  // +0x10: response queue = sink queue

    // Point the VI state pointer to our struct
    write32(0x800A7F50, 0x800A7F00);
    printf("[SWE1R] VI state initialized at 0x800A7F00, sink queue at 0x800A7E80\n");

    // osMemSize at 0x80000318 — IPL3 normally writes the detected RDRAM size
    // here, but we stub IPL3. SWE1R requires the Expansion Pak, so report 8MB.
    // The framebuffer allocator (func_80039A38) reads this to size buffers.
    // 0x318 is below the game's BSS clear (0x520+), so this value survives;
    // the framebuffer pointer table at 0x80114530 must NOT be seeded here, as
    // it sits inside the cleared range and is populated by func_80039A38.
    write32(0x80000318, 0x00800000);
    printf("[SWE1R] osMemSize=0x800000 written at 0x80000318\n");

    // The boot trampoline (0x80000480) and some libultra remnants poke the
    // RCP hardware registers directly through KSEG1 (0xA3F00000-0xA4FFFFFF:
    // RDRAM regs, SP, DPC, MI, VI, AI, PI, RI, SI). librecomp maps RDRAM as a
    // 4 GB reservation that is only readable/writable for the first 512 MB,
    // so those accesses land at rdram + 0x24400024 and fault. Open that window
    // as plain zero-filled memory: writes are absorbed, reads return 0. The
    // real register semantics are provided by the ultramodern reimplementations.
    {
        const size_t mmio_offset = 0xA3F00000u - 0x80000000u; // 0x23F00000
        const size_t mmio_size   = 0x01100000u;               // up to 0xA5000000
#if defined(_WIN32)
        DWORD old_protect = 0;
        if (!VirtualProtect(rdram + mmio_offset, mmio_size, PAGE_READWRITE, &old_protect)) {
            fprintf(stderr, "[SWE1R] WARNING: could not unprotect MMIO window\n");
        }
#else
        if (mprotect(rdram + mmio_offset, mmio_size, PROT_READ | PROT_WRITE) != 0) {
            perror("[SWE1R] WARNING: could not unprotect MMIO window");
        }
#endif
        printf("[SWE1R] MMIO window 0xA3F00000-0xA4FFFFFF mapped as scratch memory\n");
    }

    // KSEG1 mirror of RDRAM. The game addresses DMA buffers through uncached
    // pointers (OS_K0_TO_K1, 0xA0000000 | phys), which the recomp translates to
    // rdram + 0x20000000: beyond the 512 MB the runtime makes accessible, so
    // every such access faults. On hardware KSEG0 and KSEG1 are the same RAM,
    // so alias the 8 MB of RDRAM at the KSEG1 offset with shared pages: writes
    // through either segment are visible through the other.
#if defined(__APPLE__)
    {
        const mach_vm_size_t rdram_bytes = 0x00800000;            // 8 MB (Expansion Pak)
        mach_vm_address_t    target      = (mach_vm_address_t)(uintptr_t)(rdram + 0x20000000u);
        vm_prot_t cur_prot = 0, max_prot = 0;
        kern_return_t kr = mach_vm_remap(mach_task_self(), &target, rdram_bytes, 0,
                                         VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                         mach_task_self(), (mach_vm_address_t)(uintptr_t)rdram,
                                         FALSE, &cur_prot, &max_prot, VM_INHERIT_NONE);
        if (kr == KERN_SUCCESS) {
            printf("[SWE1R] KSEG1 RDRAM mirror mapped at 0xA0000000 (shared pages)\n");
        } else {
            fprintf(stderr, "[SWE1R] WARNING: KSEG1 RDRAM mirror failed: %s\n", mach_error_string(kr));
        }
    }
#else
    // TODO(win32/linux): alias with MapViewOfFile / memfd + mmap(MAP_FIXED).
#endif
}

#if defined(_WIN32)
// =============================================================================
// Map-file symbolizer
//
// Release builds have no useful PDB symbols for the recompiled funcs (SymFromAddr
// returns garbage like "wcsrchr"). The linker .map file, however, lists every
// func_XXXX with its preferred VA. We parse it at startup and resolve crash
// stack frames against it — automating the manual map lookup used during Phase 5.
// =============================================================================
struct MapSym { uint32_t rva; std::string name; };
static std::vector<MapSym> g_map_syms;
static constexpr uint64_t MAP_PREFERRED_BASE = 0x140000000ull;

static bool is_hex16_va(const std::string& s) {
    if (s.size() != 16 || s.compare(0, 10, "0000000140") != 0) return false;
    for (char c : s) if (!isxdigit((unsigned char)c)) return false;
    return true;
}

static void load_map_symbols() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::filesystem::path mapPath = std::filesystem::path(exePath).replace_extension(".map");
    std::ifstream f(mapPath);
    if (!f) {
        fprintf(stderr, "[map] no symbol map at %s (crash traces won't be symbolized)\n",
                mapPath.string().c_str());
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        // Symbol lines look like: " 0001:00025e00   <name>   0000000140026e00 f i x.obj"
        std::istringstream iss(line);
        std::vector<std::string> tok;
        for (std::string t; iss >> t; ) tok.push_back(t);
        if (tok.size() < 3) continue;
        // tok[0] must be SECTION:OFFSET
        if (tok[0].size() != 13 || tok[0][4] != ':') continue;
        for (size_t i = 2; i < tok.size(); i++) {
            if (is_hex16_va(tok[i])) {
                uint64_t va = strtoull(tok[i].c_str(), nullptr, 16);
                g_map_syms.push_back({ (uint32_t)(va - MAP_PREFERRED_BASE), tok[i - 1] });
                break;
            }
        }
    }
    std::sort(g_map_syms.begin(), g_map_syms.end(),
              [](const MapSym& a, const MapSym& b) { return a.rva < b.rva; });
    fprintf(stderr, "[map] loaded %zu symbols from %s\n",
            g_map_syms.size(), mapPath.filename().string().c_str());
}

// Returns "name + 0xNNN" for a module-relative address, or empty if unknown.
static std::string symbolize_rva(uint32_t rva) {
    if (g_map_syms.empty()) return {};
    auto it = std::upper_bound(g_map_syms.begin(), g_map_syms.end(), rva,
                               [](uint32_t v, const MapSym& m) { return v < m.rva; });
    if (it == g_map_syms.begin()) return {};
    --it;
    char buf[320];
    snprintf(buf, sizeof(buf), "%s + 0x%X", it->name.c_str(), rva - it->rva);
    return buf;
}

// =============================================================================
// Main
// =============================================================================

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    fprintf(stderr, "\n[CRASH] Exception 0x%08lX at address 0x%p\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        fprintf(stderr, "[CRASH] Access violation %s address 0x%p\n",
                ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (void*)fault);
        // Translate the fault back into an N64 address using the RDRAM base.
        if (g_rdram_base) {
            int64_t off = (int64_t)(fault - (uintptr_t)g_rdram_base);
            // MEM_B uses XOR 3 byte-swizzling, so undo it for the printed addr.
            uint32_t n64 = (uint32_t)(off + 0x80000000);
            uint32_t n64_b = (uint32_t)((off ^ 3) + 0x80000000);
            const char* tag = (off >= -0x10000 && off < 0x00900000)
                              ? "in RDRAM" : "out of RDRAM";
            fprintf(stderr, "[CRASH] rdram_base=%p offset=%+lld (%s)\n",
                    (void*)g_rdram_base, (long long)off, tag);
            fprintf(stderr, "[CRASH] -> N64 word addr ~0x%08X  byte addr ~0x%08X\n",
                    n64, n64_b);
        } else {
            fprintf(stderr, "[CRASH] rdram_base not set yet\n");
        }
    }
    // Print module + offset for the crash address
    HMODULE hMod = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &hMod);
    if (hMod) {
        char modName[MAX_PATH];
        GetModuleFileNameA(hMod, modName, MAX_PATH);
        uintptr_t offset = (uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)hMod;
        fprintf(stderr, "[CRASH] Module: %s + 0x%llX\n", modName, (unsigned long long)offset);
    }
    // Walk the stack and symbolize each frame against the linker .map. This
    // resolves the recompiled func_XXXX names that PDB-based SymFromAddr cannot.
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    uintptr_t modBase = (uintptr_t)GetModuleHandle(NULL);
    STACKFRAME64 frame = {};
    CONTEXT ctx = *ep->ContextRecord;
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    fprintf(stderr, "[CRASH] Stack trace (symbolized via .map):\n");
    for (int i = 0; i < 24; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(),
                         GetCurrentThread(), &frame, &ctx, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL))
            break;
        uint64_t pc = frame.AddrPC.Offset;
        std::string name;
        if (pc >= modBase) name = symbolize_rva((uint32_t)(pc - modBase));
        if (!name.empty())
            fprintf(stderr, "  [%d] %s   (rva 0x%X)\n", i, name.c_str(),
                    (uint32_t)(pc - modBase));
        else
            fprintf(stderr, "  [%d] 0x%llX\n", i, (unsigned long long)pc);
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

#else // !_WIN32

// =============================================================================
// POSIX crash handler (macOS / Linux)
//
// Mirrors the Win32 handler: prints the signal and fault address, translates
// the fault back into an N64 address via g_rdram_base, then dumps a native
// backtrace. backtrace_symbols_fd resolves names through dladdr, and the
// recompiled func_XXXXXXXX functions are exported symbols, so they show up
// without any map-file parsing. Kept malloc-free (fd-based symbolization).
// =============================================================================
static void posix_crash_handler(int sig, siginfo_t* info, void* /*ucontext*/) {
    const char* sig_name = (sig == SIGSEGV) ? "SIGSEGV"
                         : (sig == SIGBUS)  ? "SIGBUS"
                         : "signal";
    uintptr_t fault = info ? (uintptr_t)info->si_addr : 0;
    fprintf(stderr, "\n[CRASH] %s (%d) code=%d at fault address 0x%p\n",
            sig_name, sig, info ? info->si_code : 0, (void*)fault);

    // Translate the fault back into an N64 address using the RDRAM base.
    if (g_rdram_base) {
        int64_t off = (int64_t)(fault - (uintptr_t)g_rdram_base);
        // MEM_B uses XOR 3 byte-swizzling, so undo it for the printed addr.
        uint32_t n64 = (uint32_t)(off + 0x80000000);
        uint32_t n64_b = (uint32_t)((off ^ 3) + 0x80000000);
        const char* tag = (off >= -0x10000 && off < 0x00900000)
                          ? "in RDRAM" : "out of RDRAM";
        fprintf(stderr, "[CRASH] rdram_base=%p offset=%+lld (%s)\n",
                (void*)g_rdram_base, (long long)off, tag);
        fprintf(stderr, "[CRASH] -> N64 word addr ~0x%08X  byte addr ~0x%08X\n",
                n64, n64_b);
        // SWE1R_CRASH_DUMP=path: write the 8 MB RDRAM (native word order) for offline analysis.
        if (const char* dump = getenv("SWE1R_CRASH_DUMP")) {
            int fd = open(dump, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                ssize_t n = write(fd, g_rdram_base, 0x800000);
                close(fd);
                fprintf(stderr, "[CRASH] RDRAM dumped to %s (%zd bytes)\n", dump, n);
            }
        }
    } else {
        fprintf(stderr, "[CRASH] rdram_base not set yet\n");
    }

    fprintf(stderr, "[CRASH] Stack trace (native, via backtrace):\n");
    void* frames[48];
    int n = backtrace(frames, (int)(sizeof(frames) / sizeof(frames[0])));
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    fflush(stderr);
    _exit(1);
}

static void install_posix_crash_handlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = posix_crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGTRAP, &sa, nullptr); // in-process hardware watchpoints (SWE1R_HWWATCH)
}

#if defined(__APPLE__) && defined(__aarch64__)
// Debug: arm an ARM64 hardware write watchpoint on every thread of this process.
// The first store to the watched N64 word raises SIGTRAP and the crash handler
// prints the writer's backtrace. No debugger attached, so timing is unchanged.
static void swe1r_arm_hw_watch(uint8_t* rdram, uint32_t n64_addr) {
    // Watch n64_addr, up to two more N64 words from SWE1R_HWWATCH2 (comma list),
    // and the KSEG1 alias of n64_addr (4 hardware watchpoints at most).
    std::vector<uint64_t> hosts;
    hosts.push_back((uint64_t)(rdram + (n64_addr & 0x7FFFFF)));
    if (const char* more = std::getenv("SWE1R_HWWATCH2")) {
        for (const char* p = more; p && *p; ) { hosts.push_back((uint64_t)(rdram + (strtoul(p, nullptr, 16) & 0x7FFFFF))); p = strchr(p, ','); if (p) p++; }
    }
    hosts.push_back((uint64_t)(rdram + 0x20000000u + (n64_addr & 0x7FFFFF)));
    if (hosts.size() > 4) hosts.resize(4);
    thread_act_array_t threads; mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return;
    int ok = 0;
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        arm_debug_state64_t st{};
        mach_msg_type_number_t sc = ARM_DEBUG_STATE64_COUNT;
        thread_get_state(threads[i], ARM_DEBUG_STATE64, (thread_state_t)&st, &sc);
        for (size_t k = 0; k < hosts.size(); k++) {
            uint32_t bas = 0xFu << (hosts[k] & 4);
            st.__wvr[k] = hosts[k] & ~7ull;
            st.__wcr[k] = (bas << 5) | (2u << 3) | (2u << 1) | 1u; // BAS, store, EL0, enable
        }
        if (thread_set_state(threads[i], ARM_DEBUG_STATE64, (thread_state_t)&st, ARM_DEBUG_STATE64_COUNT) == KERN_SUCCESS) ok++;
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(thread_act_t));
    printf("[hwwatch] armed %zu watchpoints (first 0x%08X) in %d/%u threads\n", hosts.size(), n64_addr, ok, count);
}
#endif

#endif // _WIN32

int main(int argc, char* argv[]) {
    // Force unbuffered output so we see prints before crashes
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
#if defined(_WIN32)
    load_map_symbols();
    SetUnhandledExceptionFilter(crash_handler);
#else
    install_posix_crash_handlers();
#endif

    fprintf(stderr, "[DEBUG] main() entered\n");

    printf("===========================================\n");
    printf("  Star Wars Episode I: Racer\n");
    printf("  N64 Static Recompilation v%d.%d.%d\n", 0, 1, 0);
    printf("  \"Now THIS is podracing!\"\n");
    printf("===========================================\n\n");

    // Register game entry
    recomp::GameEntry game_entry{};
    game_entry.rom_hash = SWE1R_ROM_HASH;
    game_entry.internal_name = "STAR WARS EP1 RACER";
    game_entry.display_name = "Star Wars Episode I: Racer";
    game_entry.game_id = u8"swe1racer";
    game_entry.mod_game_id = "swe1racer";
    // 16 kbit EEPROM (2 KB): the game reads two 0x3F0-byte blocks at EEPROM
    // addresses 0 and 0x80. Eep4k (512 B) made librecomp's save_read overflow
    // its buffer and corrupt the heap.
    game_entry.save_type = recomp::SaveType::Eep16k;
    game_entry.is_enabled = true;
    game_entry.entrypoint_address = (gpr)(int32_t)0x80000400; // Sign-extend for MEM macros
    game_entry.entrypoint = recomp_entrypoint;
    game_entry.on_init_callback = on_game_init;

    // Register overlay/section tables
    recomp::overlays::overlay_section_table_data_t sections_data{};
    sections_data.code_sections = section_table;
    sections_data.num_code_sections = num_sections;
    sections_data.total_num_sections = num_sections;

    recomp::overlays::overlays_by_index_t overlays_data{};
    overlays_data.table = nullptr;
    overlays_data.len = 0;

    recomp::overlays::register_overlays(sections_data, overlays_data);

    if (!recomp::register_game(game_entry)) {
        fprintf(stderr, "Failed to register game!\n");
        return 1;
    }

    printf("[SWE1R] Game registered: %zu sections, %zu functions in main section\n",
           num_sections, section_table[0].num_funcs);

    // Set up config path
    recomp::register_config_path(std::filesystem::current_path());

    // Graphics defaults (RT64 reads these through update_config / at setup):
    // render at an integer multiple of the window, keep the original 4:3,
    // 60 Hz, modest MSAA. There is no settings UI yet.
    {
        ultramodern::renderer::GraphicsConfig gfx{};
        gfx.developer_mode = false;
        gfx.res_option  = ultramodern::renderer::Resolution::Auto;
        gfx.wm_option   = ultramodern::renderer::WindowMode::Windowed;
        gfx.hr_option   = ultramodern::renderer::HUDRatioMode::Original;
        gfx.api_option  = ultramodern::renderer::GraphicsApi::Auto;
        gfx.ar_option   = ultramodern::renderer::AspectRatio::Original;
        gfx.msaa_option = ultramodern::renderer::Antialiasing::MSAA4X;
        gfx.rr_option   = ultramodern::renderer::RefreshRate::Original;
        gfx.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Auto;
        gfx.rr_manual_value = 60;
        gfx.ds_option = 1;
        // Developer overrides until there is a settings UI:
        //   SWE1R_RES=original|original2x|auto   SWE1R_MSAA=0|2|4|8   SWE1R_ASPECT=original|expand
        if (const char* r = std::getenv("SWE1R_RES")) {
            std::string v(r);
            gfx.res_option = (v == "original") ? ultramodern::renderer::Resolution::Original
                           : (v == "original2x") ? ultramodern::renderer::Resolution::Original2x
                           : ultramodern::renderer::Resolution::Auto;
        }
        if (const char* m = std::getenv("SWE1R_MSAA")) {
            int v = atoi(m);
            gfx.msaa_option = (v >= 8) ? ultramodern::renderer::Antialiasing::MSAA8X
                            : (v >= 4) ? ultramodern::renderer::Antialiasing::MSAA4X
                            : (v >= 2) ? ultramodern::renderer::Antialiasing::MSAA2X
                            : ultramodern::renderer::Antialiasing::None;
        }
        if (const char* a = std::getenv("SWE1R_ASPECT")) {
            gfx.ar_option = (std::string(a) == "expand") ? ultramodern::renderer::AspectRatio::Expand
                                                          : ultramodern::renderer::AspectRatio::Original;
        }
        printf("[gfx] res=%d msaa=%d aspect=%d\n", (int)gfx.res_option, (int)gfx.msaa_option, (int)gfx.ar_option);
        ultramodern::renderer::set_graphics_config(gfx);
    }

    // Build the configuration
    recomp::Configuration cfg{};
    cfg.argc = argc;
    cfg.argv = argv;
    cfg.project_version = { 0, 1, 0, "-alpha" };
    // SP completions go to a sink queue (see swe1r_osSetEventMesg_recomp); never
    // requeue them when it is full, just drop them.
    cfg.message_queue_control.requeue_sp = false;

    // RSP callbacks
    recomp::rsp::callbacks_t rsp_callbacks{};
    rsp_callbacks.get_rsp_microcode = get_rsp_microcode;
    cfg.rsp_callbacks = rsp_callbacks;

    // Renderer callbacks
    ultramodern::renderer::callbacks_t renderer_callbacks{};
    renderer_callbacks.create_render_context = create_render_context_callback;
    cfg.renderer_callbacks = renderer_callbacks;

    // Audio callbacks
    ultramodern::audio_callbacks_t audio_callbacks{};
    audio_callbacks.queue_samples = swe1r_queue_samples;
    audio_callbacks.get_frames_remaining = swe1r_get_frames_remaining;
    audio_callbacks.set_frequency = swe1r_set_frequency;
    cfg.audio_callbacks = audio_callbacks;

    // Input callbacks
    ultramodern::input::callbacks_t input_callbacks{};
    input_callbacks.poll_input = poll_input;
    input_callbacks.get_input = get_input;
    input_callbacks.set_rumble = set_rumble;
    input_callbacks.get_connected_device_info = get_connected_device_info;
    cfg.input_callbacks = input_callbacks;

    // GFX callbacks (window management)
    ultramodern::gfx_callbacks_t gfx_callbacks{};
    gfx_callbacks.create_gfx = create_gfx;
    gfx_callbacks.create_window = create_window;
    gfx_callbacks.update_gfx = update_gfx;
    cfg.gfx_callbacks = gfx_callbacks;

    // Events callbacks
    ultramodern::events::callbacks_t events_callbacks{};
    events_callbacks.vi_callback = vi_callback;
    events_callbacks.gfx_init_callback = gfx_init_callback;
    cfg.events_callbacks = events_callbacks;

    // Error handling
    ultramodern::error_handling::callbacks_t error_callbacks{};
    error_callbacks.message_box = error_message_box;
    cfg.error_handling_callbacks = error_callbacks;

    // Threads
    ultramodern::threads::callbacks_t threads_callbacks{};
    threads_callbacks.get_game_thread_name = get_game_thread_name;
    cfg.threads_callbacks = threads_callbacks;

    printf("[SWE1R] Starting runtime...\n");
    printf("  \"It's working! IT'S WORKING!\" - Anakin Skywalker\n\n");

    // Game is started from gfx_init_callback after renderer is ready.
    // This blocks until the game exits.
    recomp::start(cfg);

    // Cleanup
    if (g_window) {
        SDL_DestroyWindow(g_window);
    }
    SDL_Quit();

    printf("\n[SWE1R] Shutdown complete.\n");
    return 0;
}
