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
// The board-agnostic call surface every Model 2 variant presents to main.cpp,
// the debug dumps and the renderer. It leaves out the coprocessor and sound
// hardware, whose types differ per board; code needing those uses the concrete
// class. CPU state is reported through the type-erased CpuStatus (machine.h)
// so the MAME-derived cores need no common virtual base.
#pragma once

#include "core/log.h"
#include "core/types.h"
#include "hw/geometrizer.h"
#include "hw/machine.h"
#include "rom/game.h"
#include "rom/rom_set.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <span>
#include <string>
#include <vector>

namespace sm2::hw {

class Model2Video;
class CommBoard;
class M2Comm;

/// The call surface a Model 2 board variant presents to the rest of the
/// program.
class Model2MachineBase : public Machine {
public:
    [[nodiscard]] MachineFamily family() const final { return MachineFamily::Model2; }

    [[nodiscard]] Model2MachineBase*       as_model2() final { return this; }
    [[nodiscard]] const Model2MachineBase* as_model2() const final { return this; }

    /// This frame's screen-space polygons, in drawing order.
    [[nodiscard]] virtual const RenderList& render_list() const = 0;

    /// Wall-clock time the geometry engine took producing render_list(),
    /// measured separately from the main CPU's instruction time (see Geometrizer).
    [[nodiscard]] virtual u64 geometry_stage_nanoseconds() const = 0;

    /// Widen the 3D raster by `columns` on each side; see Geometrizer.
    virtual void set_widescreen(u32 columns) = 0;

    /// Whether the last frame moved HUD objects out to the widescreen edges
    /// (GameSpec::hud_spread_*), so its 2D HUD should follow.
    [[nodiscard]] virtual bool hud_spread() const { return false; }

    /// A tile layer's whole 360-degree backdrop, which the game streams into the
    /// 512-wide map a column at a time. The widescreen side columns draw from it.
    struct SkyPanorama {
        static constexpr u32 kCells = 256;  ///< 2048 pixels around
        static constexpr u32 kRows  = 64;

        u32 layer      = 0;
        /// Panorama pixel at screen x = 0.
        u32 origin     = 0;
        /// Tile map row that names row 0 lands on, and how many rows follow.
        u32 first_row  = 0;
        u32 rows       = 0;  ///< zero: no panorama this frame
        /// Bumped whenever `names` changes.
        u64 generation = 0;
        std::array<u16, kCells * kRows> names{};
    };
    [[nodiscard]] virtual const SkyPanorama* sky_panorama() const { return nullptr; }

    /// True when this frame's tile layers carry something drawn over them on the
    /// CPU (Model1's above-HUD objects), so the GPU tilemap path cannot stand in.
    [[nodiscard]] virtual bool needs_cpu_tilemap() const { return false; }

    /// Draw the scenery to the edge of the course, where a game supports it.
    virtual void set_draw_distance(bool /*extended*/) {}

    /// The music board's level in percent, for the sets that have one.
    virtual void set_music_volume(u32 /*percent*/) {}

    /// The colour ramp's black level (Model2Video::set_black_level).
    virtual void set_black_level(u32 /*bias*/) {}

    // -- video, for the renderer and the debug dumps ------------------------

    [[nodiscard]] virtual std::span<const u8>  tile_ram() const  = 0;
    [[nodiscard]] virtual std::span<const u8>  char_ram() const  = 0;
    [[nodiscard]] virtual std::span<const u16> palette_ram() const       = 0;
    [[nodiscard]] virtual std::span<const u16> colour_translate() const  = 0;
    [[nodiscard]] virtual std::span<const u8>  luma_ram() const          = 0;
    [[nodiscard]] virtual std::span<const u32> texture_ram(int sheet) const = 0;

    /// The display list buffer, which the coprocessor fills and the geometry
    /// engine reads.
    [[nodiscard]] virtual std::span<const u32> buffer_ram() const = 0;

    /// The host CPU's work RAM, for state comparison against MAME's own.
    [[nodiscard]] virtual std::span<const u8> work_ram() const = 0;

    /// Byte offset in the display list where the geometry engine starts each
    /// frame. Needed to walk the buffer the way the engine does.
    [[nodiscard]] virtual u32 geometry_read_start_address() const = 0;

    /// Render test mode, bit 0 of the render mode register at 0x10000000. It
    /// stops the DSP drawing and clearing and shows a framebuffer bank
    /// directly, which is how Last Bronx draws its title screen.
    [[nodiscard]] virtual bool render_test_mode() const = 0;

    /// The two framebuffer banks, xGGGGGRRRRRBBBBB at 512 pixels per line. Which
    /// one is shown alternates with the frame number.
    [[nodiscard]] virtual std::span<const u16> framebuffer(int bank) const = 0;

    /// Cleared by the renderer once it has taken a palette change into account.
    [[nodiscard]] virtual bool palette_dirty() const = 0;
    virtual void               clear_palette_dirty() = 0;

    /// Counters that advance whenever the 3D renderer's source data changes.
    [[nodiscard]] virtual u64 texture_generation() const = 0;
    [[nodiscard]] virtual u64 table_generation() const   = 0;

    /// Counters that advance on any write to tile RAM or character RAM, so a GPU
    /// tilemap pass can tell whether its copies need refreshing.
    [[nodiscard]] virtual u64 tile_generation() const = 0;
    [[nodiscard]] virtual u64 char_generation() const = 0;

    [[nodiscard]] virtual Model2Video&       video()       = 0;
    [[nodiscard]] virtual const Model2Video& video() const = 0;

    /// Wall-clock time of Model2Video::compose() in compose_video(), in
    /// nanoseconds, without needing Model2Video complete.
    [[nodiscard]] virtual u64 tilemap_compose_nanoseconds() const = 0;

    /// Produce this frame's 2D output, refreshing the palette first if the
    /// program changed it.
    virtual void compose_video() = 0;

    // -- diagnostics -----------------------------------------------------------

    /// The interrupt latch, for reporting which sources the program has armed.
    [[nodiscard]] virtual u32 intreq() const = 0;
    [[nodiscard]] virtual u32 intena() const = 0;

    virtual void log_unmapped_summary() const = 0;
    virtual void log_burst_summary() const    = 0;
};

}  // namespace sm2::hw
