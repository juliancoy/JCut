#pragma once

#include "editor_document_core.h"
#include "timeline_fps.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace jcut {

inline double normalizedEditorClipSourceFps(const EditorClip& clip)
{
    return std::isfinite(clip.sourceFps) && clip.sourceFps >= 1.0
        ? std::min(clip.sourceFps, 1000.0)
        : static_cast<double>(kTimelineFps);
}

inline std::int64_t adjustedEditorClipLocalFrame(
    const EditorDocumentCore& document,
    const EditorClip& clip,
    std::int64_t timelineFrame)
{
    std::int64_t localFrame = std::max<std::int64_t>(
        0, timelineFrame - clip.startFrame);
    for (const EditorRenderSyncMarker& marker :
         document.renderSyncMarkers) {
        if (marker.clipId != clip.persistentId ||
            marker.frame >= timelineFrame) {
            continue;
        }
        const int magnitude = std::max(1, marker.count);
        localFrame += marker.skipFrame
            ? magnitude
            : -magnitude;
    }
    return std::max<std::int64_t>(0, localFrame);
}

inline double editorClipSourceFramePosition(
    const EditorDocumentCore& document,
    const EditorClip& clip,
    std::int64_t timelineFrame)
{
    const std::int64_t localFrame =
        adjustedEditorClipLocalFrame(
            document, clip, timelineFrame);
    const double playbackRate =
        std::isfinite(clip.playbackRate) &&
            clip.playbackRate > 0.001
        ? std::min(clip.playbackRate, 64.0)
        : 1.0;
    const double sourceFps =
        normalizedEditorClipSourceFps(clip);
    const double sourceIn =
        static_cast<double>(
            std::max<std::int64_t>(0, clip.sourceInFrame)) +
        static_cast<double>(
            std::max<std::int64_t>(
                0, clip.sourceInSubframeSamples)) *
            sourceFps / 48000.0;
    double sourceFrame = sourceIn +
        static_cast<double>(localFrame) *
            playbackRate * sourceFps /
            static_cast<double>(kTimelineFps);
    if (clip.sourceDurationFrames > 0) {
        sourceFrame = std::min(
            sourceFrame,
            static_cast<double>(
                std::max<std::int64_t>(
                    0, clip.sourceDurationFrames - 1)));
    }
    return std::max(0.0, sourceFrame);
}

inline std::int64_t editorClipSourceFrame(
    const EditorDocumentCore& document,
    const EditorClip& clip,
    std::int64_t timelineFrame)
{
    return static_cast<std::int64_t>(std::floor(
        editorClipSourceFramePosition(
            document, clip, timelineFrame)));
}

inline std::int64_t editorClipTranscriptFrame(
    const EditorDocumentCore& document,
    const EditorClip& clip,
    std::int64_t timelineFrame)
{
    return std::max<std::int64_t>(
        0,
        static_cast<std::int64_t>(std::floor(
            editorClipSourceFramePosition(
                document, clip, timelineFrame) *
            static_cast<double>(kTimelineFps) /
            normalizedEditorClipSourceFps(clip))));
}

inline std::vector<EditorExportRange>
editorTimelineRangesForTranscriptSection(
    const EditorDocumentCore& document,
    const EditorClip& clip,
    std::int64_t transcriptStartFrame,
    std::int64_t transcriptEndFrame)
{
    std::vector<EditorExportRange> ranges;
    if (clip.durationFrames <= 0 ||
        transcriptStartFrame < 0 ||
        transcriptEndFrame < transcriptStartFrame) {
        return ranges;
    }
    const std::int64_t timelineStart =
        std::max(0, clip.startFrame);
    const std::int64_t timelineEnd =
        timelineStart +
        std::max(0, clip.durationFrames - 1);
    for (std::int64_t timelineFrame = timelineStart;
         timelineFrame <= timelineEnd;
         ++timelineFrame) {
        const std::int64_t transcriptFrame =
            editorClipTranscriptFrame(
                document, clip, timelineFrame);
        if (transcriptFrame < transcriptStartFrame ||
            transcriptFrame > transcriptEndFrame) {
            continue;
        }
        if (ranges.empty() ||
            timelineFrame >
                ranges.back().endFrame + 1) {
            ranges.push_back(
                {timelineFrame, timelineFrame});
        } else {
            ranges.back().endFrame = timelineFrame;
        }
    }
    return ranges;
}

} // namespace jcut
