#version 450
layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
void main() {
    vec3 rgb = texture(u_texture, v_texCoord).rgb;
    float v = dot(rgb, vec3(0.4375, -0.3671875, -0.0703125)) + 0.5;
    outColor = vec4(v, v, v, 1.0);
}
