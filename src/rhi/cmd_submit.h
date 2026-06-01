#ifndef PEABERRY_RHI_CMD_SUBMIT_H
#define PEABERRY_RHI_CMD_SUBMIT_H

#include "peaberry/peaberry.h"

#include <stdbool.h>
#include <volk.h>

bool pb_rhi_submit_one_shot(
    pb_context *context,
    void (*record)(VkCommandBuffer cmd, void *user_data),
    void *user_data);

#endif
