#version 450

/* Phase 14.2 point-light shadow cube fragment shader. Emits normalized linear
 * distance-from-light into the depth attachment so samplerCubeShadow's compare
 * reference can be distance/range regardless of which face the fragment is in. */

layout(set = 0, binding = 15) uniform PointShadowFrame {
    mat4 face_view_proj;
    vec3 light_pos;
    float light_range;
} ps_frame;

layout(location = 0) in vec3 v_world_pos;

void main()
{
    float dist = length(v_world_pos - ps_frame.light_pos);
    /* Normalize to [0,1] over the light's range. */
    gl_FragDepth = dist / max(ps_frame.light_range, 1e-4);
}
