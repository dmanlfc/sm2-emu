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
        // Running faster than real time. Throwing the queue away rather than
        // blocking keeps the emulator's pace independent of the audio device,
        // which is what --no-throttle is for; the audio is discontinuous, and
        // there is no sensible alternative when the producer is going five times
        // too fast.
        SDL_ClearAudioStream(m_stream);
        if (!m_warned_overflow) {
            m_warned_overflow = true;
            SM2_DEBUG("audio: the queue overflowed, so audio will break up; "
                      "expected when unthrottled");
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
