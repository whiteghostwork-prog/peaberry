#version 450

layout(set = 0, binding = 0) uniform FrameData {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
    float exposure;
} frame;

layout(push_constant) uniform Push {
    mat4 model;
} push;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent;

layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec3 v_tangent;
layout(location = 4) out vec3 v_bitangent;

void main()
{
    vec4 world = push.model * vec4(in_pos, 1.0);
    v_world_pos = world.xyz;

    mat3 normal_matrix = mat3(transpose(inverse(push.model)));
    v_normal = normalize(normal_matrix * in_normal);
    v_tangent = normalize(normal_matrix * in_tangent.xyz);
    v_bitangent = normalize(cross(v_normal, v_tangent) * in_tangent.w);
    v_uv = in_uv;

    gl_Position = frame.proj * frame.view * world;
}
