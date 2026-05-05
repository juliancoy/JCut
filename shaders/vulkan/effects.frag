#version 450
layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(push_constant) uniform Push {
    mat4 u_mvp;
    float u_brightness;
    float u_contrast;
    float u_saturation;
    float u_opacity;
    vec3 u_shadows;
    vec3 u_midtones;
    vec3 u_highlights;
} pc;

float lumaOf(vec3 rgb) {
    return dot(rgb, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
    vec4 c = texture(u_texture, v_texCoord);

    float sourceAlpha = c.a;
    vec3 rgb = c.rgb;
    if (sourceAlpha > 0.00001) {
        rgb /= sourceAlpha;
    }

    float luminance = lumaOf(rgb);
    float shadowWeight = pow(1.0 - luminance, 2.0);
    float midtoneWeight = 1.0 - abs(luminance - 0.5) * 2.0;
    float highlightWeight = pow(luminance, 2.0);

    rgb *= (1.0 + pc.u_shadows * shadowWeight);
    vec3 midtoneAdjust = pc.u_midtones * midtoneWeight;
    rgb.r = pow(max(rgb.r, 0.0), 1.0 / max(0.0001, 1.0 + midtoneAdjust.r));
    rgb.g = pow(max(rgb.g, 0.0), 1.0 / max(0.0001, 1.0 + midtoneAdjust.g));
    rgb.b = pow(max(rgb.b, 0.0), 1.0 / max(0.0001, 1.0 + midtoneAdjust.b));
    rgb += pc.u_highlights * highlightWeight;

    rgb = ((rgb - 0.5) * pc.u_contrast) + 0.5 + vec3(pc.u_brightness);
    float luma = lumaOf(rgb);
    rgb = mix(vec3(luma), rgb, pc.u_saturation);
    rgb = clamp(rgb, vec3(0.0), vec3(1.0));

    c.a = clamp(sourceAlpha * pc.u_opacity, 0.0, 1.0);
    c.rgb = rgb * c.a;
    outColor = c;
}
