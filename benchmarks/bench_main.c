/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Headless GPU benchmark runner (Phase 7.2).
 */

#include "bench_baseline.h"
#include "bench_stats.h"
#include "bench_target.h"
#include "scenario.h"

#include "peaberry/peaberry.h"
#include "peaberry/peaberry_bench.h"
#include "peaberry/peaberry_frame_metrics.h"
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
    bool detailed;
    bool show_fps;
    const char *baseline_path;
    float tolerance_percent;
    const char *scenario_name;
    const char *scenario_arg;
} pb_bench_config;

typedef struct pb_bench_metric_set {
    pb_bench_stats gpu_total_ns;
    pb_bench_stats gpu_render_pass_ns;
    pb_bench_stats gpu_vertex_ns;
    pb_bench_stats gpu_fragment_ns;
    pb_bench_stats gpu_transfer_ns;
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
        "  --detailed            Pipeline-stage GPU timestamps (vertex/fragment/transfer)\n"
        "  --fps                 Include derived FPS in output (from frame times)\n"
        "  --compare PATH        Compare p95 against baseline JSON\n"
        "  --baseline PATH       Alias for --compare\n"
        "  --tolerance PCT       Regression tolerance percent (default 5)\n",
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

enum {
    PB_BENCH_OPT_COMPARE = 1001,
    PB_BENCH_OPT_DETAILED = 1002,
    PB_BENCH_OPT_FPS = 1003,
};

static bool parse_config(int argc, char **argv, pb_bench_config *cfg)
{
    static struct option long_opts[] = {
        { "width", required_argument, NULL, 'w' },
        { "height", required_argument, NULL, 'h' },
        { "frames", required_argument, NULL, 'f' },
        { "warmup", required_argument, NULL, 'u' },
        { "json", no_argument, NULL, 'j' },
        { "baseline", required_argument, NULL, 'b' },
        { "compare", required_argument, NULL, PB_BENCH_OPT_COMPARE },
        { "detailed", no_argument, NULL, PB_BENCH_OPT_DETAILED },
        { "fps", no_argument, NULL, PB_BENCH_OPT_FPS },
        { "tolerance", required_argument, NULL, 't' },
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
    while ((opt = getopt_long(argc, argv, "w:h:f:u:jb:t:", long_opts, NULL)) != -1) {
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
        case PB_BENCH_OPT_COMPARE:
            cfg->baseline_path = optarg;
            break;
        case PB_BENCH_OPT_DETAILED:
            cfg->detailed = true;
            break;
        case PB_BENCH_OPT_FPS:
            cfg->show_fps = true;
            break;
        case 't':
            cfg->tolerance_percent = strtof(optarg, NULL);
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

static bool run_benchmark(
    const pb_bench_config *cfg,
    pb_bench_metric_set *metrics,
    pb_bench_scenario_info *info,
    char *gpu_name,
    size_t gpu_name_size)
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

    if (gpu_name && gpu_name_size > 0) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pb_context_physical_device(context), &props);
        snprintf(gpu_name, gpu_name_size, "%s", props.deviceName);
    }

    const VkExtent2D extent = { cfg->width, cfg->height };
    pb_bench_scenario scenario = {0};
    if (!init_scenario(&scenario, cfg, context, extent)) {
        pb_context_destroy(context);
        return false;
    }

    pb_bench_target *target = NULL;
    if (!pb_bench_target_create(&target, context, extent, &scenario, cfg->detailed)) {
        fprintf(stderr, "failed to create benchmark render target\n");
        pb_context_destroy(context);
        return false;
    }

    *info = scenario.info;

    uint64_t *gpu_total = calloc(cfg->sample_frames, sizeof(*gpu_total));
    uint64_t *gpu_render_pass = calloc(cfg->sample_frames, sizeof(*gpu_render_pass));
    uint64_t *gpu_vertex = cfg->detailed ? calloc(cfg->sample_frames, sizeof(*gpu_vertex)) : NULL;
    uint64_t *gpu_fragment = cfg->detailed ? calloc(cfg->sample_frames, sizeof(*gpu_fragment)) : NULL;
    uint64_t *gpu_transfer = cfg->detailed ? calloc(cfg->sample_frames, sizeof(*gpu_transfer)) : NULL;
    uint64_t *cpu_submit = calloc(cfg->sample_frames, sizeof(*cpu_submit));
    if (!gpu_total || !gpu_render_pass || !cpu_submit ||
        (cfg->detailed && (!gpu_vertex || !gpu_fragment || !gpu_transfer))) {
        free(gpu_total);
        free(gpu_render_pass);
        free(gpu_vertex);
        free(gpu_fragment);
        free(gpu_transfer);
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
            free(gpu_vertex);
            free(gpu_fragment);
            free(gpu_transfer);
            free(cpu_submit);
            pb_bench_target_destroy(target);
            pb_context_destroy(context);
            return false;
        }

        if (frame >= cfg->warmup_frames) {
            gpu_total[sample_index] = bench_frame.gpu_total_ns;
            gpu_render_pass[sample_index] = bench_frame.gpu_render_pass_ns;
            cpu_submit[sample_index] = bench_frame.cpu_submit_to_idle_ns;
            if (cfg->detailed) {
                gpu_vertex[sample_index] = bench_frame.gpu_vertex_ns;
                gpu_fragment[sample_index] = bench_frame.gpu_fragment_ns;
                gpu_transfer[sample_index] = bench_frame.gpu_transfer_ns;
            }
            sample_index++;
        }
    }

    pb_bench_stats_compute(gpu_total, cfg->sample_frames, &metrics->gpu_total_ns);
    pb_bench_stats_compute(gpu_render_pass, cfg->sample_frames, &metrics->gpu_render_pass_ns);
    pb_bench_stats_compute(cpu_submit, cfg->sample_frames, &metrics->cpu_submit_to_idle_ns);
    if (cfg->detailed) {
        pb_bench_stats_compute(gpu_vertex, cfg->sample_frames, &metrics->gpu_vertex_ns);
        pb_bench_stats_compute(gpu_fragment, cfg->sample_frames, &metrics->gpu_fragment_ns);
        pb_bench_stats_compute(gpu_transfer, cfg->sample_frames, &metrics->gpu_transfer_ns);
    }

    free(gpu_total);
    free(gpu_render_pass);
    free(gpu_vertex);
    free(gpu_fragment);
    free(gpu_transfer);
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

static void print_fps_header(void)
{
    printf(
        "  %*s %10s %10s %10s %10s %10s %10s\n",
        PB_BENCH_COL_METRIC,
        "",
        "min",
        "p50",
        "p95",
        "p99",
        "max",
        "mean");
}

static void print_fps_row(const char *label, const pb_bench_stats *stats)
{
    printf(
        "  %-*s %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f\n",
        PB_BENCH_COL_METRIC,
        label,
        pb_frame_metrics_fps_from_ns(stats->min),
        pb_frame_metrics_fps_from_ns(stats->p50),
        pb_frame_metrics_fps_from_ns(stats->p95),
        pb_frame_metrics_fps_from_ns(stats->p99),
        pb_frame_metrics_fps_from_ns(stats->max),
        pb_frame_metrics_fps_from_ns(stats->mean));
}

static void print_human_report(
    const pb_bench_config *cfg,
    const pb_bench_metric_set *metrics,
    const pb_bench_scenario_info *info)
{
    printf("peaberry_bench: %s (%ux%u, warmup=%u, samples=%u%s%s)\n",
        cfg->scenario_name,
        cfg->width,
        cfg->height,
        cfg->warmup_frames,
        cfg->sample_frames,
        cfg->detailed ? ", detailed" : "",
        cfg->show_fps ? ", fps" : "");

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
    if (cfg->detailed) {
        print_stats_row("gpu_vertex_ns", &metrics->gpu_vertex_ns);
        print_stats_row("gpu_fragment_ns", &metrics->gpu_fragment_ns);
        print_stats_row("gpu_transfer_ns", &metrics->gpu_transfer_ns);
    }
    print_stats_row("cpu_submit_to_idle_ns", &metrics->cpu_submit_to_idle_ns);

    if (cfg->show_fps) {
        printf("\n  derived fps (1e9 / frame_ns)\n");
        print_fps_header();
        print_fps_row("gpu_total", &metrics->gpu_total_ns);
        print_fps_row("gpu_render_pass", &metrics->gpu_render_pass_ns);
        if (cfg->detailed) {
            print_fps_row("gpu_vertex", &metrics->gpu_vertex_ns);
            print_fps_row("gpu_fragment", &metrics->gpu_fragment_ns);
            print_fps_row("gpu_transfer", &metrics->gpu_transfer_ns);
        }
        print_fps_row("cpu_submit_to_idle", &metrics->cpu_submit_to_idle_ns);
    }

    printf("\n");
}

static void print_json_fps_field(FILE *out, const char *name, const pb_bench_stats *stats, bool trailing_comma)
{
    fprintf(
        out,
        "      \"%s\": {\"min\": %.3f, \"p50\": %.3f, \"p95\": %.3f, \"p99\": %.3f, \"max\": %.3f, \"mean\": %.3f}",
        name,
        pb_frame_metrics_fps_from_ns(stats->min),
        pb_frame_metrics_fps_from_ns(stats->p50),
        pb_frame_metrics_fps_from_ns(stats->p95),
        pb_frame_metrics_fps_from_ns(stats->p99),
        pb_frame_metrics_fps_from_ns(stats->max),
        pb_frame_metrics_fps_from_ns(stats->mean));
    if (trailing_comma) {
        fputc(',', out);
    }
    fputc('\n', out);
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
    const pb_bench_scenario_info *info,
    const char *gpu_name)
{
    FILE *out = cfg->json_output ? stdout : stdout;
    fprintf(out, "{\n");
    fprintf(out, "  \"version\": %u,\n", PB_BENCH_BASELINE_VERSION);
    if (gpu_name && gpu_name[0] != '\0') {
        fprintf(out, "  \"gpu\": \"%s\",\n", gpu_name);
    }
    fprintf(out, "  \"mode\": \"headless\",\n");
    fprintf(out, "  \"scenario\": \"%s\",\n", cfg->scenario_name);
    if (cfg->scenario_arg) {
        fprintf(out, "  \"scenario_arg\": \"%s\",\n", cfg->scenario_arg);
    }
    fprintf(out, "  \"resolution\": {\"width\": %u, \"height\": %u},\n", cfg->width, cfg->height);
    fprintf(out, "  \"frames\": {\"warmup\": %u, \"samples\": %u},\n", cfg->warmup_frames, cfg->sample_frames);
    fprintf(out, "  \"detailed\": %s,\n", cfg->detailed ? "true" : "false");
    fprintf(out, "  \"fps\": %s,\n", cfg->show_fps ? "true" : "false");
    fprintf(out, "  \"info\": {\n");
    fprintf(out, "    \"draw_calls\": %u,\n", info->draw_calls);
    fprintf(out, "    \"index_count\": %u,\n", info->index_count);
    fprintf(out, "    \"material_count\": %u,\n", info->material_count);
    fprintf(out, "    \"pixels_shaded\": %u\n", info->pixels_shaded);
    fprintf(out, "  },\n");
    fprintf(out, "  \"stats\": {\n");
    print_json_uint64_field(out, "gpu_total_ns", &metrics->gpu_total_ns, true);
    print_json_uint64_field(out, "gpu_render_pass_ns", &metrics->gpu_render_pass_ns, true);
    if (cfg->detailed) {
        print_json_uint64_field(out, "gpu_vertex_ns", &metrics->gpu_vertex_ns, true);
        print_json_uint64_field(out, "gpu_fragment_ns", &metrics->gpu_fragment_ns, true);
        print_json_uint64_field(out, "gpu_transfer_ns", &metrics->gpu_transfer_ns, true);
    }
    print_json_uint64_field(out, "cpu_submit_to_idle_ns", &metrics->cpu_submit_to_idle_ns, cfg->show_fps);
    fprintf(out, "  }");
    if (cfg->show_fps) {
        fprintf(out, ",\n  \"fps_derived\": {\n");
        print_json_fps_field(out, "gpu_total", &metrics->gpu_total_ns, true);
        print_json_fps_field(out, "gpu_render_pass", &metrics->gpu_render_pass_ns, true);
        if (cfg->detailed) {
            print_json_fps_field(out, "gpu_vertex", &metrics->gpu_vertex_ns, true);
            print_json_fps_field(out, "gpu_fragment", &metrics->gpu_fragment_ns, true);
            print_json_fps_field(out, "gpu_transfer", &metrics->gpu_transfer_ns, true);
        }
        print_json_fps_field(out, "cpu_submit_to_idle", &metrics->cpu_submit_to_idle_ns, false);
        fprintf(out, "  }\n");
    } else {
        fputc('\n', out);
    }
    fprintf(out, "}\n");
}

static bool check_baseline(
    const pb_bench_config *cfg,
    const pb_bench_metric_set *metrics,
    const char *gpu_name)
{
    if (!cfg->baseline_path) {
        return true;
    }

    pb_bench_baseline baseline;
    if (!pb_bench_baseline_load(cfg->baseline_path, &baseline)) {
        return false;
    }

    const pb_bench_compare_run run = {
        .scenario_name = cfg->scenario_name,
        .scenario_arg = cfg->scenario_arg,
        .width = cfg->width,
        .height = cfg->height,
        .gpu_name = gpu_name,
        .gpu_render_pass_p95 = metrics->gpu_render_pass_ns.p95,
    };

    return pb_bench_baseline_compare(&baseline, &run, cfg->tolerance_percent, NULL);
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
    char gpu_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {0};
    if (!run_benchmark(&cfg, &metrics, &info, gpu_name, sizeof(gpu_name))) {
        return EXIT_FAILURE;
    }

    if (cfg.json_output) {
        print_json_report(&cfg, &metrics, &info, gpu_name);
    } else {
        print_human_report(&cfg, &metrics, &info);
    }

    if (!check_baseline(&cfg, &metrics, gpu_name)) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
