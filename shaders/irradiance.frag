#version 450

layout(location = 0) in vec2 v_uv;

layout(set = 0, binding = 0) uniform samplerCube u_environment;

layout(push_constant) uniform Push {
    int face;
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

void main()
{
    vec2 uv = gl_FragCoord.xy / pc.target_size;
    vec3 normal = face_direction(uv, pc.face);

    vec3 irradiance = vec3(0.0);
    vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, normal));
    up = normalize(cross(normal, right));

    const float sample_delta = 0.025;
    float sample_count = 0.0;
    for (float phi = 0.0; phi < 2.0 * PI; phi += sample_delta) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sample_delta) {
            vec3 tangent = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 sample_dir = tangent.x * right + tangent.y * up + tangent.z * normal;
            irradiance += texture(u_environment, sample_dir).rgb * cos(theta) * sin(theta);
            sample_count += 1.0;
        }
    }
    irradiance = PI * irradiance / max(sample_count, 1.0);
    out_color = vec4(irradiance, 1.0);
}
