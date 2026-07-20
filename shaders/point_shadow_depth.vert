#version 450

/* Phase 14.2 point-light shadow cube vertex shader. Mirrors shadow_depth.vert
 * (skinning + instancing via the same push constant layout) but reads the
 * per-face view-projection and light position/range from a dedicated UBO
 * (set 0 binding 15) updated per face. World position is passed to the
 * fragment shader so it can emit linear distance-from-light into the cube. */

layout(set = 0, binding = 15) uniform PointShadowFrame {
    mat4 face_view_proj;
    vec3 light_pos;
    float light_range;
} ps_frame;

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
layout(location = 4) in vec4 in_joints;
layout(location = 5) in vec4 in_weights;

layout(location = 0) out vec3 v_world_pos;

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
    gl_Position = ps_frame.face_view_proj * world;
}
