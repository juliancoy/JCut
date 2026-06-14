#version 450
layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(set = 0, binding = 1) uniform sampler2D u_curve_lut;
layout(push_constant) uniform Push {
    mat4 u_mvp;
    float u_brightness;
    float u_contrast;
    float u_saturation;
    float u_opacity;
    vec4 u_shadows;    // rgb = shadows, a = curve LUT enabled; edge fill uses rgba = source rect
    vec4 u_midtones;   // rgb = midtones, a = mask feather radius
    vec4 u_highlights; // rgb = highlights, a = mask feather gamma; negative a = background fill mode
} pc;

float lumaOf(vec3 rgb) {
    return dot(rgb, vec3(0.2126, 0.7152, 0.0722));
}

vec4 blurredFillSample(vec2 uv) {
    vec2 texelSize = 1.0 / vec2(textureSize(u_texture, 0));
    float radius = abs(pc.u_midtones.a);
    vec4 sum = vec4(0.0);
    float weightSum = 0.0;
    for (int y = -4; y <= 4; y++) {
        for (int x = -4; x <= 4; x++) {
            float dist2 = float((x * x) + (y * y));
            float weight = 1.0 / (1.0 + dist2 * 0.28);
            vec2 sampleCoord = clamp(uv + vec2(float(x), float(y)) * texelSize * radius * 0.62,
                                     vec2(0.0),
                                     vec2(1.0));
            sum += texture(u_texture, sampleCoord) * weight;
            weightSum += weight;
        }
    }
    return sum / max(0.0001, weightSum);
}

vec4 edgeStretchFillSample(vec2 uv) {
    vec4 rect = pc.u_shadows;
    float left = min(rect.x, rect.z);
    float right = max(rect.x, rect.z);
    float top = min(rect.y, rect.w);
    float bottom = max(rect.y, rect.w);
    float width = max(0.0001, right - left);
    float height = max(0.0001, bottom - top);
    vec2 clampedOutput = vec2(clamp(uv.x, left, right),
                              clamp(uv.y, top, bottom));
    vec2 sourceUv = vec2((clampedOutput.x - left) / width,
                         (clampedOutput.y - top) / height);
    return texture(u_texture, clamp(sourceUv, vec2(0.0), vec2(1.0)));
}

void main() {
    bool edgeStretchFill = pc.u_highlights.a < -1.5;
    bool blurredFill = pc.u_highlights.a < -0.5 && !edgeStretchFill;
    bool backgroundFill = edgeStretchFill || blurredFill;
    vec4 c = edgeStretchFill
        ? edgeStretchFillSample(v_texCoord)
        : (blurredFill ? blurredFillSample(v_texCoord) : texture(u_texture, v_texCoord));

    float sourceAlpha = c.a;
    vec3 rgb = c.rgb;
    // Some hardware-direct decoder paths provide valid color but undefined/zero alpha.
    // Treat non-black, near-zero-alpha texels as opaque to avoid black preview frames.
    if (sourceAlpha <= 0.0001) {
        if (max(max(rgb.r, rgb.g), rgb.b) > 0.0001) {
            sourceAlpha = 1.0;
        } else {
            rgb = vec3(0.0);
        }
    }

    if (pc.u_midtones.a > 0.0 && sourceAlpha > 0.0) {
        vec2 texelSize = 1.0 / vec2(textureSize(u_texture, 0));
        float alphaSum = 0.0;
        int sampleCount = 0;
        int radius = int(ceil(pc.u_midtones.a));
        for (int dy = -radius; dy <= radius; dy++) {
            for (int dx = -radius; dx <= radius; dx++) {
                vec2 offset = vec2(float(dx), float(dy)) * texelSize;
                vec2 sampleCoord = clamp(v_texCoord + offset, vec2(0.0), vec2(1.0));
                alphaSum += texture(u_texture, sampleCoord).a;
                sampleCount++;
            }
        }
        float blurredAlpha = alphaSum / float(sampleCount);
        sourceAlpha = pow(blurredAlpha, 1.0 / max(0.01, pc.u_highlights.a));
    }

    if (!backgroundFill) {
        float luminance = lumaOf(rgb);
        float shadowWeight = pow(1.0 - luminance, 2.0);
        float midtoneWeight = 1.0 - abs(luminance - 0.5) * 2.0;
        float highlightWeight = pow(luminance, 2.0);

        rgb *= (1.0 + pc.u_shadows.rgb * shadowWeight);
        vec3 midtoneAdjust = pc.u_midtones.rgb * midtoneWeight;
        rgb.r = pow(max(rgb.r, 0.0), 1.0 / max(0.0001, 1.0 + midtoneAdjust.r));
        rgb.g = pow(max(rgb.g, 0.0), 1.0 / max(0.0001, 1.0 + midtoneAdjust.g));
        rgb.b = pow(max(rgb.b, 0.0), 1.0 / max(0.0001, 1.0 + midtoneAdjust.b));
        rgb += pc.u_highlights.rgb * highlightWeight;

        if (pc.u_shadows.a > 0.5) {
            float rr = texture(u_curve_lut, vec2(clamp(rgb.r, 0.0, 1.0), 0.5)).r;
            float gg = texture(u_curve_lut, vec2(clamp(rgb.g, 0.0, 1.0), 0.5)).g;
            float bb = texture(u_curve_lut, vec2(clamp(rgb.b, 0.0, 1.0), 0.5)).b;
            rr = texture(u_curve_lut, vec2(clamp(rr, 0.0, 1.0), 0.5)).a;
            gg = texture(u_curve_lut, vec2(clamp(gg, 0.0, 1.0), 0.5)).a;
            bb = texture(u_curve_lut, vec2(clamp(bb, 0.0, 1.0), 0.5)).a;
            rgb = vec3(rr, gg, bb);
        }
    }

    rgb = ((rgb - 0.5) * pc.u_contrast) + 0.5 + vec3(pc.u_brightness);
    float luma = lumaOf(rgb);
    rgb = mix(vec3(luma), rgb, pc.u_saturation);
    rgb = clamp(rgb, vec3(0.0), vec3(1.0));

    c.a = clamp(sourceAlpha * pc.u_opacity, 0.0, 1.0);
    c.rgb = rgb * c.a;
    outColor = c;
}
