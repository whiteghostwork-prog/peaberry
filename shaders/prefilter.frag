#version 450

layout(location = 0) in vec2 v_uv;

layout(set = 0, binding = 0) uniform samplerCube u_environment;

layout(push_constant) uniform Push {
    int face;
    float roughness;
    vec2 target_size;
} pc;

layout(location = 0) out vec4 out_color;

const float PI = 3.14159265359;

vec3 face_direction(vec2 uv, int face)
{
    uv = uv * 2.0 - 1.0;
    switch (face) {
    case 0: return normalize(vec3(1.0, -uv.y, -uv.x));
    case 1: return normalize(vec3(-1.0, -uv.y, uv.x));
    case 2: return normalize(vec3(uv.x, 1.0, uv.y));
    case 3: return normalize(vec3(uv.x, -1.0, -uv.y));
    case 4: return normalize(vec3(uv.x, -uv.y, 1.0));
    default: return normalize(vec3(-uv.x, -uv.y, -1.0));
    }
}

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

vec2 hammersley(uint i, uint N)
{
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    float radical = float(bits) * 2.3283064365386963e-10;
    return vec2(float(i) / float(N), radical);
}

vec3 importance_sample_ggx(vec2 xi, vec3 N, float roughness)
{
    float a = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cos_theta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));

    vec3 H = vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

void main()
{
    vec2 uv = gl_FragCoord.xy / pc.target_size;
    vec3 N = face_direction(uv, pc.face);
    vec3 R = N;
    vec3 V = R;

    vec3 prefiltered = vec3(0.0);
    float total_weight = 0.0;
    const uint sample_count = 512u;

    for (uint i = 0u; i < sample_count; ++i) {
        vec2 xi = hammersley(i, sample_count);
        vec3 H = importance_sample_ggx(xi, N, pc.roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            float D = distribution_ggx(N, H, pc.roughness);
            float NdotH = max(dot(N, H), 0.0);
            float HdotV = max(dot(H, V), 0.0);
            float pdf = (D * NdotH / max(4.0 * HdotV, 0.0000001)) + 0.0001;
            float sa_texel = 4.0 * PI / (6.0 * pc.target_size.x * pc.target_size.y);
            float mip = pc.roughness == 0.0 ? 0.0 : 0.5 * log2(sa_texel / pdf);
            prefiltered += textureLod(u_environment, L, mip).rgb * NdotL;
            total_weight += NdotL;
        }
    }

    prefiltered /= max(total_weight, 0.0001);
    out_color = vec4(prefiltered, 1.0);
}
