#version 450

layout(location = 0) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler2D u_equirect;

layout(push_constant) uniform Push {
    int face;
    vec2 target_size;
} pc;

layout(location = 0) out vec4 out_color;

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

vec2 direction_to_equirect(vec3 dir)
{
    dir = normalize(dir);
    vec2 uv;
    uv.x = atan(dir.z, dir.x) / (2.0 * 3.14159265) + 0.5;
    uv.y = asin(clamp(dir.y, -1.0, 1.0)) / 3.14159265 + 0.5;
    return uv;
}

void main()
{
    vec2 uv = gl_FragCoord.xy / pc.target_size;
    vec3 dir = face_direction(uv, pc.face);
    vec2 equirect_uv = direction_to_equirect(dir);
    out_color = vec4(texture(u_equirect, equirect_uv).rgb, 1.0);
}
