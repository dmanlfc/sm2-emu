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
#pragma once

#include "core/config.h"
#include "core/types.h"

#include <string>
#include <vector>
#include <vulkan/vulkan.h>

struct SDL_Window;

namespace sm2::osd {

/// ImGui overlay drawn on top of the emulator's output.
///
/// The GUI is an overlay that appears when the user presses F1 (or launches
/// without a ROM). It provides access to settings that persist to sm2-emu.ini
/// and a game browser for selecting ROMs.
class Gui {
public:
    Gui() = default;
    ~Gui();

    Gui(const Gui&) = delete;
    Gui& operator=(const Gui&) = delete;

    /// Initialise ImGui with the SDL3 + Vulkan backends.
    ///
    /// Call once after the Vulkan device and swapchain are created. The render
    /// pass format must match the swapchain's colour attachment.
    [[nodiscard]] bool init(SDL_Window* window, VkInstance instance,
                            VkPhysicalDevice physical_device, VkDevice device,
                            u32 graphics_family, VkQueue graphics_queue,
                            VkFormat swapchain_format, u32 image_count);

    void shutdown();

    /// Begin a new ImGui frame. Call once per frame before draw().
    void new_frame();

    /// Draw the GUI windows. Returns true if the overlay is visible.
    [[nodiscard]] bool draw(Config& config,
                            const std::vector<std::string>& gpu_names,
                            float measured_hz);

    /// Record ImGui's draw commands into the given command buffer.
    /// The command buffer must be inside a render pass / dynamic rendering scope.
    void render(VkCommandBuffer cmd);

    /// Toggle the overlay on/off.
    void toggle() { m_visible = !m_visible; }

    /// Whether the overlay is currently shown. When visible, the emulator
    /// should still run but input is captured by ImGui.
    [[nodiscard]] bool visible() const { return m_visible; }

    /// Force the overlay visible (e.g. when launched with no ROM).
    void show() { m_visible = true; }

private:
    void draw_menu_bar(Config& config);
    void draw_settings(Config& config, const std::vector<std::string>& gpu_names);
    void draw_status_bar(float measured_hz);

    bool m_visible    = false;
    bool m_initialised = false;

    // ImGui's Vulkan backend needs its own descriptor pool.
    VkDescriptorPool m_descriptor_pool = VK_NULL_HANDLE;
    VkDevice         m_device          = VK_NULL_HANDLE;
    VkFormat         m_swapchain_format = VK_FORMAT_UNDEFINED;
};

}  // namespace sm2::osd
