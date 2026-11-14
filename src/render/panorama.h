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
#pragma once

#include "core/types.h"

#include <array>
#include <span>
#include <vector>

namespace sm2::render {

/// Widescreen fill for the 2D layers behind the 3D.
///
/// Games rewrite the map as they scroll, so the side columns cannot be read from
/// it. The compose shader caches on-screen layer pixels by world column and the
/// side columns read back what has been shown. This class tracks each layer's
/// scroll to give that world column, and starts a new epoch (emptying the
/// cache) when the layer's content changes.
class PanoramaTracker {
public:
    /// World columns kept per layer; a power of two.
    static constexpr u32 kWidth  = 1024;
    static constexpr u32 kHeight = 512;

    struct State {
        /// Bit per layer: the cache applies (plain scrolling, no row scroll).
        u32                cacheable = 0;
        /// Bit per map row, per layer: the row has scrolled far without a cell
        /// changing, so it is a fixed loop the side columns can read directly.
        std::array<u64, 4> wrap_rows{};
        /// World column and row of the screen's top-left corner, per layer,
        /// modulo 65536.
        std::array<u32, 4> offset{};
        std::array<u32, 4> offset_y{};
        /// Cache entries tagged with another epoch are empty. Never zero.
        std::array<u32, 4> epoch{1, 1, 1, 1};
    };

    /// Advance by one frame of tile RAM.
    [[nodiscard]] const State& update(std::span<const u8> tile_ram);

    void reset();

private:
    struct Layer {
        bool             seen      = false;
        bool             cacheable = false;
        u32              h         = 0;
        u32              v         = 0;
        s32              world     = 0;
        s32              world_y   = 0;
        /// Scroll travelled since a cell of each map row last changed.
        std::array<u32, 64> travel{};
        std::vector<u16> cells;
    };

    std::array<Layer, 4> m_layers{};
    State                m_state;
};

}  // namespace sm2::render
