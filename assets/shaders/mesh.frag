#include "common.glsl"

in vec3 v_normal;
in vec2 v_uv;
in vec3 v_world;

uniform vec4 u_base_color;

#ifdef ALPHA_MASK
uniform float u_alpha_cutoff;
#endif

out vec4 frag_color;

void main() {
#ifdef ALPHA_MASK
    // glTF alphaMode MASK; samples texture alpha once textures land
    if (u_base_color.a < u_alpha_cutoff)
        discard;
#endif

    vec3 n = normalize(v_normal);
    vec3 l = normalize(u_light_dir.xyz);
    vec3 v = normalize(u_camera_pos.xyz - v_world);
    vec3 h = normalize(l + v);

    float ndl = max(dot(n, l), 0.0);
    float spec = pow(max(dot(n, h), 0.0), 32.0) * 0.25 * ndl;

    vec3 lit = u_base_color.rgb * (u_light_color.w + u_light_color.rgb * ndl) + vec3(spec);
    frag_color = vec4(lit, u_base_color.a);
}
