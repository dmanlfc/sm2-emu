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
// What the rest of the program needs from a sound board, whichever one a set has.
//
// Two entirely different boards turn up on Model 2 hardware: the CRX family
// carries a 68000 with an SCSP, and the original board carries the Model 1 audio
// board -- a 68000 with a YM3438 and two MultiPCMs. They share no registers, no
// clock and no sample rate. What they do share is the shape of their connection
// to everything else: the host hands them serial bytes, they hand back interleaved
// stereo samples, and something has to know how fast those samples are.
//
// Keeping that in one interface is what stops main.cpp from growing a second copy
// of the audio path per board.

#pragma once

#include "core/types.h"

#include <span>

namespace sm2::hw {

class SoundBoard {
public:
    virtual ~SoundBoard() = default;

    /// Samples generated since the last clear, interleaved stereo.
    [[nodiscard]] virtual std::span<const s16> pending_samples() const = 0;

    virtual void clear_pending_samples() = 0;

    /// Output rate in Hz. The two boards do not agree on one: the SCSP runs at
    /// 44100 and the Model 1 board's MultiPCMs at 44643, so the audio device is
    /// opened at whatever the board reports rather than at a constant.
    [[nodiscard]] virtual u32 sample_rate() const = 0;

    /// False when the set shipped no sound program, which keeps a synthetic
    /// machine in the tests usable.
    [[nodiscard]] virtual bool present() const = 0;

    /// Voices currently sounding, for the status line and the headless report.
    /// Boards count different things -- SCSP slots, MultiPCM channels -- so this
    /// is only ever a rough indication that audio is happening.
    [[nodiscard]] virtual u32 active_voices() const = 0;
};

}  // namespace sm2::hw
