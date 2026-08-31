#version 330 core

in vec3 v_normal;
in vec2 v_uv;

uniform vec4 u_base_color;

out vec4 frag_color;

// One hardcoded directional light plus ambient. Enough to read silhouette and
// surface orientation, which is all the first milestone needs.
void main() {
    vec3 n = normalize(v_normal);
    vec3 l = normalize(vec3(0.4, 0.9, 0.35));
    float ndl = max(dot(n, l), 0.0);
    vec3 lit = u_base_color.rgb * (0.28 + 0.72 * ndl);
    frag_color = vec4(lit, u_base_color.a);
}
