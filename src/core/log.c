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

#include "core/log.h"

#include <stdarg.h>
#include <stdio.h>

static const char *level_prefix(pb_log_level level)
{
    switch (level) {
    case PB_LOG_LEVEL_ERROR:
        return "error";
    case PB_LOG_LEVEL_WARN:
        return "warn";
    case PB_LOG_LEVEL_INFO:
        return "info";
    case PB_LOG_LEVEL_DEBUG:
        return "debug";
    default:
        return "?";
    }
}

void pb_log(pb_log_level level, const char *fmt, ...)
{
    va_list args;

    fprintf(stderr, "[peaberry:%s] ", level_prefix(level));
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}
