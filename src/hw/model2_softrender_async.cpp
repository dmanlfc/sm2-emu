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

#include "hw/model2_softrender_async.h"

#include "core/cpu_topology.h"
#include "core/log.h"

#include "hw/model2_machine_base.h"
#include "hw/model2_video.h"

#include <chrono>

namespace sm2::hw {

AsyncSoftRenderer::AsyncSoftRenderer(SoftRenderer& renderer)
    : m_renderer(renderer)
{
}

AsyncSoftRenderer::~AsyncSoftRenderer()
{
    wait();
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
    }
    m_work_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void AsyncSoftRenderer::begin(Model2MachineBase& machine, std::span<u32> out, bool compose)
{
    wait();

    if (m_frames_begun++ == 0) {
        const core::CpuTopology topology = core::CpuTopology::detect();
        if (m_use_slow_cores && topology.slow.size() >= 2 && topology.fast.size() >= 3) {
            // The emulation keeps the first fast core; the rest draw.
            std::vector<int> workers(topology.slow.begin() + 1, topology.slow.end());
            workers.insert(workers.end(), topology.fast.begin() + 1, topology.fast.end());
            core::pin_current_thread({topology.fast.front()});
            m_renderer.set_bands(static_cast<u32>(workers.size()) + 1);
            m_renderer.set_bands_per_worker(4);
            m_renderer.set_worker_cpus(std::move(workers));
            m_thread_cpus = {topology.slow.front()};
            SM2_INFO("software renderer: drawing on %zu slow and %zu fast core(s), emulation "
                     "on cpu%d",
                     topology.slow.size(), topology.fast.size() - 1, topology.fast.front());
        } else if (m_use_slow_cores && topology.slow.size() >= 2) {
            // big.LITTLE: the whole draw goes to the slow cores, one band each,
            // the render thread on the first as band 0.
            m_renderer.set_bands(static_cast<u32>(topology.slow.size()));
            m_renderer.set_worker_cpus(
                std::vector<int>(topology.slow.begin() + 1, topology.slow.end()));
            m_thread_cpus = {topology.slow.front()};
            SM2_INFO("software renderer: drawing on %zu slow core(s), first is cpu%d",
                     topology.slow.size(), topology.slow.front());
        } else {
            // Leave one core to the emulation.
            const u32 cores = SoftRenderer::available_cores();
            m_renderer.set_bands(cores > 1 ? cores - 1 : 1);
        }
    }

    // Copied on the caller's thread, before the emulation moves on.
    const auto palram    = machine.palette_ram();
    const auto colorxlat = machine.colour_translate();
    const auto lumaram   = machine.luma_ram();
    m_palram.assign(palram.begin(), palram.end());
    m_colorxlat.assign(colorxlat.begin(), colorxlat.end());
    m_lumaram.assign(lumaram.begin(), lumaram.end());

    const u64 generation = machine.texture_generation();
    if (&machine != m_snapshot_machine || generation != m_snapshot_generation) {
        const auto texture0 = machine.texture_ram(0);
        const auto texture1 = machine.texture_ram(1);
        m_texture0.assign(texture0.begin(), texture0.end());
        m_texture1.assign(texture1.begin(), texture1.end());
        m_snapshot_machine    = &machine;
        m_snapshot_generation = generation;
        ++m_texture_copies;
    }

    Model2Video& video = machine.video();
    if (compose) {
        // Copied: the latched registers can change before the draw is done.
        const auto shown = video.tile_ram_shown();
        const auto pens  = video.pens();
        m_tile_ram.assign(shown.begin(), shown.end());
        m_pens.assign(pens.begin(), pens.end());
        m_mask_a_hidden = video.window_mask_a_hidden();
        m_video         = &video;
        const usize pixels = static_cast<usize>(Model2Video::kWidth) * Model2Video::kHeight;
        m_below.resize(pixels);
        m_above.resize(pixels);
        m_front.resize(pixels);
    } else {
        // Swapped, not copied: compose() rewrites every pixel next frame.
        video.swap_layers(m_below, m_above);
        m_video = nullptr;
    }

    const RenderList& list = machine.render_list();
    m_list.polygons.assign(list.polygons.begin(), list.polygons.end());

    m_sources.palram           = m_palram;
    m_sources.colorxlat        = m_colorxlat;
    m_sources.lumaram          = m_lumaram;
    m_sources.texture0         = m_texture0;
    m_sources.texture1         = m_texture1;
    m_sources.below            = m_below;
    m_sources.above            = m_above;
    m_sources.background       = video.background();
    m_sources.render_test_mode = machine.render_test_mode();
    m_sources.list             = &m_list;
    m_sources.composer         = compose ? this : nullptr;
    m_out                      = out;

    if (!m_thread.joinable()) {
        m_thread = std::thread(&AsyncSoftRenderer::thread_main, this);
    }
    {
        std::lock_guard lock(m_mutex);
        m_pending.store(true, std::memory_order_release);
        ++m_generation;
    }
    m_work_cv.notify_one();
}

void AsyncSoftRenderer::forget_machine()
{
    wait();
    m_snapshot_machine    = nullptr;
    m_snapshot_generation = 0;
}

void AsyncSoftRenderer::wait()
{
    // Acquire pairs with the render thread's release store, so the output is
    // visible without the lock.
    if (!m_pending.load(std::memory_order_acquire)) {
        return;
    }
    std::unique_lock lock(m_mutex);
    m_done_cv.wait(lock, [&] { return !m_pending.load(std::memory_order_acquire); });
}

void AsyncSoftRenderer::compose_rows(u32 row_begin, u32 row_end)
{
    m_video->compose_rows(m_tile_ram, m_mask_a_hidden, m_pens, m_below.data(), m_above.data(),
                          m_front.data(), row_begin, row_end);
}

void AsyncSoftRenderer::thread_main()
{
    core::pin_current_thread(m_thread_cpus);
    u64 seen = 0;
    for (;;) {
        {
            std::unique_lock lock(m_mutex);
            m_work_cv.wait(lock, [&] { return m_quit || m_generation != seen; });
            if (m_quit) {
                return;
            }
            seen = m_generation;
        }

        const auto start = std::chrono::steady_clock::now();
        m_renderer.render(m_sources, m_out);
        const auto end = std::chrono::steady_clock::now();

        m_last_render_ms = std::chrono::duration<double, std::milli>(end - start).count();
        {
            std::lock_guard lock(m_mutex);
            m_pending.store(false, std::memory_order_release);
        }
        m_done_cv.notify_one();
    }
}

}  // namespace sm2::hw
