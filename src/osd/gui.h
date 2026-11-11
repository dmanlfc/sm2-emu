//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1 & 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
#pragma once

#include "core/config.h"
#include "core/types.h"
#include "render/backend.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct SDL_Window;

namespace sm2::osd {

class Scraper;

/// ImGui overlay drawn on top of the emulator's output.
///
/// The GUI is an overlay that appears when the user presses F1 (or launches
/// without a ROM). It provides access to settings that persist to sm2-emu.ini
/// and a game browser for selecting ROMs.
///
/// This class owns ImGui's context and its SDL3 platform backend only. Which
/// GPU API draws the widgets it builds is a render backend's concern, not
/// this one's -- see render::Backend::draw_overlay(), which calls new_frame()
/// and draw() here, then renders ImGui's resulting draw data through whatever
/// renderer backend (Vulkan today) it owns.
class Gui {
public:
    Gui() = default;
    ~Gui();

    Gui(const Gui&) = delete;
    Gui& operator=(const Gui&) = delete;

    /// Initialise ImGui and its SDL3 platform backend, with the overlay in
    /// `language` (see Config::language).
    ///
    /// Call once the window exists. The render backend initialises its own
    /// ImGui renderer backend separately, after this.
    [[nodiscard]] bool init(SDL_Window* window, const std::string& language);

    void shutdown();

    /// Begin a new ImGui frame. Call once per frame before draw(), after the
    /// render backend's own new-frame call for its ImGui renderer backend.
    void new_frame();

    /// Draw the GUI windows: the always-on FPS counter, plus the F1 settings
    /// overlay when visible. Always returns true, since the FPS counter draws
    /// every frame regardless of `visible()` — the caller should always open a
    /// rendering scope for the backend's draw_overlay().
    [[nodiscard]] bool draw(Config& config,
                            const std::vector<std::string>& gpu_names,
                            float measured_hz,
                            const char* renderer_label,
                            class Input* input);

    /// Finish this frame's ImGui build. Call once, after draw(), before the
    /// render backend submits ImGui's draw data.
    void end_frame();

    /// Toggle the overlay on/off.
    void toggle() { m_visible = !m_visible; }

    /// Whether the overlay is currently shown. When visible, the emulator
    /// should still run but input is captured by ImGui.
    [[nodiscard]] bool visible() const { return m_visible; }

    /// Force the overlay visible (e.g. when launched with no ROM).
    void show() { m_visible = true; }

    /// Where the Save button writes. Set to the path the config was loaded from
    /// so a saved file goes back to the same place (including a --config path).
    void set_config_path(std::string path) { m_config_path = std::move(path); }

    /// What Save writes for the live settings (command-line values held back).
    void set_save_filter(std::function<Config(const Config&)> filter)
    {
        m_save_filter = std::move(filter);
    }

    /// The renderer names this build offers, for the Settings dropdown.
    void set_available_renderers(std::vector<std::string> names)
    {
        m_available_renderers = std::move(names);
    }

    /// Live cabinet-link state for the Network tab, fed each frame so the GUI
    /// need not depend on the hw:: types. All zero/false when unloaded or off.
    struct LinkStatus {
        bool active  = false;  ///< a LAN transport is attached (link enabled)
        bool enabled = false;  ///< the game has switched the board on
        bool alive   = false;  ///< the ring has settled and frames flow
        u32  node    = 0;      ///< this cabinet's negotiated link id
        u32  count   = 0;      ///< cabinets the ring settled on
    };
    void set_link_status(const LinkStatus& status) { m_link_status = status; }

    // -- per-game settings -------------------------------------------------

    /// A game family, the unit per-game volume and gamepad layouts are kept by.
    struct GameFamily {
        std::string key;    ///< parent set name, the per-game settings key
        std::string title;  ///< the parent's title, shown in lists
        bool        gun     = false;
        bool        missile = false;
        u32         inputs  = 0;  ///< rom::InputFlags of its sets
        /// Which pad controls the family reads: its analogue controls on the
        /// aim stick, pedals and right lever, and a gear shift.
        bool        aim     = false;
        bool        accel   = false;
        bool        brake   = false;
        bool        lever   = false;
        bool        gears   = false;
    };

    void set_game_families(std::vector<GameFamily> families)
    {
        m_families = std::move(families);
    }

    /// The aspect the frame is shown at, which is 4:3 when 16:9 is chosen but
    /// the renderer cannot widen the frame.
    void set_shown_aspect(AspectMode aspect) { m_shown_aspect = aspect; }

    /// The running game's family, empty when none is loaded.
    void set_current_family(std::string key) { m_current_family = std::move(key); }

    void set_current_game(std::string name) { m_current_game = std::move(name); }
    void set_model3_running(bool running) { m_model3_running = running; }

    [[nodiscard]] render::PictureArea picture_area() const { return m_present_area; }

    // -- save states -------------------------------------------------------

    /// One save-state slot for the States tab, fed each frame so the GUI need
    /// not depend on the hw:: types (mirrors hw::SlotInfo).
    struct StateSlot {
        std::string label;       ///< "Quick", "Slot 1", ...
        std::string slot;        ///< the slot key: "quick", "1", ...
        bool        occupied = false;
        std::string timestamp;   ///< local time when occupied, else empty
    };

    /// Feed this game's slots (and whether a game is loaded at all) each frame.
    /// An empty list / not-loaded hides the tab's contents.
    void set_state_slots(bool game_loaded, std::vector<StateSlot> slots)
    {
        m_state_game_loaded = game_loaded;
        m_state_slots       = std::move(slots);
    }

    /// A save/load/delete the user asked for via the States tab since the last
    /// call, or nullopt. Polled by the main loop and serviced between frames.
    struct StateRequest {
        enum class Action { Save, Load, Delete };
        Action      action = Action::Save;
        std::string slot;
    };
    [[nodiscard]] std::optional<StateRequest> take_pending_state_request()
    {
        auto out = m_pending_state_request;
        m_pending_state_request.reset();
        return out;
    }

    // -- on-screen notifications -------------------------------------------

    /// Show a brief top-centered message for a couple of seconds while gameplay
    /// continues (e.g. "State saved"). Gated by config.show_notifications in
    /// draw(); calling this is harmless when they are off.
    void notify(std::string message);

    /// A game that failed to start, shown in a dialog until dismissed.
    void show_load_error(std::string game, std::string message,
                         std::vector<std::string> missing);
    [[nodiscard]] bool load_error_open() const { return m_load_error.has_value(); }

    /// True while the System tab waits for a key to bind to a shortcut.
    [[nodiscard]] bool capturing_key() const { return m_capture_hotkey >= 0; }
    /// Binds `key` (an SDL_Keycode) to the shortcut being captured; Escape
    /// cancels.
    void capture_key(Config& config, u32 key);

    /// GPU capabilities for gating the enhancement options, fed each frame so
    /// the GUI need not include the render backend header. Defaults leave the
    /// opt-in enhancements unavailable until a backend reports otherwise.
    void set_enhancement_caps(bool anisotropy, float max_anisotropy, bool blended_translucency)
    {
        m_caps_anisotropy     = anisotropy;
        m_caps_max_anisotropy = max_anisotropy;
        m_caps_blended        = blended_translucency;
    }

    /// How many custom textures the running game has loaded, for the settings.
    void set_custom_texture_count(usize count) { m_custom_texture_count = count; }

    /// True once after the user asked for custom textures to be read again.
    [[nodiscard]] bool take_texture_reload_request()
    {
        const bool out = m_texture_reload_requested;
        m_texture_reload_requested = false;
        return out;
    }

    /// Pixel extent of the backend's overlay framebuffer; new_frame() scales
    /// ImGui to it so the overlay fills the presented image (see new_frame()).
    /// Zero leaves ImGui's own value alone.
    void set_framebuffer_size(u32 width, u32 height)
    {
        m_framebuffer_width  = width;
        m_framebuffer_height = height;
    }

    // -- game picker -------------------------------------------------------

    struct PickerEntry {
        std::string name;   ///< MAME short name: launch key + cache key.
        std::string title;
        std::string year;
        std::string manufacturer;
        std::string description;

        /// One uploaded image: its texture and pixel size (size kept so the
        /// tile can letterbox rather than stretch).
        struct Art1 {
            render::Backend::TextureHandle handle = 0;
            float                          w      = 0.0f;
            float                          h      = 0.0f;
        };
        /// Artwork in display order; the highlighted game cycles through it.
        std::vector<Art1> art;
        enum class Art { Unknown, Loaded, None } art_state = Art::Unknown;
        bool metadata_loaded = false;
    };

    /// Turn the picker on with the launchable games (sorted for display), the
    /// backend (main-thread texture create/destroy) and the scraper (polled
    /// each frame). A null scraper or empty list still lists/launches by title.
    /// The backend that uploads textures the settings tabs draw.
    void set_texture_backend(render::Backend* backend) { m_backend = backend; }

    void enable_picker(std::vector<PickerEntry> entries, render::Backend* backend,
                       Scraper* scraper);

    [[nodiscard]] bool picker_active() const { return m_picker_enabled; }

    /// Turn the picker off (a game launched); entries are kept for re-showing.
    void hide_picker();

    /// The game chosen since the last call, or nullopt. Polled by the main loop.
    [[nodiscard]] std::optional<std::string> take_pending_launch();

    /// Free every picker texture. Main thread, before the backend is destroyed.
    void release_picker_textures();

private:
    void apply_scale();
    void apply_language();
    void draw_picker(Config& config);
    void poll_scraper_and_load_metadata();
    void draw_menu_bar(Config& config);
    void draw_settings(Config& config, const std::vector<std::string>& gpu_names,
                       class Input* input);
    void draw_wheel_tab(Config& config, class Input* input);
    void draw_gamepad_tab(Config& config, class Input* input);
    void draw_lightgun_tab(Config& config, class Input* input);
    void draw_network_tab(Config& config);
    void draw_states_tab(const Config& config);
    void draw_system_tab(Config& config);
    void draw_hotkeys(Config& config);
    void draw_audio_tab(Config& config);
    bool draw_volume_slider(Config& config, const GameFamily& family);
    void draw_dir_picker_popup(Config& config);
    void draw_status_bar(const Config& config, float measured_hz);
    void draw_fps_overlay(float measured_hz, const char* renderer_label);
    void draw_notification();
    void draw_crosshairs(const Config& config, const class Input* input);
    void draw_calibration_markers(const class Input* input);
    void draw_sinden_border(const Config& config);

    enum class DirPickerTarget { None, RomDir, Model1RomDir, Model3RomDir, NvramDir, ScreenshotDir, BezelDir, CrosshairDir };

    void open_dir_picker(DirPickerTarget target, const std::string& initial);

    /// Re-lists the subdirectories of m_dir_picker_path.
    void refresh_dir_picker_entries();

    SDL_Window* m_window      = nullptr;
    std::function<Config(const Config&)> m_save_filter;
    std::string m_config_path;  ///< where Save writes; empty -> default path.
    std::vector<std::string> m_available_renderers;  ///< for the Settings dropdown
    bool        m_visible     = false;
    bool        m_initialised = false;
    float       m_ui_scale    = 0.0f;  ///< applied overlay scale; 0 forces first-frame apply.
    std::string m_language;            ///< Config::language as last applied
    bool        m_language_pending = false;  ///< m_language changed; rebuild before the next frame
    u32         m_framebuffer_width  = 0;  ///< overlay target extent; 0 = use ImGui's own.
    u32         m_framebuffer_height = 0;

    /// Whether the OS cursor is currently hidden for light-gun mode. Tracked so
    /// Hide/Show is only called on a change: polling SDL_CursorVisible() every
    /// frame races the compositor re-showing the cursor on motion, which flickers.
    bool        m_cursor_hidden = false;
    bool        m_mouse_grabbed = false;

    /// The present-stage placement in effect this frame, cached from draw()'s
    /// config so the crosshair and Sinden-border helpers frame the same
    /// rectangle the backend draws the image into (see draw_crosshairs /
    /// draw_sinden_border). Kept in step with the one config value; a mismatch
    /// would drift the aim from the picture.
    AspectMode    m_present_aspect = AspectMode::FourThree;
    AspectMode    m_shown_aspect   = AspectMode::FourThree;
    ScalingMethod m_present_method = ScalingMethod::SharpBilinear;
    render::PictureArea m_present_area;

    /// GPU capabilities for gating the enhancement options (set each frame).
    bool  m_caps_anisotropy     = false;
    float m_caps_max_anisotropy = 1.0F;
    bool  m_caps_blended        = false;

    usize m_custom_texture_count     = 0;
    bool  m_texture_reload_requested = false;

    // -- wheel calibration capture state -----------------------------------
    // Which control (if any) is currently waiting for the user to operate it,
    // and the axis baseline captured when an axis calibration began.
    enum class Capture { None, Button, Axis };
    Capture m_capture       = Capture::None;
    u32     m_capture_role  = 0;   ///< Config::WheelRole being bound, when Button.
    int     m_capture_axis  = 0;   ///< which analogue control, when Axis (0=steer,1=accel,2=brake).
    std::array<s16, 16> m_axis_baseline{};

    // Gun-button bind capture: which (player, role) awaits a press, or -1.
    int m_gun_capture_player = -1;
    u32 m_gun_capture_role   = 0;

    // Button capture: the (player, role) awaiting a press, or -1, and the
    // player being edited.
    int m_pad_capture_player = -1;
    u32 m_pad_capture_role   = 0;
    int m_pad_edit_player    = 0;
    int  m_capture_hotkey     = -1;     ///< Config::Hotkey waiting for a key, or -1
    bool m_pad_shortcuts_view = false;  ///< the Guide shortcuts in place of the bindings

    // Axis capture: the (player, role) awaiting a move, or -1, and the rest
    // position when it began.
    int                 m_pad_axis_capture_player = -1;
    u32                 m_pad_axis_capture_role   = 0;
    std::array<s16, 16> m_pad_axis_baseline{};

    // -- cabinet link status -----------------------------------------------
    LinkStatus m_link_status;

    // -- per-game settings -------------------------------------------------
    std::vector<GameFamily> m_families;
    std::string             m_current_family;
    char                    m_volume_filter[64] = {};

    /// The family whose layout the Gamepad tab edits, empty for the default. Follows
    /// the running game when that changes.
    std::string m_pad_family;
    std::string m_pad_family_followed;
    char        m_pad_family_filter[64] = {};

    // -- save-state slots (States tab) -------------------------------------
    bool                        m_state_game_loaded = false;
    std::vector<StateSlot>      m_state_slots;
    std::optional<StateRequest> m_pending_state_request;

    // -- on-screen notification (transient top-centered toast) -------------
    std::string m_notify_text;
    float       m_notify_seconds_left = 0.0f;  ///< counts down by DeltaTime

    struct LoadErrorDialog {
        std::string              game;
        std::string              message;
        std::vector<std::string> missing;
        bool                     opened = false;
    };
    std::optional<LoadErrorDialog> m_load_error;
    void draw_load_error();

    // -- directory picker state (Paths tab "Browse..." buttons) -------------
    // Self-drawn (std::filesystem only), avoiding a native-dialog dependency.
    DirPickerTarget           m_dir_picker_target     = DirPickerTarget::None;
    std::string               m_dir_picker_path;       ///< editable path field
    std::vector<std::string>  m_dir_picker_subdirs;    ///< immediate children of the path above
    bool                      m_dir_picker_unreadable  = false;

    /// BeginTabItem() overrides the ID stack, so OpenPopup() must be issued
    /// from draw_dir_picker_popup() (after EndTabBar()) instead of here.
    bool                      m_dir_picker_request_open = false;

    // -- gamepad diagram ---------------------------------------------------
    /// The pad picture, decoded and uploaded on first use.
    struct PadArt {
        render::Backend::TextureHandle handle = 0;
        float                          w      = 0.0f;
        float                          h      = 0.0f;
        bool                           tried  = false;
    };
    PadArt           m_pad_art;
    render::Backend* m_backend = nullptr;
    [[nodiscard]] const PadArt* pad_art();
    void                        release_pad_art();

    // -- bezel -------------------------------------------------------------
    struct Bezel {
        std::string                    path;
        render::Backend::TextureHandle handle = 0;
        float                          w      = 0.0f;
        float                          h      = 0.0f;
        float                          window_x = 0.0f;
        float                          window_y = 0.0f;
        float                          window_w = 0.0f;
        float                          window_h = 0.0f;
    };
    Bezel       m_bezel;
    std::string m_bezel_lookup;
    std::string m_current_game;
    bool        m_model3_running = false;
    void draw_bezel(const Config& config);
    void load_bezel(const std::string& path);
    void release_bezel();

    struct CrosshairImage {
        std::string                    path;
        render::Backend::TextureHandle handle = 0;
        float                          w      = 0.0f;
        float                          h      = 0.0f;
    };
    std::array<CrosshairImage, 2> m_crosshair_images;
    std::string                   m_crosshair_lookup;
    void update_crosshair_images(const Config& config);
    void release_crosshair_images();

    // -- game picker state -------------------------------------------------
    bool                       m_picker_enabled = false;
    std::vector<PickerEntry>   m_picker_entries;
    render::Backend*           m_picker_backend = nullptr;
    Scraper*                   m_picker_scraper = nullptr;
    int                        m_picker_selected = 0;
    std::optional<std::string> m_pending_launch;
    std::string                m_picker_last_launched;  ///< reselect on return
    float                      m_picker_scroll = 0.0f;   ///< seconds since the pick (scroll clock)
    bool                       m_picker_scroll_to_sel = false;
    float                      m_picker_art_timer = 0.0f;
    int                        m_picker_art_index = 0;
};

}  // namespace sm2::osd
