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
#include "render/panorama.h"

#include <algorithm>
#include <cstdlib>

namespace sm2::render {
namespace {

constexpr u32 kNameTableWords   = 0x1000;
constexpr u32 kHorizontalScroll = 0x5000;
constexpr u32 kVerticalScroll   = 0x5004;
constexpr u32 kCellsWide        = 64;

/// A scroll step this large is a cut, not a pan.
constexpr s32 kJump = 96;

/// Visible cells changing in place means new content rather than a scroll.
constexpr u32 kChangedPercent = 10;

/// Scrolling this far with no cell rewritten: the map is not being streamed.
constexpr u32 kWrapTravel = 128;

[[nodiscard]] u16 word(std::span<const u8> ram, u32 index)
{
    const usize at = static_cast<usize>(index) * 2;
    return at + 1 < ram.size() ? static_cast<u16>(ram[at] | (ram[at + 1] << 8)) : 0;
}

}  // namespace

void PanoramaTracker::reset()
{
    m_layers = {};
    m_state  = State{};
}

const PanoramaTracker::State& PanoramaTracker::update(std::span<const u8> tile_ram)
{
    m_state.cacheable = 0;
    m_state.wrap_rows = {};
    for (u32 index = 0; index < 4; ++index) {
        Layer&    layer = m_layers[index];
        const u16 hscr  = word(tile_ram, kHorizontalScroll + index);
        const u16 vscr  = word(tile_ram, kVerticalScroll + index);
        const u16 ctrl  = word(tile_ram, kVerticalScroll + (index & ~1u));

        // A horizon split picks its layer by map row, so the even layer of the
        // pair still scrolls as one picture; the other split modes do not.
        const u32  mode      = (ctrl >> 13) & 3;
        const bool split     = mode == 1 && (index & 1) == 0;
        const bool cacheable = (hscr & 0x8000) == 0 && (vscr & 0x8000) == 0
                            && (mode == 0 || split);
        const u32 h = (0u - hscr) & 0x1ff;
        const u32 v = vscr & 0x1ff;

        bool fresh = !layer.seen || !layer.cacheable || !cacheable;
        u32  step  = 0;
        if (!fresh) {
            const s32 delta   = static_cast<s32>(((h - layer.h + 256) & 511)) - 256;
            const s32 delta_y = static_cast<s32>(((v - layer.v + 256) & 511)) - 256;
            layer.world   += delta;
            layer.world_y += delta_y;
            step = static_cast<u32>(std::abs(delta));
            fresh = std::abs(delta) > kJump || std::abs(delta_y) > kJump;
        }

        // Count the cells on screen now that differ from last frame.
        const u32        tables = split ? 2 : 1;
        std::vector<u16> cells(kNameTableWords * tables);
        for (u32 table = 0; table < tables; ++table) {
            for (u32 cell = 0; cell < kNameTableWords; ++cell) {
                cells[table * kNameTableWords + cell] =
                    word(tile_ram, (index + table) * kNameTableWords + cell);
            }
        }
        if (!fresh && layer.cells.size() == cells.size()) {
            u32 changed = 0;
            u32 total   = 0;
            for (u32 y = 0; y < 384 / 8 + 1; ++y) {
                const u32 row = ((v >> 3) + y) % kCellsWide;
                for (u32 x = 0; x < 496 / 8 + 1; ++x) {
                    const u32 column = ((h >> 3) + x) % kCellsWide;
                    for (u32 table = 0; table < tables; ++table) {
                        const u32 cell = table * kNameTableWords + row * kCellsWide + column;
                        changed += cells[cell] != layer.cells[cell] ? 1 : 0;
                        ++total;
                    }
                }
            }
            fresh = changed * 100 > total * kChangedPercent;
        }
        for (u32 row = 0; row < kCellsWide; ++row) {
            bool same = layer.seen && layer.cells.size() == cells.size();
            for (u32 table = 0; same && table < tables; ++table) {
                const u32 first = table * kNameTableWords + row * kCellsWide;
                same = std::equal(cells.begin() + first, cells.begin() + first + kCellsWide,
                                  layer.cells.begin() + first);
            }
            layer.travel[row] = same ? std::min(layer.travel[row] + step, kWrapTravel) : 0;
        }

        if (fresh && cacheable) {
            m_state.epoch[index] = (m_state.epoch[index] % 0xffff) + 1;
        }
        layer.seen      = true;
        layer.cacheable = cacheable;
        layer.h         = h;
        layer.v         = v;
        layer.cells     = std::move(cells);

        if (cacheable) {
            m_state.cacheable |= 1u << index;
            for (u32 row = 0; row < kCellsWide; ++row) {
                if (layer.travel[row] >= kWrapTravel) {
                    m_state.wrap_rows[index] |= u64{1} << row;
                }
            }
        }
        m_state.offset[index]   = static_cast<u32>(layer.world) & 0xffff;
        m_state.offset_y[index] = static_cast<u32>(layer.world_y) & 0xffff;
    }
    return m_state;
}

}  // namespace sm2::render
