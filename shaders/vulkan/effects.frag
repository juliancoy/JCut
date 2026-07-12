#version 450
layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(set = 0, binding = 1) uniform sampler2D u_curve_lut;
layout(set = 0, binding = 2) uniform sampler2D u_mask;
layout(set = 0, binding = 3) uniform sampler2D u_mask_curve_lut;
layout(set = 0, binding = 4) uniform FrameUniforms {
    vec4 outputSizeAndInverse;
    vec4 backgroundShadows;
    vec4 backgroundMidtones;
    vec4 backgroundHighlights;
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

vec3 applyCurveLut(vec3 rgb, bool maskCurveEnabled) {
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
    return rgb;
}

vec2 textureInteriorClamp(vec2 uv) {
    vec2 texSize = vec2(textureSize(u_texture, 0));
    vec2 halfTexel = 0.5 / max(vec2(1.0), texSize);
    vec2 low = min(halfTexel, vec2(0.5));
    vec2 high = max(vec2(1.0) - halfTexel, low);
    return clamp(uv, low, high);
}

vec2 safeClampRange(vec2 value, vec2 low, vec2 high) {
    vec2 safeLow = min(low, high);
    vec2 safeHigh = max(low, high);
    return clamp(value, safeLow, safeHigh);
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

vec4 edgeStretchFillSample(vec2 uv, bool sampleCompositeScreen) {
    vec2 center = pc.u_shadows.xy;
    float inverseWidth = max(0.0001, abs(pc.u_shadows.z));
    float signedInverseHeight = abs(pc.u_shadows.w) < 0.0001
        ? 0.0001
        : pc.u_shadows.w;
    vec2 texSize = vec2(textureSize(u_texture, 0));
    vec2 halfTexel = 0.5 / max(vec2(1.0), texSize);
    vec2 rawValidMin = pc.u_highlights.xy;
    vec2 rawValidMax = vec2(pc.u_highlights.z, pc.u_midtones.w);
    vec2 sampleMin = clamp(min(rawValidMin, rawValidMax), vec2(0.0), vec2(1.0));
    vec2 sampleMax = clamp(max(rawValidMin, rawValidMax), sampleMin, vec2(1.0));
    vec2 validSpan = max(sampleMax - sampleMin, vec2(0.0));
    vec2 sampleSpan = max(validSpan, vec2(1.0) / max(vec2(1.0), texSize));
    vec2 rawValidSpan = max(abs(rawValidMax - rawValidMin), vec2(1.0) / max(vec2(1.0), texSize));
    vec2 frameOutputSize = frame.outputSizeAndInverse.xy;
    float outputAspect = frameOutputSize.x / max(1.0, frameOutputSize.y);
    vec2 delta = vec2((uv.x - center.x) * outputAspect, uv.y - center.y);
    float angle = pc.u_midtones.y;
    float cosine = cos(angle);
    float sine = sin(angle);
    vec2 unrotated = vec2(cosine * delta.x + sine * delta.y,
                          -sine * delta.x + cosine * delta.y);
    vec2 sourceUv = vec2(unrotated.x * inverseWidth + 0.5,
                         unrotated.y * signedInverseHeight + 0.5);
    vec2 originalSourceUv = sourceUv;
    bool insideClipBounds = originalSourceUv.x >= 0.0 && originalSourceUv.x <= 1.0 &&
                            originalSourceUv.y >= 0.0 && originalSourceUv.y <= 1.0;
    vec2 edgePixelBasis = sampleCompositeScreen ? frameOutputSize : texSize * sampleSpan;
    vec2 edgeSpan = safeClampRange(vec2(max(1.0, pc.u_midtones.x)) /
                                       max(vec2(1.0), edgePixelBasis),
                                   halfTexel,
                                   vec2(1.0) - halfTexel);
    bool progressive = pc.u_highlights.a < -2.5;
    float power = max(0.25, pc.u_midtones.z);

    if (progressive && !sampleCompositeScreen && insideClipBounds) {
        return vec4(0.0);
    }

    if (progressive && (sourceUv.x < 0.0 || sourceUv.x > 1.0 ||
                        sourceUv.y < 0.0 || sourceUv.y > 1.0)) {
        // Trace from the transformed media center through this fragment in
        // screen space.  The first intersection is the exact visible media
        // edge; the second is the canvas edge.  This keeps the scan direction
        // independent of clip rotation and placement.
        vec2 localFromCenter = unrotated;
        vec2 edgeRatioAtFragment = abs(localFromCenter) *
            (2.0 * vec2(inverseWidth, abs(signedInverseHeight)));
        float outsideScale = max(edgeRatioAtFragment.x, edgeRatioAtFragment.y);
        float mediaEdgeScale = 1.0 / max(1.0, outsideScale);
        vec2 mediaEdgeLocal = localFromCenter * mediaEdgeScale;

        vec2 ray = delta;
        float canvasScaleX = ray.x > 0.0
            ? ((1.0 - center.x) * outputAspect) / max(0.0001, ray.x)
            : (-center.x * outputAspect) / min(-0.0001, ray.x);
        float canvasScaleY = ray.y > 0.0
            ? (1.0 - center.y) / max(0.0001, ray.y)
            : (-center.y) / min(-0.0001, ray.y);
        float canvasEdgeScale = min(canvasScaleX, canvasScaleY);
        float fillT = clamp((1.0 - mediaEdgeScale) /
                                max(0.0001, canvasEdgeScale - mediaEdgeScale),
                            0.0,
                            1.0);
        float scanT = pow(fillT, power);
        sourceUv = mediaEdgeLocal * vec2(inverseWidth, signedInverseHeight) + 0.5;

        vec2 edgeRatio = abs(mediaEdgeLocal) *
            (2.0 * vec2(inverseWidth, abs(signedInverseHeight)));
        if (edgeRatio.x >= edgeRatio.y) {
            sourceUv.x = mediaEdgeLocal.x < 0.0
                ? mix(halfTexel.x, edgeSpan.x, scanT)
                : mix(1.0 - halfTexel.x, 1.0 - edgeSpan.x, scanT);
        } else {
            sourceUv.y = mediaEdgeLocal.y * signedInverseHeight < 0.0
                ? mix(halfTexel.y, edgeSpan.y, scanT)
                : mix(1.0 - halfTexel.y, 1.0 - edgeSpan.y, scanT);
        }
    } else {
        if (sourceUv.x < 0.0) {
            float fillT = clamp(-sourceUv.x, 0.0, 1.0);
            sourceUv.x = mix(halfTexel.x, edgeSpan.x, fillT);
        } else if (sourceUv.x > 1.0) {
            float fillT = clamp(sourceUv.x - 1.0, 0.0, 1.0);
            sourceUv.x = mix(1.0 - halfTexel.x, 1.0 - edgeSpan.x, fillT);
        }
        if (sourceUv.y < 0.0) {
            float fillT = clamp(-sourceUv.y, 0.0, 1.0);
            sourceUv.y = mix(halfTexel.y, edgeSpan.y, fillT);
        } else if (sourceUv.y > 1.0) {
            float fillT = clamp(sourceUv.y - 1.0, 0.0, 1.0);
            sourceUv.y = mix(1.0 - halfTexel.y, 1.0 - edgeSpan.y, fillT);
        }
    }
    vec2 sampleHalfTexel = min(halfTexel, sampleSpan * 0.5);
    if (sampleCompositeScreen) {
        vec2 clampedLocal = vec2((sourceUv.x - 0.5) / inverseWidth,
                                 (sourceUv.y - 0.5) / signedInverseHeight);
        vec2 screenDelta = vec2(cosine * clampedLocal.x - sine * clampedLocal.y,
                                sine * clampedLocal.x + cosine * clampedLocal.y);
        vec2 screenUv = vec2(center.x + screenDelta.x / outputAspect,
                             center.y + screenDelta.y);
        vec2 compositeUv = rawValidMin + screenUv * rawValidSpan;
        vec2 sampleLow = sampleMin + sampleHalfTexel;
        vec2 sampleHigh = sampleMax - sampleHalfTexel;
        vec2 resolvedUv = safeClampRange(compositeUv, sampleLow, sampleHigh);
        vec4 resolvedSample = texture(u_texture, resolvedUv);
        if (resolvedSample.a <= 0.01) {
            vec2 outside = max(max(-originalSourceUv, originalSourceUv - vec2(1.0)),
                               vec2(0.0));
            vec2 inwardStep = vec2(0.0);
            if (outside.x >= outside.y) {
                inwardStep.x = originalSourceUv.x < 0.5 ? 1.0 : -1.0;
            } else {
                inwardStep.y = originalSourceUv.y < 0.5 ? 1.0 : -1.0;
            }
            vec2 onePixel = 1.0 / max(vec2(1.0), frameOutputSize);
            for (int i = 1; i <= 1024; ++i) {
                vec2 searchScreenUv = screenUv + inwardStep * onePixel * float(i);
                vec2 searchUv = safeClampRange(rawValidMin + searchScreenUv * rawValidSpan,
                                               sampleLow,
                                               sampleHigh);
                vec4 searchSample = texture(u_texture, searchUv);
                if (searchSample.a > 0.01) {
                    resolvedSample = searchSample;
                    break;
                }
            }
        }
        return resolvedSample;
    }

    vec2 mappedUv = sampleMin + sourceUv * sampleSpan;
    return texture(u_texture,
                   safeClampRange(mappedUv, sampleMin + sampleHalfTexel, sampleMax - sampleHalfTexel));
}

float mirroredCoord(float t) {
    float wrapped = mod(abs(t), 2.0);
    return wrapped <= 1.0 ? wrapped : 2.0 - wrapped;
}

vec4 mirrorFillSample(vec2 uv) {
    vec2 center = pc.u_shadows.xy;
    float inverseWidth = max(0.0001, abs(pc.u_shadows.z));
    float signedInverseHeight = abs(pc.u_shadows.w) < 0.0001 ? 0.0001 : pc.u_shadows.w;
    vec2 frameOutputSize = frame.outputSizeAndInverse.xy;
    float outputAspect = frameOutputSize.x / max(1.0, frameOutputSize.y);
    vec2 delta = vec2((uv.x - center.x) * outputAspect, uv.y - center.y);
    float angle = pc.u_midtones.y;
    float cosine = cos(angle);
    float sine = sin(angle);
    vec2 unrotated = vec2(cosine * delta.x + sine * delta.y,
                          -sine * delta.x + cosine * delta.y);
    vec2 sourceUv = vec2(unrotated.x * inverseWidth + 0.5,
                         unrotated.y * signedInverseHeight + 0.5);
    vec2 validMin = clamp(pc.u_highlights.xy, vec2(0.0), vec2(1.0));
    vec2 validMax = clamp(vec2(pc.u_highlights.z, pc.u_midtones.w), validMin, vec2(1.0));
    vec2 validUv = validMin + vec2(mirroredCoord(sourceUv.x), mirroredCoord(sourceUv.y)) *
                                      max(validMax - validMin, vec2(0.0001));
    return texture(u_texture, textureInteriorClamp(validUv));
}

void main() {
    bool finalCompositeProgressiveEdgeStretchFill = pc.u_highlights.a < -4.5;
    bool mirrorFill = pc.u_highlights.a < -3.5 && !finalCompositeProgressiveEdgeStretchFill;
    bool progressiveEdgeStretchFill = pc.u_highlights.a < -2.5 &&
        !mirrorFill &&
        !finalCompositeProgressiveEdgeStretchFill;
    bool edgeStretchFill = pc.u_highlights.a < -1.5 &&
        !progressiveEdgeStretchFill &&
        !mirrorFill &&
        !finalCompositeProgressiveEdgeStretchFill;
    bool blurredFill = pc.u_highlights.a < -0.5 &&
        !edgeStretchFill &&
        !progressiveEdgeStretchFill &&
        !mirrorFill &&
        !finalCompositeProgressiveEdgeStretchFill;
    bool backgroundFill = mirrorFill ||
        progressiveEdgeStretchFill ||
        edgeStretchFill ||
        blurredFill ||
        finalCompositeProgressiveEdgeStretchFill;
    vec4 c = mirrorFill
        ? mirrorFillSample(v_texCoord)
        : (progressiveEdgeStretchFill || edgeStretchFill || finalCompositeProgressiveEdgeStretchFill
            ? edgeStretchFillSample(v_texCoord, finalCompositeProgressiveEdgeStretchFill)
            : (blurredFill ? blurredFillSample(v_texCoord) : texture(u_texture, v_texCoord)));

    float sourceAlpha = c.a;
    vec3 rgb = c.rgb;
    if (finalCompositeProgressiveEdgeStretchFill && sourceAlpha > 0.01) {
        sourceAlpha = 1.0;
    }
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
            rgb = applyCurveLut(rgb, maskCurveEnabled);
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
    } else if (backgroundFill) {
        float luminance = lumaOf(rgb);
        float shadowWeight = pow(1.0 - luminance, 2.0);
        float midtoneWeight = 1.0 - abs(luminance - 0.5) * 2.0;
        float highlightWeight = pow(luminance, 2.0);

        rgb *= (1.0 + frame.backgroundShadows.rgb * shadowWeight);
        vec3 midtoneAdjust = frame.backgroundMidtones.rgb * midtoneWeight;
        rgb.r = pow(max(rgb.r, 0.0), 1.0 / max(0.0001, 1.0 + midtoneAdjust.r));
        rgb.g = pow(max(rgb.g, 0.0), 1.0 / max(0.0001, 1.0 + midtoneAdjust.g));
        rgb.b = pow(max(rgb.b, 0.0), 1.0 / max(0.0001, 1.0 + midtoneAdjust.b));
        rgb += frame.backgroundHighlights.rgb * highlightWeight;
        if (frame.backgroundShadows.a > 0.5 && frame.backgroundShadows.a < 1.5) {
            rgb = applyCurveLut(rgb, false);
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
