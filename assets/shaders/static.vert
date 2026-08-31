#version 330 core

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

uniform mat4 u_mvp;
uniform mat4 u_model;

out vec3 v_normal;
out vec2 v_uv;

void main() {
    // mat3(u_model) is only correct for uniform scale; non-uniform scale needs
    // the inverse transpose, which we do not have a case for yet
    v_normal = mat3(u_model) * a_normal;
    v_uv = a_uv;
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
