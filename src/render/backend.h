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
// The render backend seam: everything main.cpp needs from a GPU renderer,
// stated in the hardware's own terms rather than any one graphics API's.
//
// main.cpp, osd::Window and osd::Gui name this interface rather than a concrete
// backend, so the Vulkan and OpenGL/GLES backends are interchangeable and the
// render loop is not Vulkan-specific by construction.
#pragma once

#include "core/config.h"
#include "core/types.h"
#include "render/model3/build_times.h"
#include "render/model3/line_of_sight.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace sm2::hw {
class Model2MachineBase;
class Model2Video;
}  // namespace sm2::hw

namespace sm2::hw::model3 {
struct VideoFrame;
}  // namespace sm2::hw::model3

namespace sm2::osd {
class Window;
class Gui;
}  // namespace sm2::osd

namespace sm2::render {

class TextureReplacements;
namespace model3 {
class TextureDumper;
}

/// What a backend can do, so a call site need not know which backend it is.
struct Capabilities {
    /// Compute shaders and storage buffers, which the GPU tilemap pass
    /// requires. False on a GLES 3.0 / GL 3.3 floor, which uses the CPU path.
    bool compute_shaders = true;

    /// Whether the device reports GPU timestamps at all.
    bool gpu_timing = true;

    /// Anisotropic sampling support and its maximum ratio, gating and clamping
    /// the enhanced 3D texture filter.
    bool  anisotropy     = false;
    float max_anisotropy = 1.0F;

    /// Whether the fill-mask attachment carries depth, which Blended
    /// translucency uses to order its second pass.
    bool blended_translucency = false;

    /// Whether set_widescreen() can widen the native frame.
    bool widescreen = false;
};

/// The native frame's packed RGBA8 format, without naming a VkFormat outside
/// render/vk/. Not a general pixel-format enum.
enum class NativeFormat : u32 {
    Rgba8Unorm = 0,
};

// ---------------------------------------------------------------------------
// The native frame
// ---------------------------------------------------------------------------
// Declared here rather than in render/vk/vk_common.h, which aliases them, so
// every backend shares one definition.

/// The Model 2 raster. Every emulated pass draws at exactly this size: the
/// stipple translucency and texture LOD are locked to the raster grid, and the
/// composite must happen before magnification. The finished frame is scaled to
/// the window once, at the end.
constexpr u32 kNativeWidth  = 496;
constexpr u32 kNativeHeight = 384;

/// Columns added on each side of the raster in widescreen: 496 + 2 * 83 = 662,
/// which at the arcade's pixel shape is 16:9.
constexpr u32 kWideColumns = 83;

/// The native frame's width with `columns` added on each side.
[[nodiscard]] constexpr u32 frame_width(u32 columns) { return kNativeWidth + 2 * columns; }

/// The largest internal 3D render scale (N in 1..8). 8x is 3968x3072, enough to
/// render the 3D at 4K on a capable GPU; clamp_scale_to_max_dimension reduces N
/// on a device that cannot allocate a target that large.
constexpr u32 kMaxRenderScale = 8;

/// The scaled raster the 3D pass and composite run at when render_scale is N.
/// At N=1 these are the native size; the native constants keep their own
/// meaning (stipple and LOD are locked to the native grid, not the scaled one).
[[nodiscard]] constexpr u32 scaled_width(u32 render_scale) { return render_scale * kNativeWidth; }
[[nodiscard]] constexpr u32 scaled_height(u32 render_scale) { return render_scale * kNativeHeight; }

/// Largest scale in 1..requested whose N*native colour/depth target fits a
/// device's max 2D image dimension. Never below 1.
[[nodiscard]] constexpr u32 clamp_scale_to_max_dimension(u32 requested, u32 max_dimension)
{
    u32 scale = requested;
    while (scale > 1 && (frame_width(kWideColumns) * scale > max_dimension
                         || scaled_height(scale) > max_dimension)) {
        --scale;
    }
    return scale;
}

/// Aspect ratio the frame is presented at: the 496x384 raster was stretched to
/// 4:3 on an arcade monitor.
constexpr float kDisplayAspect = 4.0F / 3.0F;


// ---------------------------------------------------------------------------
// GPU stage timing
// ---------------------------------------------------------------------------
// Declared here so this header does not pull in <vulkan/vulkan.h>; vk_common.h
// aliases them.

/// The GPU-side stages the profiler reports. TilemapCompose runs first but is
/// appended so the other indices keep their meaning.
enum class GpuStage : u32 {
    TextureDecode  = 0,
    Poly3D         = 1,
    Composite      = 2,
    Present        = 3,
    TilemapCompose = 4,
    kCount         = 5,
};

/// One stage's GPU time from the latest frame that reached readback. "Did not
/// run" stays distinct from zero: texture decode rarely runs after the first frame.
struct GpuStageTime {
    double milliseconds = 0.0;
    bool   ran          = false;
};

using GpuStageTimes = std::array<GpuStageTime, static_cast<usize>(GpuStage::kCount)>;

/// Where the picture goes, as fractions of the window.
struct PictureArea {
    float x      = 0.0F;
    float y      = 0.0F;
    float width  = 1.0F;
    float height = 1.0F;

    bool operator==(const PictureArea&) const = default;
};

/// Present-stage settings. None touches a render target, so all apply live.
struct PresentOptions {
    ScalingMethod scaling_method = ScalingMethod::SharpBilinear;
    AspectMode    aspect_mode    = AspectMode::FourThree;
    PictureArea   picture_area;

    bool crt_enabled           = false;
    u32  crt_scanline_strength = 40;
    u32  crt_mask_strength     = 30;
    u32  crt_glow_strength     = 20;
    u32  crt_curvature         = 0;

    /// Picture gain, 1 unchanged. Only Model 3 sets it; Model 1/2 brightness
    /// moves their colour ramp.
    float gain = 1.0f;
};

/// Graphics enhancement settings, defaulting to faithful. Gated against
/// Capabilities at the point of use.
struct EnhancementOptions {
    TextureFilter texture_filter = TextureFilter::Faithful;
    u32           anisotropy     = 4;  // tap ceiling, clamped to the GPU max
    Upscale2D     upscale_2d     = Upscale2D::Faithful;
    Translucency  translucency   = Translucency::Stipple;
};

/// What the backend is asked to draw into and present onto.
struct BackendConfig {
    bool        enable_validation = false;
    bool        vsync             = true;
    /// Exact device name to prefer, matching enumerate_device_names()'s
    /// entries. Empty means "pick the best scoring device".
    std::string preferred_device;

    /// Internal 3D render scale, N in 1..kMaxRenderScale; the 3D pass and
    /// composite run at N*native. The software renderer forces this to 1.
    u32 render_scale = 1;

    PresentOptions     present;
    EnhancementOptions enhancement;
};

/// The render backend main.cpp drives, one frame at a time.
///
/// A frame's call sequence:
///
///   1. begin_frame() -- false when the window cannot be drawn into right now
///      (minimised, swapchain rebuilding); skip the rest of this iteration.
///   2. compute_tilemap() or upload_tilemap(), as main.cpp decides.
///   3. submit_polygons() -- skipped when the software renderer draws instead.
///   4. render_polygons() -- skipped alongside submit_polygons().
///   5. composite_native_frame(), or submit_native_frame() with the software
///      renderer's composited pixels.
///   6. request_capture() -- if a screenshot was asked for this frame.
///   7. blit_to_swapchain() -- the one magnification, letterboxed.
///   8. begin_overlay_frame() / draw_overlay() -- ImGui over the frame.
///   9. end_frame() -- submits and presents; false on a failed submission.
///
/// Capture is two-phase (request during the frame, save_capture() once the
/// submission has completed) so a capture series need not block every frame.
class Backend {
public:
    virtual ~Backend() = default;

    Backend()                           = default;
    Backend(const Backend&)             = delete;
    Backend& operator=(const Backend&)  = delete;

    [[nodiscard]] virtual bool init(osd::Window& window, const BackendConfig& config) = 0;
    virtual void               shutdown()                                            = 0;

    /// Wire up ImGui's GPU renderer backend. Call after both init() and
    /// `gui.init()`, since it reads ImGui::GetIO().
    [[nodiscard]] virtual bool init_overlay(osd::Gui& gui) = 0;

    /// Tear down what init_overlay() set up. Call before `gui.shutdown()`.
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

    /// The GPU driver reset the device, so no frame can be drawn again.
    [[nodiscard]] virtual bool device_lost() const { return false; }

    /// GPU tilemap composite, dispatched when the tile RAM, character RAM or
    /// pen table generation counters changed.
    virtual void compute_tilemap(const hw::Model2MachineBase& machine,
                                 const hw::Model2Video&       video) = 0;

    /// CPU-composited tilemap upload, for when compute_tilemap() does not
    /// apply this frame. See TilemapPass::upload().
    virtual void upload_tilemap(std::span<const u32> below, std::span<const u32> above) = 0;

    /// Triangulate and upload this frame's 3D geometry. `machine` may be
    /// null, in which case nothing is drawn (the idle bring-up display).
    virtual void submit_polygons(const hw::Model2MachineBase* machine,
                                 const hw::Model2Video&       video) = 0;

    /// Draw the uploaded geometry into the offscreen native frame.
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

    /// Record a copy of the native frame into a readback buffer, if capture was
    /// requested at init(). False on a recording failure.
    [[nodiscard]] virtual bool request_capture() = 0;

    /// Write what request_capture() captured. Only valid once the frame's
    /// submission has completed (end_frame(), then wait_idle() or the next
    /// begin_frame()).
    [[nodiscard]] virtual bool save_capture(const std::string& path) const = 0;

    /// Adopt new present options before the next blit_to_swapchain().
    virtual void set_present_options(const PresentOptions& options) = 0;

    /// Adopt new enhancement options before the next frame; reallocates nothing.
    virtual void set_enhancement_options(const EnhancementOptions& options) = 0;

    /// Custom textures to draw in place of the game's own, or null for none.
    /// The backend uploads them before its next frame and whenever their
    /// generation() changes; the caller keeps them alive until replaced.
    virtual void set_texture_replacements(TextureReplacements* replacements) = 0;

    /// Widen the native frame by `columns` on each side (0 for the hardware's
    /// own raster). Takes effect at the next begin_frame(). `wide_background`
    /// draws the 2D layers behind the 3D across the added columns: 0 leaves
    /// them black, 1 mirrors the screen edge where nothing better is known,
    /// 2 leaves such pixels black, 3 leaves them black only for a menu panel
    /// (a row-scroll layer's foreground tile). Bits 8-27 are unused; bits 28-31
    /// are the tile layers a HUD spread by set_hud_bands() is drawn on.
    /// Model 3: 0 keeps the 3D 4:3, 1 also widens the scrolling 2D below it,
    /// 2 widens the 3D alone.
    virtual void set_widescreen(u32 columns, u32 wide_background) = 0;

    /// Where a spread HUD lies, per band of 48 rows (GameSpec::hud_bands).
    virtual void set_hud_bands(const std::array<u32, 4>& bands) { static_cast<void>(bands); }

    /// Scale the finished native frame onto the window, letterboxed to the
    /// current aspect mode.
    virtual void blit_to_swapchain() = 0;

    /// Begin this frame's ImGui build. Call before gui.new_frame().
    virtual void begin_overlay_frame() = 0;

    /// Draw ImGui's draw data over the blitted frame. `active` is gui.draw()'s
    /// return value; ImGui::Render() has already run via gui.end_frame().
    virtual void draw_overlay(bool active) = 0;

    [[nodiscard]] virtual bool end_frame() = 0;

    /// Block until the device has finished everything submitted so far, as
    /// needed before destroying anything a frame refers to. Also lets go of any
    /// Model 3 VideoFrame.
    virtual void wait_idle() = 0;

    // -- Sega Model 3 ---------------------------------------------------------
    //
    // Replaces steps 2-5 above: submit_model3_frame() (CPU only; the frame may
    // be reused once it returns; may precede begin_frame()), then
    // render_model3(). Both on the render thread.
    //
    // Async: submit_model3_frame() builds on a worker and the frame must stay
    // intact until the next render_model3() (or wait_idle(), or
    // release_model3_frames()), which draws it, one frame behind.

    [[nodiscard]] virtual bool supports_model3() const { return false; }
    virtual void submit_model3_frame(const hw::model3::VideoFrame& frame) { static_cast<void>(frame); }
    virtual void render_model3() {}
    virtual void set_model3_async(bool async) { static_cast<void>(async); }
    /// The machine is about to overwrite every VideoFrame except `current`.
    virtual void release_model3_frames(const hw::model3::VideoFrame& current) { static_cast<void>(current); }
    /// Wait for an async build.
    virtual void finish_model3() {}
    /// CPU time of the last frame build, on whichever thread ran it.
    [[nodiscard]] virtual model3::BuildTimes model3_build_times() const { return {}; }

    /// Drop cached ROM models and texture state (new game).
    virtual void reset_model3() {}

    /// Collect every texture the Model 3 frames use, or null to stop.
    virtual void set_model3_texture_dumper(model3::TextureDumper* dumper) { static_cast<void>(dumper); }

    /// Line-of-sight results of the latest completed frame (frame_number 0 =
    /// none yet). Any thread. Vulkan lags up to kFramesInFlight frames.
    [[nodiscard]] virtual model3::LineOfSight model3_line_of_sight() const { return {}; }

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

/// Names of every device BackendConfig::preferred_device can name. Callable
/// before any backend is constructed.
[[nodiscard]] std::vector<std::string> enumerate_render_devices();

/// Construct the Vulkan backend, uninitialised (call init() before use).
[[nodiscard]] std::unique_ptr<Backend> create_vulkan_backend();

/// Construct the OpenGL backend, uninitialised: desktop GL 4.3 core or GLES 3.1
/// per SM2_BUILD_OPENGL_DESKTOP / SM2_BUILD_OPENGL_ES. Declared unconditionally
/// but only defined when a GL option is on; main.cpp guards the call.
[[nodiscard]] std::unique_ptr<Backend> create_opengl_backend();

}  // namespace sm2::render
