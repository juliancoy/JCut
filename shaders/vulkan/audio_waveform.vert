#version 450

void main() {
    vec2 pos;
    if (gl_VertexIndex == 0) pos = vec2(-1.0, -1.0);
    else if (gl_VertexIndex == 1) pos = vec2(1.0, -1.0);
    else if (gl_VertexIndex == 2) pos = vec2(-1.0, 1.0);
    else pos = vec2(1.0, 1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
}
