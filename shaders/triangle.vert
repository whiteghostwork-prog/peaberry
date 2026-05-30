#version 450

layout(set = 0, binding = 0) uniform FrameData {
    mat4 mvp;
} frame;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_color;

layout(location = 0) out vec3 v_color;

void main()
{
    gl_Position = frame.mvp * vec4(in_pos, 1.0);
    v_color = in_color;
}
