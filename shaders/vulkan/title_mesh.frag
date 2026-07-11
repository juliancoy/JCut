#version 450
layout(location = 0) in vec3 vNormal;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform Push {
    mat4 u_mvp; vec4 u_uvRect; vec4 u_color; vec4 u_material; vec4 u_patternRect;
} pc;
void main() {
    const vec3 light = normalize(vec3(-0.45, -0.35, 0.82));
    const float diffuse = max(0.0, dot(normalize(vNormal), light));
    const float lighting = 0.30 + diffuse * 0.70;
    outColor = vec4(pc.u_color.rgb * lighting * pc.u_color.a, pc.u_color.a);
}
