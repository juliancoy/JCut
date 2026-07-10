#version 450
layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(set = 0, binding = 1) uniform sampler2D u_curve_lut;
layout(set = 0, binding = 2) uniform sampler2D u_mask;
layout(set = 0, binding = 3) uniform sampler2D u_mask_curve_lut;
layout(set = 0, binding = 4) uniform FrameUniforms {
    vec2 outputSize;
    vec2 inverseOutputSize;
} frame;
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
    vec2 center = pc.u_shadows.xy;
    float halfWidth = max(0.0001, abs(pc.u_shadows.z));
    float signedHalfHeight = abs(pc.u_shadows.w) < 0.0001
        ? 0.0001
        : pc.u_shadows.w;
    float outputAspect = frame.outputSize.x / max(1.0, frame.outputSize.y);
    vec2 delta = vec2((uv.x - center.x) * outputAspect, uv.y - center.y);
    float angle = pc.u_midtones.y;
    float cosine = cos(angle);
    float sine = sin(angle);
    vec2 unrotated = vec2(cosine * delta.x + sine * delta.y,
                          -sine * delta.x + cosine * delta.y);
    vec2 sourceUv = vec2(unrotated.x / (2.0 * halfWidth) + 0.5,
                         unrotated.y / (2.0 * signedHalfHeight) + 0.5);
    vec2 texSize = vec2(textureSize(u_texture, 0));
    vec2 halfTexel = 0.5 / max(vec2(1.0), texSize);
    vec2 validMin = clamp(pc.u_highlights.xy, vec2(0.0), vec2(1.0));
    vec2 validMax = clamp(vec2(pc.u_highlights.z, pc.u_midtones.w), validMin, vec2(1.0));
    vec2 validSpan = max(validMax - validMin, halfTexel * 2.0);
    vec2 edgeSpan = clamp(vec2(max(1.0, pc.u_midtones.x)) /
                              max(vec2(1.0), texSize * validSpan),
                          halfTexel,
                          vec2(1.0) - halfTexel);
    bool progressive = pc.u_highlights.a < -2.5;
    float power = max(0.25, pc.u_midtones.z);

    if (sourceUv.x < 0.0) {
        float fillT = clamp(-sourceUv.x, 0.0, 1.0);
        sourceUv.x = mix(halfTexel.x, edgeSpan.x, progressive ? pow(fillT, power) : fillT);
    } else if (sourceUv.x > 1.0) {
        float fillT = clamp(sourceUv.x - 1.0, 0.0, 1.0);
        sourceUv.x = mix(1.0 - halfTexel.x, 1.0 - edgeSpan.x, progressive ? pow(fillT, power) : fillT);
    }

    if (sourceUv.y < 0.0) {
        float fillT = clamp(-sourceUv.y, 0.0, 1.0);
        sourceUv.y = mix(halfTexel.y, edgeSpan.y, progressive ? pow(fillT, power) : fillT);
    } else if (sourceUv.y > 1.0) {
        float fillT = clamp(sourceUv.y - 1.0, 0.0, 1.0);
        sourceUv.y = mix(1.0 - halfTexel.y, 1.0 - edgeSpan.y, progressive ? pow(fillT, power) : fillT);
    }
    vec2 validHalfTexel = min(halfTexel, validSpan * 0.5);
    vec2 mappedUv = validMin + sourceUv * validSpan;
    return texture(u_texture,
                   clamp(mappedUv, validMin + validHalfTexel, validMax - validHalfTexel));
}

float mirroredCoord(float t) {
    float wrapped = mod(abs(t), 2.0);
    return wrapped <= 1.0 ? wrapped : 2.0 - wrapped;
}

vec4 mirrorFillSample(vec2 uv) {
    vec2 center = pc.u_shadows.xy;
    float halfWidth = max(0.0001, abs(pc.u_shadows.z));
    float signedHalfHeight = abs(pc.u_shadows.w) < 0.0001 ? 0.0001 : pc.u_shadows.w;
    float outputAspect = frame.outputSize.x / max(1.0, frame.outputSize.y);
    vec2 delta = vec2((uv.x - center.x) * outputAspect, uv.y - center.y);
    float angle = pc.u_midtones.y;
    float cosine = cos(angle);
    float sine = sin(angle);
    vec2 unrotated = vec2(cosine * delta.x + sine * delta.y,
                          -sine * delta.x + cosine * delta.y);
    vec2 sourceUv = vec2(unrotated.x / (2.0 * halfWidth) + 0.5,
                         unrotated.y / (2.0 * signedHalfHeight) + 0.5);
    vec2 validMin = clamp(pc.u_highlights.xy, vec2(0.0), vec2(1.0));
    vec2 validMax = clamp(vec2(pc.u_highlights.z, pc.u_midtones.w), validMin, vec2(1.0));
    vec2 validUv = validMin + vec2(mirroredCoord(sourceUv.x), mirroredCoord(sourceUv.y)) *
                                      max(validMax - validMin, vec2(0.0001));
    return texture(u_texture, textureInteriorClamp(validUv));
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
        float packedFalloff = max(pc.u_highlights.a, 0.1);
        int falloff = int(floor(packedFalloff / 10.0));
        float power = clamp(packedFalloff - float(falloff) * 10.0, 0.1, 5.0);
        if (falloff == 0) {
            maskValue = pow(maskValue, 1.0 / power);
        } else if (falloff == 2) {
            maskValue = smoothstep(0.0, 1.0, maskValue);
        } else if (falloff == 3) {
            float t = maskValue;
            maskValue = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
        } else if (falloff == 4) {
            maskValue = 0.5 - 0.5 * cos(maskValue * 3.14159265359);
        } else if (falloff == 5) {
            const float k = 4.0;
            float lo = exp(-k);
            maskValue = (exp(-k * (1.0 - maskValue) * (1.0 - maskValue)) - lo) / (1.0 - lo);
        }
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
