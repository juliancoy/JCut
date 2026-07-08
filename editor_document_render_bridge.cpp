#include "editor_document_render_bridge.h"

#include "editor_shared_media.h"
#include "editor_shared_timing.h"
#include "editor_timeline_types.h"

#include <QFileInfo>

#include <algorithm>
#include <unordered_map>

namespace {

ClipMediaType clipMediaTypeFromKind(const std::string& kind)
{
    if (kind == "image") {
        return ClipMediaType::Image;
    }
    if (kind == "video") {
        return ClipMediaType::Video;
    }
    if (kind == "audio") {
        return ClipMediaType::Audio;
    }
    if (kind == "title" || kind == "graphics") {
        return ClipMediaType::Title;
    }
    return ClipMediaType::Unknown;
}

ClipMediaType inferClipMediaType(const std::string& kind, const std::string& sourcePath)
{
    const ClipMediaType explicitType = clipMediaTypeFromKind(kind);
    if (explicitType != ClipMediaType::Unknown) {
        return explicitType;
    }

    const QString suffix = QFileInfo(QString::fromStdString(sourcePath)).suffix().toLower();
    if (suffix == QStringLiteral("png") ||
        suffix == QStringLiteral("jpg") ||
        suffix == QStringLiteral("jpeg") ||
        suffix == QStringLiteral("webp") ||
        suffix == QStringLiteral("tga") ||
        suffix == QStringLiteral("tif") ||
        suffix == QStringLiteral("tiff") ||
        suffix == QStringLiteral("bmp") ||
        suffix == QStringLiteral("exr")) {
        return ClipMediaType::Image;
    }
    if (suffix == QStringLiteral("wav") ||
        suffix == QStringLiteral("mp3") ||
        suffix == QStringLiteral("aac") ||
        suffix == QStringLiteral("m4a") ||
        suffix == QStringLiteral("flac") ||
        suffix == QStringLiteral("ogg")) {
        return ClipMediaType::Audio;
    }
    if (!sourcePath.empty()) {
        return ClipMediaType::Video;
    }
    return ClipMediaType::Unknown;
}

} // namespace

namespace jcut::render {

TimelineRenderData buildTimelineRenderData(const EditorDocumentCore& document)
{
    TimelineRenderData timelineData;

    timelineData.tracks.reserve(document.tracks.size());
    std::unordered_map<int, int> trackIndexById;
    for (std::size_t index = 0; index < document.tracks.size(); ++index) {
        const EditorTrack& track = document.tracks[index];
        TimelineTrack timelineTrack;
        timelineTrack.name = QString::fromStdString(track.label);
        timelineData.tracks.push_back(timelineTrack);
        trackIndexById.emplace(track.id, static_cast<int>(index));
    }

    std::unordered_map<std::string, std::string> mediaKindById;
    for (const EditorMediaItem& mediaItem : document.mediaItems) {
        mediaKindById.emplace(mediaItem.id, mediaItem.kind);
    }

    timelineData.clips.reserve(document.clips.size());
    for (const EditorClip& clip : document.clips) {
        auto trackIt = trackIndexById.find(clip.trackId);
        if (trackIt == trackIndexById.end()) {
            continue;
        }
        TimelineClip timelineClip;
        timelineClip.id = QStringLiteral("core-clip-%1").arg(clip.id);
        timelineClip.filePath = QString::fromStdString(clip.sourcePath);
        timelineClip.label = QString::fromStdString(clip.label);
        timelineClip.trackIndex = trackIt->second;
        timelineClip.startFrame = clip.startFrame;
        timelineClip.durationFrames = std::max(1, clip.durationFrames);
        const auto mediaIt = mediaKindById.find(clip.sourcePath);
        timelineClip.mediaType = inferClipMediaType(
            mediaIt == mediaKindById.end() ? std::string{} : mediaIt->second,
            clip.sourcePath);
        timelineClip.sourceKind = isImageSequencePath(timelineClip.filePath)
            ? MediaSourceKind::ImageSequence
            : MediaSourceKind::File;
        timelineClip.videoEnabled = timelineClip.mediaType != ClipMediaType::Audio;
        timelineClip.audioEnabled = timelineClip.mediaType != ClipMediaType::Image &&
            timelineClip.mediaType != ClipMediaType::Title;
        timelineClip.hasAudio = timelineClip.mediaType == ClipMediaType::Video ||
            timelineClip.mediaType == ClipMediaType::Audio;
        if (!timelineClip.filePath.isEmpty() &&
            timelineClip.mediaType != ClipMediaType::Title) {
            const MediaProbeResult probe = probeMediaFile(
                timelineClip.filePath,
                static_cast<qreal>(timelineClip.durationFrames) /
                    static_cast<qreal>(kTimelineFps));
            if (probe.fps > 0.001) {
                timelineClip.sourceFps = probe.fps;
            }
            if (probe.durationFrames > 0) {
                timelineClip.sourceDurationFrames = probe.durationFrames;
            }
            if (probe.frameSize.isValid()) {
                timelineClip.sourceFrameSize = probe.frameSize;
            }
            if (probe.mediaType != ClipMediaType::Unknown) {
                timelineClip.mediaType = probe.mediaType;
            }
            timelineClip.sourceKind = probe.sourceKind;
            timelineClip.hasAudio = probe.hasAudio ||
                timelineClip.mediaType == ClipMediaType::Video ||
                timelineClip.mediaType == ClipMediaType::Audio;
        }
        if (timelineClip.sourceDurationFrames <= 0) {
            timelineClip.sourceDurationFrames =
                timelineClip.sourceInFrame + timelineClip.durationFrames;
        }
        timelineData.clips.push_back(std::move(timelineClip));
    }

    const int exportEndFrame = document.exportRequest.exportEndFrame > document.exportRequest.exportStartFrame
        ? static_cast<int>(document.exportRequest.exportEndFrame)
        : [&document]() {
              int maxFrame = 0;
              for (const EditorClip& clip : document.clips) {
                  maxFrame = std::max(maxFrame, clip.startFrame + clip.durationFrames);
              }
              return maxFrame;
          }();

    if (exportEndFrame > document.exportRequest.exportStartFrame) {
        ExportRangeSegment segment;
        segment.startFrame = document.exportRequest.exportStartFrame;
        segment.endFrame = exportEndFrame;
        timelineData.exportRanges.push_back(segment);
    }

    return timelineData;
}

} // namespace jcut::render
