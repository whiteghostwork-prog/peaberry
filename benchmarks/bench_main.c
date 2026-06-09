/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Headless GPU benchmark runner (Phase 7.2).
 */

#include "bench_stats.h"
#include "bench_target.h"
#include "scenario.h"

#include "peaberry/peaberry.h"
#include "peaberry/peaberry_bench.h"
#include "peaberry/peaberry_vk.h"

#include <getopt.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

typedef struct pb_bench_config {
    uint32_t width;
    uint32_t height;
    uint32_t warmup_frames;
    uint32_t sample_frames;
    bool json_output;
    const char *baseline_path;
    float tolerance_percent;
    const char *compare_config_path;
    const char *scenario_name;
    const char *scenario_arg;
} pb_bench_config;

typedef struct pb_bench_metric_set {
    pb_bench_stats gpu_total_ns;
    pb_bench_stats gpu_render_pass_ns;
    pb_bench_stats cpu_submit_to_idle_ns;
} pb_bench_metric_set;

static void print_usage(const char *prog)
{
    fprintf(
        stderr,
        "Usage: %s [options] <scenario> [scenario-arg]\n"
        "\n"
        "Scenarios:\n"
        "  clear                 Empty render pass (clear only)\n"
        "  sphere                PBR sphere with IBL and material maps\n"
        "  gltf <path>           glTF model via PBR forward pass\n"
        "\n"
        "Options:\n"
        "  --width W             Framebuffer width (default 1920)\n"
        "  --height H            Framebuffer height (default 1080)\n"
        "  --frames N            Sample frames (default 100)\n"
        "  --warmup N            Warmup frames discarded (default 10)\n"
        "  --json                Print JSON instead of a table\n"
        "  --baseline PATH       Compare p95 against baseline JSON\n"
        "  --tolerance PCT       Regression tolerance percent (default 5)\n"
        "  --compare-config PATH Alternate pb_context config (Phase 8+)\n",
        prog);
}

static bool parse_u32(const char *text, uint32_t *out)
{
    if (!text || !out) {
        return false;
    }

    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (!end || *end != '\0' || value > UINT32_MAX) {
        return false;
    }

    *out = (uint32_t)value;
    return true;
}

static bool parse_config(int argc, char **argv, pb_bench_config *cfg)
{
    static struct option long_opts[] = {
        { "width", required_argument, NULL, 'w' },
        { "height", required_argument, NULL, 'h' },
        { "frames", required_argument, NULL, 'f' },
        { "warmup", required_argument, NULL, 'u' },
        { "json", no_argument, NULL, 'j' },
        { "baseline", required_argument, NULL, 'b' },
        { "tolerance", required_argument, NULL, 't' },
        { "compare-config", required_argument, NULL, 'c' },
        { "help", no_argument, NULL, '?' },
        { NULL, 0, NULL, 0 },
    };

    memset(cfg, 0, sizeof(*cfg));
    cfg->width = 1920;
    cfg->height = 1080;
    cfg->warmup_frames = 10;
    cfg->sample_frames = 100;
    cfg->tolerance_percent = 5.0f;

    int opt = 0;
    while ((opt = getopt_long(argc, argv, "w:h:f:u:jb:t:c:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'w':
            if (!parse_u32(optarg, &cfg->width)) {
                return false;
            }
            break;
        case 'h':
            if (!parse_u32(optarg, &cfg->height)) {
                return false;
            }
            break;
        case 'f':
            if (!parse_u32(optarg, &cfg->sample_frames) || cfg->sample_frames == 0) {
                return false;
            }
            break;
        case 'u':
            if (!parse_u32(optarg, &cfg->warmup_frames)) {
                return false;
            }
            break;
        case 'j':
            cfg->json_output = true;
            break;
        case 'b':
            cfg->baseline_path = optarg;
            break;
        case 't':
            cfg->tolerance_percent = strtof(optarg, NULL);
            break;
        case 'c':
            cfg->compare_config_path = optarg;
            break;
        default:
            return false;
        }
    }

    if (optind >= argc) {
        return false;
    }

    cfg->scenario_name = argv[optind++];
    if (optind < argc) {
        cfg->scenario_arg = argv[optind];
    }

    return true;
}

static bool init_scenario(
    pb_bench_scenario *scenario,
    const pb_bench_config *cfg,
    pb_context *context,
    VkExtent2D extent)
{
    if (strcmp(cfg->scenario_name, "clear") == 0) {
        return pb_bench_scenario_clear_init(scenario, context, extent);
    }

    if (strcmp(cfg->scenario_name, "sphere") == 0) {
        return pb_bench_scenario_sphere_init(scenario, context, extent);
    }

    if (strcmp(cfg->scenario_name, "gltf") == 0) {
        if (!cfg->scenario_arg) {
            fprintf(stderr, "gltf scenario requires a model path\n");
            return false;
        }
        return pb_bench_scenario_gltf_init(scenario, context, extent, cfg->scenario_arg);
    }

    fprintf(stderr, "unknown scenario: %s\n", cfg->scenario_name);
    return false;
}

static bool run_benchmark(const pb_bench_config *cfg, pb_bench_metric_set *metrics, pb_bench_scenario_info *info)
{
    pb_context *context = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry_bench",
            .enable_validation = false,
            .enable_surface = false,
        });
    if (!context) {
        fprintf(stderr, "failed to create Vulkan context\n");
        return false;
    }

    if (!pb_context_init_headless_device(context)) {
        fprintf(stderr, "failed to initialize headless Vulkan device\n");
        pb_context_destroy(context);
        return false;
    }

    if (cfg->compare_config_path) {
        fprintf(stderr, "note: --compare-config is reserved for Phase 8 (ignored)\n");
    }

    const VkExtent2D extent = { cfg->width, cfg->height };
    pb_bench_scenario scenario = {0};
    if (!init_scenario(&scenario, cfg, context, extent)) {
        pb_context_destroy(context);
        return false;
    }

    pb_bench_target *target = NULL;
    if (!pb_bench_target_create(&target, context, extent, &scenario)) {
        fprintf(stderr, "failed to create benchmark render target\n");
        pb_context_destroy(context);
        return false;
    }

    *info = scenario.info;

    uint64_t *gpu_total = calloc(cfg->sample_frames, sizeof(*gpu_total));
    uint64_t *gpu_render_pass = calloc(cfg->sample_frames, sizeof(*gpu_render_pass));
    uint64_t *cpu_submit = calloc(cfg->sample_frames, sizeof(*cpu_submit));
    if (!gpu_total || !gpu_render_pass || !cpu_submit) {
        free(gpu_total);
        free(gpu_render_pass);
        free(cpu_submit);
        pb_bench_target_destroy(target);
        pb_context_destroy(context);
        return false;
    }

    const uint32_t total_frames = cfg->warmup_frames + cfg->sample_frames;
    uint32_t sample_index = 0;

    for (uint32_t frame = 0; frame < total_frames; ++frame) {
        pb_bench_frame bench_frame;
        if (!pb_bench_target_run_frame(target, &bench_frame)) {
            fprintf(stderr, "benchmark frame %u failed\n", frame);
            free(gpu_total);
            free(gpu_render_pass);
            free(cpu_submit);
            pb_bench_target_destroy(target);
            pb_context_destroy(context);
            return false;
        }

        if (frame >= cfg->warmup_frames) {
            gpu_total[sample_index] = bench_frame.gpu_total_ns;
            gpu_render_pass[sample_index] = bench_frame.gpu_render_pass_ns;
            cpu_submit[sample_index] = bench_frame.cpu_submit_to_idle_ns;
            sample_index++;
        }
    }

    pb_bench_stats_compute(gpu_total, cfg->sample_frames, &metrics->gpu_total_ns);
    pb_bench_stats_compute(gpu_render_pass, cfg->sample_frames, &metrics->gpu_render_pass_ns);
    pb_bench_stats_compute(cpu_submit, cfg->sample_frames, &metrics->cpu_submit_to_idle_ns);

    free(gpu_total);
    free(gpu_render_pass);
    free(cpu_submit);
    pb_bench_target_destroy(target);
    pb_context_destroy(context);
    return true;
}

enum {
    PB_BENCH_COL_METRIC = 22,
    PB_BENCH_COL_VALUE = 10,
};

static void print_stats_header(void)
{
    printf(
        "  %*s %10s %10s %10s %10s %10s %10s %10s\n",
        PB_BENCH_COL_METRIC,
        "",
        "min",
        "p50",
        "p95",
        "p99",
        "max",
        "mean",
        "stddev");
}

static void print_stats_row(const char *label, const pb_bench_stats *stats)
{
    const double ms = 1e-6;

    printf(
        "  %-*s %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f\n",
        PB_BENCH_COL_METRIC,
        label,
        stats->min * ms,
        stats->p50 * ms,
        stats->p95 * ms,
        stats->p99 * ms,
        stats->max * ms,
        (double)stats->mean * ms,
        stats->stddev * ms);
}

static void print_human_report(
    const pb_bench_config *cfg,
    const pb_bench_metric_set *metrics,
    const pb_bench_scenario_info *info)
{
    printf("peaberry_bench: %s (%ux%u, warmup=%u, samples=%u)\n",
        cfg->scenario_name,
        cfg->width,
        cfg->height,
        cfg->warmup_frames,
        cfg->sample_frames);

    printf("\n  workload\n");
    printf("  %10s %10s %10s %10s\n", "draws", "indices", "materials", "pixels");
    printf(
        "  %10u %10u %10u %10u\n",
        info->draw_calls,
        info->index_count,
        info->material_count,
        info->pixels_shaded);

    printf("\n  timing (ms)\n");
    print_stats_header();
    print_stats_row("gpu_total_ns", &metrics->gpu_total_ns);
    print_stats_row("gpu_render_pass_ns", &metrics->gpu_render_pass_ns);
    print_stats_row("cpu_submit_to_idle_ns", &metrics->cpu_submit_to_idle_ns);
    printf("\n");
}

static void print_json_uint64_field(FILE *out, const char *name, const pb_bench_stats *stats, bool trailing_comma)
{
    fprintf(
        out,
        "      \"%s\": {\"min\": %llu, \"p50\": %llu, \"p95\": %llu, \"p99\": %llu, \"max\": %llu, \"mean\": %llu, \"stddev\": %.3f}",
        name,
        (unsigned long long)stats->min,
        (unsigned long long)stats->p50,
        (unsigned long long)stats->p95,
        (unsigned long long)stats->p99,
        (unsigned long long)stats->max,
        (unsigned long long)stats->mean,
        stats->stddev);
    if (trailing_comma) {
        fputc(',', out);
    }
    fputc('\n', out);
}

static void print_json_report(
    const pb_bench_config *cfg,
    const pb_bench_metric_set *metrics,
    const pb_bench_scenario_info *info)
{
    FILE *out = cfg->json_output ? stdout : stdout;
    fprintf(out, "{\n");
    fprintf(out, "  \"version\": 1,\n");
    fprintf(out, "  \"scenario\": \"%s\",\n", cfg->scenario_name);
    if (cfg->scenario_arg) {
        fprintf(out, "  \"scenario_arg\": \"%s\",\n", cfg->scenario_arg);
    }
    fprintf(out, "  \"resolution\": {\"width\": %u, \"height\": %u},\n", cfg->width, cfg->height);
    fprintf(out, "  \"frames\": {\"warmup\": %u, \"samples\": %u},\n", cfg->warmup_frames, cfg->sample_frames);
    fprintf(out, "  \"info\": {\n");
    fprintf(out, "    \"draw_calls\": %u,\n", info->draw_calls);
    fprintf(out, "    \"index_count\": %u,\n", info->index_count);
    fprintf(out, "    \"material_count\": %u,\n", info->material_count);
    fprintf(out, "    \"pixels_shaded\": %u\n", info->pixels_shaded);
    fprintf(out, "  },\n");
    fprintf(out, "  \"stats\": {\n");
    print_json_uint64_field(out, "gpu_total_ns", &metrics->gpu_total_ns, true);
    print_json_uint64_field(out, "gpu_render_pass_ns", &metrics->gpu_render_pass_ns, true);
    print_json_uint64_field(out, "cpu_submit_to_idle_ns", &metrics->cpu_submit_to_idle_ns, false);
    fprintf(out, "  }\n");
    fprintf(out, "}\n");
}

static bool read_baseline_p95(const char *path, uint64_t *out_p95)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "failed to open baseline: %s\n", path);
        return false;
    }

    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    if (size <= 0) {
        fclose(file);
        return false;
    }

    fseek(file, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1);
    if (!text) {
        fclose(file);
        return false;
    }

    const size_t read = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[read] = '\0';

    const char *section = strstr(text, "\"gpu_render_pass_ns\"");
    if (!section) {
        free(text);
        fprintf(stderr, "baseline JSON missing gpu_render_pass_ns\n");
        return false;
    }

    const char *p95_key = strstr(section, "\"p95\"");
    if (!p95_key) {
        free(text);
        fprintf(stderr, "baseline JSON missing gpu_render_pass_ns.p95\n");
        return false;
    }

    const char *colon = strchr(p95_key, ':');
    if (!colon) {
        free(text);
        return false;
    }

    *out_p95 = strtoull(colon + 1, NULL, 10);
    free(text);
    return true;
}

static bool check_baseline(
    const pb_bench_config *cfg,
    const pb_bench_metric_set *metrics)
{
    if (!cfg->baseline_path) {
        return true;
    }

    uint64_t baseline_p95 = 0;
    if (!read_baseline_p95(cfg->baseline_path, &baseline_p95)) {
        return false;
    }

    const uint64_t current_p95 = metrics->gpu_render_pass_ns.p95;
    if (baseline_p95 == 0) {
        return true;
    }

    const double regression =
        ((double)current_p95 - (double)baseline_p95) / (double)baseline_p95 * 100.0;
    fprintf(
        stderr,
        "baseline compare: gpu_render_pass_ns p95 current=%.3f ms baseline=%.3f ms delta=%+.2f%% (tolerance %.1f%%)\n",
        current_p95 / 1e6,
        baseline_p95 / 1e6,
        regression,
        cfg->tolerance_percent);

    if (regression > (double)cfg->tolerance_percent) {
        fprintf(stderr, "benchmark regression exceeded tolerance\n");
        return false;
    }

    return true;
}

int main(int argc, char **argv)
{
    pb_bench_config cfg;
    if (!parse_config(argc, argv, &cfg)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    pb_bench_metric_set metrics = {0};
    pb_bench_scenario_info info = {0};
    if (!run_benchmark(&cfg, &metrics, &info)) {
        return EXIT_FAILURE;
    }

    if (cfg.json_output) {
        print_json_report(&cfg, &metrics, &info);
    } else {
        print_human_report(&cfg, &metrics, &info);
    }

    if (!check_baseline(&cfg, &metrics)) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
