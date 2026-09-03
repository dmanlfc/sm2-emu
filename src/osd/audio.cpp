//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// sm2-emu — A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
//
// See audio.h.

#include "osd/audio.h"

#include "core/log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <vector>

namespace sm2::osd {
namespace {

constexpr u32 kChannels = 2;

}  // namespace

Audio::~Audio()
{
    shutdown();
}

bool Audio::init(u32 sample_rate)
{
    if (m_stream != nullptr) {
        return true;
    }
    if (sample_rate == 0) {
        SM2_WARN("audio: a sample rate of zero was requested");
        return false;
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SM2_WARN("audio: SDL_InitSubSystem failed: %s", SDL_GetError());
        return false;
    }

    SDL_AudioSpec spec{};
    spec.format   = SDL_AUDIO_S16;
    spec.channels = static_cast<int>(kChannels);
    spec.freq     = static_cast<int>(sample_rate);

    // Binding to the default playback device rather than naming one: SDL follows
    // the system default afterwards, so plugging in headphones does the expected
    // thing without the emulator being restarted.
    m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                        nullptr, nullptr);
    if (m_stream == nullptr) {
        SM2_WARN("audio: no output device (%s); running silent", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    m_sample_rate = sample_rate;
    SDL_ResumeAudioStreamDevice(m_stream);

    // Prime the queue with silence so it starts near the target depth instead of
    // climbing from empty: the first real frames then sit on a cushion and a slow
    // opening frame cannot underrun the device before the queue has filled.
    const usize prime_frames =
        static_cast<usize>(kTargetQueuedMilliseconds) * sample_rate / 1000;
    const std::vector<s16> silence(prime_frames * kChannels, 0);
    SDL_PutAudioStreamData(m_stream, silence.data(),
                           static_cast<int>(silence.size() * sizeof(s16)));

    const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(m_stream);
    SDL_AudioSpec device_spec{};
    int frames = 0;
    if (SDL_GetAudioDeviceFormat(device, &device_spec, &frames)) {
        SM2_INFO("audio: %u Hz stereo into a %d Hz %d-channel device, %d-frame buffer",
                 sample_rate, device_spec.freq, device_spec.channels, frames);
    } else {
        SM2_INFO("audio: %u Hz stereo", sample_rate);
    }
    return true;
}

void Audio::shutdown()
{
    if (m_stream == nullptr) {
        return;
    }
    SDL_DestroyAudioStream(m_stream);
    m_stream = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void Audio::submit(std::span<const s16> samples)
{
    if (m_stream == nullptr || samples.empty()) {
        return;
    }

    if (queued_milliseconds() > kMaxQueuedMilliseconds) {
        // Producer outrunning the device (--no-throttle / fast-forward). Draining
        // reads from the front, so this drops the oldest samples down to the
        // target -- unlike SDL_ClearAudioStream, which would gap the whole buffer.
        const u64 keep_bytes =
            static_cast<u64>(kTargetQueuedMilliseconds) * m_sample_rate
            * sizeof(s16) * kChannels / 1000;
        int queued = SDL_GetAudioStreamQueued(m_stream);
        while (queued > 0 && static_cast<u64>(queued) > keep_bytes) {
            u8  scratch[4096];
            const int want = static_cast<int>(
                std::min<u64>(sizeof(scratch), static_cast<u64>(queued) - keep_bytes));
            const int got = SDL_GetAudioStreamData(m_stream, scratch, want);
            if (got <= 0) {
                break;
            }
            queued -= got;
        }
        if (!m_warned_overflow) {
            m_warned_overflow = true;
            SM2_DEBUG("audio: the queue overflowed and was trimmed; expected when "
                      "running faster than real time");
        }
    }

    if (!SDL_PutAudioStreamData(m_stream, samples.data(),
                                static_cast<int>(samples.size_bytes()))) {
        SM2_WARN("audio: SDL_PutAudioStreamData failed: %s", SDL_GetError());
    }
}

void Audio::set_paused(bool paused)
{
    if (m_stream == nullptr) {
        return;
    }
    if (paused) {
        SDL_ClearAudioStream(m_stream);
        SDL_PauseAudioStreamDevice(m_stream);
    } else {
        SDL_ResumeAudioStreamDevice(m_stream);
    }
}

u32 Audio::queued_milliseconds() const
{
    if (m_stream == nullptr || m_sample_rate == 0) {
        return 0;
    }
    const int bytes = SDL_GetAudioStreamQueued(m_stream);
    if (bytes <= 0) {
        return 0;
    }
    const u64 frames = static_cast<u64>(bytes) / (sizeof(s16) * kChannels);
    return static_cast<u32>(frames * 1000 / m_sample_rate);
}

}  // namespace sm2::osd
