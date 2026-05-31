#version 450

layout(set = 0, binding = 0) uniform FrameData {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} frame;

layout(set = 0, binding = 1) uniform MaterialLight {
    vec3 light_dir;
    float _pad0;
    vec3 albedo_factor;
    float metallic_factor;
    vec3 light_color;
    float roughness_factor;
} material;

layout(set = 0, binding = 2) uniform sampler2D u_albedo;
layout(set = 0, binding = 3) uniform sampler2D u_metallic_roughness;

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;

layout(location = 0) out vec4 out_color;

const float PI = 3.14159265359;

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

void main()
{
    vec3 albedo = texture(u_albedo, v_uv).rgb * material.albedo_factor;
    vec3 mr_sample = texture(u_metallic_roughness, v_uv).rgb;
    float roughness = mr_sample.g * material.roughness_factor;
    float metallic = mr_sample.b * material.metallic_factor;
    roughness = clamp(roughness, 0.04, 1.0);

    vec3 N = normalize(v_normal);
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
    vec3 direct = (kD * albedo / PI + specular) * radiance * NdotL;
    vec3 ambient = vec3(0.015) * albedo;
    vec3 color = ambient + direct;

    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    out_color = vec4(color, 1.0);
}
