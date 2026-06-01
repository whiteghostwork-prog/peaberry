#ifndef PEABERRY_RHI_OFFSCREEN_H
#define PEABERRY_RHI_OFFSCREEN_H

#include "peaberry/peaberry.h"

#include <stdbool.h>
#include <volk.h>

typedef struct pb_rhi_offscreen_target {
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkImageView color_view;
    VkExtent2D extent;
} pb_rhi_offscreen_target;

bool pb_rhi_offscreen_render_pass_create(
    pb_context *context,
    VkFormat color_format,
    VkRenderPass *out_render_pass);

void pb_rhi_offscreen_render_pass_destroy(pb_context *context, VkRenderPass render_pass);

bool pb_rhi_offscreen_framebuffer_create(
    pb_context *context,
    VkRenderPass render_pass,
    VkImageView color_view,
    VkExtent2D extent,
    VkFramebuffer *out_framebuffer);

void pb_rhi_offscreen_framebuffer_destroy(pb_context *context, VkFramebuffer framebuffer);

bool pb_rhi_offscreen_target_create(
    pb_context *context,
    VkFormat color_format,
    VkImageView color_view,
    VkExtent2D extent,
    pb_rhi_offscreen_target *target);

void pb_rhi_offscreen_target_destroy(pb_context *context, pb_rhi_offscreen_target *target);

#endif
