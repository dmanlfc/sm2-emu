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
// What the rest of the program needs from a sound board, whichever one a set has.
//
// The CRX family carries a 68000 with an SCSP; the original board carries the
// Model 1 audio board (a 68000, a YM3438 and two MultiPCMs). They share nothing
// but the shape of the connection: serial bytes in, interleaved stereo out.

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

    /// Output rate in Hz: 44100 for the SCSP, 44643 for the Model 1 board.
    [[nodiscard]] virtual u32 sample_rate() const = 0;

    /// False when the set shipped no sound program, which keeps a synthetic
    /// machine in the tests usable.
    [[nodiscard]] virtual bool present() const = 0;

    /// Voices currently sounding; only a rough indication, as boards count
    /// different things.
    [[nodiscard]] virtual u32 active_voices() const = 0;
};

}  // namespace sm2::hw
