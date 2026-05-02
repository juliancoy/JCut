#version 450
layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
void main() {
    vec3 rgb = texture(u_texture, v_texCoord).rgb;
    float y = dot(rgb, vec3(0.2578125, 0.50390625, 0.09765625)) + 0.0625;
    outColor = vec4(y, y, y, 1.0);
}
