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

#ifndef PEABERRY_LOG_H
#define PEABERRY_LOG_H

typedef enum pb_log_level {
    PB_LOG_LEVEL_ERROR = 0,
    PB_LOG_LEVEL_WARN,
    PB_LOG_LEVEL_INFO,
    PB_LOG_LEVEL_DEBUG,
} pb_log_level;

void pb_log(pb_log_level level, const char *fmt, ...);

#define pb_log_error(...) pb_log(PB_LOG_LEVEL_ERROR, __VA_ARGS__)
#define pb_log_warn(...) pb_log(PB_LOG_LEVEL_WARN, __VA_ARGS__)
#define pb_log_info(...) pb_log(PB_LOG_LEVEL_INFO, __VA_ARGS__)
#define pb_log_debug(...) pb_log(PB_LOG_LEVEL_DEBUG, __VA_ARGS__)

#endif
