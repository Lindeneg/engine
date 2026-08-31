#version 330 core

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in uvec4 a_joints;
layout(location = 4) in vec4 a_weights;

// must match RENDER_MAX_JOINTS in render.h
const int MAX_JOINTS = 128;

// A uniform block, not a plain array: GL 3.3 only guarantees 1024 vertex
// uniform components, which a full 65-bone rig would exceed on its own.
layout(std140) uniform Joints { mat4 u_joints[MAX_JOINTS]; };

uniform mat4 u_mvp;
uniform mat4 u_model;

out vec3 v_normal;
out vec2 v_uv;

void main() {
    mat4 skin = a_weights.x * u_joints[a_joints.x] + a_weights.y * u_joints[a_joints.y] +
                a_weights.z * u_joints[a_joints.z] + a_weights.w * u_joints[a_joints.w];

    // Weights that sum to zero would collapse the vertex to the origin, which
    // reads as "the mesh vanished" rather than "this vertex is unweighted".
    if (dot(a_weights, vec4(1.0)) < 1e-4)
        skin = mat4(1.0);

    vec4 pos = skin * vec4(a_pos, 1.0);
    vec3 nrm = mat3(skin) * a_normal;

    v_normal = mat3(u_model) * nrm;
    v_uv = a_uv;
    gl_Position = u_mvp * pos;
}
