#version 450

layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_glyphAtlas;

layout(push_constant) uniform Push {
    mat4 u_mvp;
    vec4 u_uvRect;
    vec4 u_color;
    vec4 u_material;
    vec4 u_patternRect;
} pc;

void main() {
    vec4 sampleColor = texture(u_glyphAtlas, v_texCoord);
    float coverage = sampleColor.a;
    vec3 rgb = pc.u_color.rgb;
    int style = int(pc.u_material.x + 0.5);
    float scale = max(pc.u_material.y, 0.1);
    vec2 p = gl_FragCoord.xy / max(scale, 0.1);
    if (style == 1) {
        float glow = smoothstep(0.02, 0.95, coverage);
        rgb = mix(rgb * 0.55, min(vec3(1.0), rgb * 1.85 + vec3(0.10, 0.18, 0.24)), glow);
    } else if (style == 2) {
        float stripe = step(0.48, fract((p.x + p.y) * 0.075));
        rgb = mix(rgb * 0.48, min(vec3(1.0), rgb * 1.25 + vec3(0.08)), stripe);
    } else if (style == 3) {
        vec2 grid = abs(fract(p * 0.075) - 0.5);
        float line = 1.0 - smoothstep(0.018, 0.055, min(grid.x, grid.y));
        rgb = mix(rgb * 0.62, min(vec3(1.0), rgb * 1.35 + vec3(0.06, 0.10, 0.14)), line);
    } else if (style == 4) {
        vec2 patternUv = pc.u_patternRect.xy + fract(p * 0.014) * pc.u_patternRect.zw;
        vec3 pattern = texture(u_glyphAtlas, patternUv).rgb;
        rgb = mix(rgb * 0.28, pattern, 0.82);
    } else if (style == 5) {
        float alpha = sampleColor.a * pc.u_color.a;
        outColor = vec4(sampleColor.rgb * alpha, alpha);
        return;
    }
    float alpha = coverage * pc.u_color.a;
    outColor = vec4(rgb * alpha, alpha);
}
