#version 450
layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(push_constant) uniform Push {
    mat4 u_mvp;
    float u_opacity;
} pc;
void main() {
    vec4 c = texture(u_texture, v_texCoord);
    c.a *= clamp(pc.u_opacity, 0.0, 1.0);
    c.rgb *= c.a;
    outColor = c;
}
