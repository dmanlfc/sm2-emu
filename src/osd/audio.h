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
// Audio output.
//
// The SCSP produces 44100 Hz stereo, and the machine hands over roughly 767
// frames of it per video frame. This pushes those into an SDL_AudioStream, which
// resamples to whatever the device wants and buffers on our behalf.
//
// SDL3's audio streams are pull-driven underneath but can be fed by pushing, with
// no callback and no locking, which is what this does: the emulation thread is the
// only thread involved. That is worth having because the alternative -- a callback
// on SDL's audio thread reading a ring buffer the emulator writes -- needs either
// a lock in the emulator's inner loop or a lock-free queue, and neither buys
// anything when the producer already runs in real time.

#pragma once

#include "core/types.h"

#include <span>

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
    /// Returns false if there is no usable device. That is not fatal: a machine
    /// with no audio device still has to run, so the caller is expected to carry
    /// on and let submit() do nothing.
    [[nodiscard]] bool init(u32 sample_rate);
    void shutdown();

    [[nodiscard]] bool active() const { return m_stream != nullptr; }

    /// Queue interleaved stereo samples for playback.
    ///
    /// Drops the oldest audio if the queue has grown past kMaxQueuedMilliseconds,
    /// which happens when the emulator runs unthrottled: a capture producing
    /// frames five times faster than real time would otherwise build an
    /// ever-growing delay and eventually exhaust memory.
    void submit(std::span<const s16> samples);

    /// Stop and start playback, for the pause key. Queued audio is discarded on
    /// pause so that resuming does not replay the last fraction of a second.
    void set_paused(bool paused);

    /// How much audio is waiting to be played, in milliseconds. Zero when there is
    /// no device.
    [[nodiscard]] u32 queued_milliseconds() const;

    /// Queue trimmed to the target once it passes the ceiling. The ceiling is
    /// several frames deep so a heavy graphics frame that briefly starves the
    /// once-per-frame submit does not underrun the device (an audible pop).
    static constexpr u32 kTargetQueuedMilliseconds = 80;
    static constexpr u32 kMaxQueuedMilliseconds    = 200;

private:
    SDL_AudioStream* m_stream = nullptr;
    u32              m_sample_rate = 0;
    bool             m_warned_overflow = false;
};

}  // namespace sm2::osd
