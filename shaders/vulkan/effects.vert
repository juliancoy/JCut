#version 450
layout(location = 0) out vec2 v_texCoord;
layout(push_constant) uniform Push {
    mat4 u_mvp;
    float u_brightness;
    float u_contrast;
    float u_saturation;
    float u_opacity;
    vec4 u_shadows;
    vec4 u_midtones;
    vec4 u_highlights;
} pc;
void main() {
    vec2 pos;
    if (gl_VertexIndex == 0) pos = vec2(-1.0, -1.0);
    else if (gl_VertexIndex == 1) pos = vec2(1.0, -1.0);
    else if (gl_VertexIndex == 2) pos = vec2(-1.0, 1.0);
    else pos = vec2(1.0, 1.0);
    v_texCoord = vec2(pos.x * 0.5 + 0.5, pos.y * 0.5 + 0.5);
    gl_Position = pc.u_mvp * vec4(pos, 0.0, 1.0);
}
