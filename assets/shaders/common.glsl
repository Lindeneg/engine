// Shared by every stage. The loader splices this in wherever a shader writes
// #include "common.glsl", so it exists once, as a file an author can open --
// not as a string literal in the engine.
//
// Mirrors shader::FrameData. The two are siblings and change together.

layout(std140) uniform Frame {
    mat4 u_view_proj;
    vec4 u_camera_pos;  // xyz
    vec4 u_light_dir;   // xyz, normalized, pointing toward the light
    vec4 u_light_color; // rgb, w is ambient strength
};
