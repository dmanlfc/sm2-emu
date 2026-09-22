//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
// The software path run one frame behind the emulation, on its own thread.
// begin() snapshots what the rasteriser reads and hands it to a render thread;
// wait() collects the frame. Same output as the synchronous path, one frame
// later.
#pragma once

#include "core/types.h"
#include "hw/geometrizer.h"
#include "hw/model2_softrender.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace sm2::hw {

class Model2MachineBase;

class AsyncSoftRenderer {
public:
    explicit AsyncSoftRenderer(SoftRenderer& renderer);
    ~AsyncSoftRenderer();

    AsyncSoftRenderer(const AsyncSoftRenderer&)            = delete;
    AsyncSoftRenderer& operator=(const AsyncSoftRenderer&) = delete;

    /// Snapshot the machine's frame and start drawing it into `out`, which is
    /// off limits until wait() returns.
    void begin(Model2MachineBase& machine, std::span<u32> out);

    /// Block until the frame begun by begin() is complete.
    void wait();

    /// On a big.LITTLE host, draw on the slow cores. Set before the first
    /// begin(); on by default.
    void set_use_slow_cores(bool enable) { m_use_slow_cores = enable; }

    /// Call before the machine is destroyed, so a new one at the same address
    /// is not mistaken for the one already snapshotted.
    void forget_machine();

    /// Whether a begin() has not yet been collected by wait().
    [[nodiscard]] bool pending() const { return m_pending.load(std::memory_order_acquire); }

    /// Milliseconds the render thread spent on the last frame, for --profile.
    [[nodiscard]] double last_render_ms() const { return m_last_render_ms; }

    /// How many begin() calls copied texture RAM, out of how many.
    [[nodiscard]] u32 texture_copies() const { return m_texture_copies; }
    [[nodiscard]] u32 frames_begun() const { return m_frames_begun; }

private:
    void thread_main();

    SoftRenderer& m_renderer;

    // The snapshot; texture RAM only when texture_generation() moves.
    std::vector<u16> m_palram;
    std::vector<u16> m_colorxlat;
    std::vector<u8>  m_lumaram;
    std::vector<u32> m_texture0;
    std::vector<u32> m_texture1;
    std::vector<u32> m_below;
    std::vector<u32> m_above;
    RenderList       m_list;
    SoftFrameSources m_sources;
    std::span<u32>   m_out;

    const Model2MachineBase* m_snapshot_machine    = nullptr;
    u64                      m_snapshot_generation = 0;
    u32                      m_texture_copies      = 0;
    u32                      m_frames_begun        = 0;

    bool                    m_use_slow_cores = true;
    std::vector<int>        m_thread_cpus;
    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_work_cv;
    std::condition_variable m_done_cv;
    u64                     m_generation = 0;
    std::atomic<bool>       m_pending{false};
    bool                    m_quit       = false;
    double                  m_last_render_ms = 0.0;
};

}  // namespace sm2::hw
