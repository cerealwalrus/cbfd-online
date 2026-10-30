// SDL2 sound output for the RT64 build.
//
// The game's audio thread hands over each buffer the audio microcode mixed
// (osAiSetNextBuffer) and paces itself by how much is still queued
// (osAiGetLength), so the SDL device is used in queue mode: the reported
// remaining frames are what SDL hasn't played yet.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

#include <SDL.h>

#include "recompui/config.h"

#include "conker.hpp"

namespace {
    constexpr int channels = 2;

    std::mutex audio_mutex;
    SDL_AudioDeviceID device = 0;
    std::vector<float> convert_buffer;

    // (possess159) Online: how near the sound card's queue came to running dry as each of the game's sound buffers came (silence
    // played while it's dry: a crackle), in the session's log. A buffer comes about every 33 ms; the queue holds about 40 ms then.
    struct QueueWatch {
        uint32_t frequency = 0;
        uint32_t buffers = 0, dry = 0, low = 0, fewest = 0xFFFFFFFFu, dry_told = 0;
        double longest_gap_ms = 0.0;
        std::chrono::steady_clock::time_point last{};
    };
    QueueWatch watch;

    void watch_queue(uint32_t queued) {
        const auto now = std::chrono::steady_clock::now();
        const bool had_last = watch.last.time_since_epoch().count() != 0;
        const double gap = had_last ? std::chrono::duration<double, std::milli>(now - watch.last).count() : 0.0;
        watch.last = now;
        if (!conker::netsession::active() || watch.frequency == 0) {
            return;
        }
        watch.buffers++;
        watch.longest_gap_ms = std::max(watch.longest_gap_ms, gap);
        watch.fewest = std::min(watch.fewest, queued);
        if (queued == 0 && had_last) {
            watch.dry++;
            if (watch.dry_told < 30) {
                watch.dry_told++;
                char line[200];
                std::snprintf(line, sizeof(line), "[audio] f=%u the sound card's queue had run dry when the game's next sound came, %.0f ms after the one "
                    "before: silence played meanwhile", (unsigned)conker::netsession::frame(), gap);
                conker::netsession::log(line);
            }
        } else if (queued < watch.frequency / 80u) {
            watch.low++;
        }
        if (watch.buffers >= 1800) {
            char line[300];
            std::snprintf(line, sizeof(line), "[audio] f=%u the last %u of the game's sound buffers: the sound card's queue had run dry before %u of them, "
                "was under 12 ms before %u (the least %u frames, %.1f ms); the longest between two %.0f ms", (unsigned)conker::netsession::frame(),
                (unsigned)watch.buffers, (unsigned)watch.dry, (unsigned)watch.low, (unsigned)watch.fewest,
                watch.fewest * 1000.0 / (double)watch.frequency, watch.longest_gap_ms);
            conker::netsession::log(line);
            watch.buffers = watch.dry = watch.low = 0;
            watch.fewest = 0xFFFFFFFFu;
            watch.longest_gap_ms = 0.0;
        }
    }

    void open_device(uint32_t frequency) {
        if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            std::fprintf(stderr, "[audio] SDL audio init failed: %s\n", SDL_GetError());
            return;
        }
        if (device != 0) {
            SDL_CloseAudioDevice(device);
            device = 0;
        }
        SDL_AudioSpec want{};
        want.freq = (int)frequency;
        want.format = AUDIO_F32SYS;
        want.channels = channels;
        want.samples = 256; // ~12 ms at 22 kHz: SDL pulls from the queue in chunks this size
        // No allowed changes: SDL converts to whatever the output device uses, so the
        // queue stays in the game's sample rate.
        device = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
        if (device == 0) {
            std::fprintf(stderr, "[audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
            return;
        }
        SDL_PauseAudioDevice(device, 0);
    }
}

void conker::audio::set_frequency(uint32_t frequency) {
    std::lock_guard lock{audio_mutex};
    open_device(frequency);
    watch.frequency = frequency;
    watch.last = {};
}

void conker::audio::queue_samples(int16_t* samples, size_t sample_count) {
    std::lock_guard lock{audio_mutex};
    if (device == 0) {
        return;
    }
    // RDRAM is kept as native-endian 32-bit words, so each stereo frame's two 16-bit
    // samples read back swapped: [i + 1] is the left channel and [i] the right.
    // Scaled by the main volume from the Sound settings (0-100).
    const float scale = static_cast<float>(recompui::config::sound::get_main_volume()) / (100.0f * 32768.0f);
    watch_queue(SDL_GetQueuedAudioSize(device) / (channels * sizeof(float)));   // (possess159)
    convert_buffer.resize(sample_count);
    for (size_t i = 0; i + 1 < sample_count; i += channels) {
        convert_buffer[i + 0] = samples[i + 1] * scale;
        convert_buffer[i + 1] = samples[i + 0] * scale;
    }
    SDL_QueueAudio(device, convert_buffer.data(), (Uint32)(sample_count * sizeof(float)));
}

size_t conker::audio::get_frames_remaining() {
    std::lock_guard lock{audio_mutex};
    if (device == 0) {
        return 0;
    }
    // On hardware AI_LEN only counts what is left of the buffer playing now, not the
    // one queued behind it. Conker's audio thread (func_100095A0) makes its next
    // buffer short (552 frames instead of 736) whenever 249 or more samples are
    // left, so reporting the whole SDL queue makes it underrun and pop. Leave one
    // buffer out, so the queue settles at about a buffer and a half.
    constexpr uint32_t one_buffer = 736;
    uint32_t queued = SDL_GetQueuedAudioSize(device) / (channels * sizeof(float));
    return queued > one_buffer ? queued - one_buffer : 0;
}
