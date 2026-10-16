// Audio output for Star Wars Episode I: Racer.
//
// The game's audio manager (~0x80007300) mixes with the recompiled aspMain
// microcode and hands each buffer to osAiSetNextBuffer, which ultramodern
// forwards here as interleaved stereo int16. We play it through an SDL queued
// audio device at the game's AI frequency (22050 Hz).
//
// Pacing: the game's audio scheduler (0x8002E134) runs once per retrace and
// starts a new audio frame (~1088-1104 frames, 20 per second) when its 64-bit
// clock passes a deadline AND osAiGetStatus says the AI FIFO is not full;
// when full it retries ~24 ms later. On hardware that FIFO-full bit is what
// locks production to the DAC rate, so swe1r_osAiGetStatus_recomp reports
// "full" while more than ~1.5 game buffers are still queued here. The frame
// size itself comes from osAiGetLength (get_frames_remaining), clamped by the
// game to a minimum of 1088 frames.
//
// Env vars:
//   SWE1R_MUTE=1       open no device; a wall-clock drain stands in for it so
//                      the game's pacing stays the same.
//   SWE1R_AUDIO_LOG=1  once per second: frequency, frames received, queue
//                      depth, peak |sample|, microcode task counts.

#include <SDL.h>

#include "recomp.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

uint32_t swe1r_audio_task_count();        // src/main.cpp
uint32_t swe1r_audio_resume_skip_count(); // src/main.cpp

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kChannels = 2;
constexpr int kBytesPerFrame = kChannels * sizeof(int16_t);
constexpr Uint16 kDeviceBufferFrames = 512;

std::mutex g_mutex;                 // guards everything below
SDL_AudioDeviceID g_device = 0;
uint32_t g_frequency = 0;
std::vector<int16_t> g_swap;

// Virtual queue, used when muted or when no device could be opened.
double g_virtual_frames = 0.0;
Clock::time_point g_virtual_last{};

// Log state.
Clock::time_point g_log_last{};
uint64_t g_log_frames = 0;
uint32_t g_log_buffers = 0;
int g_log_peak = 0;
uint32_t g_log_clears = 0;
uint32_t g_log_min_q = UINT32_MAX, g_log_max_q = 0;
uint32_t g_log_full = 0, g_log_status = 0, g_log_underruns = 0;

uint32_t g_last_buffer_frames = 0;
uint64_t g_total_buffers = 0;

bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

bool muted() {
    static const bool m = env_flag("SWE1R_MUTE");
    return m;
}

bool logging() {
    static const bool l = env_flag("SWE1R_AUDIO_LOG");
    return l;
}

// Frames still queued. Caller holds g_mutex.
uint32_t queued_frames_locked() {
    if (g_device != 0) {
        return SDL_GetQueuedAudioSize(g_device) / kBytesPerFrame;
    }
    Clock::time_point now = Clock::now();
    if (g_virtual_last != Clock::time_point{}) {
        double elapsed = std::chrono::duration<double>(now - g_virtual_last).count();
        g_virtual_frames = std::max(0.0, g_virtual_frames - elapsed * g_frequency);
    }
    g_virtual_last = now;
    return static_cast<uint32_t>(g_virtual_frames);
}

void close_device_locked() {
    if (g_device != 0) {
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }
}

void open_device_locked(uint32_t freq) {
    close_device_locked();
    g_virtual_frames = 0.0;
    g_virtual_last = Clock::time_point{};
    if (muted()) return;

    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[audio] SDL audio init failed: %s (continuing silent)\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec want{};
    want.freq = static_cast<int>(freq);
    want.format = AUDIO_S16SYS;
    want.channels = kChannels;
    want.samples = kDeviceBufferFrames;
    want.callback = nullptr; // queued audio
    SDL_AudioSpec have{};
    // No allowed changes: SDL converts to whatever the hardware runs at.
    g_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_device == 0) {
        fprintf(stderr, "[audio] SDL_OpenAudioDevice(%u Hz) failed: %s (continuing silent)\n", freq, SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(g_device, 0);
    printf("[audio] device %u open: %d Hz, %d ch, %u-frame buffer (driver %s)\n",
           (unsigned)g_device, have.freq, have.channels, (unsigned)have.samples,
           SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
}

void maybe_log_locked(uint32_t queued) {
    if (!logging()) return;
    g_log_min_q = std::min(g_log_min_q, queued);
    g_log_max_q = std::max(g_log_max_q, queued);
    Clock::time_point now = Clock::now();
    if (g_log_last == Clock::time_point{}) {
        g_log_last = now;
        return;
    }
    double elapsed = std::chrono::duration<double>(now - g_log_last).count();
    if (elapsed < 1.0) return;
    printf("[audio] freq=%u Hz recv=%.0f frames/s (%u bufs) queued=%u [min %u max %u] peak=%d fifo_full=%u/%u underruns=%u tasks=%u resumes_skipped=%u%s%s\n",
           g_frequency, g_log_frames / elapsed, g_log_buffers, queued,
           g_log_min_q == UINT32_MAX ? 0 : g_log_min_q, g_log_max_q, g_log_peak,
           g_log_full, g_log_status, g_log_underruns,
           swe1r_audio_task_count(), swe1r_audio_resume_skip_count(),
           g_log_clears ? " QUEUE-CLEARED" : "", g_device == 0 ? " (no device)" : "");
    fflush(stdout);
    g_log_last = now;
    g_log_frames = 0;
    g_log_buffers = 0;
    g_log_peak = 0;
    g_log_clears = 0;
    g_log_min_q = UINT32_MAX;
    g_log_max_q = 0;
    g_log_full = 0;
    g_log_status = 0;
    g_log_underruns = 0;
}

} // namespace

// sample_count is the number of int16 values (2 per stereo frame).
void swe1r_queue_samples(int16_t* samples, size_t sample_count) {
    size_t frames = sample_count / kChannels;
    if (frames == 0) return;

    std::lock_guard lock{ g_mutex };
    if (g_frequency == 0) return;

    // RDRAM is word-swapped, so each stereo pair arrives right/left.
    g_swap.resize(frames * kChannels);
    int peak = g_log_peak;
    for (size_t i = 0; i < frames * kChannels; i += 2) {
        int16_t l = samples[i + 1], r = samples[i];
        g_swap[i] = l;
        g_swap[i + 1] = r;
        peak = std::max({ peak, std::abs((int)l), std::abs((int)r) });
    }
    g_log_peak = peak;
    g_log_frames += frames;
    g_log_buffers++;

    uint32_t queued = queued_frames_locked();
    if (queued == 0 && g_total_buffers > 4) g_log_underruns++;
    g_total_buffers++;
    g_last_buffer_frames = static_cast<uint32_t>(frames);
    // Safety valve: never let latency run away (half a second).
    if (queued > g_frequency / 2) {
        if (g_device != 0) SDL_ClearQueuedAudio(g_device);
        g_virtual_frames = 0.0;
        g_log_clears++;
        queued = 0;
    }
    if (g_device != 0) {
        if (SDL_QueueAudio(g_device, g_swap.data(), static_cast<Uint32>(frames * kBytesPerFrame)) != 0) {
            static uint32_t errors = 0;
            if (++errors <= 5) fprintf(stderr, "[audio] SDL_QueueAudio failed: %s\n", SDL_GetError());
        }
    } else {
        g_virtual_frames += static_cast<double>(frames);
    }
    maybe_log_locked(queued + static_cast<uint32_t>(frames));
}

// osAiGetStatus (0x8008ADA0, a raw AI_STATUS read in the ROM; redirected here
// by tools/post_recomp_patches.py). Bit 31 = FIFO full, bit 30 = busy.
extern "C" void swe1r_osAiGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ g_mutex };
    uint32_t status = 0;
    if (g_frequency != 0) {
        uint32_t buffer = g_last_buffer_frames ? g_last_buffer_frames : g_frequency / 20;
        uint32_t queued = queued_frames_locked();
        if (queued > 0) status |= 0x40000000u;
        if (queued > buffer + buffer / 2) {
            status |= 0x80000000u;
            g_log_full++;
        }
    }
    g_log_status++;
    ctx->r2 = (int32_t)status;
}

size_t swe1r_get_frames_remaining() {
    std::lock_guard lock{ g_mutex };
    if (g_frequency == 0) return 0;
    return queued_frames_locked();
}

void swe1r_set_frequency(uint32_t freq) {
    std::lock_guard lock{ g_mutex };
    if (freq == 0 || freq > 200000) {
        fprintf(stderr, "[audio] ignoring bogus frequency %u Hz\n", freq);
        return;
    }
    if (freq == g_frequency && (g_device != 0 || muted())) return;
    printf("[audio] frequency %u Hz%s\n", freq, muted() ? " (muted: SWE1R_MUTE)" : "");
    g_frequency = freq;
    open_device_locked(freq);
}
