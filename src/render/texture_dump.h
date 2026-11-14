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

#pragma once

#include "core/types.h"
#include "render/texture_key.h"

#include <array>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sm2::hw {
class Model2MachineBase;
class SoftRenderer;
}  // namespace sm2::hw

namespace sm2::render {

/// The dump's index.html around its JSON description.
[[nodiscard]] std::string texture_index_html(const std::string& json);

/// Collects each distinct texture the 3D polygons sample and, on finish(),
/// writes one PNG per texture, upright and in its most visible colouring, plus
/// an index.html to browse them with a screenshot of where each first appeared.
class TextureDumper {
public:
    TextureDumper(std::string directory, std::string game);
    ~TextureDumper();

    TextureDumper(const TextureDumper&)            = delete;
    TextureDumper& operator=(const TextureDumper&) = delete;

    /// Scan the current frame's render list. Call once per emulated frame.
    void scan(hw::Model2MachineBase& machine);

    /// Write everything collected. `machine` may be null once the game is gone,
    /// which only leaves out the whole-sheet images.
    void finish(const hw::Model2MachineBase* machine);

private:
    struct Variant {
        u32                 colour_base = 0;
        u32                 luma_table  = 0;
        u8                  luma        = 0;
        double              area        = 0.0;
        std::array<u32, 16> palette{};
    };

    struct Outline {
        std::vector<float> points;
    };

    struct Texture {
        u32                   width       = 0;
        u32                   height      = 0;
        std::vector<u8>       texels;
        bool                  translucent = false;
        double                area        = 0.0;
        u32                   frames_seen = 0;
        u32                   last_frame  = 0;
        u32                   first_frame = 0;
        /// Area-weighted sum of the normalised screen-to-texel Jacobian.
        std::array<double, 4> orientation{};
        std::vector<Variant>  variants;
        std::vector<Outline>  outlines;
    };

    struct PendingPng {
        std::string     path;
        u32             width    = 0;
        u32             height   = 0;
        u32             channels = 0;
        std::vector<u8> pixels;
    };

    void queue_png(std::string path, u32 width, u32 height, u32 channels,
                   std::vector<u8> pixels);
    void write_loop();

    std::string   m_directory;
    std::string   m_game;
    u32           m_frame           = 0;
    u32           m_frames_captured = 0;
    TextureHasher m_hasher;
    std::unordered_map<u64, Texture>  m_textures;
    std::vector<u64>                  m_order;
    std::vector<u32>                  m_tone;
    std::unique_ptr<hw::SoftRenderer> m_renderer;

    std::mutex              m_queue_mutex;
    std::condition_variable m_queue_ready;
    std::deque<PendingPng>  m_queue;
    bool                    m_stopping = false;
    std::thread             m_writer;  // last: starts once everything above exists
};

}  // namespace sm2::render
