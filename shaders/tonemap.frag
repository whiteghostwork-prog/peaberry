#version 450

/* Phase 15.1 post-process: sample the HDR scene color produced by the forward
 * pass, apply exposure + ACES filmic tonemap, and write LDR. The caller is
 * responsible for presenting via an sRGB swapchain format (so we do NOT apply
 * a pow(1/2.2) gamma here — the sRGB target handles the encode).
 *
 * Phase 15.2: exposure now comes from an SSBO (binding 1) rather than a push
 * constant.
 *
 * Phase 15.3: bloom result (binding 2) is added to the scene HDR before ACES,
 * scaled by the bloom_intensity push constant (default 0 — no bloom). When no
 * bloom pass is wired in, the post pass binds a 1x1 black texture here so the
 * contribution is zero and the output is unchanged. */

layout(set = 0, binding = 0) uniform sampler2D u_hdr;

/* Phase 15.2: exposure is a single float in a storage buffer. It must be an
 * SSBO (not a UBO) because exposure_average.comp writes it back each frame
 * (UBOs are read-only in shaders). The post pass binds either the exposure
 * pass's SSBO (auto-adapted) or its own internal static SSBO (Phase 15.1
 * backward-compat path). */
layout(set = 0, binding = 1) readonly buffer Exposure {
    float exposure;
} exposure_ubo;

/* Phase 15.3: bloom result texture. Black when no bloom pass is bound. */
layout(set = 0, binding = 2) uniform sampler2D u_bloom;

layout(push_constant) uniform PostPush {
    float bloom_intensity;   /* 0 = no bloom (default) */
} post;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

vec3 aces_tonemap(vec3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

void main()
{
    vec3 hdr = texture(u_hdr, v_uv).rgb;
    vec3 bloom = texture(u_bloom, v_uv).rgb * post.bloom_intensity;
    vec3 tonemapped = aces_tonemap((hdr + bloom) * exposure_ubo.exposure);
    out_color = vec4(tonemapped, 1.0);
}
