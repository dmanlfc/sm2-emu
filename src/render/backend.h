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
// The render backend seam: everything main.cpp needs from a GPU renderer,
// stated in the hardware's own terms rather than any one graphics API's.
//
// main.cpp, osd::Window and osd::Gui name this interface rather than a concrete
// backend, so the Vulkan and OpenGL/GLES backends are interchangeable and the
// render loop is not Vulkan-specific by construction.
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

/// What a backend can be asked to do, queried rather than assumed, so a call
/// site asks "does this backend support X" instead of knowing which backend it
/// is.
struct Capabilities {
    /// Compute shaders and storage buffers, which the GPU tilemap pass
    /// requires. True on Vulkan 1.3, false on a GLES 3.0 / GL 3.3 floor -- which
    /// is why TilemapPass's CPU path still exists as a fallback.
    bool compute_shaders = true;

    /// Whether the device reports GPU timestamps at all -- see
    /// Context::supports_gpu_timing()'s own documentation of why this must
    /// stay distinguishable from "zero", not collapsed into it.
    bool gpu_timing = true;
};

/// A native RGBA8 pixel format identifier, backend-neutral.
///
/// Exists only because `render::vk::kNativeColourFormat` is a `VkFormat` and
/// nothing outside `render/vk/` should know that. Every backend stores the
/// native frame as packed RGBA8, so this is not a general pixel-format enum.
enum class NativeFormat : u32 {
    Rgba8Unorm = 0,
};

// ---------------------------------------------------------------------------
// The native frame
// ---------------------------------------------------------------------------
//
// Declared here, not in render/vk/vk_common.h, for the same reason GpuStage
// is: none of the three constants below names a Vulkan type, so every backend
// -- Vulkan, OpenGL, OpenGL ES -- reads the same definition instead of each
// carrying its own copy. vk_common.h aliases them, as it already aliases
// GpuStage.

/// The Model 2 raster. Every emulated pass draws at exactly this size.
///
/// Nothing is rendered at the window's resolution. The hardware's
/// translucency is a checkerboard stipple locked to the raster grid and its
/// texture level of detail comes from raster-pixel derivatives, so both would
/// come out at the wrong scale; and the three-way composite between the
/// tilemap layers and the 3D has to happen before any magnification or its
/// blends are performed on interpolated colours. The finished frame is scaled
/// to the window once, at the end.
constexpr u32 kNativeWidth  = 496;
constexpr u32 kNativeHeight = 384;

/// The largest internal 3D render scale (N in 1..4). 4x is 1984x1536, within
/// every device this project targets.
constexpr u32 kMaxRenderScale = 4;

/// The scaled raster the 3D pass and composite run at when render_scale is N.
/// At N=1 these are the native size; the native constants keep their own
/// meaning (stipple and LOD are locked to the native grid, not the scaled one).
[[nodiscard]] constexpr u32 scaled_width(u32 render_scale) { return render_scale * kNativeWidth; }
[[nodiscard]] constexpr u32 scaled_height(u32 render_scale) { return render_scale * kNativeHeight; }

/// Largest scale in 1..requested whose N*native colour/depth target fits a
/// device's max 2D image/texture dimension. 4x is 1984x1536, within every
/// device this project targets, so this only reduces N on a device that
/// genuinely cannot allocate the target -- a guard, not an expected path. A
/// limit that cannot hold even native (< 496) still returns 1: native is the
/// floor, and a device that small cannot run the renderer at all.
[[nodiscard]] constexpr u32 clamp_scale_to_max_dimension(u32 requested, u32 max_dimension)
{
    u32 scale = requested;
    while (scale > 1 && (scaled_width(scale) > max_dimension
                         || scaled_height(scale) > max_dimension)) {
        --scale;
    }
    return scale;
}

/// Aspect ratio the frame is presented at.
///
/// The raster is 496x384, which is 1.29:1, but an arcade monitor stretched it
/// to the usual 4:3, so a square-pixel presentation would be noticeably
/// narrow.
constexpr float kDisplayAspect = 4.0F / 3.0F;

// ---------------------------------------------------------------------------
// GPU stage timing
// ---------------------------------------------------------------------------
//
// Declared here, not in render/vk/vk_common.h, because neither type below
// names a Vulkan type: they are milliseconds, a bool and a small enum. Living
// here is what lets vk_common.h alias them (`vk::GpuStage = render::GpuStage`)
// instead of the reverse, so this header can be included without pulling in
// <vulkan/vulkan.h>.

/// The GPU-side stages the profiler reports: texture decode dispatch, 3D pass,
/// tilemap/3D composite, present blit. TilemapCompose runs before the other
/// four but is appended so the existing indices keep their meaning.
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

    /// Internal 3D render scale, N in 1..kMaxRenderScale. The 3D pass and
    /// composite run at N*native; at 1 the pipeline is native and behaves
    /// exactly as before this feature. The software renderer forces this to 1.
    u32 render_scale = 1;
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

    // -- overlay textures (for the game picker's box art) --------------------

    /// Opaque GUI-texture handle; 0 is none. Kept API-neutral (a u64, not an
    /// ImTextureID) so this header pulls in neither imgui.h nor any GPU type.
    using TextureHandle = u64;

    /// Upload w*h RGBA8 (row 0 = top) as an overlay texture; 0 on failure. Main
    /// thread, between begin_frame() and end_frame() (the Vulkan path records
    /// the upload into the current frame's command buffer). `rgba` is w*h*4 bytes.
    [[nodiscard]] virtual TextureHandle create_texture(u32 w, u32 h, const u8* rgba) = 0;

    /// Release a create_texture() handle. Safe even if a submitted frame still
    /// samples it: the backend defers the GPU free past the frames in flight.
    virtual void destroy_texture(TextureHandle handle) = 0;

    /// The ImTextureID (as void*) for ImGui::Image(); nullptr for 0/unknown.
    [[nodiscard]] virtual void* texture_imgui_id(TextureHandle handle) const = 0;

    // -- per-frame sequence, in the order documented above --------------------

    [[nodiscard]] virtual bool begin_frame() = 0;

    /// GPU tilemap composite: dispatches the compute shader
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

    // -- GPU stage timing ----------------------------------------------------

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

    /// Pixel extent of the image the overlay is drawn into (swapchain / default
    /// framebuffer). The GUI scales ImGui to this so the overlay fills the
    /// presented image where SDL under-reports the surface size (Wayland).
    virtual void overlay_framebuffer_size(u32* width, u32* height) const = 0;
};

/// Names of every device a backend could be asked to prefer via
/// BackendConfig::preferred_device, for a --list-gpus report or a settings
/// dropdown. Free rather than a Backend method: it must be callable before
/// any backend has been constructed.
[[nodiscard]] std::vector<std::string> enumerate_render_devices();

/// Construct the Vulkan backend, uninitialised (call init() before use).
/// The caller (main.cpp) names this and Backend, never VulkanBackend itself.
[[nodiscard]] std::unique_ptr<Backend> create_vulkan_backend();

/// Construct the OpenGL backend, uninitialised. Whether this creates a
/// desktop GL 4.3 core or a GLES 3.1 context depends on which CMake option
/// was active at build time (SM2_BUILD_OPENGL_DESKTOP vs SM2_BUILD_OPENGL_ES;
/// they are mutually exclusive).
///
/// Declared unconditionally so main.cpp can name it without an #ifdef; only
/// defined (in sm2_render_gl) when either GL option was on at configure time.
/// main.cpp guards the *call* behind
/// `#if defined(SM2_HAVE_OPENGL_DESKTOP) || defined(SM2_HAVE_OPENGL_ES)`,
/// so a build without this backend fails at --graphics-backend parsing with
/// a named error rather than failing to link over an undefined symbol it
/// was never going to call.
[[nodiscard]] std::unique_ptr<Backend> create_opengl_backend();

}  // namespace sm2::render
