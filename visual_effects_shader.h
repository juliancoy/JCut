#pragma once

namespace editor {

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
        uniform float u_texture_mode;
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
                if (sourceAlpha > 0.0001) {
                    rgb /= sourceAlpha;
                } else {
                    rgb = vec3(0.0);
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

            rgb = ((rgb - 0.5) * u_contrast) + 0.5 + vec3(u_brightness);
            rgb = mix(vec3(luminance), rgb, u_saturation);
            rgb = clamp(rgb, 0.0, 1.0);
            color.a = clamp(sourceAlpha * u_opacity, 0.0, 1.0);
            color.rgb = rgb * color.a;
            gl_FragColor = color;
        }
    )";
}

}  // namespace editor
