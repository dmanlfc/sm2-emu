//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1 & 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
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

/// Corner of the output DC blocker. Low enough to leave the bass alone, high
/// enough to settle within a few tenths of a second.
constexpr float kDcCornerHz = 5.0f;

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
    // Opened in the device's own format so SDL resamples, not the system mixer.
    m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr,
                                        nullptr, nullptr);
    if (m_stream == nullptr) {
        SM2_WARN("audio: no output device (%s); running silent", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    if (!SDL_SetAudioStreamFormat(m_stream, &spec, nullptr)) {
        SM2_WARN("audio: SDL_SetAudioStreamFormat failed (%s); running silent", SDL_GetError());
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    m_sample_rate = sample_rate;
    m_dc_pole = 1.0f - 2.0f * 3.14159265f * kDcCornerHz / static_cast<float>(sample_rate);
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

    block_dc(samples);
    if (!SDL_PutAudioStreamData(m_stream, m_filtered.data(),
                                static_cast<int>(m_filtered.size() * sizeof(s16)))) {
        SM2_WARN("audio: SDL_PutAudioStreamData failed: %s", SDL_GetError());
    }
}

void Audio::block_dc(std::span<const s16> samples)
{
    // Sound chips such as the SCSP can leave a DC offset in their digital
    // output (its effects DSP truncates towards negative); the cabinet never
    // passes it to the speakers. One-pole high-pass, per channel.
    m_filtered.resize(samples.size());
    for (usize i = 0; i < samples.size(); ++i) {
        const usize ch  = i % kChannels;
        const float in  = static_cast<float>(samples[i]);
        const float out = in - m_dc_in[ch] + m_dc_pole * m_dc_out[ch];
        m_dc_in[ch]  = in;
        m_dc_out[ch] = out;
        m_filtered[i] = static_cast<s16>(std::clamp(out, -32768.0f, 32767.0f));
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
