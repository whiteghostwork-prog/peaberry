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

#include "wsi.h"

#include "peaberry/peaberry_bench.h"
#include "peaberry/peaberry_frame_metrics.h"
#include "peaberry/peaberry_vk.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PB_MAX_FRAMES_IN_FLIGHT 2

struct pb_example_wsi {
    pb_context *context;
    GLFWwindow *window;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    VkRenderPass render_pass;
    VkImage *images;
    VkImageView *image_views;
    VkFramebuffer *framebuffers;
    uint32_t image_count;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffers[PB_MAX_FRAMES_IN_FLIGHT];
    VkSemaphore image_available[PB_MAX_FRAMES_IN_FLIGHT];
    VkSemaphore render_finished[PB_MAX_FRAMES_IN_FLIGHT];
    VkFence in_flight[PB_MAX_FRAMES_IN_FLIGHT];
    uint32_t frame_index;
    uint32_t current_image;
    bool framebuffer_resized;
    float clear_color[4];
    uint32_t width;
    uint32_t height;
    VkFormat depth_format;
    VkImage depth_image;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;
    bool stats_enabled;
    bool stats_have_gpu_sample;
    pb_rhi_query_pool *stats_query_pool;
    uint64_t stats_wall_start_ns;
    uint64_t stats_submit_time_ns[PB_MAX_FRAMES_IN_FLIGHT];
    pb_bench_frame stats_bench_frame;
    pb_frame_metrics stats_last_metrics;
    pb_frame_metrics_accumulator stats_accumulator;
    char base_title[128];
};

static void framebuffer_size_callback(GLFWwindow *window, int width, int height)
{
    pb_example_wsi *wsi = glfwGetWindowUserPointer(window);
    if (wsi == NULL || width == 0 || height == 0) {
        return;
    }

    wsi->width = (uint32_t)width;
    wsi->height = (uint32_t)height;
    wsi->framebuffer_resized = true;
}

static bool choose_depth_format(VkPhysicalDevice physical_device, VkFormat *out_format)
{
    const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physical_device, candidates[i], &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            *out_format = candidates[i];
            return true;
        }
    }

    return false;
}

static void destroy_depth_resources(pb_example_wsi *wsi)
{
    VkDevice device = pb_context_device(wsi->context);

    if (wsi->depth_view) {
        vkDestroyImageView(device, wsi->depth_view, NULL);
        wsi->depth_view = VK_NULL_HANDLE;
    }
    if (wsi->depth_image) {
        vkDestroyImage(device, wsi->depth_image, NULL);
        wsi->depth_image = VK_NULL_HANDLE;
    }
    if (wsi->depth_memory) {
        vkFreeMemory(device, wsi->depth_memory, NULL);
        wsi->depth_memory = VK_NULL_HANDLE;
    }
}

static bool create_depth_resources(pb_example_wsi *wsi)
{
    VkPhysicalDevice physical_device = pb_context_physical_device(wsi->context);
    VkDevice device = pb_context_device(wsi->context);

    if (!choose_depth_format(physical_device, &wsi->depth_format)) {
        return false;
    }

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = wsi->depth_format,
        .extent = { wsi->extent.width, wsi->extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    if (vkCreateImage(device, &image_info, NULL, &wsi->depth_image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device, wsi->depth_image, &mem_reqs);

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

    uint32_t mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_reqs.memoryTypeBits & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            mem_type = i;
            break;
        }
    }

    if (mem_type == UINT32_MAX) {
        vkDestroyImage(device, wsi->depth_image, NULL);
        wsi->depth_image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };

    if (vkAllocateMemory(device, &alloc_info, NULL, &wsi->depth_memory) != VK_SUCCESS) {
        vkDestroyImage(device, wsi->depth_image, NULL);
        wsi->depth_image = VK_NULL_HANDLE;
        return false;
    }

    if (vkBindImageMemory(device, wsi->depth_image, wsi->depth_memory, 0) != VK_SUCCESS) {
        destroy_depth_resources(wsi);
        return false;
    }

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = wsi->depth_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = wsi->depth_format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };

    if (vkCreateImageView(device, &view_info, NULL, &wsi->depth_view) != VK_SUCCESS) {
        destroy_depth_resources(wsi);
        return false;
    }

    return true;
}

static bool create_render_pass(pb_example_wsi *wsi)
{
    VkAttachmentDescription attachments[2] = {
        {
            .format = wsi->format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        },
        {
            .format = wsi->depth_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        },
    };

    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentReference depth_ref = {
        .attachment = 1,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
        .pDepthStencilAttachment = &depth_ref,
    };

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };

    return vkCreateRenderPass(pb_context_device(wsi->context), &render_pass_info, NULL, &wsi->render_pass) ==
        VK_SUCCESS;
}

static bool create_framebuffers(pb_example_wsi *wsi)
{
    wsi->framebuffers = calloc(wsi->image_count, sizeof(*wsi->framebuffers));
    if (!wsi->framebuffers) {
        return false;
    }

    for (uint32_t i = 0; i < wsi->image_count; ++i) {
        VkImageView attachments[] = { wsi->image_views[i], wsi->depth_view };
        VkFramebufferCreateInfo fb_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = wsi->render_pass,
            .attachmentCount = 2,
            .pAttachments = attachments,
            .width = wsi->extent.width,
            .height = wsi->extent.height,
            .layers = 1,
        };

        if (vkCreateFramebuffer(pb_context_device(wsi->context), &fb_info, NULL, &wsi->framebuffers[i]) !=
            VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

static void destroy_swapchain_resources(pb_example_wsi *wsi)
{
    VkDevice device = pb_context_device(wsi->context);

    destroy_depth_resources(wsi);

    if (wsi->framebuffers) {
        for (uint32_t i = 0; i < wsi->image_count; ++i) {
            if (wsi->framebuffers[i]) {
                vkDestroyFramebuffer(device, wsi->framebuffers[i], NULL);
            }
        }
        free(wsi->framebuffers);
        wsi->framebuffers = NULL;
    }

    if (wsi->image_views) {
        for (uint32_t i = 0; i < wsi->image_count; ++i) {
            if (wsi->image_views[i]) {
                vkDestroyImageView(device, wsi->image_views[i], NULL);
            }
        }
        free(wsi->image_views);
        wsi->image_views = NULL;
    }

    if (wsi->images) {
        free(wsi->images);
        wsi->images = NULL;
    }

    if (wsi->swapchain) {
        vkDestroySwapchainKHR(device, wsi->swapchain, NULL);
        wsi->swapchain = VK_NULL_HANDLE;
    }

    wsi->image_count = 0;
}

static bool choose_surface_format(
    VkPhysicalDevice physical_device,
    VkSurfaceKHR surface,
    VkSurfaceFormatKHR *out)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &count, NULL);
    if (count == 0) {
        return false;
    }

    VkSurfaceFormatKHR *formats = calloc(count, sizeof(*formats));
    if (!formats) {
        return false;
    }

    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &count, formats);

    *out = formats[0];
    for (uint32_t i = 0; i < count; ++i) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            *out = formats[i];
            break;
        }
    }

    free(formats);
    return true;
}

static bool choose_present_mode(
    VkPhysicalDevice physical_device,
    VkSurfaceKHR surface,
    VkPresentModeKHR *out)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, NULL);
    if (count == 0) {
        return false;
    }

    VkPresentModeKHR *modes = calloc(count, sizeof(*modes));
    if (!modes) {
        return false;
    }

    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, modes);

    *out = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < count; ++i) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
            *out = VK_PRESENT_MODE_MAILBOX_KHR;
            break;
        }
    }

    free(modes);
    return true;
}

static VkExtent2D choose_extent(
    VkPhysicalDevice physical_device,
    VkSurfaceKHR surface,
    uint32_t width,
    uint32_t height)
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &caps);

    if (caps.currentExtent.width != UINT32_MAX) {
        return caps.currentExtent;
    }

    VkExtent2D extent = {width, height};
    extent.width = extent.width < caps.minImageExtent.width ? caps.minImageExtent.width : extent.width;
    extent.width = extent.width > caps.maxImageExtent.width ? caps.maxImageExtent.width : extent.width;
    extent.height = extent.height < caps.minImageExtent.height ? caps.minImageExtent.height : extent.height;
    extent.height = extent.height > caps.maxImageExtent.height ? caps.maxImageExtent.height : extent.height;
    return extent;
}

static bool create_swapchain_internal(pb_example_wsi *wsi, uint32_t width, uint32_t height)
{
    VkPhysicalDevice physical_device = pb_context_physical_device(wsi->context);
    VkDevice device = pb_context_device(wsi->context);

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, wsi->surface, &caps);

    VkSurfaceFormatKHR surface_format;
    VkPresentModeKHR present_mode;
    if (!choose_surface_format(physical_device, wsi->surface, &surface_format) ||
        !choose_present_mode(physical_device, wsi->surface, &present_mode)) {
        return false;
    }

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    wsi->format = surface_format.format;
    wsi->extent = choose_extent(physical_device, wsi->surface, width, height);

    uint32_t queue_family_indices[] = {
        pb_context_graphics_queue_family(wsi->context),
        pb_context_present_queue_family(wsi->context),
    };

    VkSwapchainCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = wsi->surface,
        .minImageCount = image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = wsi->extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
    };

    if (pb_context_graphics_queue_family(wsi->context) != pb_context_present_queue_family(wsi->context)) {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = queue_family_indices;
    }

    if (vkCreateSwapchainKHR(device, &create_info, NULL, &wsi->swapchain) != VK_SUCCESS) {
        return false;
    }

    vkGetSwapchainImagesKHR(device, wsi->swapchain, &wsi->image_count, NULL);
    wsi->images = calloc(wsi->image_count, sizeof(*wsi->images));
    wsi->image_views = calloc(wsi->image_count, sizeof(*wsi->image_views));
    if (!wsi->images || !wsi->image_views) {
        return false;
    }

    vkGetSwapchainImagesKHR(device, wsi->swapchain, &wsi->image_count, wsi->images);

    for (uint32_t i = 0; i < wsi->image_count; ++i) {
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = wsi->images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = wsi->format,
            .components = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };

        if (vkCreateImageView(device, &view_info, NULL, &wsi->image_views[i]) != VK_SUCCESS) {
            return false;
        }
    }

    if (!create_depth_resources(wsi)) {
        return false;
    }

    return create_framebuffers(wsi);
}

static bool recreate_if_needed(pb_example_wsi *wsi)
{
    if (!wsi->framebuffer_resized) {
        return true;
    }

    vkDeviceWaitIdle(pb_context_device(wsi->context));
    destroy_swapchain_resources(wsi);

    if (!create_swapchain_internal(wsi, wsi->width, wsi->height)) {
        return false;
    }

    wsi->framebuffer_resized = false;
    return true;
}

static bool init_swapchain(pb_example_wsi *wsi)
{
    VkSurfaceFormatKHR surface_format;
    if (!choose_surface_format(
            pb_context_physical_device(wsi->context),
            wsi->surface,
            &surface_format)) {
        return false;
    }
    wsi->format = surface_format.format;

    if (!choose_depth_format(pb_context_physical_device(wsi->context), &wsi->depth_format)) {
        return false;
    }

    if (!create_render_pass(wsi)) {
        return false;
    }

    if (!create_swapchain_internal(wsi, wsi->width, wsi->height)) {
        return false;
    }

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = pb_context_graphics_queue_family(wsi->context),
    };

    if (vkCreateCommandPool(pb_context_device(wsi->context), &pool_info, NULL, &wsi->command_pool) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = wsi->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = PB_MAX_FRAMES_IN_FLIGHT,
    };

    if (vkAllocateCommandBuffers(pb_context_device(wsi->context), &alloc_info, wsi->command_buffers) != VK_SUCCESS) {
        return false;
    }

    VkSemaphoreCreateInfo sem_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };

    for (uint32_t i = 0; i < PB_MAX_FRAMES_IN_FLIGHT; ++i) {
        if (vkCreateSemaphore(pb_context_device(wsi->context), &sem_info, NULL, &wsi->image_available[i]) !=
                VK_SUCCESS ||
            vkCreateSemaphore(pb_context_device(wsi->context), &sem_info, NULL, &wsi->render_finished[i]) !=
                VK_SUCCESS ||
            vkCreateFence(pb_context_device(wsi->context), &fence_info, NULL, &wsi->in_flight[i]) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

static void shutdown_swapchain(pb_example_wsi *wsi)
{
    if (!pb_context_device_ready(wsi->context)) {
        return;
    }

    VkDevice device = pb_context_device(wsi->context);
    vkDeviceWaitIdle(device);

    for (uint32_t i = 0; i < PB_MAX_FRAMES_IN_FLIGHT; ++i) {
        if (wsi->in_flight[i]) {
            vkDestroyFence(device, wsi->in_flight[i], NULL);
        }
        if (wsi->image_available[i]) {
            vkDestroySemaphore(device, wsi->image_available[i], NULL);
        }
        if (wsi->render_finished[i]) {
            vkDestroySemaphore(device, wsi->render_finished[i], NULL);
        }
    }

    if (wsi->command_pool) {
        vkDestroyCommandPool(device, wsi->command_pool, NULL);
        wsi->command_pool = VK_NULL_HANDLE;
    }

    destroy_swapchain_resources(wsi);

    if (wsi->render_pass) {
        vkDestroyRenderPass(device, wsi->render_pass, NULL);
        wsi->render_pass = VK_NULL_HANDLE;
    }

    if (wsi->stats_query_pool) {
        pb_rhi_query_pool_destroy(wsi->context, wsi->stats_query_pool);
        wsi->stats_query_pool = NULL;
    }
}

pb_example_wsi *pb_example_wsi_create(const pb_example_wsi_desc *desc)
{
    if (!desc || !desc->context || !desc->title) {
        return NULL;
    }

    pb_example_wsi *wsi = calloc(1, sizeof(*wsi));
    if (!wsi) {
        return NULL;
    }

    wsi->context = desc->context;
    wsi->width = desc->width ? desc->width : 1280;
    wsi->height = desc->height ? desc->height : 720;

    if (!glfwInit()) {
        free(wsi);
        return NULL;
    }

    if (!glfwVulkanSupported()) {
        glfwTerminate();
        free(wsi);
        return NULL;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    wsi->window = glfwCreateWindow(
        (int)wsi->width,
        (int)wsi->height,
        desc->title,
        NULL,
        NULL);
    if (!wsi->window) {
        glfwTerminate();
        free(wsi);
        return NULL;
    }

    glfwSetWindowUserPointer(wsi->window, wsi);
    glfwSetFramebufferSizeCallback(wsi->window, framebuffer_size_callback);

    if (glfwCreateWindowSurface(pb_context_instance(wsi->context), wsi->window, NULL, &wsi->surface) != VK_SUCCESS) {
        glfwDestroyWindow(wsi->window);
        glfwTerminate();
        free(wsi);
        return NULL;
    }

    if (!pb_context_device_ready(wsi->context)) {
        if (!pb_context_init_device(wsi->context, wsi->surface)) {
            vkDestroySurfaceKHR(pb_context_instance(wsi->context), wsi->surface, NULL);
            glfwDestroyWindow(wsi->window);
            glfwTerminate();
            free(wsi);
            return NULL;
        }
    }

    if (!init_swapchain(wsi)) {
        vkDestroySurfaceKHR(pb_context_instance(wsi->context), wsi->surface, NULL);
        glfwDestroyWindow(wsi->window);
        glfwTerminate();
        free(wsi);
        return NULL;
    }

    if (desc->title) {
        snprintf(wsi->base_title, sizeof(wsi->base_title), "%s", desc->title);
    }

    if (desc->enable_stats) {
        pb_example_wsi_set_stats_enabled(wsi, true);
    }

    return wsi;
}

void pb_example_wsi_destroy(pb_example_wsi *wsi)
{
    if (!wsi) {
        return;
    }

    shutdown_swapchain(wsi);

    if (wsi->surface) {
        vkDestroySurfaceKHR(pb_context_instance(wsi->context), wsi->surface, NULL);
    }

    if (wsi->window) {
        glfwDestroyWindow(wsi->window);
    }

    glfwTerminate();
    free(wsi);
}

void pb_example_wsi_poll(pb_example_wsi *wsi)
{
    if (wsi) {
        glfwPollEvents();
    }
}

bool pb_example_wsi_should_close(const pb_example_wsi *wsi)
{
    return !wsi || glfwWindowShouldClose(wsi->window);
}

void *pb_example_wsi_window(const pb_example_wsi *wsi)
{
    return wsi ? wsi->window : NULL;
}

bool pb_example_wsi_begin_frame(pb_example_wsi *wsi, float r, float g, float b, float a)
{
    if (!wsi) {
        return false;
    }

    int fb_width = 0;
    int fb_height = 0;
    glfwGetFramebufferSize(wsi->window, &fb_width, &fb_height);
    if (fb_width == 0 || fb_height == 0) {
        return false;
    }

    wsi->width = (uint32_t)fb_width;
    wsi->height = (uint32_t)fb_height;

    if (!recreate_if_needed(wsi)) {
        return false;
    }

    /* Viewport / render area must match the active swapchain extent. */
    if (wsi->extent.width == 0 || wsi->extent.height == 0) {
        return false;
    }

    wsi->clear_color[0] = r;
    wsi->clear_color[1] = g;
    wsi->clear_color[2] = b;
    wsi->clear_color[3] = a;

    if (wsi->stats_enabled) {
        wsi->stats_wall_start_ns = pb_bench_now_ns();
    }

    vkWaitForFences(
        pb_context_device(wsi->context),
        1,
        &wsi->in_flight[wsi->frame_index],
        VK_TRUE,
        UINT64_MAX);

    if (wsi->stats_enabled && wsi->stats_query_pool) {
        if (wsi->stats_have_gpu_sample) {
            uint64_t ticks[PB_RHI_TS_QUERY_COUNT] = {0};
            if (pb_rhi_query_pool_read_timestamps(
                    wsi->context,
                    wsi->stats_query_pool,
                    ticks,
                    PB_RHI_TS_QUERY_COUNT)) {
                pb_rhi_query_pool_fill_frame(
                    wsi->context,
                    wsi->stats_query_pool,
                    ticks,
                    PB_RHI_TS_QUERY_COUNT,
                    &wsi->stats_bench_frame);
            }

            const uint64_t submit_time = wsi->stats_submit_time_ns[wsi->frame_index];
            if (submit_time > 0) {
                const uint64_t now = pb_bench_now_ns();
                if (now > submit_time) {
                    wsi->stats_bench_frame.cpu_submit_to_idle_ns = now - submit_time;
                }
            }
        }
    }

    VkResult acquire_result = vkAcquireNextImageKHR(
        pb_context_device(wsi->context),
        wsi->swapchain,
        UINT64_MAX,
        wsi->image_available[wsi->frame_index],
        VK_NULL_HANDLE,
        &wsi->current_image);

    if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
        wsi->framebuffer_resized = true;
        return false;
    }
    if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
        return false;
    }

    vkResetFences(pb_context_device(wsi->context), 1, &wsi->in_flight[wsi->frame_index]);

    VkCommandBuffer cmd = wsi->command_buffers[wsi->frame_index];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
        return false;
    }

    if (wsi->stats_enabled && wsi->stats_query_pool) {
        pb_rhi_query_pool_cmd_reset(cmd, wsi->stats_query_pool);
        pb_rhi_query_pool_write_timestamp(
            cmd,
            wsi->stats_query_pool,
            PB_RHI_TS_CMD_START,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    }

    VkClearValue clears[2] = {
        { .color = { { wsi->clear_color[0], wsi->clear_color[1], wsi->clear_color[2], wsi->clear_color[3] } } },
        { .depthStencil = { 1.0f, 0 } },
    };
    VkRenderPassBeginInfo rp_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = wsi->render_pass,
        .framebuffer = wsi->framebuffers[wsi->current_image],
        .renderArea = { .extent = wsi->extent },
        .clearValueCount = 2,
        .pClearValues = clears,
    };

    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    if (wsi->stats_enabled && wsi->stats_query_pool) {
        pb_rhi_query_pool_write_timestamp(
            cmd,
            wsi->stats_query_pool,
            PB_RHI_TS_RENDER_PASS_START,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }

    return true;
}

VkRenderPass pb_example_wsi_render_pass(const pb_example_wsi *wsi)
{
    return wsi ? wsi->render_pass : VK_NULL_HANDLE;
}

VkExtent2D pb_example_wsi_extent(const pb_example_wsi *wsi)
{
    VkExtent2D extent = {0, 0};
    if (wsi) {
        extent = wsi->extent;
    }
    return extent;
}

VkCommandBuffer pb_example_wsi_command_buffer(const pb_example_wsi *wsi)
{
    if (!wsi) {
        return VK_NULL_HANDLE;
    }
    return wsi->command_buffers[wsi->frame_index];
}

bool pb_example_wsi_end_frame(pb_example_wsi *wsi)
{
    if (!wsi) {
        return false;
    }

    VkCommandBuffer cmd = wsi->command_buffers[wsi->frame_index];

    if (wsi->stats_enabled && wsi->stats_query_pool) {
        pb_rhi_query_pool_write_timestamp(
            cmd,
            wsi->stats_query_pool,
            PB_RHI_TS_RENDER_PASS_END,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }

    vkCmdEndRenderPass(cmd);

    if (wsi->stats_enabled && wsi->stats_query_pool) {
        pb_rhi_query_pool_write_timestamp(
            cmd,
            wsi->stats_query_pool,
            PB_RHI_TS_CMD_END,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        return false;
    }

    const uint64_t submit_start_ns =
        wsi->stats_enabled ? pb_bench_now_ns() : 0;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &wsi->image_available[wsi->frame_index],
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &wsi->command_buffers[wsi->frame_index],
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &wsi->render_finished[wsi->frame_index],
    };

    if (vkQueueSubmit(pb_context_graphics_queue(wsi->context), 1, &submit_info, wsi->in_flight[wsi->frame_index]) !=
        VK_SUCCESS) {
        return false;
    }

    if (wsi->stats_enabled) {
        wsi->stats_submit_time_ns[wsi->frame_index] = submit_start_ns;
        wsi->stats_have_gpu_sample = true;
    }

    const uint64_t present_start_ns = wsi->stats_enabled ? pb_bench_now_ns() : 0;

    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &wsi->render_finished[wsi->frame_index],
        .swapchainCount = 1,
        .pSwapchains = &wsi->swapchain,
        .pImageIndices = &wsi->current_image,
    };

    VkResult present_result = vkQueuePresentKHR(pb_context_present_queue(wsi->context), &present_info);
    if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
        wsi->framebuffer_resized = true;
    } else if (present_result != VK_SUCCESS) {
        return false;
    }

    if (wsi->stats_enabled) {
        const uint64_t present_end_ns = pb_bench_now_ns();
        const uint64_t wall_frame_ns =
            wsi->stats_wall_start_ns > 0 && present_end_ns > wsi->stats_wall_start_ns
            ? present_end_ns - wsi->stats_wall_start_ns
            : 0;
        const uint64_t cpu_present_ns =
            present_start_ns > 0 && present_end_ns > present_start_ns ? present_end_ns - present_start_ns : 0;

        pb_frame_metrics_from_bench_frame(
            &wsi->stats_bench_frame,
            wall_frame_ns,
            cpu_present_ns,
            PB_FRAME_BUDGET_60HZ_NS,
            &wsi->stats_last_metrics);

        const double now_s = glfwGetTime();
        pb_frame_metrics_accumulator_push(&wsi->stats_accumulator, now_s, wall_frame_ns);
        if (wsi->stats_accumulator.wall_fps > 0.0) {
            wsi->stats_last_metrics.wall_fps = wsi->stats_accumulator.wall_fps;
        }
    }

    wsi->frame_index = (wsi->frame_index + 1) % PB_MAX_FRAMES_IN_FLIGHT;
    return true;
}

void pb_example_wsi_set_stats_enabled(pb_example_wsi *wsi, bool enabled)
{
    if (!wsi) {
        return;
    }

    wsi->stats_enabled = enabled;
    if (!enabled) {
        return;
    }

    if (!wsi->stats_query_pool && pb_context_device_ready(wsi->context)) {
        if (!pb_rhi_query_pool_create(wsi->context, false, &wsi->stats_query_pool)) {
            wsi->stats_enabled = false;
            return;
        }
    }

    pb_frame_metrics_accumulator_init(&wsi->stats_accumulator, 1.0);
    pb_bench_frame_zero(&wsi->stats_bench_frame);
    pb_frame_metrics_zero(&wsi->stats_last_metrics);
}

bool pb_example_wsi_stats_enabled(const pb_example_wsi *wsi)
{
    return wsi && wsi->stats_enabled;
}

bool pb_example_wsi_last_metrics(const pb_example_wsi *wsi, pb_frame_metrics *out)
{
    if (!wsi || !out || !wsi->stats_enabled) {
        return false;
    }

    *out = wsi->stats_last_metrics;
    return true;
}

bool pb_example_wsi_update_stats_title(pb_example_wsi *wsi)
{
    if (!wsi || !wsi->stats_enabled || !wsi->window) {
        return false;
    }

    char overlay[160];
    if (pb_frame_metrics_format_overlay(&wsi->stats_last_metrics, overlay, sizeof(overlay)) <= 0) {
        return false;
    }

    char title[320];
    snprintf(title, sizeof(title), "%s | %s", wsi->base_title, overlay);
    glfwSetWindowTitle(wsi->window, title);
    return true;
}
