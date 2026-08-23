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
#include "render/vk/context.h"

#include "core/log.h"
#include "osd/window.h"

#include <vk_mem_alloc.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <set>

namespace sm2::render::vk {
namespace {

constexpr u32 kRequiredApiVersion = VK_API_VERSION_1_3;

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

[[nodiscard]] bool has_extension(const std::vector<VkExtensionProperties>& available,
                                 const char*                              wanted)
{
    return std::any_of(available.begin(), available.end(),
                       [wanted](const VkExtensionProperties& property) {
                           return std::strcmp(property.extensionName, wanted) == 0;
                       });
}

[[nodiscard]] std::vector<VkExtensionProperties> instance_extensions()
{
    u32 count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkExtensionProperties> properties(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, properties.data()) != VK_SUCCESS) {
        return {};
    }
    return properties;
}

[[nodiscard]] std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice device)
{
    u32 count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkExtensionProperties> properties(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, properties.data())
        != VK_SUCCESS) {
        return {};
    }
    return properties;
}

[[nodiscard]] bool validation_layer_available()
{
    u32 count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
        return false;
    }
    return std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& layer) {
        return std::strcmp(layer.layerName, kValidationLayer) == 0;
    });
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
    VkDebugUtilsMessageTypeFlagsEXT             types,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*                                       user_data)
{
    (void)types;
    (void)user_data;

    const char* message = (data != nullptr && data->pMessage != nullptr) ? data->pMessage : "";

    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        SM2_ERROR("vulkan: %s", message);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        SM2_WARN("vulkan: %s", message);
    } else {
        SM2_DEBUG("vulkan: %s", message);
    }

    // Returning VK_TRUE would abort the offending call, which is never what we
    // want: the message is the useful part.
    return VK_FALSE;
}

struct QueueFamilies {
    std::optional<u32> graphics;
    std::optional<u32> present;

    [[nodiscard]] bool complete() const { return graphics.has_value() && present.has_value(); }
};

[[nodiscard]] QueueFamilies find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface)
{
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    QueueFamilies result;
    for (u32 index = 0; index < count; ++index) {
        if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0
            && !result.graphics.has_value()) {
            result.graphics = index;
        }

        VkBool32 present_supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, index, surface, &present_supported);
        if (present_supported == VK_TRUE && !result.present.has_value()) {
            result.present = index;
        }
    }
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

Context::~Context()
{
    shutdown();
}

bool Context::init(osd::Window& window, const ContextConfig& config)
{
    m_window = &window;
    m_vsync  = config.vsync;

    if (!create_instance(config)) {
        return false;
    }
    if (config.enable_validation && !create_debug_messenger()) {
        SM2_WARN("continuing without the debug messenger");
    }
    // Not on osd::Window: a surface is a Vulkan resource, not a window
    // property, so creating it belongs with everything else this class owns.
    if (!SDL_Vulkan_CreateSurface(window.handle(), m_instance, nullptr, &m_surface)) {
        SM2_ERROR("SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }
    if (!select_physical_device(config)) {
        return false;
    }
    if (!create_device()) {
        return false;
    }
    if (!create_allocator()) {
        return false;
    }
    if (!create_command_resources()) {
        return false;
    }
    if (!create_sync_resources()) {
        return false;
    }
    if (!create_query_pool()) {
        return false;
    }
    if (!create_swapchain(VK_NULL_HANDLE)) {
        return false;
    }
    return true;
}

void Context::shutdown()
{
    if (m_device != VK_NULL_HANDLE) {
        // Everything below assumes nothing is in flight.
        SM2_VK_WARN_ON_FAIL(vkDeviceWaitIdle(m_device));
    }

    destroy_swapchain_views();
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }

    for (VkSemaphore semaphore : m_render_finished) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, semaphore, nullptr);
        }
    }
    m_render_finished.clear();
    m_images_in_flight.clear();

    for (u32 frame = 0; frame < kFramesInFlight; ++frame) {
        if (m_image_available[frame] != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, m_image_available[frame], nullptr);
            m_image_available[frame] = VK_NULL_HANDLE;
        }
        if (m_in_flight[frame] != VK_NULL_HANDLE) {
            vkDestroyFence(m_device, m_in_flight[frame], nullptr);
            m_in_flight[frame] = VK_NULL_HANDLE;
        }
    }

    if (m_command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_command_pool, nullptr);
        m_command_pool = VK_NULL_HANDLE;
        m_command_buffers.fill(VK_NULL_HANDLE);
    }

    if (m_query_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(m_device, m_query_pool, nullptr);
        m_query_pool = VK_NULL_HANDLE;
    }

    if (m_allocator != nullptr) {
        vmaDestroyAllocator(m_allocator);
        m_allocator = nullptr;
    }
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    if (m_debug_messenger != VK_NULL_HANDLE) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy != nullptr) {
            destroy(m_instance, m_debug_messenger, nullptr);
        }
        m_debug_messenger = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }

    m_physical_device = VK_NULL_HANDLE;
    m_window          = nullptr;
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

bool Context::create_instance(const ContextConfig& config)
{
    u32 loader_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&loader_version) != VK_SUCCESS) {
        loader_version = VK_API_VERSION_1_0;
    }
    if (loader_version < kRequiredApiVersion) {
        SM2_ERROR("the Vulkan loader reports %u.%u, but 1.3 is required",
                  VK_API_VERSION_MAJOR(loader_version),
                  VK_API_VERSION_MINOR(loader_version));
        return false;
    }

    // As with the surface above, this is a Vulkan-side question SDL happens to
    // answer, not a property of the window itself.
    Uint32              extension_count = 0;
    const char* const*  extension_names = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    if (extension_names == nullptr) {
        SM2_ERROR("SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        return false;
    }
    std::vector<const char*> extensions(extension_names, extension_names + extension_count);
    if (extensions.empty()) {
        SM2_ERROR("SDL reported no required Vulkan instance extensions");
        return false;
    }

    const std::vector<VkExtensionProperties> available = instance_extensions();

    VkInstanceCreateFlags create_flags = 0;
    if (has_extension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        // Without this the loader hides drivers that implement only a portable
        // subset, and vkEnumeratePhysicalDevices comes back empty on MoltenVK.
        // Gated on the extension being advertised rather than on the platform.
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        create_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        m_portability = true;
    }

    bool want_validation = config.enable_validation;
    if (want_validation) {
        if (!has_extension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            SM2_WARN("%s is unavailable; validation messages will not be captured",
                     VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            want_validation = false;
        } else if (!validation_layer_available()) {
            SM2_WARN("%s is not installed; continuing without validation", kValidationLayer);
            want_validation = false;
        } else {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    }

    VkApplicationInfo app_info{};
    app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName   = "sm2-emu";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName        = "sm2-emu";
    app_info.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion         = kRequiredApiVersion;

    const char* layers[] = {kValidationLayer};

    VkInstanceCreateInfo create_info{};
    create_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.flags                   = create_flags;
    create_info.pApplicationInfo        = &app_info;
    create_info.enabledExtensionCount   = static_cast<u32>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();
    create_info.enabledLayerCount       = want_validation ? 1u : 0u;
    create_info.ppEnabledLayerNames     = want_validation ? layers : nullptr;

    // Attaching the messenger to instance creation captures diagnostics from
    // vkCreateInstance itself, which the standalone messenger cannot see.
    VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
    messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                   | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                               | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                               | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger_info.pfnUserCallback = debug_callback;
    if (want_validation) {
        create_info.pNext = &messenger_info;
    }

    const VkResult result = vkCreateInstance(&create_info, nullptr, &m_instance);
    if (result == VK_ERROR_INCOMPATIBLE_DRIVER) {
        SM2_ERROR("vkCreateInstance reported no compatible driver. On macOS this "
                  "usually means MoltenVK is not discoverable: source the Vulkan "
                  "SDK's setup-env.sh so VK_DRIVER_FILES points at "
                  "MoltenVK_icd.json.");
        return false;
    }
    if (result != VK_SUCCESS) {
        SM2_ERROR("vkCreateInstance failed: %s", result_string(result));
        return false;
    }

    SM2_INFO("Vulkan instance created (loader %u.%u.%u, portability %s, validation %s)",
             VK_API_VERSION_MAJOR(loader_version),
             VK_API_VERSION_MINOR(loader_version),
             VK_API_VERSION_PATCH(loader_version),
             m_portability ? "enabled" : "not needed",
             want_validation ? "on" : "off");
    return true;
}

bool Context::create_debug_messenger()
{
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
    if (create == nullptr) {
        SM2_WARN("vkCreateDebugUtilsMessengerEXT is unavailable");
        return false;
    }

    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debug_callback;

    SM2_VK_TRY(create(m_instance, &info, nullptr, &m_debug_messenger));
    return true;
}

// ---------------------------------------------------------------------------
// Physical device
// ---------------------------------------------------------------------------

bool Context::select_physical_device(const ContextConfig& config)
{
    u32 count = 0;
    SM2_VK_TRY(vkEnumeratePhysicalDevices(m_instance, &count, nullptr));
    if (count == 0) {
        SM2_ERROR("the Vulkan loader reported no physical devices");
        SM2_ERROR("This means no driver (ICD) is registered, not that the "
                  "hardware is unsupported.");
#if defined(__APPLE__)
        SM2_ERROR("On macOS, register MoltenVK by sourcing the Vulkan SDK's "
                  "setup-env.sh, which sets VK_DRIVER_FILES to its "
                  "MoltenVK_icd.json.");
#else
        SM2_ERROR("On Linux, install the driver package for your GPU "
                  "(mesa-vulkan-drivers, nvidia-driver, amdvlk) and check that "
                  "/usr/share/vulkan/icd.d contains a JSON manifest.");
#endif
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    SM2_VK_TRY(vkEnumeratePhysicalDevices(m_instance, &count, devices.data()));

    VkPhysicalDevice best       = VK_NULL_HANDLE;
    int              best_score = -1;
    QueueFamilies    best_families;

    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);

        if (properties.apiVersion < kRequiredApiVersion) {
            SM2_DEBUG("skipping '%s': reports Vulkan %u.%u, need 1.3",
                      properties.deviceName,
                      VK_API_VERSION_MAJOR(properties.apiVersion),
                      VK_API_VERSION_MINOR(properties.apiVersion));
            continue;
        }

        const QueueFamilies families = find_queue_families(candidate, m_surface);
        if (!families.complete()) {
            SM2_DEBUG("skipping '%s': no graphics+present queue", properties.deviceName);
            continue;
        }

        const std::vector<VkExtensionProperties> available = device_extensions(candidate);
        if (!has_extension(available, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            SM2_DEBUG("skipping '%s': no swapchain support", properties.deviceName);
            continue;
        }

        // Everything Model 2 needs is core 1.3, but confirm the two features we
        // build the whole renderer on are actually present rather than merely
        // implied by the version.
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &features13;
        vkGetPhysicalDeviceFeatures2(candidate, &features2);

        if (features13.dynamicRendering != VK_TRUE || features13.synchronization2 != VK_TRUE
            || features13.shaderDemoteToHelperInvocation != VK_TRUE) {
            SM2_DEBUG("skipping '%s': dynamicRendering=%d synchronization2=%d "
                      "shaderDemoteToHelperInvocation=%d",
                      properties.deviceName,
                      features13.dynamicRendering,
                      features13.synchronization2,
                      features13.shaderDemoteToHelperInvocation);
            continue;
        }

        int score = 1;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 4;
        } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            score += 2;
        }

        if (!config.preferred_device.empty()
            && config.preferred_device == properties.deviceName) {
            SM2_INFO("using configured device '%s'", properties.deviceName);
            best          = candidate;
            best_families = families;
            break;
        }

        if (score > best_score) {
            best_score    = score;
            best          = candidate;
            best_families = families;
        }
    }

    if (best == VK_NULL_HANDLE) {
        SM2_ERROR("no physical device met the requirements (Vulkan 1.3 with "
                  "dynamic rendering, synchronization2, a graphics+present "
                  "queue and swapchain support)");
        return false;
    }

    if (!config.preferred_device.empty()) {
        VkPhysicalDeviceProperties chosen{};
        vkGetPhysicalDeviceProperties(best, &chosen);
        if (config.preferred_device != chosen.deviceName) {
            SM2_WARN("configured device '%s' was not found; using '%s' instead",
                     config.preferred_device.c_str(), chosen.deviceName);
        }
    }

    m_physical_device = best;
    m_graphics_family = *best_families.graphics;
    m_present_family  = *best_families.present;
    vkGetPhysicalDeviceProperties(m_physical_device, &m_device_properties);

    SM2_INFO("device '%s' (Vulkan %u.%u.%u, driver 0x%08x, queues gfx=%u present=%u)",
             m_device_properties.deviceName,
             VK_API_VERSION_MAJOR(m_device_properties.apiVersion),
             VK_API_VERSION_MINOR(m_device_properties.apiVersion),
             VK_API_VERSION_PATCH(m_device_properties.apiVersion),
             m_device_properties.driverVersion,
             m_graphics_family,
             m_present_family);

    // Gated on the graphics queue family's own timestampValidBits rather than
    // just VkPhysicalDeviceLimits::timestampPeriod > 0: the period can be
    // nonzero while the specific queue family reports zero valid bits, meaning
    // that family cannot report timestamps even though some other queue on the
    // device could. The design doc's benchmark deliberately checks this rather
    // than assuming support -- a device that cannot time itself must say so, not
    // report zeros that look measured.
    {
        u32 family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &family_count,
                                                 families.data());
        const bool valid = m_graphics_family < family_count
                        && families[m_graphics_family].timestampValidBits > 0
                        && m_device_properties.limits.timestampPeriod > 0.0F;
        m_timestamp_period = valid ? m_device_properties.limits.timestampPeriod : 0.0F;
        if (!valid) {
            SM2_WARN("device does not report GPU timestamps on the graphics queue; "
                     "the benchmark's GPU-side figures will be unavailable");
        }
    }

    pick_stencil_format();
    return m_stencil_format != VK_FORMAT_UNDEFINED;
}

void Context::pick_stencil_format()
{
    // S8_UINT first: Model 2 needs a stencil-only fill mask and no depth at
    // all, so a stencil-only attachment is both correct and the cheapest thing
    // to keep in tile memory. It is optional in Vulkan, however, and notably
    // absent on much NVIDIA hardware, hence the combined fallbacks whose depth
    // aspect simply goes unused.
    constexpr VkFormat candidates[] = {
        VK_FORMAT_S8_UINT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D16_UNORM_S8_UINT,
    };

    for (VkFormat format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_physical_device, format, &properties);
        if ((properties.optimalTilingFeatures
             & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
            m_stencil_format = format;
            SM2_INFO("fill-mask attachment format: %s",
                     format == VK_FORMAT_S8_UINT ? "S8_UINT (stencil only)"
                                                 : "combined depth+stencil (depth unused)");
            return;
        }
    }

    SM2_ERROR("no stencil-capable attachment format is available");
    m_stencil_format = VK_FORMAT_UNDEFINED;
}

bool Context::stencil_format_has_depth() const
{
    return m_stencil_format != VK_FORMAT_S8_UINT;
}

// ---------------------------------------------------------------------------
// Logical device
// ---------------------------------------------------------------------------

bool Context::create_device()
{
    const std::set<u32> unique_families{m_graphics_family, m_present_family};

    const float priority = 1.0F;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    queue_infos.reserve(unique_families.size());
    for (u32 family : unique_families) {
        VkDeviceQueueCreateInfo info{};
        info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        info.queueFamilyIndex = family;
        info.queueCount       = 1;
        info.pQueuePriorities = &priority;
        queue_infos.push_back(info);
    }

    std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    const std::vector<VkExtensionProperties> available = device_extensions(m_physical_device);
    if (has_extension(available, "VK_KHR_portability_subset")) {
        // Required to be enabled when the device advertises it, MoltenVK being
        // the case that matters here.
        extensions.push_back("VK_KHR_portability_subset");
    }

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    // What a shader's `discard` compiles to when the target is Vulkan 1.3. The 3D
    // pass needs the discarded fragment to leave the stencil fill mask alone, which
    // is how a stippled polygon lets what is behind it through. Required to be
    // supported by any 1.3 implementation, but still has to be asked for.
    features13.shaderDemoteToHelperInvocation = VK_TRUE;

    // Nothing exotic: the geometry pipeline runs on the CPU exactly as the
    // hardware's did, so no geometry or tessellation stages are requested.
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features13;

    VkDeviceCreateInfo create_info{};
    create_info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pNext                   = &features2;
    create_info.queueCreateInfoCount    = static_cast<u32>(queue_infos.size());
    create_info.pQueueCreateInfos       = queue_infos.data();
    create_info.enabledExtensionCount   = static_cast<u32>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();

    SM2_VK_TRY(vkCreateDevice(m_physical_device, &create_info, nullptr, &m_device));

    vkGetDeviceQueue(m_device, m_graphics_family, 0, &m_graphics_queue);
    vkGetDeviceQueue(m_device, m_present_family, 0, &m_present_queue);
    return true;
}

bool Context::create_allocator()
{
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info{};
    info.vulkanApiVersion = kRequiredApiVersion;
    info.physicalDevice   = m_physical_device;
    info.device           = m_device;
    info.instance         = m_instance;
    info.pVulkanFunctions = &functions;

    SM2_VK_TRY(vmaCreateAllocator(&info, &m_allocator));
    return true;
}

// ---------------------------------------------------------------------------
// Commands and synchronisation
// ---------------------------------------------------------------------------

bool Context::create_command_resources()
{
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = m_graphics_family;
    SM2_VK_TRY(vkCreateCommandPool(m_device, &pool_info, nullptr, &m_command_pool));

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool        = m_command_pool;
    alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = kFramesInFlight;
    SM2_VK_TRY(vkAllocateCommandBuffers(m_device, &alloc_info, m_command_buffers.data()));
    return true;
}

bool Context::create_sync_resources()
{
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (u32 frame = 0; frame < kFramesInFlight; ++frame) {
        SM2_VK_TRY(vkCreateSemaphore(m_device, &semaphore_info, nullptr,
                                     &m_image_available[frame]));
        SM2_VK_TRY(vkCreateFence(m_device, &fence_info, nullptr, &m_in_flight[frame]));
    }
    return true;
}

bool Context::create_query_pool()
{
    if (!supports_gpu_timing()) {
        // Not an error: benchmark reporting degrades to "unavailable" rather
        // than failing the whole context, since the emulator must still run on
        // a device that lacks timestamp support.
        return true;
    }

    VkQueryPoolCreateInfo info{};
    info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kQueriesPerFrame * kFramesInFlight;
    SM2_VK_TRY(vkCreateQueryPool(m_device, &info, nullptr, &m_query_pool));

    // Every query must be reset once before its first use, including before the
    // first read of its availability bit -- vkCmdResetQueryPool is the only way
    // to do that without VK_EXT_host_query_reset, which this renderer does not
    // otherwise need. begin_frame() resets each slot's own range on every frame,
    // but not before its *first* use: for the first kFramesInFlight frames,
    // read_back_stage_times() would run against a range no reset had touched
    // yet. Confirmed by Vulkan validation (VUID-vkGetQueryPoolResults-None-09401,
    // "query not reset") before this fix. So the whole pool is reset once here,
    // borrowing the first command buffer for a throwaway submission before any
    // frame recording begins.
    const VkCommandBuffer cmd = m_command_buffers[0];
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    SM2_VK_TRY(vkBeginCommandBuffer(cmd, &begin_info));
    vkCmdResetQueryPool(cmd, m_query_pool, 0, info.queryCount);
    SM2_VK_TRY(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
    SM2_VK_TRY(vkQueueSubmit(m_graphics_queue, 1, &submit, VK_NULL_HANDLE));
    SM2_VK_TRY(vkQueueWaitIdle(m_graphics_queue));

    return true;
}

bool Context::create_present_semaphores()
{
    for (VkSemaphore semaphore : m_render_finished) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, semaphore, nullptr);
        }
    }
    m_render_finished.assign(m_swapchain_images.size(), VK_NULL_HANDLE);

    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (VkSemaphore& semaphore : m_render_finished) {
        SM2_VK_TRY(vkCreateSemaphore(m_device, &info, nullptr, &semaphore));
    }

    m_images_in_flight.assign(m_swapchain_images.size(), VK_NULL_HANDLE);
    return true;
}

// ---------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------

bool Context::create_swapchain(VkSwapchainKHR old_swapchain)
{
    VkSurfaceCapabilitiesKHR capabilities{};
    SM2_VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical_device, m_surface,
                                                         &capabilities));

    // -- extent ----------------------------------------------------------
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == std::numeric_limits<u32>::max()) {
        // The surface defers to us; use the drawable size in pixels, which is
        // not the window size under HiDPI.
        u32 width = 0;
        u32 height = 0;
        m_window->drawable_size(&width, &height);
        extent.width  = std::clamp(width, capabilities.minImageExtent.width,
                                   capabilities.maxImageExtent.width);
        extent.height = std::clamp(height, capabilities.minImageExtent.height,
                                   capabilities.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        SM2_DEBUG("swapchain creation deferred: surface extent is zero");
        return false;
    }

    // -- format ----------------------------------------------------------
    u32 format_count = 0;
    SM2_VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical_device, m_surface,
                                                    &format_count, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    SM2_VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical_device, m_surface,
                                                    &format_count, formats.data()));
    if (formats.empty()) {
        SM2_ERROR("the surface reports no supported formats");
        return false;
    }

    // UNORM in preference to SRGB. The hardware's colour pipeline ends in a
    // gamma table we apply ourselves; letting the swapchain add a
    // linear-to-sRGB encode on top would wash the image out.
    VkSurfaceFormatKHR chosen = formats[0];
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if ((candidate.format == VK_FORMAT_B8G8R8A8_UNORM
             || candidate.format == VK_FORMAT_R8G8B8A8_UNORM)
            && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = candidate;
            break;
        }
    }

    // -- present mode ----------------------------------------------------
    u32 mode_count = 0;
    SM2_VK_TRY(vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical_device, m_surface,
                                                         &mode_count, nullptr));
    std::vector<VkPresentModeKHR> modes(mode_count);
    SM2_VK_TRY(vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical_device, m_surface,
                                                         &mode_count, modes.data()));

    const auto supports = [&modes](VkPresentModeKHR mode) {
        return std::find(modes.begin(), modes.end(), mode) != modes.end();
    };

    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;  // always available
    if (!m_vsync) {
        // Model 2 runs at ~57.5 Hz, which divides into no common display rate,
        // so pacing is done in software. Without vsync, prefer a mode that does
        // not block: MAILBOX if the driver has it, otherwise IMMEDIATE.
        if (supports(VK_PRESENT_MODE_MAILBOX_KHR)) {
            present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
        } else if (supports(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
            present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
    }

    // -- image count -----------------------------------------------------
    u32 image_count = std::max(capabilities.minImageCount + 1, kFramesInFlight + 1);
    if (capabilities.maxImageCount != 0) {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }

    // COLOR_ATTACHMENT so early phases can render straight into the swapchain
    // image; TRANSFER_DST so later phases can render offscreen at the native
    // 496x384 and blit up.
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0) {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    } else {
        SM2_WARN("the surface does not support TRANSFER_DST usage; offscreen "
                 "rendering will have to composite through a draw");
    }
    // Nothing reads the swapchain back, so no TRANSFER_SRC is asked for. Frame
    // capture takes the native 496x384 frame instead, which PresentPass owns and
    // creates with that usage itself; that also means capture works on a surface
    // that would not have allowed it.

    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface          = m_surface;
    create_info.minImageCount    = image_count;
    create_info.imageFormat      = chosen.format;
    create_info.imageColorSpace  = chosen.colorSpace;
    create_info.imageExtent      = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage       = usage;
    create_info.preTransform     = capabilities.currentTransform;
    create_info.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode      = present_mode;
    create_info.clipped          = VK_TRUE;
    create_info.oldSwapchain     = old_swapchain;

    const u32 families[] = {m_graphics_family, m_present_family};
    if (m_graphics_family != m_present_family) {
        create_info.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices   = families;
    } else {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    SM2_VK_TRY(vkCreateSwapchainKHR(m_device, &create_info, nullptr, &swapchain));

    // Only tear the old one down once the new one exists, so a failure above
    // leaves us still able to present.
    destroy_swapchain_views();
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    }
    m_swapchain              = swapchain;
    m_swapchain_format       = chosen.format;
    m_swapchain_colour_space = chosen.colorSpace;
    m_swapchain_extent       = extent;
    m_present_mode           = present_mode;

    u32 actual_count = 0;
    SM2_VK_TRY(vkGetSwapchainImagesKHR(m_device, m_swapchain, &actual_count, nullptr));
    m_swapchain_images.resize(actual_count);
    SM2_VK_TRY(vkGetSwapchainImagesKHR(m_device, m_swapchain, &actual_count,
                                       m_swapchain_images.data()));

    m_swapchain_views.resize(actual_count, VK_NULL_HANDLE);
    for (u32 index = 0; index < actual_count; ++index) {
        VkImageViewCreateInfo view_info{};
        view_info.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image                       = m_swapchain_images[index];
        view_info.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format                      = m_swapchain_format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        SM2_VK_TRY(vkCreateImageView(m_device, &view_info, nullptr,
                                     &m_swapchain_views[index]));
    }

    if (!create_present_semaphores()) {
        return false;
    }

    const char* mode_name = present_mode == VK_PRESENT_MODE_FIFO_KHR      ? "FIFO"
                          : present_mode == VK_PRESENT_MODE_MAILBOX_KHR   ? "MAILBOX"
                          : present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE"
                                                                          : "other";
    SM2_INFO("swapchain %ux%u, %u images, present mode %s",
             extent.width, extent.height, actual_count, mode_name);

    m_needs_recreation = false;
    return true;
}

void Context::destroy_swapchain_views()
{
    for (VkImageView view : m_swapchain_views) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, view, nullptr);
        }
    }
    m_swapchain_views.clear();
    m_swapchain_images.clear();
}

void Context::wait_idle()
{
    if (m_device != VK_NULL_HANDLE) {
        SM2_VK_WARN_ON_FAIL(vkDeviceWaitIdle(m_device));
    }
}

bool Context::recreate_swapchain()
{
    if (m_window == nullptr) {
        return false;
    }

    u32 width = 0;
    u32 height = 0;
    m_window->settle_drawable_size(&width, &height);
    if (width == 0 || height == 0) {
        // Minimised. Leave the old swapchain alone; begin_frame will keep
        // skipping until there is something to draw into.
        m_needs_recreation = true;
        return false;
    }

    // Presents referencing the old swapchain must have retired before its
    // semaphores and views are destroyed.
    SM2_VK_WARN_ON_FAIL(vkDeviceWaitIdle(m_device));

    return create_swapchain(m_swapchain);
}

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------

bool Context::begin_frame()
{
    if (m_frame_active) {
        SM2_ERROR("begin_frame called while a frame was already recording");
        return false;
    }
    if (m_window == nullptr || m_window->minimised()) {
        return false;
    }
    if (m_needs_recreation && !recreate_swapchain()) {
        return false;
    }

    // Wait for the frame that last used this slot, so its command buffer and
    // per-frame resources are free to overwrite.
    SM2_VK_TRY(vkWaitForFences(m_device, 1, &m_in_flight[m_frame_index], VK_TRUE, UINT64_MAX));

    u32            image_index = 0;
    const VkResult acquire     = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
                                                       m_image_available[m_frame_index],
                                                       VK_NULL_HANDLE, &image_index);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        // The semaphore was not signalled, so it is safe to reuse next frame.
        return recreate_swapchain() ? begin_frame() : false;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        SM2_ERROR("vkAcquireNextImageKHR failed: %s", result_string(acquire));
        return false;
    }
    if (acquire == VK_SUBOPTIMAL_KHR) {
        // Present anyway this frame, then rebuild. Bailing out here would drop
        // a frame for a condition that is merely inefficient.
        m_needs_recreation = true;
    }

    m_image_index = image_index;

    // The frame ring and the swapchain image ring can drift apart, so an image
    // may still be owned by a different in-flight frame.
    if (m_images_in_flight[image_index] != VK_NULL_HANDLE
        && m_images_in_flight[image_index] != m_in_flight[m_frame_index]) {
        SM2_VK_TRY(vkWaitForFences(m_device, 1, &m_images_in_flight[image_index],
                                   VK_TRUE, UINT64_MAX));
    }
    m_images_in_flight[image_index] = m_in_flight[m_frame_index];

    SM2_VK_TRY(vkResetFences(m_device, 1, &m_in_flight[m_frame_index]));

    const VkCommandBuffer command_buffer = m_command_buffers[m_frame_index];
    SM2_VK_TRY(vkResetCommandBuffer(command_buffer, 0));

    // The fence wait above guarantees this slot's GPU work from kFramesInFlight
    // frames ago has retired, so its query results (if any stage ran) are ready
    // to read now -- the last point before vkCmdResetQueryPool below discards
    // them.
    read_back_stage_times(m_frame_index);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    SM2_VK_TRY(vkBeginCommandBuffer(command_buffer, &begin_info));

    if (m_query_pool != VK_NULL_HANDLE) {
        // Queries must be reset before first use and before every reuse, and
        // only outside a render pass -- both satisfied here, right after the
        // command buffer opens.
        vkCmdResetQueryPool(command_buffer, m_query_pool,
                            m_frame_index * kQueriesPerFrame, kQueriesPerFrame);
    }

    m_frame_active = true;
    return true;
}

bool Context::end_frame()
{
    if (!m_frame_active) {
        SM2_ERROR("end_frame called without a matching begin_frame");
        return false;
    }
    m_frame_active = false;

    const VkCommandBuffer command_buffer = m_command_buffers[m_frame_index];
    SM2_VK_TRY(vkEndCommandBuffer(command_buffer));

    VkSemaphoreSubmitInfo wait{};
    wait.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = m_image_available[m_frame_index];
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signal{};
    signal.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = m_render_finished[m_image_index];
    signal.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkCommandBufferSubmitInfo command_info{};
    command_info.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command_info.commandBuffer = command_buffer;

    VkSubmitInfo2 submit{};
    submit.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount   = 1;
    submit.pWaitSemaphoreInfos      = &wait;
    submit.commandBufferInfoCount   = 1;
    submit.pCommandBufferInfos      = &command_info;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos    = &signal;

    SM2_VK_TRY(vkQueueSubmit2(m_graphics_queue, 1, &submit, m_in_flight[m_frame_index]));

    VkPresentInfoKHR present{};
    present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores    = &m_render_finished[m_image_index];
    present.swapchainCount     = 1;
    present.pSwapchains        = &m_swapchain;
    present.pImageIndices      = &m_image_index;

    const VkResult result = vkQueuePresentKHR(m_present_queue, &present);

    // Advance regardless of the present result: the submission happened and its
    // fence belongs to this slot.
    m_frame_index = (m_frame_index + 1) % kFramesInFlight;

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        m_needs_recreation = true;
        return true;
    }
    if (result != VK_SUCCESS) {
        SM2_ERROR("vkQueuePresentKHR failed: %s", result_string(result));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// GPU stage timing
// ---------------------------------------------------------------------------

void Context::write_timestamp(VkPipelineStageFlags2 stage_mask, GpuStage stage, bool is_end)
{
    if (m_query_pool == VK_NULL_HANDLE) {
        return;
    }
    const u32 stage_index = static_cast<u32>(stage);
    const u32 query =
        m_frame_index * kQueriesPerFrame + stage_index * kQueriesPerStage + (is_end ? 1u : 0u);
    vkCmdWriteTimestamp2(m_command_buffers[m_frame_index], stage_mask, m_query_pool, query);
}

void Context::read_back_stage_times(u32 frame_index)
{
    if (m_query_pool == VK_NULL_HANDLE) {
        return;
    }

    // WITH_AVAILABILITY rather than WAIT: this slot's fence has already been
    // waited on by the caller (begin_frame, just above), so the results are
    // known ready, but a stage that never wrote its pair this frame (the decode
    // dispatch, skipped when texture_generation did not change) leaves its two
    // queries unwritten, and WAIT would hang forever on those rather than
    // reporting "not available".
    struct Pair {
        u64 value;
        u64 available;
    };
    std::array<Pair, kQueriesPerFrame> raw{};
    const VkResult result = vkGetQueryPoolResults(
        m_device, m_query_pool, frame_index * kQueriesPerFrame, kQueriesPerFrame,
        sizeof(raw), raw.data(), sizeof(Pair),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS && result != VK_NOT_READY) {
        SM2_VK_WARN_ON_FAIL(result);
        return;
    }

    for (u32 stage_index = 0; stage_index < kStageCount; ++stage_index) {
        GpuStageTime& out = m_last_stage_times[stage_index];
        const Pair&   begin = raw[stage_index * kQueriesPerStage + 0];
        const Pair&   end   = raw[stage_index * kQueriesPerStage + 1];
        if (begin.available == 0 || end.available == 0) {
            // Either truly never written (a skipped stage this frame) or the
            // driver has not finished writing it back yet; both cases mean
            // "nothing to report" rather than "measured zero".
            out = GpuStageTime{};
            continue;
        }
        const u64 ticks  = end.value - begin.value;
        out.milliseconds = static_cast<double>(ticks) * static_cast<double>(m_timestamp_period)
                          / 1'000'000.0;
        out.ran = true;
    }
}

GpuStageTimes Context::read_stage_times()
{
    return m_last_stage_times;
}

// ---------------------------------------------------------------------------
// Device enumeration for configuration UI
// ---------------------------------------------------------------------------

std::vector<std::string> Context::enumerate_device_names()
{
    // A throwaway instance: this runs before any window exists, so it cannot
    // reuse the real one.
    VkApplicationInfo app_info{};
    app_info.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.apiVersion = kRequiredApiVersion;

    std::vector<const char*> extensions;
    VkInstanceCreateFlags    flags = 0;
    if (has_extension(instance_extensions(), VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VkInstanceCreateInfo create_info{};
    create_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.flags                   = flags;
    create_info.pApplicationInfo        = &app_info;
    create_info.enabledExtensionCount   = static_cast<u32>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS) {
        return {};
    }

    u32 count = 0;
    std::vector<std::string> names;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS && count > 0) {
        std::vector<VkPhysicalDevice> devices(count);
        if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) == VK_SUCCESS) {
            names.reserve(count);
            for (VkPhysicalDevice device : devices) {
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(device, &properties);
                names.emplace_back(properties.deviceName);
            }
        }
    }

    vkDestroyInstance(instance, nullptr);
    return names;
}

}  // namespace sm2::render::vk
