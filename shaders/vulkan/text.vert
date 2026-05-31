#version 450

layout(location = 0) out vec2 v_texCoord;

layout(push_constant) uniform Push {
    mat4 u_mvp;
    vec4 u_uvRect;
    vec4 u_color;
} pc;

void main() {
    vec2 pos;
    vec2 unitUv;
    if (gl_VertexIndex == 0) {
        pos = vec2(-1.0, -1.0);
        unitUv = vec2(0.0, 1.0);
    } else if (gl_VertexIndex == 1) {
        pos = vec2(1.0, -1.0);
        unitUv = vec2(1.0, 1.0);
    } else if (gl_VertexIndex == 2) {
        pos = vec2(-1.0, 1.0);
        unitUv = vec2(0.0, 0.0);
    } else {
        pos = vec2(1.0, 1.0);
        unitUv = vec2(1.0, 0.0);
    }
    v_texCoord = pc.u_uvRect.xy + (unitUv * pc.u_uvRect.zw);
    gl_Position = pc.u_mvp * vec4(pos, 0.0, 1.0);
}
