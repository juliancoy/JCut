#version 450

#define SPECTROGRAM_MAX_BINS 2400

layout(location = 0) out vec4 outColor;

layout(std430, set = 0, binding = 1) readonly buffer WaveformBins {
    vec2 u_bins[];
};

layout(std430, set = 0, binding = 9) readonly buffer SpeakerTintBins {
    vec4 u_speakerTint[];
};

layout(std430, set = 0, binding = 10) readonly buffer SpectrogramHistory {
    float u_spectrogram[];
};

layout(push_constant) uniform Push {
    vec4 u_panel;
    vec4 u_graph;
    vec4 u_params;
    vec4 u_flags;
    vec4 u_playhead;
} pc;

float lineAlpha(float value, float target, float halfWidth) {
    return 1.0 - smoothstep(halfWidth, halfWidth + 1.0, abs(value - target));
}

vec3 panelColor(vec2 frag) {
    float t = clamp((frag.y - pc.u_panel.y) / max(1.0, pc.u_panel.w), 0.0, 1.0);
    return mix(vec3(0.074, 0.125, 0.165), vec3(0.031, 0.063, 0.094), t);
}

vec3 tintedGraphColor(vec3 baseColor, int bin) {
    vec4 tint = u_speakerTint[bin];
    return mix(baseColor, tint.rgb, clamp(tint.a, 0.0, 1.0));
}

vec3 spectrogramColor(float value)
{
    vec3 low = vec3(0.015, 0.029, 0.055);
    vec3 midLow = vec3(0.047, 0.263, 0.420);
    vec3 mid = vec3(0.149, 0.647, 0.604);
    vec3 high = vec3(0.953, 0.820, 0.247);
    vec3 peak = vec3(0.973, 0.973, 0.973);
    float v = clamp(value, 0.0, 1.0);
    if (v < 0.33) {
        return mix(low, midLow, v / 0.33);
    }
    if (v < 0.66) {
        return mix(midLow, mid, (v - 0.33) / 0.33);
    }
    if (v < 0.90) {
        return mix(mid, high, (v - 0.66) / 0.24);
    }
    return mix(high, peak, (v - 0.90) / 0.10);
}

vec3 applyPlayheadOverlay(vec3 color, vec2 frag, vec2 graphMin, vec2 graphMax)
{
    if (pc.u_playhead.y <= 0.5) {
        return color;
    }
    float rowCount = max(1.0, pc.u_params.z);
    float rowH = pc.u_graph.w / rowCount;
    int rowIndex = clamp(int(floor((frag.y - graphMin.y) / max(1.0, rowH))), 0, int(rowCount - 1.0));
    if (rowIndex != int(pc.u_playhead.w + 0.5)) {
        return color;
    }
    float headX = graphMin.x + clamp(pc.u_playhead.x, 0.0, 1.0) * max(1.0, graphMax.x - graphMin.x);
    float core = lineAlpha(frag.x, headX, 0.7);
    float glow = lineAlpha(frag.x, headX, pc.u_playhead.z);
    vec3 glowColor = vec3(1.0, 0.839, 0.459);
    color = mix(color, glowColor, glow * 0.28);
    color = mix(color, vec3(1.0, 0.965, 0.78), core * 0.92);
    return color;
}

void main() {
    vec2 frag = gl_FragCoord.xy;
    vec2 panelMin = pc.u_panel.xy;
    vec2 panelMax = pc.u_panel.xy + pc.u_panel.zw;
    if (frag.x < panelMin.x || frag.y < panelMin.y || frag.x > panelMax.x || frag.y > panelMax.y) {
        discard;
    }

    vec3 color = panelColor(frag);
    float alpha = 1.0;
    vec2 graphMin = pc.u_graph.xy;
    vec2 graphMax = pc.u_graph.xy + pc.u_graph.zw;
    bool inGraph = frag.x >= graphMin.x && frag.x <= graphMax.x &&
                   frag.y >= graphMin.y && frag.y <= graphMax.y;
    if (!inGraph) {
        outColor = vec4(color, alpha);
        return;
    }

    int totalBins = max(1, int(pc.u_params.x + 0.5));
    int binsPerRow = max(1, int(pc.u_params.y + 0.5));
    int rowCount = max(1, int(pc.u_params.z + 0.5));
    bool spectrumMode = pc.u_params.w < 0.0;
    if (spectrumMode) {
        int freqBins = max(1, totalBins);
        int displayColumns = max(1, int(pc.u_params.y + 0.5) * max(1, int(pc.u_params.z + 0.5)));
        int historyColumns = max(1, int(pc.u_flags.y + 0.5));
        int binsPerRow = max(1, int(pc.u_params.y + 0.5));
        int rows = max(1, int(pc.u_params.z + 0.5));
        float rowH = pc.u_graph.w / float(rows);
        int row = clamp(int(floor((frag.y - graphMin.y) / max(1.0, rowH))), 0, rows - 1);
        float rowTop = graphMin.y + float(row) * rowH;
        float localX = clamp((frag.x - graphMin.x) / max(1.0, pc.u_graph.z), 0.0, 1.0);
        float localY = clamp((frag.y - rowTop) / max(1.0, rowH), 0.0, 1.0);
        int colInRow = clamp(int(floor(localX * float(binsPerRow))), 0, binsPerRow - 1);
        int timelineCol = row * binsPerRow + colInRow;
        float historyPosition = ((float(timelineCol) + 0.5) / float(displayColumns)) * float(historyColumns) - 0.5;
        int historyCol0 = clamp(int(floor(historyPosition)), 0, historyColumns - 1);
        int historyCol1 = clamp(historyCol0 + 1, 0, historyColumns - 1);
        float historyFrac = clamp(historyPosition - floor(historyPosition), 0.0, 1.0);
        bool validColumn = timelineCol < displayColumns;
        int freqBin = clamp(int(floor((1.0 - localY) * float(freqBins))), 0, freqBins - 1);
        float value0 = validColumn
            ? clamp(u_spectrogram[historyCol0 * SPECTROGRAM_MAX_BINS + freqBin], 0.0, 1.0)
            : 0.0;
        float value1 = validColumn
            ? clamp(u_spectrogram[historyCol1 * SPECTROGRAM_MAX_BINS + freqBin], 0.0, 1.0)
            : 0.0;
        float value = mix(value0, value1, historyFrac);
        color = mix(color, vec3(0.039, 0.071, 0.102), 0.36);
        color = tintedGraphColor(color, clamp(timelineCol, 0, displayColumns - 1));
        color = mix(color, spectrogramColor(value), value * 0.95);
        float topBorder = lineAlpha(frag.y, rowTop, 0.75);
        float bottomBorder = lineAlpha(frag.y, rowTop + rowH, 0.75);
        float leftBorder = lineAlpha(frag.x, graphMin.x, 0.75);
        float rightBorder = lineAlpha(frag.x, graphMax.x, 0.75);
        color = mix(color, vec3(0.47, 0.60, 0.70), max(max(topBorder, bottomBorder), max(leftBorder, rightBorder)) * 0.14);
        color = applyPlayheadOverlay(color, frag, graphMin, graphMax);
        outColor = vec4(color, alpha);
        return;
    }
    float rowH = pc.u_graph.w / float(rowCount);
    int row = clamp(int(floor((frag.y - graphMin.y) / max(1.0, rowH))), 0, rowCount - 1);
    float rowTop = graphMin.y + float(row) * rowH;
    float centerY = rowTop + rowH * 0.5;
    float localX = clamp((frag.x - graphMin.x) / max(1.0, pc.u_graph.z), 0.0, 1.0);
    int rowStart = row * binsPerRow;
    int rowEnd = min(totalBins, rowStart + binsPerRow);
    int rowBins = max(1, rowEnd - rowStart);
    int binInRow = clamp(int(round(localX * float(max(1, rowBins - 1)))), 0, rowBins - 1);
    int bin = clamp(rowStart + binInRow, 0, totalBins - 1);
    vec2 minMax = u_bins[bin];
    float minV = clamp(min(minMax.x, minMax.y), -1.0, 1.0);
    float maxV = clamp(max(minMax.x, minMax.y), -1.0, 1.0);
    float halfRange = rowH * 0.5;
    float yTop = centerY - maxV * halfRange;
    float yBottom = centerY - minV * halfRange;

    color = mix(color, vec3(0.039, 0.071, 0.102), 0.30);
    color = tintedGraphColor(color, bin);
    float border = max(
        max(lineAlpha(frag.x, graphMin.x, 0.75), lineAlpha(frag.x, graphMax.x, 0.75)),
        max(lineAlpha(frag.y, rowTop, 0.75), lineAlpha(frag.y, rowTop + rowH, 0.75)));
    color = mix(color, vec3(0.50, 0.64, 0.75), border * 0.18);
    color = mix(color, vec3(0.78, 0.88, 0.94), lineAlpha(frag.y, centerY, 0.65) * 0.12);

    float dbMarks[4] = float[](0.0, -6.0, -12.0, -18.0);
    for (int i = 0; i < 4; ++i) {
        float amp = pow(10.0, dbMarks[i] / 20.0);
        float a = max(lineAlpha(frag.y, centerY - amp * halfRange, 0.65),
                      lineAlpha(frag.y, centerY + amp * halfRange, 0.65));
        color = mix(color, vec3(0.616, 0.937, 1.0), a * (i == 0 ? 0.20 : 0.10));
    }

    if (pc.u_flags.y > 0.5) {
        float peak = max(abs(minV), abs(maxV));
        if (peak >= pc.u_flags.z) {
            color = mix(color, vec3(0.486, 0.839, 0.671), 0.20);
        }
    }

    if (pc.u_flags.x > 0.5) {
        float fill = step(yTop, frag.y) * step(frag.y, yBottom);
        float outline = max(lineAlpha(frag.y, yTop, 0.9), lineAlpha(frag.y, yBottom, 0.9));
        float readyScale = pc.u_flags.w > 0.5 ? 1.0 : 0.35;
        color = mix(color, vec3(0.498, 0.847, 0.929), fill * 0.30 * readyScale);
        color = mix(color, vec3(0.616, 0.937, 1.0), outline * 0.85 * readyScale);
    }

    color = applyPlayheadOverlay(color, frag, graphMin, graphMax);
    outColor = vec4(color * alpha, alpha);
}
