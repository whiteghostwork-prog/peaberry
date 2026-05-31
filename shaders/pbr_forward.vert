#version 450

layout(set = 0, binding = 0) uniform FrameData {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} frame;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;

void main()
{
    vec4 world = frame.model * vec4(in_pos, 1.0);
    v_world_pos = world.xyz;
    v_normal = mat3(transpose(inverse(frame.model))) * in_normal;
    v_uv = in_uv;
    gl_Position = frame.proj * frame.view * world;
}
