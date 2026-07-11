#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 0) out vec3 vNormal;
layout(push_constant) uniform Push {
    mat4 u_mvp; vec4 u_uvRect; vec4 u_color; vec4 u_material; vec4 u_patternRect;
} pc;
void main() {
    gl_Position = pc.u_mvp * vec4(inPosition, 1.0);
    float cy = cos(pc.u_material.z), sy = sin(pc.u_material.z);
    float cx = cos(pc.u_material.w), sx = sin(pc.u_material.w);
    float cz = cos(pc.u_uvRect.w), sz = sin(pc.u_uvRect.w);
    vec3 normal = vec3(cy * inNormal.x + sy * inNormal.z,
                       inNormal.y,
                       -sy * inNormal.x + cy * inNormal.z);
    normal = vec3(normal.x, cx * normal.y - sx * normal.z, sx * normal.y + cx * normal.z);
    vNormal = normalize(vec3(cz * normal.x - sz * normal.y,
                             sz * normal.x + cz * normal.y,
                             normal.z));
}
