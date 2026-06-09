/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bench_baseline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *find_json_key(const char *text, const char *key)
{
    if (!text || !key) {
        return NULL;
    }

    char pattern[128];
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern)) {
        return NULL;
    }

    return strstr(text, pattern);
}

static bool read_json_string(const char *text, const char *key, char *out, size_t out_size)
{
    const char *key_pos = find_json_key(text, key);
    if (!key_pos || out_size == 0) {
        return false;
    }

    const char *colon = strchr(key_pos, ':');
    if (!colon) {
        return false;
    }

    const char *start = strchr(colon, '"');
    if (!start) {
        return false;
    }
    ++start;

    const char *end = strchr(start, '"');
    if (!end || end <= start) {
        return false;
    }

    const size_t len = (size_t)(end - start);
    if (len + 1 > out_size) {
        return false;
    }

    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static bool read_json_u32(const char *text, const char *key, uint32_t *out)
{
    const char *key_pos = find_json_key(text, key);
    if (!key_pos || !out) {
        return false;
    }

    const char *colon = strchr(key_pos, ':');
    if (!colon) {
        return false;
    }

    char *end = NULL;
    const unsigned long value = strtoul(colon + 1, &end, 10);
    if (!end || value > UINT32_MAX) {
        return false;
    }

    *out = (uint32_t)value;
    return true;
}

static bool read_nested_u64(const char *text, const char *section, const char *field, uint64_t *out)
{
    const char *section_pos = find_json_key(text, section);
    if (!section_pos || !out) {
        return false;
    }

    const char *field_pos = find_json_key(section_pos, field);
    if (!field_pos) {
        return false;
    }

    const char *colon = strchr(field_pos, ':');
    if (!colon) {
        return false;
    }

    char *end = NULL;
    const unsigned long long value = strtoull(colon + 1, &end, 10);
    if (!end) {
        return false;
    }

    *out = value;
    return true;
}

static bool read_file_text(const char *path, char **out_text, size_t *out_size)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "failed to open baseline: %s\n", path);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }

    const long size = ftell(file);
    if (size <= 0) {
        fclose(file);
        fprintf(stderr, "baseline file is empty: %s\n", path);
        return false;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    char *text = malloc((size_t)size + 1);
    if (!text) {
        fclose(file);
        return false;
    }

    const size_t read = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[read] = '\0';
    *out_text = text;
    *out_size = read;
    return true;
}

bool pb_bench_baseline_load(const char *path, pb_bench_baseline *out)
{
    if (!path || !out) {
        return false;
    }

    char *text = NULL;
    size_t text_size = 0;
    if (!read_file_text(path, &text, &text_size)) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    if (!read_json_u32(text, "version", &out->version)) {
        fprintf(stderr, "baseline JSON missing version\n");
        free(text);
        return false;
    }

    if (out->version != PB_BENCH_BASELINE_VERSION) {
        fprintf(stderr, "unsupported baseline version %u (expected %u)\n", out->version, PB_BENCH_BASELINE_VERSION);
        free(text);
        return false;
    }

    if (!read_json_string(text, "scenario", out->scenario, sizeof(out->scenario))) {
        fprintf(stderr, "baseline JSON missing scenario\n");
        free(text);
        return false;
    }

    read_json_string(text, "scenario_arg", out->scenario_arg, sizeof(out->scenario_arg));
    read_json_string(text, "gpu", out->gpu, sizeof(out->gpu));
    read_json_string(text, "mode", out->mode, sizeof(out->mode));

    const char *resolution = find_json_key(text, "resolution");
    if (!resolution ||
        !read_json_u32(resolution, "width", &out->width) ||
        !read_json_u32(resolution, "height", &out->height)) {
        fprintf(stderr, "baseline JSON missing resolution.width/height\n");
        free(text);
        return false;
    }

    if (!read_nested_u64(text, "gpu_render_pass_ns", "p95", &out->gpu_render_pass_p95)) {
        fprintf(stderr, "baseline JSON missing stats.gpu_render_pass_ns.p95\n");
        free(text);
        return false;
    }

    free(text);
    return true;
}

static bool strings_equal_or_empty(const char *a, const char *b)
{
    if (!a || a[0] == '\0' || !b || b[0] == '\0') {
        return true;
    }

    return strcmp(a, b) == 0;
}

bool pb_bench_baseline_compare(
    const pb_bench_baseline *baseline,
    const pb_bench_compare_run *run,
    float tolerance_percent,
    double *out_delta_percent)
{
    if (!baseline || !run) {
        return false;
    }

    if (strcmp(baseline->scenario, run->scenario_name) != 0) {
        fprintf(
            stderr,
            "baseline scenario mismatch: baseline=%s current=%s\n",
            baseline->scenario,
            run->scenario_name);
        return false;
    }

    if (baseline->width != run->width || baseline->height != run->height) {
        fprintf(
            stderr,
            "baseline resolution mismatch: baseline=%ux%u current=%ux%u\n",
            baseline->width,
            baseline->height,
            run->width,
            run->height);
        return false;
    }

    if (baseline->scenario_arg[0] != '\0' &&
        run->scenario_arg &&
        strcmp(baseline->scenario_arg, run->scenario_arg) != 0) {
        fprintf(
            stderr,
            "baseline scenario_arg mismatch: baseline=%s current=%s\n",
            baseline->scenario_arg,
            run->scenario_arg);
        return false;
    }

    if (!strings_equal_or_empty(baseline->gpu, run->gpu_name)) {
        fprintf(
            stderr,
            "warning: GPU differs from baseline (baseline=%s current=%s)\n",
            baseline->gpu,
            run->gpu_name ? run->gpu_name : "(unknown)");
    }

    if (baseline->gpu_render_pass_p95 == 0) {
        fprintf(stderr, "baseline gpu_render_pass_ns p95 is zero\n");
        return false;
    }

    const double delta =
        ((double)run->gpu_render_pass_p95 - (double)baseline->gpu_render_pass_p95) /
        (double)baseline->gpu_render_pass_p95 * 100.0;

    if (out_delta_percent) {
        *out_delta_percent = delta;
    }

    fprintf(
        stderr,
        "baseline compare: gpu_render_pass_ns p95 current=%.3f ms baseline=%.3f ms delta=%+.2f%% (tolerance %.1f%%)\n",
        run->gpu_render_pass_p95 / 1e6,
        baseline->gpu_render_pass_p95 / 1e6,
        delta,
        tolerance_percent);

    if (delta > (double)tolerance_percent) {
        fprintf(stderr, "benchmark regression exceeded tolerance\n");
        return false;
    }

    return true;
}
