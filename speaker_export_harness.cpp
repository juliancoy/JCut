#include "speaker_export_harness.h"

#include "clip_serialization.h"
#include "editor_shared.h"
#include "render.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTextStream>
#include <QVector>

#include <cmath>

namespace editor {
namespace {

QVector<TimelineClip> loadClips(const QJsonObject& root) {
    QVector<TimelineClip> clips;
    const QJsonArray jsonClips = root.value(QStringLiteral("timeline")).toArray();
    clips.reserve(jsonClips.size());
    for (const QJsonValue& value : jsonClips) {
        if (!value.isObject()) {
            continue;
        }
        TimelineClip clip = clipFromJson(value.toObject());
        if (clip.trackIndex < 0) {
            clip.trackIndex = clips.size();
        }
        if (!clip.filePath.isEmpty() || clip.mediaType == ClipMediaType::Title) {
            clips.push_back(clip);
        }
    }
    return clips;
}

QVector<TimelineTrack> loadTracks(const QJsonObject& root) {
    QVector<TimelineTrack> tracks;
    const QJsonArray jsonTracks = root.value(QStringLiteral("tracks")).toArray();
    tracks.reserve(jsonTracks.size());
    for (int i = 0; i < jsonTracks.size(); ++i) {
        const QJsonObject obj = jsonTracks.at(i).toObject();
        TimelineTrack track;
        track.name = obj.value(QStringLiteral("name")).toString(QStringLiteral("Track %1").arg(i + 1));
        track.height = qMax(28, obj.value(QStringLiteral("height")).toInt(44));
        if (obj.contains(QStringLiteral("visualMode"))) {
            track.visualMode = trackVisualModeFromString(obj.value(QStringLiteral("visualMode")).toString());
        } else if (obj.contains(QStringLiteral("visualEnabled")) &&
                   !obj.value(QStringLiteral("visualEnabled")).toBool(true)) {
            track.visualMode = TrackVisualMode::Hidden;
        }
        track.audioEnabled = obj.value(QStringLiteral("audioEnabled")).toBool(true);
        tracks.push_back(track);
    }
    return tracks;
}

QVector<RenderSyncMarker> loadRenderSyncMarkers(const QJsonObject& root) {
    QVector<RenderSyncMarker> markers;
    const QJsonArray jsonMarkers = root.value(QStringLiteral("renderSyncMarkers")).toArray();
    markers.reserve(jsonMarkers.size());
    for (const QJsonValue& value : jsonMarkers) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject obj = value.toObject();
        RenderSyncMarker marker;
        marker.clipId = obj.value(QStringLiteral("clipId")).toString();
        marker.frame = qMax<int64_t>(0, obj.value(QStringLiteral("frame")).toVariant().toLongLong());
        marker.action = renderSyncActionFromString(obj.value(QStringLiteral("action")).toString());
        marker.count = qMax(1, obj.value(QStringLiteral("count")).toInt(1));
        markers.push_back(marker);
    }
    return markers;
}

const TimelineClip* findTargetClip(const QVector<TimelineClip>& clips,
                                   const QString& explicitClipId,
                                   const QString& selectedClipId) {
    if (!explicitClipId.trimmed().isEmpty()) {
        for (const TimelineClip& clip : clips) {
            if (clip.id == explicitClipId.trimmed()) {
                return &clip;
            }
        }
        return nullptr;
    }

    if (!selectedClipId.trimmed().isEmpty()) {
        for (const TimelineClip& clip : clips) {
            if (clip.id == selectedClipId.trimmed()) {
                return &clip;
            }
        }
    }

    for (const TimelineClip& clip : clips) {
        if (!clip.filePath.isEmpty() && clip.durationFrames > 0) {
            return &clip;
        }
    }
    return nullptr;
}

void appendMergedRange(QVector<ExportRangeSegment>& ranges, int64_t startFrame, int64_t endFrame) {
    if (endFrame < startFrame) {
        return;
    }
    if (ranges.isEmpty() || startFrame > ranges.constLast().endFrame + 1) {
        ranges.push_back(ExportRangeSegment{startFrame, endFrame});
        return;
    }
    ranges.last().endFrame = qMax(ranges.last().endFrame, endFrame);
}

QSet<QString> parseSpeakerSet(const QStringList& rawValues) {
    QSet<QString> speakerSet;
    for (const QString& rawValue : rawValues) {
        for (const QString& token : rawValue.split(',', Qt::SkipEmptyParts)) {
            const QString trimmed = token.trimmed();
            if (!trimmed.isEmpty()) {
                speakerSet.insert(trimmed);
            }
        }
    }
    return speakerSet;
}

QVector<ExportRangeSegment> buildSourceWordRanges(const QJsonObject& transcriptRoot,
                                                  const QSet<QString>& speakerSet) {
    QVector<ExportRangeSegment> sourceWordRanges;
    const QJsonArray segments = transcriptRoot.value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segmentValue : segments) {
        const QJsonObject segmentObj = segmentValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QStringLiteral("speaker")).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            const QString wordText = wordObj.value(QStringLiteral("word")).toString().trimmed();
            if (wordText.isEmpty()) {
                continue;
            }
            QString wordSpeaker = wordObj.value(QStringLiteral("speaker")).toString().trimmed();
            if (wordSpeaker.isEmpty()) {
                wordSpeaker = segmentSpeaker;
            }
            if (!speakerSet.contains(wordSpeaker)) {
                continue;
            }
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
            if (startSeconds < 0.0 || endSeconds < startSeconds) {
                continue;
            }
            const int64_t startFrame =
                qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)));
            const int64_t endFrame =
                qMax<int64_t>(startFrame,
                              static_cast<int64_t>(std::ceil(endSeconds * kTimelineFps)) - 1);
            appendMergedRange(sourceWordRanges, startFrame, endFrame);
        }
    }
    return sourceWordRanges;
}

QVector<ExportRangeSegment> mapSourceToTimelineRanges(const TimelineClip& clip,
                                                      const QVector<RenderSyncMarker>& markers,
                                                      const QVector<ExportRangeSegment>& sourceWordRanges) {
    QVector<ExportRangeSegment> timelineRanges;
    timelineRanges.reserve(sourceWordRanges.size());

    int sourceRangeIndex = 0;
    const int64_t clipStartFrame = clip.startFrame;
    const int64_t clipEndFrame = clip.startFrame + clip.durationFrames - 1;

    for (int64_t timelineFrame = clipStartFrame; timelineFrame <= clipEndFrame; ++timelineFrame) {
        const int64_t transcriptFrame = transcriptFrameForClipAtTimelineSample(
            clip, frameToSamples(timelineFrame), markers);
        while (sourceRangeIndex < sourceWordRanges.size() &&
               sourceWordRanges.at(sourceRangeIndex).endFrame < transcriptFrame) {
            ++sourceRangeIndex;
        }
        if (sourceRangeIndex >= sourceWordRanges.size()) {
            break;
        }
        const ExportRangeSegment& sourceRange = sourceWordRanges.at(sourceRangeIndex);
        if (transcriptFrame < sourceRange.startFrame || transcriptFrame > sourceRange.endFrame) {
            continue;
        }
        appendMergedRange(timelineRanges, timelineFrame, timelineFrame);
    }

    return timelineRanges;
}

QString inferOutputFormat(const QString& explicitFormat, const QString& outputPath, const QJsonObject& root) {
    if (!explicitFormat.trimmed().isEmpty()) {
        return explicitFormat.trimmed().toLower();
    }

    const QString fromState = root.value(QStringLiteral("outputFormat")).toString().trimmed().toLower();
    if (!fromState.isEmpty()) {
        return fromState;
    }

    const QString ext = QFileInfo(outputPath).suffix().trimmed().toLower();
    if (!ext.isEmpty()) {
        return ext;
    }

    return QStringLiteral("mp4");
}

}  // namespace

int runSpeakerExportHarness(const SpeakerExportHarnessConfig& config) {
    QTextStream out(stdout);
    QTextStream err(stderr);

    const QSet<QString> speakerSet = parseSpeakerSet(config.speakerIds);
    if (config.statePath.trimmed().isEmpty() ||
        config.outputPath.trimmed().isEmpty() ||
        speakerSet.isEmpty()) {
        err << "Usage error: require --state, --output, and at least one --speaker-id.\n";
        return 2;
    }

    QFile stateFile(config.statePath);
    if (!stateFile.open(QIODevice::ReadOnly)) {
        err << "Failed to open state file: " << config.statePath << "\n";
        return 2;
    }

    QJsonParseError stateParseError;
    const QJsonDocument stateDoc = QJsonDocument::fromJson(stateFile.readAll(), &stateParseError);
    if (stateParseError.error != QJsonParseError::NoError || !stateDoc.isObject()) {
        err << "Invalid state JSON: " << stateParseError.errorString() << "\n";
        return 2;
    }
    const QJsonObject root = stateDoc.object();

    QVector<TimelineClip> clips = loadClips(root);
    if (clips.isEmpty()) {
        err << "State has no clips.\n";
        return 2;
    }
    const QVector<TimelineTrack> tracks = loadTracks(root);
    const QVector<RenderSyncMarker> markers = loadRenderSyncMarkers(root);

    const TimelineClip* targetClip = findTargetClip(
        clips,
        config.clipId,
        root.value(QStringLiteral("selectedClipId")).toString());
    if (!targetClip || targetClip->durationFrames <= 0) {
        err << "Could not resolve a valid target clip.\n";
        return 2;
    }

    const QString transcriptPath = activeTranscriptPathForClipFile(targetClip->filePath);
    QFile transcriptFile(transcriptPath);
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        err << "Transcript not found for clip: " << transcriptPath << "\n";
        return 2;
    }

    QJsonParseError transcriptParseError;
    const QJsonDocument transcriptDoc =
        QJsonDocument::fromJson(transcriptFile.readAll(), &transcriptParseError);
    if (transcriptParseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
        err << "Invalid transcript JSON: " << transcriptParseError.errorString() << "\n";
        return 2;
    }

    const QVector<ExportRangeSegment> sourceWordRanges =
        buildSourceWordRanges(transcriptDoc.object(), speakerSet);
    if (sourceWordRanges.isEmpty()) {
        err << "No spoken words found for selected speaker(s) in transcript.\n";
        return 2;
    }

    const QVector<ExportRangeSegment> timelineRanges =
        mapSourceToTimelineRanges(*targetClip, markers, sourceWordRanges);
    if (timelineRanges.isEmpty()) {
        err << "Could not map selected speaker words to timeline frames.\n";
        return 2;
    }

    RenderRequest request;
    request.outputPath = config.outputPath;
    request.outputFormat = inferOutputFormat(config.outputFormat, config.outputPath, root);
    if (config.outputSizeOverride) {
        request.outputSize = config.outputSize;
    } else {
        request.outputSize = QSize(
            root.value(QStringLiteral("outputWidth")).toInt(1080),
            root.value(QStringLiteral("outputHeight")).toInt(1920));
    }
    request.correctionsEnabled = root.value(QStringLiteral("correctionsEnabled")).toBool(true);
    request.useProxyMedia = root.value(QStringLiteral("renderUseProxies")).toBool(false);
    if (config.useProxyOverride) {
        request.useProxyMedia = config.useProxyMedia;
    }

    request.clips = clips;
    request.tracks = tracks;
    request.renderSyncMarkers = markers;
    request.exportRanges = timelineRanges;
    request.exportStartFrame = timelineRanges.constFirst().startFrame;
    request.exportEndFrame = timelineRanges.constLast().endFrame;

    if (request.useProxyMedia) {
        for (TimelineClip& clip : request.clips) {
            const QString proxyPath = playbackProxyPathForClip(clip);
            if (!proxyPath.isEmpty()) {
                clip.filePath = proxyPath;
            }
        }
    }

    const int64_t totalFrames =
        request.exportEndFrame >= request.exportStartFrame
            ? (request.exportEndFrame - request.exportStartFrame + 1)
            : 0;
    out << "Harness start\n";
    out << "  state: " << config.statePath << "\n";
    out << "  output: " << config.outputPath << "\n";
    out << "  clipId: " << targetClip->id << "\n";
    out << "  transcript: " << transcriptPath << "\n";
    out << "  speakers: " << QStringList(speakerSet.values()).join(QStringLiteral(", ")) << "\n";
    out << "  outputSize: " << request.outputSize.width() << "x" << request.outputSize.height() << "\n";
    out << "  format: " << request.outputFormat << "\n";
    out << "  useProxy: " << (request.useProxyMedia ? "true" : "false") << "\n";
    out << "  rangeFrames: " << request.exportStartFrame << "-" << request.exportEndFrame
        << " (" << totalFrames << ")\n";
    out.flush();

    QElapsedTimer timer;
    timer.start();
    int64_t lastReportedFrame = -1;
    const RenderResult result = renderTimelineToFile(
        request,
        [&](const RenderProgress& progress) {
            if (progress.timelineFrame != lastReportedFrame) {
                lastReportedFrame = progress.timelineFrame;
                out << "progress frame=" << progress.timelineFrame
                    << " complete=" << progress.framesCompleted << "/" << progress.totalFrames
                    << " gpu=" << (progress.usingGpu ? "1" : "0")
                    << " enc=" << progress.encoderLabel << "\n";
                out.flush();
            }
            return true;
        });

    out << "Harness done in " << timer.elapsed() << "ms\n";
    out << "  success: " << (result.success ? "true" : "false") << "\n";
    out << "  cancelled: " << (result.cancelled ? "true" : "false") << "\n";
    out << "  usedGpu: " << (result.usedGpu ? "true" : "false") << "\n";
    out << "  usedHardwareEncode: " << (result.usedHardwareEncode ? "true" : "false") << "\n";
    out << "  encoder: " << result.encoderLabel << "\n";
    out << "  framesRendered: " << result.framesRendered << "\n";
    out << "  message: " << result.message << "\n";
    out.flush();

    return result.success ? 0 : 1;
}

}  // namespace editor
