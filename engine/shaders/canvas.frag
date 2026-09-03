in vec2 v_uv;
in vec4 v_color;

// a coverage atlas reads as (1, 1, 1, r), so text, fills and images all land here
uniform sampler2D u_texture;

out vec4 frag_color;

void main() { frag_color = v_color * texture(u_texture, v_uv); }
