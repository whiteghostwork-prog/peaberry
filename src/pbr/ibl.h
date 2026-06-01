#ifndef PEABERRY_PBR_IBL_H
#define PEABERRY_PBR_IBL_H

#include "peaberry/peaberry.h"
#include "rhi/cubemap.h"
#include "rhi/texture.h"

#include <stdbool.h>

typedef struct pb_ibl_environment {
    pb_rhi_texture brdf_lut;
    pb_rhi_cubemap irradiance;
    pb_rhi_cubemap prefilter;
    float prefilter_max_lod;
} pb_ibl_environment;

typedef struct pb_ibl_environment_desc {
    pb_context *context;
    const char *equirect_hdr_path;
    const char *shader_dir;
} pb_ibl_environment_desc;

bool pb_ibl_environment_create(const pb_ibl_environment_desc *desc, pb_ibl_environment *out_env);
void pb_ibl_environment_destroy(pb_context *context, pb_ibl_environment *env);

#endif
