#include "common.glsl"

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

#ifdef SKINNED
layout(location = 3) in uvec4 a_joints;
layout(location = 4) in vec4 a_weights;

// A uniform block, not a plain array: GL 3.3 guarantees only 1024 vertex
// uniform components, which a full rig would exceed on its own. MAX_JOINTS is
// injected by the loader from SHADER_MAX_JOINTS, so it has one definition.
layout(std140) uniform Joints { mat4 u_joints[MAX_JOINTS]; };
#endif

uniform mat4 u_model;
uniform mat3 u_normal_mat;

out vec3 v_normal;
out vec2 v_uv;
out vec3 v_world;

void main() {
    vec3 pos = a_pos;
    vec3 nrm = a_normal;

#ifdef SKINNED
    mat4 skin = a_weights.x * u_joints[a_joints.x] + a_weights.y * u_joints[a_joints.y] +
                a_weights.z * u_joints[a_joints.z] + a_weights.w * u_joints[a_joints.w];

    // Weights summing to zero would collapse the vertex onto the origin, which
    // reads as "the mesh vanished" rather than "this vertex is unweighted".
    if (dot(a_weights, vec4(1.0)) < 1e-4)
        skin = mat4(1.0);

    pos = (skin * vec4(pos, 1.0)).xyz;
    nrm = mat3(skin) * nrm;
#endif

    vec4 world = u_model * vec4(pos, 1.0);

    v_world = world.xyz;
    v_normal = u_normal_mat * nrm;
    v_uv = a_uv;

    gl_Position = u_view_proj * world;
}
