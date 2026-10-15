// RT64 Render Context for Star Wars Episode I: Racer
// Bridges ultramodern's RendererContext with the RT64 renderer

// NOMINMAX must be set globally via CMake (target_compile_definitions)
// Include RT64 first to avoid Windows.h macro conflicts
#include "hle/rt64_application.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdlib>

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/game.hpp"

// Static buffers for RT64 Core (RSP memory, not used in HLE mode but required)
static uint8_t s_dmem[0x1000];
static uint8_t s_imem[0x1000];

// DPC registers (dummy storage - RT64 HLE doesn't use these directly)
static uint32_t s_dpc_start = 0;
static uint32_t s_dpc_end = 0;
static uint32_t s_dpc_current = 0;
static uint32_t s_dpc_status = 0;
static uint32_t s_dpc_clock = 0;
static uint32_t s_dpc_bufbusy = 0;
static uint32_t s_dpc_pipebusy = 0;
static uint32_t s_dpc_tmem = 0;
static uint32_t s_mi_intr = 0;

static void dummy_check_interrupts() {
    // No-op: ultramodern handles interrupts
}

// Map ultramodern's GraphicsConfig onto RT64's UserConfiguration (same policy
// as Zelda64Recomp: "Auto" resolution = integer scale of the window, etc.).
static RT64::UserConfiguration::AspectRatio to_rt64(ultramodern::renderer::AspectRatio v) {
    using U = ultramodern::renderer::AspectRatio; using R = RT64::UserConfiguration::AspectRatio;
    switch (v) { case U::Expand: return R::Expand; case U::Manual: return R::Manual; default: return R::Original; }
}
static RT64::UserConfiguration::Antialiasing to_rt64(ultramodern::renderer::Antialiasing v) {
    using U = ultramodern::renderer::Antialiasing; using R = RT64::UserConfiguration::Antialiasing;
    switch (v) { case U::MSAA2X: return R::MSAA2X; case U::MSAA4X: return R::MSAA4X; case U::MSAA8X: return R::MSAA8X; default: return R::None; }
}
static RT64::UserConfiguration::RefreshRate to_rt64(ultramodern::renderer::RefreshRate v) {
    using U = ultramodern::renderer::RefreshRate; using R = RT64::UserConfiguration::RefreshRate;
    switch (v) { case U::Display: return R::Display; case U::Manual: return R::Manual; default: return R::Original; }
}
static RT64::UserConfiguration::InternalColorFormat to_rt64(ultramodern::renderer::HighPrecisionFramebuffer v) {
    using U = ultramodern::renderer::HighPrecisionFramebuffer; using R = RT64::UserConfiguration::InternalColorFormat;
    switch (v) { case U::On: return R::High; case U::Off: return R::Standard; default: return R::Automatic; }
}

static void apply_user_config(RT64::Application* app, const ultramodern::renderer::GraphicsConfig& config) {
    using Res = ultramodern::renderer::Resolution;
    switch (config.res_option) {
        case Res::Original:
            app->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            app->userConfig.resolutionMultiplier = std::max(config.ds_option, 1);
            app->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
        case Res::Original2x:
            app->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            app->userConfig.resolutionMultiplier = 2.0 * std::max(config.ds_option, 1);
            app->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
        default:
            app->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
            app->userConfig.downsampleMultiplier = 1;
            break;
    }
    using Hud = ultramodern::renderer::HUDRatioMode;
    switch (config.hr_option) {
        case Hud::Clamp16x9:
            app->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
            app->userConfig.extAspectTarget = 16.0 / 9.0;
            break;
        case Hud::Full:
            app->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Expand;
            break;
        default:
            app->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
            break;
    }
    app->userConfig.aspectRatio = to_rt64(config.ar_option);
    app->userConfig.antialiasing = to_rt64(config.msaa_option);
    app->userConfig.refreshRate = to_rt64(config.rr_option);
    app->userConfig.refreshRateTarget = config.rr_manual_value;
    app->userConfig.internalColorFormat = to_rt64(config.hpfb_option);
    app->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
    app->userConfig.developerMode = config.developer_mode;
}

// Display-list watchdog: the gfx thread can disappear inside RT64's interpreter
// when a display list never ends. The VI thread checks this and dumps the list.
std::atomic<uint32_t> g_current_dl_addr{0};
std::atomic<int64_t>  g_current_dl_start_ns{0};
static uint8_t* g_watchdog_rdram = nullptr;

void swe1r_dl_watchdog_tick() {
    uint32_t addr = g_current_dl_addr.load(std::memory_order_relaxed);
    if (addr == 0 || g_watchdog_rdram == nullptr) return;
    int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
    int64_t start = g_current_dl_start_ns.load(std::memory_order_relaxed);
    static bool dumped = false;
    if (!dumped && now - start > 2'000'000'000LL) {
        dumped = true;
        fprintf(stderr, "[watchdog] display list at RDRAM 0x%08X running for >2 s; dumping 64 KB to dl_dump.bin\n", addr);
        FILE* f = fopen("dl_dump.bin", "wb");
        if (f) { fwrite(g_watchdog_rdram + addr, 1, 0x10000, f); fclose(f); }
    }
}

extern "C" void swe1r_signal_sp_done(); // main.cpp

class SWE1RRenderContext : public ultramodern::renderer::RendererContext {
public:
    SWE1RRenderContext(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode)
        : rdram_(rdram), window_handle_(window_handle), developer_mode_(developer_mode)
    {
        g_watchdog_rdram = rdram;
        setup_result = ultramodern::renderer::SetupResult::Success;
#if defined(_WIN32)
        chosen_api = ultramodern::renderer::GraphicsApi::D3D12;
#elif defined(__APPLE__)
        chosen_api = ultramodern::renderer::GraphicsApi::Metal;
#else
        chosen_api = ultramodern::renderer::GraphicsApi::Vulkan;
#endif

        // Get VI register pointers from ultramodern
        vi_regs_ = ultramodern::renderer::get_vi_regs();

        // Set up the RT64 Core structure
        RT64::Application::Core core{};
#if defined(_WIN32)
        core.window = window_handle.window;
#elif defined(__APPLE__)
        // plume::RenderWindow on Apple is { NSWindow*, CAMetalLayer* }.
        core.window.window = window_handle.window;
        core.window.view = window_handle.view;
#else
        core.window = window_handle;
#endif
        core.RDRAM = rdram;
        core.DMEM = s_dmem;
        core.IMEM = s_imem;

        // ROM header (first 0x40 bytes)
        auto rom = recomp::get_rom();
        if (rom.size() >= 0x40) {
            rom_header_.assign(rom.begin(), rom.begin() + 0x40);
            core.HEADER = rom_header_.data();
        } else {
            rom_header_.resize(0x40, 0);
            core.HEADER = rom_header_.data();
        }

        // MI/DPC registers (dummy)
        core.MI_INTR_REG = &s_mi_intr;
        core.DPC_START_REG = &s_dpc_start;
        core.DPC_END_REG = &s_dpc_end;
        core.DPC_CURRENT_REG = &s_dpc_current;
        core.DPC_STATUS_REG = &s_dpc_status;
        core.DPC_CLOCK_REG = &s_dpc_clock;
        core.DPC_BUFBUSY_REG = &s_dpc_bufbusy;
        core.DPC_PIPEBUSY_REG = &s_dpc_pipebusy;
        core.DPC_TMEM_REG = &s_dpc_tmem;

        // VI registers - point into ultramodern's ViRegs struct
        core.VI_STATUS_REG = &vi_regs_->VI_STATUS_REG;
        core.VI_ORIGIN_REG = &vi_regs_->VI_ORIGIN_REG;
        core.VI_WIDTH_REG = &vi_regs_->VI_WIDTH_REG;
        core.VI_INTR_REG = &vi_regs_->VI_INTR_REG;
        core.VI_V_CURRENT_LINE_REG = &vi_regs_->VI_V_CURRENT_LINE_REG;
        core.VI_TIMING_REG = &vi_regs_->VI_TIMING_REG;
        core.VI_V_SYNC_REG = &vi_regs_->VI_V_SYNC_REG;
        core.VI_H_SYNC_REG = &vi_regs_->VI_H_SYNC_REG;
        core.VI_LEAP_REG = &vi_regs_->VI_LEAP_REG;
        core.VI_H_START_REG = &vi_regs_->VI_H_START_REG;
        core.VI_V_START_REG = &vi_regs_->VI_V_START_REG;
        core.VI_V_BURST_REG = &vi_regs_->VI_V_BURST_REG;
        core.VI_X_SCALE_REG = &vi_regs_->VI_X_SCALE_REG;
        core.VI_Y_SCALE_REG = &vi_regs_->VI_Y_SCALE_REG;

        core.checkInterrupts = dummy_check_interrupts;

        // Configure RT64 application
        RT64::ApplicationConfiguration app_config;
        app_config.appId = "swe1racer";
        app_config.detectDataPath = true;
        // Graphics settings come from ultramodern's GraphicsConfig (set in
        // main.cpp), not from an rt64.json next to the binary.
        app_config.useConfigurationFile = false;

        // Create the RT64 Application
        printf("[SWE1R] Creating RT64 application...\n");
        app_ = std::make_unique<RT64::Application>(core, app_config);

        // Apply the user configuration before setup so the swap chain,
        // render targets and present mode are created with it.
        apply_user_config(app_.get(), ultramodern::renderer::get_graphics_config());
        app_->enhancementConfig.f3dex.forceBranch = true;
        app_->enhancementConfig.textureLOD.scale = true;
#if defined(_WIN32)
        app_->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
#elif defined(__APPLE__)
        app_->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Metal;
#else
        app_->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
#endif

        // Run setup (creates graphics device, swap chain, shader caches, etc.)
        // The thread id is only meaningful for the Win32 message filter.
        uint32_t setup_thread_id = 0;
#if defined(_WIN32)
        setup_thread_id = window_handle.thread_id;
#endif
        auto result = app_->setup(setup_thread_id);

        switch (result) {
        case RT64::Application::SetupResult::Success:
            printf("[SWE1R] RT64 initialized successfully\n");
            is_valid_ = true;
            break;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound:
            fprintf(stderr, "[SWE1R] RT64: Dynamic libraries not found\n");
            setup_result = ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
            break;
        case RT64::Application::SetupResult::InvalidGraphicsAPI:
            fprintf(stderr, "[SWE1R] RT64: Invalid graphics API\n");
            setup_result = ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
            break;
        case RT64::Application::SetupResult::GraphicsAPINotFound:
            fprintf(stderr, "[SWE1R] RT64: Graphics API not found\n");
            setup_result = ultramodern::renderer::SetupResult::GraphicsAPINotFound;
            break;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound:
            fprintf(stderr, "[SWE1R] RT64: Graphics device not found\n");
            setup_result = ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
            break;
        }
    }

    ~SWE1RRenderContext() override {
        if (app_) {
            printf("[SWE1R] Destroying RT64 application\n");
            app_->end();
            app_.reset();
        }
    }

    bool valid() override { return is_valid_; }

    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override {
        if (!app_ || old_config == new_config) {
            return false;
        }
        if (new_config.wm_option != old_config.wm_option) {
            app_->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
        }
        apply_user_config(app_.get(), new_config);
        app_->updateUserConfig(true);
        if (new_config.msaa_option != old_config.msaa_option) {
            app_->updateMultisampling();
        }
        return true;
    }

    void enable_instant_present() override {
        // RT64 doesn't have a separate instant present mode
    }

    void send_dl(const OSTask* task) override {
        if (!app_ || !is_valid_) return;

        dl_count_++;
        if (dl_count_ <= 5 || (dl_count_ % 60 == 0)) {
            printf("[SWE1R] send_dl #%u: data_ptr=0x%08X ucode=0x%08X ucode_data=0x%08X type=%u\n",
                   dl_count_, (uint32_t)task->t.data_ptr, (uint32_t)task->t.ucode,
                   (uint32_t)task->t.ucode_data, task->t.type);
        }

        // Tell RT64's HLE interpreter which microcode/GBI to use BEFORE parsing
        // the display list. Without this the interpreter has no GBI command map
        // and crashes in processDisplayLists. ucode/ucode_data/data_ptr are MIPS
        // virtual addresses; mask to the RDRAM physical offset RT64 indexes by.
        uint32_t ucode      = (uint32_t)task->t.ucode      & 0x3FFFFFF;
        uint32_t ucode_data = (uint32_t)task->t.ucode_data & 0x3FFFFFF;
        uint32_t dl_address = (uint32_t)task->t.data_ptr   & 0x3FFFFFF;
        auto t0 = std::chrono::steady_clock::now();
        g_current_dl_addr.store(dl_address, std::memory_order_relaxed);
        g_current_dl_start_ns.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
        // Diagnostic: detect the game rewriting the root list while RT64 parses it.
        uint64_t sum_before = dl_checksum(dl_address);
        app_->state->rsp->reset();
        app_->interpreter->loadUCodeGBI(ucode, ucode_data, true);
        app_->processDisplayLists(rdram_, dl_address, 0, true);
        g_current_dl_addr.store(0, std::memory_order_relaxed);
        if (dl_checksum(dl_address) != sum_before) {
            dl_modified_during_parse_++;
            if (dl_modified_during_parse_ <= 5) {
                printf("[perf] WARNING: display list 0x%08X changed while RT64 was parsing it (#%u)\n",
                       dl_address, dl_modified_during_parse_);
            }
        }
        // The RSP is done with the list: now tell the game.
        swe1r_signal_sp_done();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        dl_time_total_ms_ += ms;
        if (ms > dl_time_max_ms_) dl_time_max_ms_ = ms;
        if (ms > 100.0) {
            printf("[perf] slow DL #%u: %.0f ms (data=0x%08X)\n", dl_count_, ms, (uint32_t)task->t.data_ptr);
        }
    }

    void send_dummy_workload(uint32_t fb_address) override {
        // ultramodern only enqueues dummy workloads from the VI thread while
        // is_game_started() is false. This game calls recomp::start_game from
        // gfx_init_callback, i.e. immediately after the renderer comes up, so
        // at most a handful of these arrive and there is nothing to draw yet.
        // Ignoring them is safe; log once so it is visible in traces.
        static bool logged = false;
        if (!logged) {
            logged = true;
            printf("[SWE1R] send_dummy_workload fb=0x%08X (ignored)\n", fb_address);
        }
    }

    void update_screen() override {
        if (!app_ || !is_valid_) return;

        screen_count_++;
        auto t0 = std::chrono::steady_clock::now();
        if (screen_count_ <= 5 || (screen_count_ % 60 == 0)) {
            printf("[SWE1R] update_screen #%u (dl +%u, dl time %.0f ms total, max %.0f ms; present max %.0f ms)\n",
                   screen_count_, dl_count_ - dl_count_at_last_report_, dl_time_total_ms_, dl_time_max_ms_, present_max_ms_);
            dl_count_at_last_report_ = dl_count_;
            dl_time_total_ms_ = 0.0; dl_time_max_ms_ = 0.0; present_max_ms_ = 0.0;
        }
        // Trace the VI register set RT64 presents from, whenever it changes.
        {
            const ultramodern::renderer::ViRegs& r = *vi_regs_;
            uint32_t sig = r.VI_STATUS_REG ^ (r.VI_WIDTH_REG * 31u) ^ (r.VI_H_START_REG * 131u) ^
                           (r.VI_V_START_REG * 1031u) ^ (r.VI_X_SCALE_REG * 7u) ^ (r.VI_Y_SCALE_REG * 13u);
            if (sig != last_vi_sig_) {
                last_vi_sig_ = sig;
                printf("[vi] regs: status=0x%08X origin=0x%08X width=%u h_start=0x%08X v_start=0x%08X "
                       "x_scale=0x%08X y_scale=0x%08X v_sync=0x%08X\n",
                       r.VI_STATUS_REG, r.VI_ORIGIN_REG, r.VI_WIDTH_REG, r.VI_H_START_REG,
                       r.VI_V_START_REG, r.VI_X_SCALE_REG, r.VI_Y_SCALE_REG, r.VI_V_SYNC_REG);
            }
        }

        app_->updateScreen();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > present_max_ms_) present_max_ms_ = ms;
        if (ms > 100.0) printf("[perf] slow update_screen #%u: %.0f ms\n", screen_count_, ms);
    }

    void shutdown() override {
        printf("[SWE1R] Render context shutdown\n");
        if (app_) {
            app_->end();
            app_.reset();
        }
        is_valid_ = false;

        // librecomp's start() joins the event threads (we are on one of them)
        // and then unmaps RDRAM, but it never stops the game's own OS threads,
        // which keep polling their message queues and crash on the unmapped
        // memory. Flush the save file and leave right here instead.
        ultramodern::join_saving_thread();
        printf("[SWE1R] Shutdown complete.\n");
        fflush(stdout);
        fflush(stderr);
        std::_Exit(0);
    }

    uint32_t get_display_framerate() const override {
        return 60; // TODO: query from RT64/display
    }

    float get_resolution_scale() const override {
        return 1.0f;
    }

private:
    uint64_t dl_checksum(uint32_t dl_offset) const {
        // FNV-1a over the first 16 KB of the root list (enough to catch rewrites).
        const uint32_t* p = (const uint32_t*)(rdram_ + dl_offset);
        uint64_t h = 1469598103934665603ULL;
        for (size_t i = 0; i < 0x4000 / 4; i++) { h ^= p[i]; h *= 1099511628211ULL; }
        return h;
    }
    uint32_t dl_modified_during_parse_ = 0;

    uint8_t* rdram_;
    ultramodern::renderer::WindowHandle window_handle_;
    bool developer_mode_;
    bool is_valid_ = false;
    ultramodern::renderer::ViRegs* vi_regs_ = nullptr;
    std::unique_ptr<RT64::Application> app_;
    std::vector<uint8_t> rom_header_;
    uint32_t dl_count_ = 0;
    uint32_t screen_count_ = 0;
    uint32_t last_vi_sig_ = 0xFFFFFFFFu;
    uint32_t dl_count_at_last_report_ = 0;
    double dl_time_total_ms_ = 0.0;
    double dl_time_max_ms_ = 0.0;
    double present_max_ms_ = 0.0;
};

// Factory function
std::unique_ptr<ultramodern::renderer::RendererContext> create_swe1r_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<SWE1RRenderContext>(rdram, window_handle, developer_mode);
}
