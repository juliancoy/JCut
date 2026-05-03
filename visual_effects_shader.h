#pragma once

namespace editor {

inline constexpr int kMaxShaderCorrectionPolygons = 8;
inline constexpr int kMaxShaderCorrectionPoints = 128;

inline const char* visualEffectsVertexShaderSource() {
    return R"(
        attribute vec2 a_position;
        attribute vec2 a_texCoord;
        uniform mat4 u_mvp;
        varying vec2 v_texCoord;
        void main() {
            v_texCoord = a_texCoord;
            gl_Position = u_mvp * vec4(a_position, 0.0, 1.0);
        }
    )";
}

inline const char* visualEffectsFragmentShaderSource() {
    return R"(
        uniform sampler2D u_texture;
        uniform sampler2D u_texture_uv;
        uniform sampler2D u_curve_lut;
        uniform float u_texture_mode;
        uniform float u_unpremultiply_input;
        uniform float u_curve_enabled;
        uniform float u_brightness;
        uniform float u_contrast;
        uniform float u_saturation;
        uniform float u_opacity;
        uniform float u_feather_radius;
        uniform float u_feather_gamma;
        uniform vec2 u_texel_size;
        uniform vec3 u_shadows;
        uniform vec3 u_midtones;
        uniform vec3 u_highlights;
        varying vec2 v_texCoord;

        float smoothShadows(float luma) {
            return pow(1.0 - luma, 2.0);
        }
        float smoothMidtones(float luma) {
            return 1.0 - abs(luma - 0.5) * 2.0;
        }
        float smoothHighlights(float luma) {
            return pow(luma, 2.0);
        }

        void main() {
            vec4 color;
            float sourceAlpha;
            vec3 rgb;
            if (u_texture_mode > 0.5) {
                float y = texture2D(u_texture, v_texCoord).r;
                vec2 uv = texture2D(u_texture_uv, v_texCoord).rg - vec2(0.5, 0.5);
                rgb = vec3(y + 1.4020 * uv.y,
                           y - 0.344136 * uv.x - 0.714136 * uv.y,
                           y + 1.7720 * uv.x);
                rgb = clamp(rgb, 0.0, 1.0);
                sourceAlpha = 1.0;
            } else {
                color = texture2D(u_texture, v_texCoord);
                sourceAlpha = color.a;
                rgb = color.rgb;
                if (sourceAlpha <= 0.0001) {
                    rgb = vec3(0.0);
                } else if (u_unpremultiply_input > 0.5) {
                    rgb /= sourceAlpha;
                }
            }

            if (u_feather_radius > 0.0 && sourceAlpha > 0.0) {
                float alphaSum = 0.0;
                int sampleCount = 0;
                int radius = int(ceil(u_feather_radius));
                for (int dy = -radius; dy <= radius; dy++) {
                    for (int dx = -radius; dx <= radius; dx++) {
                        vec2 offset = vec2(float(dx), float(dy)) * u_texel_size;
                        vec2 sampleCoord = clamp(v_texCoord + offset, 0.0, 1.0);
                        if (u_texture_mode > 0.5) {
                            alphaSum += 1.0;
                        } else {
                            alphaSum += texture2D(u_texture, sampleCoord).a;
                        }
                        sampleCount++;
                    }
                }
                float blurredAlpha = alphaSum / float(sampleCount);
                sourceAlpha = pow(blurredAlpha, 1.0 / max(0.01, u_feather_gamma));
            }

            float luminance = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
            float shadowWeight = smoothShadows(luminance);
            rgb *= (1.0 + u_shadows * shadowWeight);

            float midtoneWeight = smoothMidtones(luminance);
            vec3 midtoneAdjust = u_midtones * midtoneWeight;
            rgb = pow(rgb, vec3(1.0) / (vec3(1.0) + midtoneAdjust));

            float highlightWeight = smoothHighlights(luminance);
            rgb += u_highlights * highlightWeight;

            if (u_curve_enabled > 0.5) {
                float rr = texture2D(u_curve_lut, vec2(clamp(rgb.r, 0.0, 1.0), 0.5)).r;
                float gg = texture2D(u_curve_lut, vec2(clamp(rgb.g, 0.0, 1.0), 0.5)).g;
                float bb = texture2D(u_curve_lut, vec2(clamp(rgb.b, 0.0, 1.0), 0.5)).b;
                rr = texture2D(u_curve_lut, vec2(clamp(rr, 0.0, 1.0), 0.5)).a;
                gg = texture2D(u_curve_lut, vec2(clamp(gg, 0.0, 1.0), 0.5)).a;
                bb = texture2D(u_curve_lut, vec2(clamp(bb, 0.0, 1.0), 0.5)).a;
                rgb = vec3(rr, gg, bb);
            }

            rgb = ((rgb - 0.5) * u_contrast) + 0.5 + vec3(u_brightness);
            rgb = mix(vec3(luminance), rgb, u_saturation);
            rgb = clamp(rgb, 0.0, 1.0);
            color.a = clamp(sourceAlpha * u_opacity, 0.0, 1.0);
            color.rgb = rgb * color.a;
            gl_FragColor = color;
        }
    )";
}

inline const char* correctionMaskVertexShaderSource() {
    return R"(
        attribute vec2 a_position;
        uniform mat4 u_mvp;
        void main() {
            gl_Position = u_mvp * vec4(a_position, 0.0, 1.0);
        }
    )";
}

inline const char* correctionMaskFragmentShaderSource() {
    return R"(
        void main() {
            gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
        }
    )";
}

inline const char* visualEffectsVertexShaderSourceVulkan() {
    return R"(
        #version 450
        layout(location = 0) in vec2 a_position;
        layout(location = 1) in vec2 a_texCoord;
        layout(location = 0) out vec2 v_texCoord;
        layout(push_constant) uniform Push {
            mat4 u_mvp;
        } pc;
        void main() {
            v_texCoord = a_texCoord;
            gl_Position = pc.u_mvp * vec4(a_position, 0.0, 1.0);
        }
    )";
}

inline const char* visualEffectsFragmentShaderSourceVulkan() {
    return R"(
        #version 450
        layout(location = 0) in vec2 v_texCoord;
        layout(location = 0) out vec4 outColor;

        layout(set = 0, binding = 0) uniform sampler2D u_texture;
        layout(set = 0, binding = 1) uniform sampler2D u_texture_uv;
        layout(set = 0, binding = 2) uniform sampler2D u_curve_lut;

        layout(push_constant) uniform Params {
            float u_texture_mode;
            float u_unpremultiply_input;
            float u_curve_enabled;
            float u_brightness;
            float u_contrast;
            float u_saturation;
            float u_opacity;
            float u_feather_radius;
            float u_feather_gamma;
            vec2 u_texel_size;
            vec3 u_shadows;
            vec3 u_midtones;
            vec3 u_highlights;
        } p;

        float smoothShadows(float luma) { return pow(1.0 - luma, 2.0); }
        float smoothMidtones(float luma) { return 1.0 - abs(luma - 0.5) * 2.0; }
        float smoothHighlights(float luma) { return pow(luma, 2.0); }

        void main() {
            vec4 color;
            float sourceAlpha;
            vec3 rgb;
            if (p.u_texture_mode > 0.5) {
                float y = texture(u_texture, v_texCoord).r;
                vec2 uv = texture(u_texture_uv, v_texCoord).rg - vec2(0.5, 0.5);
                rgb = vec3(y + 1.4020 * uv.y,
                           y - 0.344136 * uv.x - 0.714136 * uv.y,
                           y + 1.7720 * uv.x);
                rgb = clamp(rgb, 0.0, 1.0);
                sourceAlpha = 1.0;
            } else {
                color = texture(u_texture, v_texCoord);
                sourceAlpha = color.a;
                rgb = color.rgb;
                if (sourceAlpha <= 0.0001) {
                    rgb = vec3(0.0);
                } else if (p.u_unpremultiply_input > 0.5) {
                    rgb /= sourceAlpha;
                }
            }

            if (p.u_feather_radius > 0.0 && sourceAlpha > 0.0) {
                float alphaSum = 0.0;
                int sampleCount = 0;
                int radius = int(ceil(p.u_feather_radius));
                for (int dy = -radius; dy <= radius; dy++) {
                    for (int dx = -radius; dx <= radius; dx++) {
                        vec2 offset = vec2(float(dx), float(dy)) * p.u_texel_size;
                        vec2 sampleCoord = clamp(v_texCoord + offset, 0.0, 1.0);
                        if (p.u_texture_mode > 0.5) {
                            alphaSum += 1.0;
                        } else {
                            alphaSum += texture(u_texture, sampleCoord).a;
                        }
                        sampleCount++;
                    }
                }
                float blurredAlpha = alphaSum / float(sampleCount);
                sourceAlpha = pow(blurredAlpha, 1.0 / max(0.01, p.u_feather_gamma));
            }

            float luminance = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
            float shadowWeight = smoothShadows(luminance);
            rgb *= (1.0 + p.u_shadows * shadowWeight);

            float midtoneWeight = smoothMidtones(luminance);
            vec3 midtoneAdjust = p.u_midtones * midtoneWeight;
            rgb = pow(rgb, vec3(1.0) / (vec3(1.0) + midtoneAdjust));

            float highlightWeight = smoothHighlights(luminance);
            rgb += p.u_highlights * highlightWeight;

            if (p.u_curve_enabled > 0.5) {
                float rr = texture(u_curve_lut, vec2(clamp(rgb.r, 0.0, 1.0), 0.5)).r;
                float gg = texture(u_curve_lut, vec2(clamp(rgb.g, 0.0, 1.0), 0.5)).g;
                float bb = texture(u_curve_lut, vec2(clamp(rgb.b, 0.0, 1.0), 0.5)).b;
                rr = texture(u_curve_lut, vec2(clamp(rr, 0.0, 1.0), 0.5)).a;
                gg = texture(u_curve_lut, vec2(clamp(gg, 0.0, 1.0), 0.5)).a;
                bb = texture(u_curve_lut, vec2(clamp(bb, 0.0, 1.0), 0.5)).a;
                rgb = vec3(rr, gg, bb);
            }

            rgb = ((rgb - 0.5) * p.u_contrast) + 0.5 + vec3(p.u_brightness);
            rgb = mix(vec3(luminance), rgb, p.u_saturation);
            rgb = clamp(rgb, 0.0, 1.0);
            color.a = clamp(sourceAlpha * p.u_opacity, 0.0, 1.0);
            color.rgb = rgb * color.a;
            outColor = color;
        }
    )";
}

inline const char* correctionMaskVertexShaderSourceVulkan() {
    return R"(
        #version 450
        layout(location = 0) in vec2 a_position;
        layout(push_constant) uniform Push { mat4 u_mvp; } pc;
        void main() {
            gl_Position = pc.u_mvp * vec4(a_position, 0.0, 1.0);
        }
    )";
}

inline const char* correctionMaskFragmentShaderSourceVulkan() {
    return R"(
        #version 450
        layout(location = 0) out vec4 outColor;
        void main() {
            outColor = vec4(0.0, 0.0, 0.0, 0.0);
        }
    )";
}

}  // namespace editor
