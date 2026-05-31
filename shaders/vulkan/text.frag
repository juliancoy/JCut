#version 450

layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_glyphAtlas;

layout(push_constant) uniform Push {
    mat4 u_mvp;
    vec4 u_uvRect;
    vec4 u_color;
} pc;

void main() {
    float coverage = texture(u_glyphAtlas, v_texCoord).a;
    float alpha = coverage * pc.u_color.a;
    outColor = vec4(pc.u_color.rgb * alpha, alpha);
}
