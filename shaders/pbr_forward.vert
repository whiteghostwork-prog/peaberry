#version 450

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

layout(set = 0, binding = 10) readonly buffer SkinPalettes {
    mat4 joint_matrices[];
} skin_palettes;

layout(set = 0, binding = 12) readonly buffer InstanceMatrices {
    mat4 models[];
} instances;

layout(push_constant) uniform Push {
    mat4 model;
    uint skinned;
    uint palette_base;
    uint instanced;
    uint instance_base;
} push;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent;
layout(location = 4) in vec4 in_joints;
layout(location = 5) in vec4 in_weights;

layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec3 v_tangent;
layout(location = 4) out vec3 v_bitangent;

mat4 fetch_joint_matrix(uint joint_index)
{
    return skin_palettes.joint_matrices[push.palette_base + joint_index];
}

mat4 compute_skin_matrix()
{
    mat4 skin = mat4(0.0);
    skin += in_weights.x * fetch_joint_matrix(uint(in_joints.x));
    skin += in_weights.y * fetch_joint_matrix(uint(in_joints.y));
    skin += in_weights.z * fetch_joint_matrix(uint(in_joints.z));
    skin += in_weights.w * fetch_joint_matrix(uint(in_joints.w));
    return skin;
}

void main()
{
    mat4 model = push.model;
    if (push.instanced != 0u) {
        model = instances.models[push.instance_base + gl_InstanceIndex];
    } else if (push.skinned != 0u) {
        model = push.model * compute_skin_matrix();
    }

    vec4 world = model * vec4(in_pos, 1.0);
    v_world_pos = world.xyz;

    mat3 normal_matrix = mat3(transpose(inverse(model)));
    v_normal = normalize(normal_matrix * in_normal);
    v_tangent = normalize(normal_matrix * in_tangent.xyz);
    v_bitangent = normalize(cross(v_normal, v_tangent) * in_tangent.w);
    v_uv = in_uv;

    gl_Position = frame.proj * frame.view * world;
}
