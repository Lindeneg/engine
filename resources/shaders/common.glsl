// Shared by every stage via #include. Mirrors shader::FrameData; the two change together.

layout(std140) uniform Frame {
    mat4 u_view_proj;
    vec4 u_camera_pos;  // xyz
    vec4 u_light_dir;   // xyz, normalized, pointing toward the light
    vec4 u_light_color; // rgb, w is ambient strength
};
