// ██╗  ██╗███████╗██╗███████╗██╗  ██╗ ██████╗
// ██║ ██╔╝██╔════╝██║██╔════╝██║  ██║██╔═══██╗
// █████╔╝ █████╗  ██║███████╗███████║██║   ██║
// ██╔═██╗ ██╔══╝  ██║╚════██║██╔══██║██║   ██║
// ██║  ██╗███████╗██║███████║██║  ██║╚██████╔╝
// ╚═╝  ╚═╝╚══════╝╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝
//            継 承  ( k e i s h ō )
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
//
// Audio output.
//
// Samples are pushed from the emulation thread into an SDL_AudioStream, which
// resamples and buffers, so no callback or locking is needed.

#pragma once

#include "core/types.h"

#include <span>
#include <vector>

struct SDL_AudioStream;

namespace sm2::osd {

class Audio {
public:
    Audio() = default;
    ~Audio();

    Audio(const Audio&)            = delete;
    Audio& operator=(const Audio&) = delete;

    /// Open the default output device for `sample_rate` Hz interleaved stereo
    /// 16-bit.
    ///
    /// Returns false if there is no usable device. Not fatal: the caller carries
    /// on and submit() does nothing.
    [[nodiscard]] bool init(u32 sample_rate);
    void shutdown();

    [[nodiscard]] bool active() const { return m_stream != nullptr; }

    /// Queue interleaved stereo samples for playback.
    ///
    /// Drops the oldest audio if the queue has grown past kMaxQueuedMilliseconds,
    /// as it does when the emulator runs unthrottled.
    void submit(std::span<const s16> samples);

    /// Stop and start playback, for the pause key. Queued audio is discarded on
    /// pause so that resuming does not replay the last fraction of a second.
    void set_paused(bool paused);

    /// How much audio is waiting to be played, in milliseconds. Zero when there is
    /// no device.
    [[nodiscard]] u32 queued_milliseconds() const;

    /// Queue trimmed to the target once it passes the ceiling. The ceiling is
    /// several frames deep so a slow frame does not underrun the device.
    static constexpr u32 kTargetQueuedMilliseconds = 80;
    static constexpr u32 kMaxQueuedMilliseconds    = 200;

private:
    /// Remove DC the way the cabinet's AC-coupled output stage does.
    void block_dc(std::span<const s16> samples);

    SDL_AudioStream* m_stream = nullptr;
    u32              m_sample_rate = 0;
    bool             m_warned_overflow = false;

    float            m_dc_pole = 0.0f;
    float            m_dc_in[2]  = {};  ///< Previous input, per channel.
    float            m_dc_out[2] = {};  ///< Previous output, per channel.
    std::vector<s16> m_filtered;
};

}  // namespace sm2::osd
