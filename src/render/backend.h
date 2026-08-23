//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// sm2-emu — A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
// The render backend seam: everything main.cpp needs from a GPU renderer,
// stated in the hardware's own terms rather than any one graphics API's.
//
// Today there is exactly one implementation, render::vk::VulkanBackend, and
// this interface exists so that main.cpp, osd::Window and osd::Gui name it
// instead. The point is not flexibility for its own sake -- it is that phase 9
// (a GLES 3.1 / GL 4.3 backend, per the phase 8 design doc) can be built
// against this interface without touching the three files above a second
// time, and that main.cpp's render-loop logic (the frame sequencing,
// profiling scopes and capture bookkeeping) stops being Vulkan-specific by
// construction rather than by discipline.
#pragma once

#include "core/types.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace sm2::hw {
class Model2MachineBase;
class Model2Video;
}  // namespace sm2::hw

namespace sm2::osd {
class Window;
class Gui;
}  // namespace sm2::osd

namespace sm2::render {

/// What a backend can be asked to do, queried rather than assumed.
///
/// Exists so an optimisation added later (per phase 8 design.md §4) can ask
/// "does this backend support X" instead of every call site needing its own
/// `#ifdef`-shaped knowledge of which backend it is talking to. Today there is
/// one backend and every flag is true for it; the fields exist for the
/// backend that does not have them all.
struct Capabilities {
    /// Compute shaders and storage buffers, which the GPU tilemap pass (phase
    /// 8 task 4) requires. True on Vulkan 1.3; would be false on a GLES 3.0 /
    /// GL 3.3 floor, which is exactly why `TilemapPass::upload()`'s CPU path
    /// still exists as a fallback rather than having been deleted.
    bool compute_shaders = true;

    /// Whether the device reports GPU timestamps at all -- see
    /// Context::supports_gpu_timing()'s own documentation of why this must
    /// stay distinguishable from "zero", not collapsed into it.
    bool gpu_timing = true;
};

/// A native RGBA8 pixel format identifier, backend-neutral.
///
/// Exists only because `render::vk::kNativeColourFormat` is a `VkFormat`, and
/// nothing outside `render/vk/` should need to know that. Every backend today
/// (and every one phase 9 is likely to add) stores the native frame as
/// packed RGBA8, so this is deliberately not a general pixel-format enum.
enum class NativeFormat : u32 {
    Rgba8Unorm = 0,
};

// ---------------------------------------------------------------------------
// GPU stage timing (phase 8 benchmark, design.md requirement 1.2)
// ---------------------------------------------------------------------------
//
// Declared here, not in render/vk/vk_common.h, because neither type below
// names a Vulkan type: they are milliseconds, a bool and a small enum. Living
// here is what lets vk_common.h alias them (`vk::GpuStage = render::GpuStage`)
// instead of the reverse, so this header can be included without pulling in
// <vulkan/vulkan.h>.

/// The GPU-side stages the phase 8 benchmark reports. The first four are in
/// the order the design doc names them: the texture decode dispatch, the 3D
/// pass, the tilemap/3D composite and the present blit. TilemapCompose is the
/// task 4 addition -- the compute dispatch that produces the below/above
/// tilemap surfaces themselves, which chronologically runs before all four of
/// the others but is appended here rather than inserted, so the existing
/// indices keep their meaning.
enum class GpuStage : u32 {
    TextureDecode  = 0,
    Poly3D         = 1,
    Composite      = 2,
    Present        = 3,
    TilemapCompose = 4,
    kCount         = 5,
};

/// One stage's GPU time from the most recently completed frame that reached
/// readback, or "did not run" if that stage's begin/end pair was never written
/// -- the texture decode dispatch only runs when texture_generation changes,
/// which is almost always after the first frame, so "zero" and "did not run"
/// must stay distinguishable rather than the latter reading as a free stage.
struct GpuStageTime {
    double milliseconds = 0.0;
    bool   ran          = false;
};

using GpuStageTimes = std::array<GpuStageTime, static_cast<usize>(GpuStage::kCount)>;

/// What the backend is asked to draw into and present onto.
struct BackendConfig {
    bool        enable_validation = false;
    bool        vsync             = true;
    /// Exact device name to prefer, matching enumerate_device_names()'s
    /// entries. Empty means "pick the best scoring device".
    std::string preferred_device;
};

/// The render backend main.cpp drives, one frame at a time.
///
/// The call sequence a frame makes, in order, mirrors exactly what 0.7.0's
/// main.cpp did directly against Context/TilemapPass/Poly3DPass/PresentPass/
/// FrameCapture -- this interface did not invent a new shape, it named the
/// existing one:
///
///   1. begin_frame() -- returns false on a window that cannot be drawn into
///      right now (minimised, swapchain rebuilding); the caller should skip
///      the rest of the loop body for this iteration.
///   2. compute_tilemap() or upload_tilemap() -- the caller chooses based on
///      whether the GPU tilemap path applies this frame (see main.cpp's
///      own use_gpu_tilemap condition, which is a machine/mode question, not
///      a backend one).
///   3. submit_polygons() -- triangulates and uploads this frame's 3D
///      geometry. Skipped when the software renderer is drawing instead.
///   4. render_polygons() -- draws the uploaded geometry into the offscreen
///      native frame. Skipped alongside submit_polygons().
///   5. Either composite_native_frame() (the Vulkan three-way composite:
///      tilemap below, then the 3D output already drawn by render_polygons(),
///      then tilemap above) or submit_native_frame() (hands the software
///      renderer's already-composited pixels to the same native target, so
///      capture and presentation stay renderer-agnostic).
///   6. request_capture() -- if a screenshot was asked for this frame.
///   7. blit_to_swapchain() -- the one magnification, letterboxed to 4:3.
///   8. begin_overlay_frame() / draw_overlay() -- ImGui, drawn over the
///      already-presented frame.
///   9. end_frame() -- submits and presents. Returns false on a failed
///      submission.
///
/// Capture is two-phase (request during the frame, save_capture() once the
/// submission has completed) rather than a single blocking call, because a
/// numbered capture series is saved every frame (after a wait) while a single
/// final screenshot is saved once after the whole run -- collapsing that into
/// one call would either force a wait every frame or lose the single-shot
/// path's timing.
class Backend {
public:
    virtual ~Backend() = default;

    Backend()                           = default;
    Backend(const Backend&)             = delete;
    Backend& operator=(const Backend&)  = delete;

    [[nodiscard]] virtual bool init(osd::Window& window, const BackendConfig& config) = 0;
    virtual void               shutdown()                                            = 0;

    /// Wire up whichever GPU renderer backend ImGui needs. Must be called
    /// after both init() (the window and swapchain must exist) and
    /// `gui.init()` (ImGui's own context must exist first: the renderer
    /// backend's init call reads ImGui::GetIO(), which asserts otherwise).
    [[nodiscard]] virtual bool init_overlay(osd::Gui& gui) = 0;

    /// Tear down what init_overlay() set up. Must be called before
    /// `gui.shutdown()` destroys ImGui's context, for the same reason in
    /// reverse: the renderer backend's own shutdown call also reads
    /// ImGui::GetIO().
    virtual void shutdown_overlay() = 0;

    [[nodiscard]] virtual Capabilities capabilities() const = 0;

    // -- per-frame sequence, in the order documented above --------------------

    [[nodiscard]] virtual bool begin_frame() = 0;

    /// GPU tilemap composite (phase 8 task 4): dispatches the compute shader
    /// against tile RAM, character RAM and the pen table if their generation
    /// counters changed.
    virtual void compute_tilemap(const hw::Model2MachineBase& machine,
                                 const hw::Model2Video&       video) = 0;

    /// CPU-composited tilemap upload, for when compute_tilemap() does not
    /// apply this frame (render test mode, or a --soft-render comparison
    /// capture needing a fresh CPU oracle -- see TilemapPass::upload()'s own
    /// documentation of the two cases and why they share staging buffers).
    virtual void upload_tilemap(std::span<const u32> below, std::span<const u32> above) = 0;

    /// Triangulate and upload this frame's 3D geometry. `machine` may be
    /// null, in which case nothing is drawn (the idle bring-up display).
    virtual void submit_polygons(const hw::Model2MachineBase* machine,
                                 const hw::Model2Video&       video) = 0;

    /// Draw the geometry submit_polygons() uploaded into the offscreen native
    /// frame.
    virtual void render_polygons() = 0;

    /// The hardware's three-way composite: tilemap layers of priority
    /// category zero, then the 3D output render_polygons() already drew,
    /// then category one. `skip_3d` is render test mode's framebuffer
    /// overlay case, where the 3D stage is not drawn at all.
    virtual void composite_native_frame(u32 background_rgba, bool skip_3d) = 0;

    /// Replace the native frame with pixels the software renderer already
    /// composited, in place of steps 3-5. `pixels` must hold
    /// native_width() * native_height() RGBA8 texels.
    virtual void submit_native_frame(std::span<const u32> pixels) = 0;

    /// Record a copy of the native frame into a readback buffer this frame,
    /// if capture support was requested at init(). Returns false on a
    /// recording failure; a backend with no capture support configured
    /// always returns true and stores nothing.
    [[nodiscard]] virtual bool request_capture() = 0;

    /// Write what request_capture() captured. Only valid once the frame's
    /// submission has completed -- the caller must have called end_frame()
    /// and, for anything but the very next frame's begin_frame(), waited for
    /// the device to go idle first.
    [[nodiscard]] virtual bool save_capture(const std::string& path) const = 0;

    /// Scale the finished native frame onto the window, letterboxed to 4:3.
    virtual void blit_to_swapchain() = 0;

    /// Begin this frame's ImGui build on whichever GPU renderer backend ImGui
    /// is using. Call before gui.new_frame() -- ImGui's own renderer backend
    /// must see its new-frame call before ImGui::NewFrame() does.
    virtual void begin_overlay_frame() = 0;

    /// Draw ImGui's draw data, built by the caller's own gui.draw() in
    /// between begin_overlay_frame() and this call, over the already-blitted
    /// window contents. `active` is draw()'s return value -- true whenever
    /// there is anything to submit, which since the always-on FPS overlay is
    /// every frame a machine is loaded. ImGui::Render() has already run by
    /// the time this is called (via gui.end_frame()) regardless of `active`,
    /// since ImGui requires that every frame it opened with new_frame().
    virtual void draw_overlay(bool active) = 0;

    [[nodiscard]] virtual bool end_frame() = 0;

    /// Block until the device has finished everything submitted so far.
    /// Required before destroying anything a submitted frame still refers
    /// to, and before save_capture() can trust a capture is complete.
    virtual void wait_idle() = 0;

    // -- GPU stage timing (phase 8 benchmark) --------------------------------

    [[nodiscard]] virtual bool          supports_gpu_timing() const = 0;
    [[nodiscard]] virtual GpuStageTimes read_stage_times()          = 0;

    // -- diagnostics ----------------------------------------------------------

    [[nodiscard]] virtual u32  drawn_polygons() const = 0;
    [[nodiscard]] virtual u32  triangles() const      = 0;
    [[nodiscard]] virtual u32  blank_polygons() const = 0;
    [[nodiscard]] virtual const char* device_name() const = 0;

    // -- native frame geometry, for the caller's own buffers -----------------

    [[nodiscard]] virtual u32          native_width() const  = 0;
    [[nodiscard]] virtual u32          native_height() const = 0;
    [[nodiscard]] virtual NativeFormat native_format() const = 0;
};

/// Names of every device a backend could be asked to prefer via
/// BackendConfig::preferred_device, for a --list-gpus report or a settings
/// dropdown. Free rather than a Backend method: it must be callable before
/// any backend has been constructed.
[[nodiscard]] std::vector<std::string> enumerate_render_devices();

/// Construct the Vulkan backend, uninitialised (call init() before use).
///
/// The only factory today, matching hw::create_machine()'s shape: a caller
/// (main.cpp) names this function and Backend, never render::vk::VulkanBackend
/// itself, so a second backend added in a later phase needs no change here
/// beyond this function choosing between them.
[[nodiscard]] std::unique_ptr<Backend> create_vulkan_backend();

}  // namespace sm2::render
