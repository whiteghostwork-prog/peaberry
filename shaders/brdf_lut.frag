#version 450

layout(location = 0) in vec2 v_uv;

layout(location = 0) out vec2 out_brdf;

const float PI = 3.14159265359;

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

vec2 integrate_brdf(float NdotV, float roughness)
{
    vec3 V = vec3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    float A = 0.0;
    float B = 0.0;
    vec3 N = vec3(0.0, 0.0, 1.0);
    const uint sample_count = 512u;

    for (uint i = 0u; i < sample_count; ++i) {
        float xi1 = float(i) / float(sample_count);
        float xi2 = fract(sin(float(i) * 12.9898) * 43758.5453);

        float a = roughness * roughness;
        float phi = 2.0 * PI * xi1;
        float cos_theta = sqrt((1.0 - xi2) / (1.0 + (a * a - 1.0) * xi2));
        float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));

        vec3 H = vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0);
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);

        if (NdotL > 0.0) {
            float G = geometry_smith(N, V, L, roughness);
            float G_Vis = (G * VdotH) / max(NdotH * NdotV, 0.0000001);
            float Fc = pow(1.0 - VdotH, 5.0);
            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }

    return vec2(A, B) / float(sample_count);
}

void main()
{
    vec2 uv = gl_FragCoord.xy / vec2(512.0, 512.0);
    out_brdf = integrate_brdf(max(uv.x, 0.0001), max(uv.y, 0.0001));
}
