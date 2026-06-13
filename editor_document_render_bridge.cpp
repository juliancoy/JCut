#include "editor_document_render_bridge.h"

#include "editor_timeline_types.h"

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
        timelineClip.mediaType = mediaIt == mediaKindById.end()
            ? ClipMediaType::Unknown
            : clipMediaTypeFromKind(mediaIt->second);
        timelineClip.videoEnabled = timelineClip.mediaType != ClipMediaType::Audio;
        timelineClip.audioEnabled = timelineClip.mediaType != ClipMediaType::Image &&
            timelineClip.mediaType != ClipMediaType::Title;
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
