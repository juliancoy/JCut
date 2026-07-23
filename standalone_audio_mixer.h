#pragma once

#include "editor_document_core.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace jcut::standalone_render::audio {

inline constexpr int kSampleRate = 48000;
inline constexpr int kChannelCount = 2;
inline constexpr std::int64_t kSamplesPerTimelineFrame = kSampleRate / 30;

struct DecodedAudioClip {
    struct TranscriptNormalizeSegment {
        std::int64_t startSourceSample = 0;
        std::int64_t endSourceSampleExclusive = 0;
        float gain = 1.0f;
    };
    std::vector<float> samples;
    std::int64_t sourceStartSample = 0;
    // Maps canonical source-sample positions into this cache. A clip decoded
    // through pitch-preserving speed conversion uses 1/playbackRate.
    double sourceSampleScale = 1.0;
    std::vector<TranscriptNormalizeSegment> transcriptNormalizeSegments;
    bool valid = false;
};

using DecodedAudioCache = std::unordered_map<int, DecodedAudioClip>;

// Decodes every enabled audio clip to the mixer's canonical 48 kHz stereo
// float layout. Relative media paths are resolved against rootDirectory.
// A clip that explicitly advertises audio but cannot be decoded is reported as
// an export error instead of silently producing a video-only file.
[[nodiscard]] bool decodeDocumentAudio(const EditorDocumentCore& document,
                                       const std::string& rootDirectory,
                                       DecodedAudioCache* cacheOut,
                                       std::string* errorOut = nullptr,
                                       const std::vector<EditorExportRange>*
                                           timelineRanges = nullptr);

[[nodiscard]] std::int64_t clipTimelineStartSamples(const EditorClip& clip);
[[nodiscard]] std::int64_t clipTimelineDurationSamples(const EditorClip& clip);
[[nodiscard]] std::int64_t sourceSampleForClipAtTimelineSample(
    const EditorDocumentCore& document,
    const EditorClip& clip,
    std::int64_t timelineSample);

// Mixes stereo float output at 48 kHz. timelineSampleStep maps output samples
// onto the source timeline and is normally 1.0; callers that pitch-preserve a
// completed mix keep it at 1.0 and stretch afterward.
void mixAudioChunk(const EditorDocumentCore& document,
                   const DecodedAudioCache& cache,
                   float* output,
                   int frames,
                   std::int64_t chunkStartSample,
                   double timelineSampleStep = 1.0);

} // namespace jcut::standalone_render::audio
