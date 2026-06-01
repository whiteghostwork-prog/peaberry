/*
 * Copyright 2026 The Peaberry Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "vk/context.h"

#include "core/log.h"
#include "rhi/alloc.h"

#include <vulkan/vulkan_wayland.h>
#include <stdlib.h>
#include <string.h>

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *callback_data,
    void *user_data)
{
    (void)type;
    (void)user_data;

    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        pb_log_warn("%s", callback_data->pMessage);
    }
    return VK_FALSE;
}

static bool has_layer(const char *name)
{
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, NULL);
    if (count == 0) {
        return false;
    }

    VkLayerProperties *layers = calloc(count, sizeof(*layers));
    if (!layers) {
        return false;
    }

    vkEnumerateInstanceLayerProperties(&count, layers);
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(layers[i].layerName, name) == 0) {
            found = true;
            break;
        }
    }

    free(layers);
    return found;
}

static bool has_extension(
    const char *const *required,
    uint32_t required_count)
{
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
    if (count == 0) {
        return required_count == 0;
    }

    VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
    if (!extensions) {
        return false;
    }

    vkEnumerateInstanceExtensionProperties(NULL, &count, extensions);

    for (uint32_t r = 0; r < required_count; ++r) {
        bool found = false;
        for (uint32_t i = 0; i < count; ++i) {
            if (strcmp(extensions[i].extensionName, required[r]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            free(extensions);
            return false;
        }
    }

    free(extensions);
    return true;
}

static bool pick_queue_families(
    VkPhysicalDevice physical_device,
    VkSurfaceKHR surface,
    uint32_t *graphics_family,
    uint32_t *present_family)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    if (!families) {
        return false;
    }

    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, families);

    *graphics_family = UINT32_MAX;
    *present_family = UINT32_MAX;

    for (uint32_t i = 0; i < count; ++i) {
        if ((*graphics_family == UINT32_MAX) && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            *graphics_family = i;
        }

        if (surface != VK_NULL_HANDLE) {
            VkBool32 present_support = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, i, surface, &present_support);
            if (present_support) {
                *present_family = i;
            }
        }

        if (*graphics_family != UINT32_MAX && (surface == VK_NULL_HANDLE || *present_family != UINT32_MAX)) {
            break;
        }
    }

    free(families);

    if (*graphics_family == UINT32_MAX) {
        return false;
    }

    if (surface == VK_NULL_HANDLE) {
        *present_family = *graphics_family;
        return true;
    }

    return *present_family != UINT32_MAX;
}

static bool pick_physical_device(
    VkInstance instance,
    VkSurfaceKHR surface,
    VkPhysicalDevice *physical_device)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (count == 0) {
        pb_log_error("No Vulkan-capable GPU found");
        return false;
    }

    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) {
        return false;
    }

    vkEnumeratePhysicalDevices(instance, &count, devices);

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t graphics_family = 0;
        uint32_t present_family = 0;
        if (!pick_queue_families(devices[i], surface, &graphics_family, &present_family)) {
            continue;
        }

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        pb_log_info("Selected GPU: %s", props.deviceName);
        *physical_device = devices[i];
        free(devices);
        return true;
    }

    pb_log_error("No suitable GPU queue families found");
    free(devices);
    return false;
}

bool pb_vk_context_init_instance(
    pb_vk_context *ctx,
    const char *app_name,
    bool enable_validation,
    bool enable_surface)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->surface_enabled = enable_surface;

    if (volkInitialize() != VK_SUCCESS) {
        pb_log_error("volkInitialize failed");
        return false;
    }

    ctx->validation_enabled = enable_validation && has_layer("VK_LAYER_KHRONOS_validation");

    if (enable_validation && !ctx->validation_enabled) {
        pb_log_warn("Validation requested but VK_LAYER_KHRONOS_validation is unavailable");
    }

    const char *extensions[8];
    uint32_t ext_count = 0;

    if (enable_surface) {
        extensions[ext_count++] = VK_KHR_SURFACE_EXTENSION_NAME;
        extensions[ext_count++] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
    }

    const char *layers[1];
    uint32_t layer_count = 0;
    if (ctx->validation_enabled) {
        extensions[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        layers[layer_count++] = "VK_LAYER_KHRONOS_validation";
    }

    if (!has_extension(extensions, ext_count)) {
        pb_log_error("Missing required Vulkan instance extensions");
        return false;
    }

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = app_name,
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "peaberry",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = VK_API_VERSION_1_2,
    };

    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = ext_count,
        .ppEnabledExtensionNames = extensions,
        .enabledLayerCount = layer_count,
        .ppEnabledLayerNames = layers,
    };

    VkDebugUtilsMessengerCreateInfoEXT debug_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_callback,
    };

    if (ctx->validation_enabled) {
        create_info.pNext = &debug_create_info;
    }

    if (vkCreateInstance(&create_info, NULL, &ctx->instance) != VK_SUCCESS) {
        pb_log_error("vkCreateInstance failed");
        return false;
    }

    volkLoadInstance(ctx->instance);

    if (ctx->validation_enabled) {
        if (vkCreateDebugUtilsMessengerEXT(ctx->instance, &debug_create_info, NULL, &ctx->debug_messenger) !=
            VK_SUCCESS) {
            pb_log_error("vkCreateDebugUtilsMessengerEXT failed");
            pb_vk_context_shutdown(ctx);
            return false;
        }
    }

    return true;
}

void pb_vk_context_shutdown(pb_vk_context *ctx)
{
    pb_rhi_alloc_shutdown(ctx);

    if (ctx->device) {
        vkDeviceWaitIdle(ctx->device);
        vkDestroyDevice(ctx->device, NULL);
        ctx->device = VK_NULL_HANDLE;
    }

    if (ctx->debug_messenger) {
        vkDestroyDebugUtilsMessengerEXT(ctx->instance, ctx->debug_messenger, NULL);
        ctx->debug_messenger = VK_NULL_HANDLE;
    }

    if (ctx->instance) {
        vkDestroyInstance(ctx->instance, NULL);
        ctx->instance = VK_NULL_HANDLE;
    }
}

bool pb_vk_context_init_device(pb_vk_context *ctx, VkSurfaceKHR surface)
{
    if (!pick_physical_device(ctx->instance, surface, &ctx->physical_device)) {
        return false;
    }

    if (!pick_queue_families(
            ctx->physical_device,
            surface,
            &ctx->graphics_queue_family,
            &ctx->present_queue_family)) {
        return false;
    }

    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_infos[2];
    uint32_t queue_info_count = 0;

    queue_infos[queue_info_count++] = (VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = ctx->graphics_queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };

    if (ctx->present_queue_family != ctx->graphics_queue_family) {
        queue_infos[queue_info_count++] = (VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = ctx->present_queue_family,
            .queueCount = 1,
            .pQueuePriorities = &queue_priority,
        };
    }

    const char *device_extensions[1];
    uint32_t device_extension_count = 0;
    if (surface != VK_NULL_HANDLE) {
        device_extensions[device_extension_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    }

    VkPhysicalDeviceFeatures device_features = {0};

    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = queue_info_count,
        .pQueueCreateInfos = queue_infos,
        .enabledExtensionCount = device_extension_count,
        .ppEnabledExtensionNames = device_extension_count > 0 ? device_extensions : NULL,
        .pEnabledFeatures = &device_features,
    };

    if (vkCreateDevice(ctx->physical_device, &device_info, NULL, &ctx->device) != VK_SUCCESS) {
        pb_log_error("vkCreateDevice failed");
        return false;
    }

    volkLoadDevice(ctx->device);
    vkGetDeviceQueue(ctx->device, ctx->graphics_queue_family, 0, &ctx->graphics_queue);
    vkGetDeviceQueue(ctx->device, ctx->present_queue_family, 0, &ctx->present_queue);

    if (!pb_rhi_alloc_init(ctx)) {
        vkDestroyDevice(ctx->device, NULL);
        ctx->device = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

bool pb_vk_context_init_headless_device(pb_vk_context *ctx)
{
    return pb_vk_context_init_device(ctx, VK_NULL_HANDLE);
}
