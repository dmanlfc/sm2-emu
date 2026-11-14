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
// Portions derived from Supermodel's Src/Graphics/New3D/R3DShaderTriangles.h,
// R3DShaderCommon.h and R3DFrameBuffers.cpp, Copyright (c) 2003-2026 The
// Supermodel Team, GPL-3.0-or-later.
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// A FramePlan drawn on the CPU, as the GPU passes draw it: the same passes,
// depth and stencil rules, shader maths and composite. Bands of rows run on a
// pool of threads; each walks the whole plan in order, so the result does not
// depend on the thread count.
#pragma once

#include "core/types.h"
#include "render/model3/line_of_sight.h"
#include "render/model3/model3_frame.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace sm2::render::model3 {

class SoftRenderer {
public:
    SoftRenderer();
    ~SoftRenderer();
    SoftRenderer(const SoftRenderer&)            = delete;
    SoftRenderer& operator=(const SoftRenderer&) = delete;

    void set_smooth_2d(bool smooth) { m_smooth_2d = smooth; }

    /// The built frame as RGBA8, plan().target_width by target_height.
    void render(FrameBuilder& frame, std::span<u32> out);

    /// What the last render() sampled (frame_number 0 if nothing).
    [[nodiscard]] const LineOfSight& line_of_sight() const { return m_los; }

    struct Tri;
    struct Chunk;
    struct Draw;

private:
    void parallel(u32 count, const std::function<void(u32)>& job);
    void worker_loop();

    void setup(FrameBuilder& frame);
    void setup_chunk(Chunk& chunk);
    void draw_band(s32 y0, s32 y1, std::span<u32> out);
    void draw_range(u32 begin, u32 end, int layer, s32 y0, s32 y1, u32 los_slot);
    void raster(const Tri& t, const Draw& d, int layer, s32 y0, s32 y1, u32 los_slot);
    void composite(s32 y0, s32 y1, std::span<u32> out);

    const FramePlan*           m_plan     = nullptr;
    std::span<const u16>       m_texels;
    std::span<const u32>       m_layers[2];
    u32                        m_layer_width = 0;
    bool                       m_smooth_2d = false;
    u32                        m_width     = 0;
    u32                        m_height    = 0;

    std::vector<Draw>          m_draws;
    std::vector<Chunk>         m_chunks;
    std::vector<u32>           m_draw_chunk;  // first chunk of each draw, then the end

    // Targets: colour layers as RGBA8, depth, stencil, and the copies
    // the first translucency layer is drawn between.
    std::vector<u32>   m_colour[3];
    std::vector<float> m_depth;
    std::vector<u8>    m_stencil;
    std::vector<float> m_depth_copy;
    std::vector<u8>    m_stencil_copy;

    struct LosSample {
        u32  depth_bits = 0;
        bool no_return  = false;
        bool hit        = false;
    };
    std::vector<LosSample> m_los_samples;  // one per pass
    LineOfSight            m_los;

    // Thread pool: jobs [0, m_job_count) are claimed in turn.
    std::vector<std::thread>     m_threads;
    std::mutex                   m_mutex;
    std::condition_variable      m_work_cv;
    std::condition_variable      m_done_cv;
    const std::function<void(u32)>* m_job = nullptr;
    u32                          m_job_count = 0;
    std::atomic<u32>             m_next{0};
    u32                          m_busy       = 0;
    u64                          m_generation = 0;
    bool                         m_quit       = false;
};

}  // namespace sm2::render::model3
