#version 450
layout(location = 0) in vec2 a_position;
layout(push_constant) uniform Push {
    mat4 u_mvp;
} pc;
void main() {
    gl_Position = pc.u_mvp * vec4(a_position, 0.0, 1.0);
}
