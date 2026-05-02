#version 450
layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
void main() {
    vec3 rgb = texture(u_texture, v_texCoord).rgb;
    float u = dot(rgb, vec3(-0.1484375, -0.2890625, 0.4375)) + 0.5;
    outColor = vec4(u, u, u, 1.0);
}
