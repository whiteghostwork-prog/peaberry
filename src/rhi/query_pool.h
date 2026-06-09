/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_RHI_QUERY_POOL_H
#define PEABERRY_RHI_QUERY_POOL_H

#include "peaberry/peaberry_bench.h"

struct pb_rhi_query_pool {
    VkQueryPool handle;
    float timestamp_period_ns;
    bool timestamps_supported;
    bool detailed;
    uint32_t query_count;
};

#endif
