#pragma once

namespace jcut::audio {

struct DynamicsSettingsCore {
    bool amplifyEnabled = false;
    double amplifyDb = 0.0;
    bool normalizeEnabled = false;
    double normalizeTargetDb = -1.0;
    bool selectiveNormalizeEnabled = false;
    double selectiveNormalizeMinSegmentSeconds = 0.5;
    double selectiveNormalizePeakDb = -12.0;
    int selectiveNormalizePasses = 1;
    bool selectiveNormalizeOverlayVisible = true;
    bool transcriptNormalizeEnabled = false;
    bool peakReductionEnabled = false;
    double peakThresholdDb = -6.0;
    bool limiterEnabled = false;
    double limiterThresholdDb = -1.0;
    bool compressorEnabled = false;
    double compressorThresholdDb = -18.0;
    double compressorRatio = 3.0;
    bool softClipEnabled = false;
    bool stereoToMonoEnabled = false;
    bool waveformPreviewPostProcessing = true;

    bool operator==(const DynamicsSettingsCore&) const = default;
};

DynamicsSettingsCore normalizedDynamicsSettingsCore(
    DynamicsSettingsCore settings);

// Matches the Qt preview engine's processing order and bounds. Samples are
// interleaved floating-point PCM in [-1, 1].
void processAudioDynamicsCore(float* samples,
                              int frames,
                              int channelCount,
                              int sampleRate,
                              const DynamicsSettingsCore& settings);

} // namespace jcut::audio
