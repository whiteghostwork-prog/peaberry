#version 460
#extension GL_EXT_ray_query : require

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
    float _pad;
} frame;

layout(set = 0, binding = 1) uniform MaterialLight {
    vec3 light_dir;
    float _pad0;
    vec3 albedo_factor;
    float metallic_factor;
    vec3 light_color;
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

layout(set = 0, binding = 2) uniform sampler2D u_albedo;
layout(set = 0, binding = 3) uniform sampler2D u_metallic_roughness;
layout(set = 0, binding = 4) uniform sampler2D u_normal;
layout(set = 0, binding = 5) uniform samplerCube u_irradiance;
layout(set = 0, binding = 6) uniform samplerCube u_prefilter;
layout(set = 0, binding = 7) uniform sampler2D u_brdf_lut;
layout(set = 0, binding = 8) uniform sampler2D u_occlusion;
layout(set = 0, binding = 9) uniform sampler2D u_emissive;
layout(set = 0, binding = 11) uniform sampler2DShadow u_shadow_map;
layout(set = 0, binding = 13) uniform accelerationStructureEXT u_tlas;

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

vec3 aces_tonemap(vec3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

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

vec3 trace_reflection(vec3 origin, vec3 dir, float roughness)
{
    vec3 fallback = textureLod(u_prefilter, dir, roughness * MAX_REFLECTION_LOD).rgb;

    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsOpaqueEXT, 0xFF, origin, 0.01, dir, 1000.0);
    while (rayQueryProceedEXT(rq)) {
    }

    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
        return fallback;
    }

    /* Single-bounce hit: neutral surface tint (material lookup deferred). */
    return vec3(0.32, 0.34, 0.38);
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
    vec3 L = normalize(material.light_dir);
    vec3 H = normalize(V + L);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float NDF = distribution_ggx(N, H, roughness);
    float G = geometry_smith(N, V, L, roughness);
    vec3 F = fresnel_schlick(max(dot(H, V), 0.0), F0);

    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0);
    vec3 specular = numerator / max(denominator, 0.0001);

    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    float NdotL = max(dot(N, L), 0.0);

    vec3 radiance = material.light_color;
    float shadow = calc_shadow(v_world_pos, N, L);
    vec3 direct = (kD * albedo / PI + specular) * radiance * NdotL * shadow;

    vec3 irradiance = texture(u_irradiance, N).rgb;
    vec3 diffuse_ibl = irradiance * albedo;

    vec3 R = reflect(-V, N);
    vec3 prefiltered = trace_reflection(v_world_pos + N * 0.02, R, roughness);
    vec2 brdf = texture(u_brdf_lut, vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specular_ibl = prefiltered * (fresnel_schlick_roughness(max(dot(N, V), 0.0), F0, roughness) * brdf.x + brdf.y);

    vec3 ambient = kD * diffuse_ibl + specular_ibl;

    float occlusion = mix(1.0, texture(u_occlusion, uv_occlusion).r, material.occlusion_strength);

    vec3 color = (direct + ambient) * occlusion;

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
                const float lit = calc_shadow_from_proj(proj_coords, N, L);
                color *= mix(0.35, 1.0, lit);
            }
        }
    }

    color = aces_tonemap(color * frame.exposure);
    color = pow(color, vec3(1.0 / 2.2));

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
