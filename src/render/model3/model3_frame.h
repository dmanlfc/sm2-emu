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
// One Model 3 frame built on the CPU for a GPU backend. submit() copies all it
// needs out of the VideoFrame (only the VROM view is kept). submit_async()
// builds on a worker thread instead, which also runs run_async() jobs in
// order; wait() before touching the builder.
#pragma once

#include "core/types.h"
#include "hw/model3/video_frame.h"
#include "render/model3/build_times.h"
#include "render/model3/frame_plan.h"
#include "render/model3/line_of_sight.h"
#include "render/model3/scene.h"
#include "render/model3/texture_key.h"
#include "render/model3/textures.h"
#include "render/model3/tilegen.h"

#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace sm2::render {
class TextureReplacements;
}

namespace sm2::render::model3 {

class TextureDumper;

class FrameBuilder {
public:
    FrameBuilder() = default;
    ~FrameBuilder();
    FrameBuilder(const FrameBuilder&)            = delete;
    FrameBuilder& operator=(const FrameBuilder&) = delete;

    void set_geometry(const SceneGeometry& geometry) { m_scene.set_geometry(geometry); }
    [[nodiscard]] const SceneGeometry& geometry() const { return m_scene.geometry(); }

    void reset();

    void submit(const hw::model3::VideoFrame& frame, bool encode_depth);

    // The frame must stay intact until wait() returns.
    void submit_async(const hw::model3::VideoFrame& frame, bool encode_depth);
    void wait();
    /// Runs `job` on the worker after what is queued; wait(ticket) for it.
    u64 run_async(std::function<void()> job);
    void wait(u64 ticket);
    /// wait(), then forget the last frame unless it is `keep`.
    void release(const hw::model3::VideoFrame* keep = nullptr);
    /// wait(), then rebuild the last frame if it was built for other geometry.
    void match_geometry(const SceneGeometry& geometry, bool encode_depth);
    /// The frame of the last submit, or null after reset().
    [[nodiscard]] const hw::model3::VideoFrame* last_frame() const { return m_last; }
    /// CPU time of the last build, on whichever thread ran it.
    [[nodiscard]] const BuildTimes& build_times() const { return m_times; }

    [[nodiscard]] bool has_frame() const { return m_has_frame; }
    [[nodiscard]] u64  frame_number() const { return m_frame_number; }
    /// Advances with every build.
    [[nodiscard]] u64  build_serial() const { return m_builds; }

    [[nodiscard]] const Scene&     scene() const { return m_scene; }
    [[nodiscard]] const FramePlan& plan() const { return m_plan; }

    [[nodiscard]] bool top_layer_hides_3d() const;
    [[nodiscard]] TextureSheets&   textures() { return m_textures; }

    /// Custom textures to point the plan at; null for none.
    void set_texture_replacements(const TextureReplacements* replacements)
    {
        wait();
        m_replacements = replacements;
    }
    void set_texture_dumper(TextureDumper* dumper)
    {
        wait();
        m_dumper = dumper;
    }

    [[nodiscard]] std::span<const u32> layer(int index) const;

private:
    void build(const hw::model3::VideoFrame& frame, bool encode_depth);
    void plan_layers();
    void apply_replacements();
    void worker();

    Scene         m_scene;
    TextureSheets m_textures;
    TileGen       m_tilegen;
    FramePlan     m_plan;
    TextureHasher m_hasher;
    const TextureReplacements* m_replacements = nullptr;
    TextureDumper*             m_dumper       = nullptr;
    bool m_has_frame      = false;
    u64  m_frame_number   = 0;
    u64  m_builds         = 0;

    const hw::model3::VideoFrame* m_last = nullptr;
    BuildTimes                    m_times;

    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_jobs;
    u64  m_queued = 0;  // main thread only
    u64  m_done   = 0;
    bool m_quit   = false;
};

}  // namespace sm2::render::model3
