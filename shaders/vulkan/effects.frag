#version 450
layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(set = 0, binding = 1) uniform sampler2D u_curve_lut;
layout(set = 0, binding = 2) uniform sampler2D u_mask;
layout(set = 0, binding = 3) uniform sampler2D u_mask_curve_lut;
layout(push_constant) uniform Push {
    mat4 u_mvp;
    float u_brightness;
    float u_contrast;
    float u_saturation;
    float u_opacity;
    vec4 u_shadows;    // rgb = shadows, a = curve LUT enabled; edge fill uses rgba = source rect
    vec4 u_midtones;   // rgb = midtones; edge fill uses x = edge px, y = progressive, z = power; a = mask feather radius
    vec4 u_highlights; // rgb = highlights, a = mask feather gamma; negative a = background fill mode
} pc;

float lumaOf(vec3 rgb) {
    return dot(rgb, vec3(0.2126, 0.7152, 0.0722));
}

vec2 textureInteriorClamp(vec2 uv) {
    vec2 texSize = vec2(textureSize(u_texture, 0));
    vec2 halfTexel = 0.5 / max(vec2(1.0), texSize);
    return clamp(uv, halfTexel, vec2(1.0) - halfTexel);
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
            vec2 sampleCoord = textureInteriorClamp(uv + vec2(float(x), float(y)) * texelSize * radius * 0.62);
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
    vec2 texSize = vec2(textureSize(u_texture, 0));
    vec2 halfTexel = 0.5 / max(vec2(1.0), texSize);
    vec2 edgeSpan = clamp(vec2(max(1.0, pc.u_midtones.x)) / max(vec2(1.0), texSize),
                          halfTexel,
                          vec2(1.0) - halfTexel);
    bool progressive = pc.u_midtones.y > 0.5;
    float power = max(0.25, pc.u_midtones.z);

    if (uv.x < left) {
        float fillT = clamp((left - uv.x) / max(0.0001, left), 0.0, 1.0);
        sourceUv.x = mix(halfTexel.x, edgeSpan.x, progressive ? pow(fillT, power) : fillT);
    } else if (uv.x > right) {
        float fillT = clamp((uv.x - right) / max(0.0001, 1.0 - right), 0.0, 1.0);
        sourceUv.x = mix(1.0 - halfTexel.x, 1.0 - edgeSpan.x, progressive ? pow(fillT, power) : fillT);
    }

    if (uv.y < top) {
        float fillT = clamp((top - uv.y) / max(0.0001, top), 0.0, 1.0);
        sourceUv.y = mix(halfTexel.y, edgeSpan.y, progressive ? pow(fillT, power) : fillT);
    } else if (uv.y > bottom) {
        float fillT = clamp((uv.y - bottom) / max(0.0001, 1.0 - bottom), 0.0, 1.0);
        sourceUv.y = mix(1.0 - halfTexel.y, 1.0 - edgeSpan.y, progressive ? pow(fillT, power) : fillT);
    }
    return texture(u_texture, textureInteriorClamp(sourceUv));
}

float mirroredCoord(float t) {
    float wrapped = mod(abs(t), 2.0);
    return wrapped <= 1.0 ? wrapped : 2.0 - wrapped;
}

vec4 mirrorFillSample(vec2 uv) {
    vec4 rect = pc.u_shadows;
    float left = min(rect.x, rect.z);
    float right = max(rect.x, rect.z);
    float top = min(rect.y, rect.w);
    float bottom = max(rect.y, rect.w);
    float width = max(0.0001, right - left);
    float height = max(0.0001, bottom - top);
    vec2 sourceUv = vec2((uv.x - left) / width,
                         (uv.y - top) / height);
    return texture(u_texture, textureInteriorClamp(vec2(mirroredCoord(sourceUv.x),
                                                        mirroredCoord(sourceUv.y))));
}

void main() {
    bool mirrorFill = pc.u_highlights.a < -3.5;
    bool progressiveEdgeStretchFill = pc.u_highlights.a < -2.5 && !mirrorFill;
    bool edgeStretchFill = pc.u_highlights.a < -1.5 && !progressiveEdgeStretchFill && !mirrorFill;
    bool blurredFill = pc.u_highlights.a < -0.5 &&
        !edgeStretchFill &&
        !progressiveEdgeStretchFill &&
        !mirrorFill;
    bool backgroundFill = mirrorFill || progressiveEdgeStretchFill || edgeStretchFill || blurredFill;
    vec4 c = mirrorFill
        ? mirrorFillSample(v_texCoord)
        : (progressiveEdgeStretchFill || edgeStretchFill
            ? edgeStretchFillSample(v_texCoord)
            : (blurredFill ? blurredFillSample(v_texCoord) : texture(u_texture, v_texCoord)));

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
                vec2 sampleCoord = textureInteriorClamp(v_texCoord + offset);
                alphaSum += texture(u_texture, sampleCoord).a;
                sampleCount++;
            }
        }
        float blurredAlpha = alphaSum / float(sampleCount);
        sourceAlpha = pow(blurredAlpha, 1.0 / max(0.01, pc.u_highlights.a));
    }

    bool synth3d = pc.u_shadows.a > 3.5 && pc.u_shadows.a < 4.5;

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

        bool maskOverlay = pc.u_shadows.a > 1.5 && pc.u_shadows.a < 2.5;
        bool maskOnly = pc.u_shadows.a > 2.5;
        bool maskCurveEnabled = maskOverlay && pc.u_midtones.a < -0.5;
        bool curveEnabled = (pc.u_shadows.a > 0.5 && pc.u_shadows.a < 1.5) ||
                            maskCurveEnabled;
        if (curveEnabled) {
            float rr = maskCurveEnabled
                ? texture(u_mask_curve_lut, vec2(clamp(rgb.r, 0.0, 1.0), 0.5)).r
                : texture(u_curve_lut, vec2(clamp(rgb.r, 0.0, 1.0), 0.5)).r;
            float gg = maskCurveEnabled
                ? texture(u_mask_curve_lut, vec2(clamp(rgb.g, 0.0, 1.0), 0.5)).g
                : texture(u_curve_lut, vec2(clamp(rgb.g, 0.0, 1.0), 0.5)).g;
            float bb = maskCurveEnabled
                ? texture(u_mask_curve_lut, vec2(clamp(rgb.b, 0.0, 1.0), 0.5)).b
                : texture(u_curve_lut, vec2(clamp(rgb.b, 0.0, 1.0), 0.5)).b;
            rgb = vec3(rr, gg, bb);
            float curveLuma = lumaOf(rgb);
            float remappedLuma = maskCurveEnabled
                ? texture(u_mask_curve_lut, vec2(clamp(curveLuma, 0.0, 1.0), 0.5)).a
                : texture(u_curve_lut, vec2(clamp(curveLuma, 0.0, 1.0), 0.5)).a;
            if (curveLuma > 0.0001) {
                rgb *= remappedLuma / curveLuma;
            } else {
                rgb = vec3(remappedLuma);
            }
        }

        if (synth3d) {
            vec2 centered = v_texCoord - vec2(0.5);
            float radius = length(centered);
            float scan = 0.94 + 0.06 * sin(v_texCoord.y * 96.0);
            vec3 synthTone = vec3(0.88 + v_texCoord.x * 0.18,
                                  0.96,
                                  1.08 - v_texCoord.y * 0.20);
            rgb = clamp(rgb * synthTone * scan, vec3(0.0), vec3(1.0));
            rgb += vec3(0.035, 0.012, 0.055) * smoothstep(0.18, 0.72, radius);
            sourceAlpha *= 1.0 - smoothstep(0.50, 0.74, radius) * 0.34;
        }
    }

    rgb = ((rgb - 0.5) * pc.u_contrast) + 0.5 + vec3(pc.u_brightness);
    float luma = lumaOf(rgb);
    rgb = mix(vec3(luma), rgb, pc.u_saturation);
    rgb = clamp(rgb, vec3(0.0), vec3(1.0));

    bool maskOverlay = pc.u_shadows.a > 1.5 && pc.u_shadows.a < 2.5;
    bool maskOnly = pc.u_shadows.a > 2.5;
    if (maskOverlay || maskOnly) {
        float maskValue = clamp(texture(u_mask, v_texCoord).r, 0.0, 1.0);
        if (maskOnly) {
            outColor = vec4(vec3(maskValue), maskValue * pc.u_opacity);
            return;
        }
        sourceAlpha *= maskValue;
    }

    c.a = clamp(sourceAlpha * pc.u_opacity, 0.0, 1.0);
    c.rgb = rgb * c.a;
    outColor = c;
}
