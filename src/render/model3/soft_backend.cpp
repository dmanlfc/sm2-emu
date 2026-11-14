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

#include "render/model3/soft_backend.h"

#include "render/model3/model3_frame.h"
#include "render/model3/soft_render.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <vector>

namespace sm2::render {

namespace {

class SoftwareModel3Backend final : public Backend {
public:
    explicit SoftwareModel3Backend(std::unique_ptr<Backend> presenter) : m_inner(std::move(presenter)) {}
    ~SoftwareModel3Backend() override { m_frame.wait(); }

    bool init(osd::Window& window, const BackendConfig& config) override { return m_inner->init(window, config); }
    void shutdown() override
    {
        m_frame.release();
        m_inner->shutdown();
    }
    bool init_overlay(osd::Gui& gui) override { return m_inner->init_overlay(gui); }
    void shutdown_overlay() override { m_inner->shutdown_overlay(); }
    [[nodiscard]] Capabilities capabilities() const override { return m_inner->capabilities(); }

    TextureHandle create_texture(u32 w, u32 h, const u8* rgba) override { return m_inner->create_texture(w, h, rgba); }
    void destroy_texture(TextureHandle handle) override { m_inner->destroy_texture(handle); }
    [[nodiscard]] void* texture_imgui_id(TextureHandle handle) const override { return m_inner->texture_imgui_id(handle); }

    bool begin_frame() override
    {
        m_columns = m_pending_columns;
        return m_inner->begin_frame();
    }
    [[nodiscard]] bool device_lost() const override { return m_inner->device_lost(); }
    void compute_tilemap(const hw::Model2MachineBase& machine, const hw::Model2Video& video) override
    {
        m_inner->compute_tilemap(machine, video);
    }
    void upload_tilemap(std::span<const u32> below, std::span<const u32> above) override
    {
        m_inner->upload_tilemap(below, above);
    }
    void submit_polygons(const hw::Model2MachineBase* machine, const hw::Model2Video& video) override
    {
        m_inner->submit_polygons(machine, video);
    }
    void render_polygons() override { m_inner->render_polygons(); }
    void composite_native_frame(u32 background_rgba, bool skip_3d) override
    {
        m_inner->composite_native_frame(background_rgba, skip_3d);
    }
    void submit_native_frame(std::span<const u32> pixels) override { m_inner->submit_native_frame(pixels); }
    bool request_capture() override { return m_inner->request_capture(); }
    [[nodiscard]] bool save_capture(const std::string& path) const override { return m_inner->save_capture(path); }
    void set_present_options(const PresentOptions& options) override { m_inner->set_present_options(options); }
    void set_enhancement_options(const EnhancementOptions& options) override
    {
        m_smooth_2d = options.upscale_2d != Upscale2D::Faithful;
        m_inner->set_enhancement_options(options);
    }
    void set_texture_replacements(TextureReplacements* replacements) override
    {
        m_inner->set_texture_replacements(replacements);
    }
    void set_widescreen(u32 columns, u32 wide_background) override
    {
        m_pending_columns = columns;
        m_wide            = wide_background;
        m_inner->set_widescreen(columns, wide_background);
    }
    void set_hud_bands(const std::array<u32, 4>& bands) override { m_inner->set_hud_bands(bands); }
    void blit_to_swapchain() override { m_inner->blit_to_swapchain(); }
    void begin_overlay_frame() override { m_inner->begin_overlay_frame(); }
    void draw_overlay(bool active) override { m_inner->draw_overlay(active); }
    bool end_frame() override { return m_inner->end_frame(); }
    void wait_idle() override
    {
        m_frame.release();
        m_inner->wait_idle();
    }

    // -- Model 3 ----------------------------------------------------------------

    [[nodiscard]] bool supports_model3() const override { return true; }

    void submit_model3_frame(const hw::model3::VideoFrame& frame) override
    {
        m_frame.wait();
        m_frame.set_geometry(geometry());
        if (m_async) {
            m_frame.submit_async(frame, false);
            m_frame.run_async([this] { draw(); });
        } else {
            m_frame.submit(frame, false);
        }
    }

    void render_model3() override
    {
        m_frame.wait();
        m_frame.match_geometry(geometry(), false);
        if (!m_frame.has_frame()) {
            return;
        }
        if (m_drawn_build != m_frame.build_serial()) {
            draw();
        }
        // Publish line of sight with the delay Model3Pass reads it back with,
        // so a game sees the same results on either renderer.
        const u64 r    = m_records++;
        LosSlot&  read = m_los_slots[(r + 1) % m_los_slots.size()];
        if (read.set) {
            const std::lock_guard<std::mutex> lock(m_los_mutex);
            m_los = read.los;
        }
        m_los_slots[r % m_los_slots.size()] = LosSlot{m_renderer.line_of_sight(), true};

        const u32 w = m_inner->native_width();
        const u32 h = m_inner->native_height();
        const u32 pw = m_frame.plan().target_width;
        const u32 ph = m_frame.plan().target_height;
        if (w == pw && h == ph) {
            m_inner->submit_native_frame(m_pixels);
            return;
        }
        m_native.assign(static_cast<usize>(w) * h, 0xFF000000u);
        for (u32 y = 0; y < std::min(h, ph); y++) {
            std::memcpy(&m_native[static_cast<usize>(y) * w], &m_pixels[static_cast<usize>(y) * pw],
                        std::min(w, pw) * sizeof(u32));
        }
        m_inner->submit_native_frame(m_native);
    }

    void set_model3_async(bool async) override { m_async = async; }
    void release_model3_frames(const hw::model3::VideoFrame& current) override { m_frame.release(&current); }
    void finish_model3() override { m_frame.wait(); }
    [[nodiscard]] model3::BuildTimes model3_build_times() const override
    {
        model3::BuildTimes times = m_frame.build_times();
        times.record             = m_draw_ms;
        return times;
    }
    void reset_model3() override { m_frame.reset(); }
    void set_model3_texture_dumper(model3::TextureDumper* dumper) override { m_frame.set_texture_dumper(dumper); }
    [[nodiscard]] model3::LineOfSight model3_line_of_sight() const override
    {
        const std::lock_guard<std::mutex> lock(m_los_mutex);
        return m_los;
    }

    // -------------------------------------------------------------------------

    [[nodiscard]] bool          supports_gpu_timing() const override { return m_inner->supports_gpu_timing(); }
    [[nodiscard]] GpuStageTimes read_stage_times() override { return m_inner->read_stage_times(); }
    [[nodiscard]] u32 drawn_polygons() const override { return m_inner->drawn_polygons(); }
    [[nodiscard]] u32 triangles() const override { return m_inner->triangles(); }
    [[nodiscard]] u32 blank_polygons() const override { return m_inner->blank_polygons(); }
    [[nodiscard]] const char* device_name() const override { return m_inner->device_name(); }
    [[nodiscard]] u32 native_width() const override { return m_inner->native_width(); }
    [[nodiscard]] u32 native_height() const override { return m_inner->native_height(); }
    [[nodiscard]] NativeFormat native_format() const override { return m_inner->native_format(); }
    void overlay_framebuffer_size(u32* width, u32* height) const override
    {
        m_inner->overlay_framebuffer_size(width, height);
    }

private:
    [[nodiscard]] model3::SceneGeometry geometry() const { return {1, m_columns, m_wide != 0, m_wide == 1}; }

    void draw()
    {
        const auto start = std::chrono::steady_clock::now();
        const model3::FramePlan& plan = m_frame.plan();
        m_pixels.resize(static_cast<usize>(plan.target_width) * plan.target_height);
        m_renderer.set_smooth_2d(m_smooth_2d);
        m_renderer.render(m_frame, m_pixels);
        m_drawn_build = m_frame.build_serial();
        m_draw_ms     = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    struct LosSlot {
        model3::LineOfSight los;
        bool                set = false;
    };

    std::unique_ptr<Backend> m_inner;
    model3::FrameBuilder     m_frame;
    model3::SoftRenderer     m_renderer;
    std::vector<u32>         m_pixels;
    std::vector<u32>         m_native;
    bool                     m_async     = false;
    bool                     m_smooth_2d = false;
    u32                      m_columns   = 0;
    u32                      m_pending_columns = 0;
    u32                      m_wide      = 0;
    u64                      m_drawn_build = 0;
    double                   m_draw_ms     = 0.0;

    u64                     m_records = 0;
    std::array<LosSlot, 4>  m_los_slots{};  // Model3Pass's kFramesInFlight + 1 slots
    mutable std::mutex      m_los_mutex;
    model3::LineOfSight     m_los;
};

}  // namespace

std::unique_ptr<Backend> create_software_model3_backend(std::unique_ptr<Backend> presenter)
{
    return std::make_unique<SoftwareModel3Backend>(std::move(presenter));
}

}  // namespace sm2::render
