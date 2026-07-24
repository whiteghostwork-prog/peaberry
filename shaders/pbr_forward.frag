#version 450

layout(set = 0, binding = 0) uniform FrameData {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
    float exposure;
    mat4 light_view;
    mat4 light_proj;
    float shadow_bias;
    float shadows_enabled;
    float shadow_bias_slope;
    float shadow_texel_size;
    float shadow_debug;
    float ibl_intensity;
} frame;

layout(set = 0, binding = 1) uniform Material {
    vec3 albedo_factor;
    float metallic_factor;
    float roughness_factor;
    float occlusion_strength;
    vec3 emissive_factor;
    float alpha_cutoff;
    float base_color_alpha;
    float alpha_mode;
    float double_sided;
    vec4 uv_transform_a[5];
    vec4 uv_transform_b[5];
} material;

#define PB_LIGHT_TYPE_DIRECTIONAL 0
#define PB_LIGHT_TYPE_POINT       1
#define PB_LIGHT_MAX              8

struct pb_light {
    vec3 position;
    float range;
    vec3 direction;
    uint  type;
    vec3 color;
    uint  shadow_map_index;   /* < PB_POINT_SHADOW_MAX: claim a cube shadow slot */
    vec4 _pad;                /* pad to 64 bytes to match the C struct / std140 stride */
};

layout(set = 0, binding = 13) uniform LightList {
    pb_light lights[PB_LIGHT_MAX];
    uint light_count;
} light_list;

/* Phase 14.2: per-point-light shadow cube array. Each cube stores normalized
 * linear distance from its owning light; sampled as samplerCubeShadow with
 * compare ref = distance/range. */
#define PB_POINT_SHADOW_MAX 4
layout(set = 0, binding = 14) uniform samplerCubeShadow u_point_shadows[PB_POINT_SHADOW_MAX];

layout(set = 0, binding = 2) uniform sampler2D u_albedo;
layout(set = 0, binding = 3) uniform sampler2D u_metallic_roughness;
layout(set = 0, binding = 4) uniform sampler2D u_normal;
layout(set = 0, binding = 5) uniform samplerCube u_irradiance;
layout(set = 0, binding = 6) uniform samplerCube u_prefilter;
layout(set = 0, binding = 7) uniform sampler2D u_brdf_lut;
layout(set = 0, binding = 8) uniform sampler2D u_occlusion;
layout(set = 0, binding = 9) uniform sampler2D u_emissive;
layout(set = 0, binding = 11) uniform sampler2DShadow u_shadow_map;

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec3 v_tangent;
layout(location = 4) in vec3 v_bitangent;

layout(location = 0) out vec4 out_color;

const float PI = 3.14159265359;
const float MAX_REFLECTION_LOD = 4.0;

float distribution_ggx(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return a2 / max(denom, 0.0000001);
}

float geometry_schlick_ggx(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 0.0000001);
}

float geometry_smith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float ggx1 = geometry_schlick_ggx(max(dot(N, V), 0.0), roughness);
    float ggx2 = geometry_schlick_ggx(max(dot(N, L), 0.0), roughness);
    return ggx1 * ggx2;
}

vec3 fresnel_schlick(float cos_theta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cos_theta, 5.0);
}

vec3 fresnel_schlick_roughness(float cos_theta, vec3 F0, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - cos_theta, 5.0);
}

/* Shadow map UV + depth in light clip space (xy in [0,1] after perspective divide). */
bool shadow_light_proj(vec3 world_pos, out vec3 proj_coords)
{
    vec4 light_clip = frame.light_proj * frame.light_view * vec4(world_pos, 1.0);
    proj_coords = light_clip.xyz / light_clip.w;
    proj_coords.xy = proj_coords.xy * 0.5 + 0.5;
    return true;
}

float calc_shadow_from_proj(vec3 proj_coords, vec3 N, vec3 L)
{
    float NdotL = max(dot(N, L), 0.0);
    float slope_bias = frame.shadow_bias_slope * sqrt(1.0 - NdotL * NdotL) / max(NdotL, 0.0001);
    float bias = max(frame.shadow_bias * (1.0 - NdotL) + slope_bias, 0.0001);
    float depth_ref = proj_coords.z - bias;

    float shadow = 0.0;
    vec2 texel = vec2(frame.shadow_texel_size);
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec2 offset = vec2(float(x), float(y)) * texel;
            shadow += texture(u_shadow_map, vec3(proj_coords.xy + offset, depth_ref));
        }
    }

    return shadow / 9.0;
}

float calc_shadow(vec3 world_pos, vec3 N, vec3 L)
{
    if (frame.shadows_enabled < 0.5) {
        return 1.0;
    }

    vec3 proj_coords;
    shadow_light_proj(world_pos, proj_coords);

    if (proj_coords.x < 0.0 || proj_coords.x > 1.0 ||
        proj_coords.y < 0.0 || proj_coords.y > 1.0 ||
        proj_coords.z < 0.0 || proj_coords.z > 1.0) {
        return 1.0;
    }

    return calc_shadow_from_proj(proj_coords, N, L);
}

vec2 transform_uv(vec2 uv, int slot)
{
    vec4 params = material.uv_transform_a[slot];
    if (params.w < 0.5) {
        return uv;
    }

    vec2 scale = material.uv_transform_b[slot].xy;
    uv *= scale;
    float c = cos(params.z);
    float s = sin(params.z);
    uv = vec2(c * uv.x - s * uv.y, s * uv.x + c * uv.y);
    return uv + params.xy;
}

/* Per-light Cook-Torrance BRDF evaluation. Returns the specular term and
 * writes the diffuse energy fraction (kD = (1 - kS) * (1 - metallic)). Shared
 * between the direct light loop. */
vec3 direct_brdf(vec3 N, vec3 V, vec3 L, float metallic, float roughness, vec3 F0, out vec3 kD)
{
    vec3 H = normalize(V + L);
    float NDF = distribution_ggx(N, H, roughness);
    float G = geometry_smith(N, V, L, roughness);
    vec3 F = fresnel_schlick(max(dot(H, V), 0.0), F0);

    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0);
    vec3 specular = numerator / max(denominator, 0.0001);

    vec3 kS = F;
    kD = (vec3(1.0) - kS) * (1.0 - metallic);
    return specular;
}

/* Point-light attenuation. range <= 0 means no falloff (infinite range).
 * Otherwise a smooth inverse-square windowed to zero at range so the light
 * turns off cleanly at its boundary. */
float point_attenuation(float dist, float range)
{
    if (range <= 0.0) {
        return 1.0 / max(dist * dist, 1e-4);
    }
    float r2 = range * range;
    float d2 = dist * dist;
    /* Smooth window: (1 - d2/r2)^2 attenuates to 0 at d = range. */
    float window = max(1.0 - d2 / r2, 0.0);
    return window * window / max(d2, 1e-4);
}

void main()
{
    vec2 uv_albedo = transform_uv(v_uv, 0);
    vec2 uv_mr = transform_uv(v_uv, 1);
    vec2 uv_normal = transform_uv(v_uv, 2);
    vec2 uv_occlusion = transform_uv(v_uv, 3);
    vec2 uv_emissive = transform_uv(v_uv, 4);

    vec4 albedo_sample = texture(u_albedo, uv_albedo);
    vec3 albedo = albedo_sample.rgb * material.albedo_factor;
    float alpha = albedo_sample.a * material.base_color_alpha;
    vec3 mr_sample = texture(u_metallic_roughness, uv_mr).rgb;
    float roughness = mr_sample.g * material.roughness_factor;
    float metallic = mr_sample.b * material.metallic_factor;
    roughness = clamp(roughness, 0.04, 1.0);

    vec3 tangent_normal = texture(u_normal, uv_normal).rgb * 2.0 - 1.0;
    vec3 T = normalize(v_tangent);
    vec3 B = normalize(v_bitangent);
    vec3 N_geom = normalize(v_normal);
    if (material.double_sided > 0.5 && !gl_FrontFacing) {
        N_geom = -N_geom;
    }
    vec3 N = N_geom;
    if (length(cross(N_geom, T)) > 1e-4) {
        vec3 bitangent = B;
        if (material.double_sided > 0.5 && !gl_FrontFacing) {
            T = -T;
            bitangent = -bitangent;
        }
        N = normalize(mat3(T, bitangent, N_geom) * tangent_normal);
    }

    vec3 V = normalize(frame.camera_pos - v_world_pos);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    /* Direct lighting: loop over the bound light list. Slot 0 is the directional
     * (the only shadowed light); slots 1..n are unshadowed point lights. */
    vec3 direct = vec3(0.0);
    for (uint i = 0; i < light_list.light_count && i < PB_LIGHT_MAX; ++i) {
        pb_light light = light_list.lights[i];

        vec3 L;
        float attenuation;
        if (light.type == PB_LIGHT_TYPE_DIRECTIONAL) {
            L = normalize(light.direction);
            attenuation = 1.0;
        } else { /* PB_LIGHT_TYPE_POINT */
            vec3 to_light = light.position - v_world_pos;
            float dist = length(to_light);
            L = to_light / max(dist, 1e-4);
            attenuation = point_attenuation(dist, light.range);
        }

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0) {
            continue;
        }

        vec3 kD;
        vec3 specular = direct_brdf(N, V, L, metallic, roughness, F0, kD);

        /* Shadow factor: directional at slot 0 uses the 2D shadow map; point
         * lights sample their claimed cube shadow slot (Phase 14.2). Cube
         * stores normalized linear distance from the light; we compare the
         * fragment's normalized distance against it. */
        float shadow = 1.0;
        if (i == 0u) {
            shadow = calc_shadow(v_world_pos, N, L);
        } else if (light.type == PB_LIGHT_TYPE_POINT && light.shadow_map_index < PB_POINT_SHADOW_MAX) {
            vec3 to_frag = v_world_pos - light.position;
            float dist = length(to_frag);
            float ref = dist / max(light.range, 1e-4);
            shadow = texture(u_point_shadows[light.shadow_map_index], vec4(to_frag, ref));
        }

        vec3 radiance = light.color * attenuation;
        direct += (kD * albedo / PI + specular) * radiance * NdotL * shadow;
    }

    /* Ambient kD for IBL: use the camera-view reflection approximation. */
    vec3 R = reflect(-V, N);
    vec3 kD_ambient = (vec3(1.0) - fresnel_schlick_roughness(max(dot(N, V), 0.0), F0, roughness)) * (1.0 - metallic);

    vec3 irradiance = texture(u_irradiance, N).rgb;
    vec3 diffuse_ibl = irradiance * albedo;

    vec3 prefiltered = textureLod(u_prefilter, R, roughness * MAX_REFLECTION_LOD).rgb;
    vec2 brdf = texture(u_brdf_lut, vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specular_ibl = prefiltered * (fresnel_schlick_roughness(max(dot(N, V), 0.0), F0, roughness) * brdf.x + brdf.y);

    vec3 ambient = (kD_ambient * diffuse_ibl + specular_ibl) * frame.ibl_intensity;

    /* occlusion: sample R channel, mix with strength, modulate ambient + direct */
    float occlusion = mix(1.0, texture(u_occlusion, uv_occlusion).r, material.occlusion_strength);

    vec3 color = (direct + ambient) * occlusion;

    /* emissive: add unlit contribution */
    vec3 emissive = texture(u_emissive, uv_emissive).rgb * material.emissive_factor;
    color += emissive;

    if (frame.shadow_debug > 0.5) {
        if (frame.shadows_enabled < 0.5) {
            color = mix(color, vec3(1.0, 0.2, 0.2), 0.5);
        } else {
            vec3 proj_coords;
            shadow_light_proj(v_world_pos, proj_coords);

            if (proj_coords.x < 0.0 || proj_coords.x > 1.0 ||
                proj_coords.y < 0.0 || proj_coords.y > 1.0 ||
                proj_coords.z < 0.0 || proj_coords.z > 1.0) {
                color = mix(color, vec3(0.2, 0.35, 0.9), 0.45);
            } else {
                const vec3 L_dir = normalize(light_list.lights[0].direction);
                const float lit = calc_shadow_from_proj(proj_coords, N, L_dir);
                /* Neutral darken — preserve albedo hue, no color cast. */
                color *= mix(0.35, 1.0, lit);
            }
        }
    }

    /* Linear HDR output — tonemap + sRGB encode happen in the post pass
     * (Phase 15.1). Exposure has moved to the post pass as well. */

    if (material.alpha_mode > 0.5 && material.alpha_mode < 1.5) {
        if (alpha < material.alpha_cutoff) {
            discard;
        }
        alpha = 1.0;
    } else if (material.alpha_mode < 0.5) {
        alpha = 1.0;
    }

    out_color = vec4(color, alpha);
}
