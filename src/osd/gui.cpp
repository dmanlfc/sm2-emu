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
#include "osd/gui.h"
#include "core/log.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>

namespace sm2::osd {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Gui::~Gui()
{
    if (m_initialised) {
        shutdown();
    }
}

bool Gui::init(SDL_Window* window, VkInstance instance,
               VkPhysicalDevice physical_device, VkDevice device,
               u32 graphics_family, VkQueue graphics_queue,
               VkFormat swapchain_format, u32 image_count)
{
    // ImGui's Vulkan backend needs a descriptor pool for its font texture.
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 },
    };
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets       = 16;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes    = pool_sizes;

    if (vkCreateDescriptorPool(device, &pool_info, nullptr, &m_descriptor_pool) != VK_SUCCESS) {
        SM2_ERROR("gui: failed to create descriptor pool");
        return false;
    }
    m_device = device;
    m_swapchain_format = swapchain_format;

    // ImGui context.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;  // No imgui.ini — settings live in sm2-emu.ini.

    // Dark style with some tweaks.
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding   = 4.0f;
    style.FrameRounding    = 2.0f;
    style.GrabRounding     = 2.0f;
    style.WindowBorderSize = 0.0f;

    // SDL3 platform backend.
    ImGui_ImplSDL3_InitForVulkan(window);

    // Vulkan renderer backend — using dynamic rendering (no VkRenderPass).
    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.ApiVersion      = VK_API_VERSION_1_3;
    init_info.Instance        = instance;
    init_info.PhysicalDevice  = physical_device;
    init_info.Device          = device;
    init_info.QueueFamily     = graphics_family;
    init_info.Queue           = graphics_queue;
    init_info.DescriptorPool  = m_descriptor_pool;
    init_info.MinImageCount   = image_count;
    init_info.ImageCount      = image_count;
    init_info.MSAASamples     = VK_SAMPLE_COUNT_1_BIT;
    init_info.UseDynamicRendering = true;
    init_info.CheckVkResultFn = [](VkResult result) {
        if (result != VK_SUCCESS) {
            SM2_ERROR("gui: Vulkan error %d", static_cast<int>(result));
        }
    };

    // Dynamic rendering format info — must point to stable storage.
    init_info.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &m_swapchain_format;

    if (!ImGui_ImplVulkan_Init(&init_info)) {
        SM2_ERROR("gui: ImGui_ImplVulkan_Init failed");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(device, m_descriptor_pool, nullptr);
        m_descriptor_pool = VK_NULL_HANDLE;
        return false;
    }

    m_initialised = true;
    SM2_INFO("gui: initialised (ImGui %s)", IMGUI_VERSION);
    return true;
}

void Gui::shutdown()
{
    if (!m_initialised) return;

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    if (m_descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptor_pool, nullptr);
        m_descriptor_pool = VK_NULL_HANDLE;
    }

    m_initialised = false;
}

// ---------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------

void Gui::new_frame()
{
    if (!m_initialised) return;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

bool Gui::draw(Config& config, const std::vector<std::string>& gpu_names,
               float measured_hz)
{
    if (!m_visible) return false;

    draw_menu_bar(config);
    draw_settings(config, gpu_names);
    draw_status_bar(measured_hz);

    return true;
}

void Gui::render(VkCommandBuffer cmd)
{
    if (!m_initialised) return;
    // ImGui::Render() must be called every frame after NewFrame(), regardless
    // of whether we intend to draw anything.
    ImGui::Render();
    if (!m_visible || cmd == VK_NULL_HANDLE) return;
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

// ---------------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------------

void Gui::draw_menu_bar(Config& config)
{
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Hide overlay", "F1")) {
                m_visible = false;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) {
                SDL_Event quit_event{};
                quit_event.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit_event);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Settings")) {
            ImGui::MenuItem("Vsync", nullptr, &config.vsync);
            ImGui::MenuItem("Fullscreen", nullptr, &config.fullscreen);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

// ---------------------------------------------------------------------------
// Settings window
// ---------------------------------------------------------------------------

void Gui::draw_settings(Config& config, const std::vector<std::string>& gpu_names)
{
    ImGui::SetNextWindowPos(ImVec2(20, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 340), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Settings", &m_visible)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("SettingsTabs")) {
        // -- Video tab -----------------------------------------------------
        if (ImGui::BeginTabItem("Video")) {
            ImGui::Checkbox("Vsync", &config.vsync);
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Wait for vertical blank before presenting.\n"
                                  "Prevents tearing but adds up to one frame of latency.");
            }

            ImGui::Checkbox("Fullscreen", &config.fullscreen);

            // GPU selection.
            if (!gpu_names.empty()) {
                ImGui::Separator();
                ImGui::Text("GPU");

                // Find current selection.
                int current = 0;
                for (int i = 0; i < static_cast<int>(gpu_names.size()); ++i) {
                    if (gpu_names[i] == config.gpu) {
                        current = i + 1;  // 0 is "Auto"
                        break;
                    }
                }

                // Build combo items.
                if (ImGui::BeginCombo("##gpu", current == 0 ? "Auto (best)" : gpu_names[current - 1].c_str())) {
                    if (ImGui::Selectable("Auto (best)", current == 0)) {
                        config.gpu.clear();
                    }
                    for (int i = 0; i < static_cast<int>(gpu_names.size()); ++i) {
                        const bool selected = (current == i + 1);
                        if (ImGui::Selectable(gpu_names[i].c_str(), selected)) {
                            config.gpu = gpu_names[i];
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
            }

            // Window size (only meaningful in windowed mode).
            if (!config.fullscreen) {
                ImGui::Separator();
                ImGui::Text("Window size");
                int w = static_cast<int>(config.window_width);
                int h = static_cast<int>(config.window_height);
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("Width", &w, 16, 64);
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("Height", &h, 16, 64);
                config.window_width  = static_cast<u32>(std::max(496, w));
                config.window_height = static_cast<u32>(std::max(384, h));
            }

            ImGui::EndTabItem();
        }

        // -- Paths tab -----------------------------------------------------
        if (ImGui::BeginTabItem("Paths")) {
            ImGui::Text("NVRAM directory");
            char nvram_buf[256];
            std::snprintf(nvram_buf, sizeof(nvram_buf), "%s", config.nvram_dir.c_str());
            if (ImGui::InputText("##nvram", nvram_buf, sizeof(nvram_buf))) {
                config.nvram_dir = nvram_buf;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Where EEPROM and battery-backed RAM are stored.\n"
                                  "Relative to the working directory.");
            }

            ImGui::Spacing();
            ImGui::Text("ROM database");
            char xml_buf[256];
            std::snprintf(xml_buf, sizeof(xml_buf), "%s", config.games_xml.c_str());
            if (ImGui::InputText("##gamesxml", xml_buf, sizeof(xml_buf))) {
                config.games_xml = xml_buf;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Path to games.xml. Leave empty to search\n"
                                  "beside the executable and in the system data directory.");
            }

            ImGui::EndTabItem();
        }

        // -- About tab -----------------------------------------------------
        if (ImGui::BeginTabItem("About")) {
            ImGui::Text("sm2-emu — A Sega Model 2 arcade emulator");
            ImGui::Spacing();
            ImGui::Text("Copyright (c) 2025+ Daniel Martin (dmanlfc)");
            ImGui::Text("BSD 3-Clause licence. See LICENSE.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("Emulation derived from the MAME project.");
            ImGui::Text("See NOTICE for per-component attribution.");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // Save button at the bottom.
    ImGui::Separator();
    if (ImGui::Button("Save settings")) {
        const std::string path = default_config_path();
        if (save_config(path, config)) {
            SM2_INFO("gui: settings saved to %s", path.c_str());
        } else {
            SM2_ERROR("gui: could not save settings to %s", path.c_str());
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Saved to sm2-emu.ini");

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Status bar (bottom of screen)
// ---------------------------------------------------------------------------

void Gui::draw_status_bar(float measured_hz)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float bar_height = ImGui::GetFrameHeight() + 4.0f;

    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - bar_height));
    ImGui::SetNextWindowSize(
        ImVec2(viewport->WorkSize.x, bar_height));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus
                           | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 2));
    if (ImGui::Begin("##StatusBar", nullptr, flags)) {
        ImGui::Text("%.1f Hz", static_cast<double>(measured_hz));
        ImGui::SameLine(ImGui::GetWindowWidth() - 120);
        ImGui::Text("F1: toggle overlay");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace sm2::osd
