#include "editor_runtime_qt_bridge.h"

#include "editor_timeline_types.h"

#include <unordered_set>

namespace {

std::string clipKind(ClipMediaType type)
{
    switch (type) {
    case ClipMediaType::Image:
        return "image";
    case ClipMediaType::Video:
        return "video";
    case ClipMediaType::Audio:
        return "audio";
    case ClipMediaType::Title:
        return "title";
    case ClipMediaType::Unknown:
    default:
        return "unknown";
    }
}

std::string clipLabel(const TimelineClip& clip)
{
    if (!clip.label.trimmed().isEmpty()) {
        return clip.label.toStdString();
    }
    if (!clip.filePath.trimmed().isEmpty()) {
        return clip.filePath.section(QLatin1Char('/'), -1).toStdString();
    }
    return std::string("clip");
}

} // namespace

namespace jcut {

EditorDocumentCore buildEditorDocumentCore(const QString& projectName,
                                           const QVector<TimelineClip>& clips,
                                           const QVector<TimelineTrack>& tracks)
{
    EditorDocumentCore document;
    document.projectName = projectName.toStdString();

    document.tracks.reserve(static_cast<std::size_t>(tracks.size()));
    for (int i = 0; i < tracks.size(); ++i) {
        const TimelineTrack& track = tracks.at(i);
        document.tracks.push_back({
            i + 1,
            track.name.trimmed().isEmpty() ? ("Track " + std::to_string(i + 1)) : track.name.toStdString(),
            i == 0
        });
    }

    std::unordered_set<std::string> seenMediaIds;
    document.clips.reserve(static_cast<std::size_t>(clips.size()));
    int nextClipId = 1;
    for (const TimelineClip& clip : clips) {
        const int trackId = clip.trackIndex + 1;
        const std::string sourcePath = clip.filePath.toStdString();
        if (!sourcePath.empty() && seenMediaIds.insert(sourcePath).second) {
            document.mediaItems.push_back({
                sourcePath,
                clipLabel(clip),
                clipKind(clip.mediaType)
            });
        }
        document.clips.push_back({
            nextClipId++,
            trackId,
            clipLabel(clip),
            static_cast<int>(clip.startFrame),
            static_cast<int>(clip.durationFrames),
            document.clips.empty(),
            sourcePath
        });
    }

    return document;
}

} // namespace jcut
