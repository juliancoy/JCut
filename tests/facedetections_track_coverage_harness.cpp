#include "../facestream_tracking.h"
#include "../facestream_artifact_utils.h"
#include "../json_io_utils.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace {

struct Options {
    QString projectStatePath =
        QDir(QStringLiteral(JCUT_SOURCE_DIR)).filePath(QStringLiteral("projects/default/state.json"));
    QString clipId;
    QString outputDir;
    QString runnerPath =
        QDir(QStringLiteral(JCUT_BINARY_DIR)).filePath(QStringLiteral("jcut_vulkan_facestream_offscreen"));
    QString detector = QStringLiteral("jcut-dnn");
    int startFrame = 0;
    int maxFrames = 0;
    double minDetectionRecall = 0.95;
    double minTrackPrecision = 0.95;
    double minWorstFrameRecall = 0.50;
    double maxLowRecallFrameRatio = 0.05;
    int minTrackWorthyChain = 10;
    bool keepOutput = false;
    bool rerun = false;
    bool retrack = false;
    QStringList passthroughArgs;
};

struct ClipSelection {
    QString clipId;
    QString filePath;
    int durationFrames = 0;
};

struct DetectionObservation {
    qint64 frame = -1;
    QRectF box;
    QSize frameSize;
    double confidence = 0.0;
};

struct TrackObservation {
    int trackId = -1;
    qint64 frame = -1;
    QRectF box;
    double score = 0.0;
};

struct MatchCandidate {
    int detectionIndex = -1;
    int trackIndex = -1;
    double iou = 0.0;
    double centerDistanceRatio = 0.0;
    double score = 0.0;
};

struct FrameCoverageStats {
    qint64 frame = -1;
    int detectionCount = 0;
    int trackCount = 0;
    int matchedCount = 0;
    double detectionRecall = 1.0;
    double trackPrecision = 1.0;
};

struct CoverageStats {
    int totalDetections = 0;
    int eligibleDetections = 0;
    int matchedDetections = 0;
    int totalTrackObservations = 0;
    int matchedTrackObservations = 0;
    int framesWithDetections = 0;
    int framesWithEligibleDetections = 0;
    int longestDetectionChain = 0;
    int lowRecallFrames = 0;
    double detectionRecall = 1.0;
    double trackPrecision = 1.0;
    double lowRecallFrameRatio = 0.0;
    double worstFrameRecall = 1.0;
    QVector<FrameCoverageStats> worstFrames;
};

struct ArtifactInputs {
    QString sourceDescription;
    QString clipIdOverride;
    QString videoPathOverride;
    QJsonObject detectionsRoot;
    QJsonObject tracksRoot;
};

void printUsage()
{
    std::cout
        << "Usage: facestream_track_coverage_harness [options] [-- passthrough-runner-args]\n"
        << "Options:\n"
        << "  --project-state PATH         Project state JSON. Default: projects/default/state.json\n"
        << "  --clip-id ID                 Clip id to evaluate. Default: selectedClipId from the project state.\n"
        << "  --output-dir DIR             Directory for detections.bin/tracks.bin. Default: temp dir.\n"
        << "  --runner PATH                Offscreen FaceStream runner. Default: build/jcut_vulkan_facestream_offscreen\n"
        << "  --detector NAME              Detector for the runner. Default: jcut-dnn\n"
        << "  --start-frame N              Source frame to start scanning from. Default: 0\n"
        << "  --max-frames N               Max source frames to scan. Default: 0 (full clip)\n"
        << "  --min-detection-recall X     Minimum matched detections ratio. Default: 0.95\n"
        << "  --min-track-precision X      Minimum matched track observations ratio. Default: 0.95\n"
        << "  --min-worst-frame-recall X   Minimum recall on the worst non-empty frame. Default: 0.50\n"
        << "  --max-low-recall-frame-ratio X  Max fraction of eligible frames allowed below min worst-frame recall. Default: 0.05\n"
        << "  --min-track-worthy-chain N   Minimum continuity chain that makes zero tracks a failure. Default: 10\n"
        << "  --rerun                      Recompute detections/tracks when persisted artifacts are unavailable.\n"
        << "  --retrack                    Rebuild tracks.bin from an existing detections.bin in --output-dir.\n"
        << "  --keep-output                Do not delete the temp output directory.\n";
}

bool parseArgs(const QStringList& args, Options* options)
{
    if (!options) {
        return false;
    }

    bool passthroughMode = false;
    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args.at(i);
        if (passthroughMode) {
            options->passthroughArgs.push_back(arg);
            continue;
        }
        if (arg == QStringLiteral("--")) {
            passthroughMode = true;
            continue;
        }
        auto nextValue = [&](const char* name) -> QString {
            if ((i + 1) >= args.size()) {
                std::cerr << "Missing value for " << name << "\n";
                return {};
            }
            return args.at(++i);
        };
        if (arg == QStringLiteral("--project-state")) {
            options->projectStatePath = nextValue("--project-state");
        } else if (arg == QStringLiteral("--clip-id")) {
            options->clipId = nextValue("--clip-id");
        } else if (arg == QStringLiteral("--output-dir")) {
            options->outputDir = nextValue("--output-dir");
        } else if (arg == QStringLiteral("--runner")) {
            options->runnerPath = nextValue("--runner");
        } else if (arg == QStringLiteral("--detector")) {
            options->detector = nextValue("--detector");
        } else if (arg == QStringLiteral("--start-frame")) {
            options->startFrame = nextValue("--start-frame").toInt();
        } else if (arg == QStringLiteral("--max-frames")) {
            options->maxFrames = qMax(0, nextValue("--max-frames").toInt());
        } else if (arg == QStringLiteral("--min-detection-recall")) {
            options->minDetectionRecall = nextValue("--min-detection-recall").toDouble();
        } else if (arg == QStringLiteral("--min-track-precision")) {
            options->minTrackPrecision = nextValue("--min-track-precision").toDouble();
        } else if (arg == QStringLiteral("--min-worst-frame-recall")) {
            options->minWorstFrameRecall = nextValue("--min-worst-frame-recall").toDouble();
        } else if (arg == QStringLiteral("--max-low-recall-frame-ratio")) {
            options->maxLowRecallFrameRatio = qBound(0.0, nextValue("--max-low-recall-frame-ratio").toDouble(), 1.0);
        } else if (arg == QStringLiteral("--min-track-worthy-chain")) {
            options->minTrackWorthyChain = qMax(2, nextValue("--min-track-worthy-chain").toInt());
        } else if (arg == QStringLiteral("--rerun")) {
            options->rerun = true;
        } else if (arg == QStringLiteral("--retrack")) {
            options->retrack = true;
        } else if (arg == QStringLiteral("--keep-output")) {
            options->keepOutput = true;
        } else if (arg == QStringLiteral("--help") || arg == QStringLiteral("-h")) {
            printUsage();
            return false;
        } else {
            std::cerr << "Unrecognized argument: " << arg.toStdString() << "\n";
            return false;
        }
    }
    return true;
}

bool resolveClipSelection(const QString& projectStatePath,
                          const QString& requestedClipId,
                          ClipSelection* selectionOut,
                          QString* errorOut)
{
    if (selectionOut) {
        *selectionOut = ClipSelection{};
    }

    QJsonObject root;
    if (!jcut::jsonio::readJsonFile(projectStatePath, &root, errorOut)) {
        return false;
    }

    const QString clipId = requestedClipId.trimmed().isEmpty()
        ? root.value(QStringLiteral("selectedClipId")).toString().trimmed()
        : requestedClipId.trimmed();
    if (clipId.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Project state did not resolve a selected clip id.");
        }
        return false;
    }

    const QJsonArray timeline = root.value(QStringLiteral("timeline")).toArray();
    for (const QJsonValue& value : timeline) {
        const QJsonObject clip = value.toObject();
        if (clip.value(QStringLiteral("id")).toString().trimmed() != clipId) {
            continue;
        }
        const QString filePath = clip.value(QStringLiteral("filePath")).toString().trimmed();
        if (filePath.isEmpty()) {
            if (errorOut) {
                *errorOut = QStringLiteral("Resolved clip has an empty filePath.");
            }
            return false;
        }
        if (selectionOut) {
            selectionOut->clipId = clipId;
            selectionOut->filePath = filePath;
            selectionOut->durationFrames = clip.value(QStringLiteral("durationFrames")).toInt(0);
        }
        return true;
    }

    if (errorOut) {
        *errorOut = QStringLiteral("Clip %1 was not found in %2").arg(clipId, projectStatePath);
    }
    return false;
}

QRectF approximateTrackBox(const QJsonObject& trackDetection, const QSize& frameSize)
{
    const double centerX =
        qBound(0.0, trackDetection.value(QStringLiteral("x")).toDouble(0.5), 1.0) * frameSize.width();
    const double centerY =
        qBound(0.0, trackDetection.value(QStringLiteral("y")).toDouble(0.5), 1.0) * frameSize.height();
    const double side =
        qBound(1.0,
               trackDetection.value(QStringLiteral("box")).toDouble(0.0) *
                   static_cast<double>(qMax(1, qMin(frameSize.width(), frameSize.height()))),
               static_cast<double>(qMax(1, qMax(frameSize.width(), frameSize.height()))));
    return QRectF(centerX - (side * 0.5), centerY - (side * 0.5), side, side);
}

QRectF exactOrApproximateTrackBox(const QJsonObject& trackDetection, const QSize& frameSize)
{
    const QRectF exact(trackDetection.value(QStringLiteral("track_box_x")).toDouble(),
                       trackDetection.value(QStringLiteral("track_box_y")).toDouble(),
                       trackDetection.value(QStringLiteral("track_box_w")).toDouble(),
                       trackDetection.value(QStringLiteral("track_box_h")).toDouble());
    if (exact.isValid() && !exact.isEmpty()) {
        return exact;
    }
    return approximateTrackBox(trackDetection, frameSize);
}

double normalizedCenterDistance(const QRectF& a, const QRectF& b)
{
    const QPointF delta = a.center() - b.center();
    const double distance = std::hypot(delta.x(), delta.y());
    const double scale = qMax(1.0, qMax(qMax(a.width(), a.height()), qMax(b.width(), b.height())));
    return distance / scale;
}

double areaRatio(const QRectF& a, const QRectF& b)
{
    const double areaA = qMax(1.0, a.width() * a.height());
    const double areaB = qMax(1.0, b.width() * b.height());
    return qMax(areaA, areaB) / qMin(areaA, areaB);
}

double aspectDelta(const QRectF& a, const QRectF& b)
{
    const double aspectA = a.height() > 0.0 ? a.width() / a.height() : 1.0;
    const double aspectB = b.height() > 0.0 ? b.width() / b.height() : 1.0;
    return std::abs(aspectA - aspectB);
}

bool detectionsLookTrackableTogether(const QRectF& a, const QRectF& b)
{
    const double iou = jcut::facestream::continuityIou(a, b);
    const double centerDistanceRatio = normalizedCenterDistance(a, b);
    if (centerDistanceRatio > 1.75 || areaRatio(a, b) > 2.5 || aspectDelta(a, b) > 0.65) {
        return false;
    }
    return iou >= 0.05 || centerDistanceRatio <= 0.40;
}

void collectObservations(const QJsonObject& detectionsRoot,
                         const QJsonObject& tracksRoot,
                         QHash<qint64, QVector<DetectionObservation>>* detectionsByFrameOut,
                         QHash<qint64, QVector<TrackObservation>>* tracksByFrameOut,
                         QString* errorOut)
{
    if (detectionsByFrameOut) {
        detectionsByFrameOut->clear();
    }
    if (tracksByFrameOut) {
        tracksByFrameOut->clear();
    }

    QHash<qint64, QSize> frameSizes;
    const QJsonArray frames = detectionsRoot.value(QStringLiteral("frames")).toArray();
    for (const QJsonValue& frameValue : frames) {
        const QJsonObject frameObject = frameValue.toObject();
        const qint64 frame = frameObject.value(QStringLiteral("frame")).toVariant().toLongLong();
        const QSize frameSize(frameObject.value(QStringLiteral("frame_width")).toInt(0),
                              frameObject.value(QStringLiteral("frame_height")).toInt(0));
        if (frame < 0 || frameSize.width() <= 0 || frameSize.height() <= 0) {
            continue;
        }
        frameSizes.insert(frame, frameSize);
        QVector<DetectionObservation>& frameDetections = (*detectionsByFrameOut)[frame];
        const QJsonArray detections = frameObject.value(QStringLiteral("detections")).toArray();
        frameDetections.reserve(frameDetections.size() + detections.size());
        for (const QJsonValue& detectionValue : detections) {
            const QJsonObject detection = detectionValue.toObject();
            const QRectF box(detection.value(QStringLiteral("x")).toDouble(),
                             detection.value(QStringLiteral("y")).toDouble(),
                             detection.value(QStringLiteral("w")).toDouble(),
                             detection.value(QStringLiteral("h")).toDouble());
            if (!box.isValid() || box.isEmpty()) {
                continue;
            }
            frameDetections.push_back(DetectionObservation{
                frame,
                box,
                frameSize,
                detection.value(QStringLiteral("confidence")).toDouble()});
        }
    }

    const QJsonArray tracks = tracksRoot.value(QStringLiteral("tracks")).toArray();
    for (const QJsonValue& trackValue : tracks) {
        const QJsonObject track = trackValue.toObject();
        const int trackId = track.value(QStringLiteral("track_id")).toInt(-1);
        if (trackId < 0) {
            continue;
        }
        const QJsonArray trackDetections = track.value(QStringLiteral("detections")).toArray();
        for (const QJsonValue& detectionValue : trackDetections) {
            const QJsonObject detection = detectionValue.toObject();
            const qint64 frame = detection.value(QStringLiteral("frame")).toVariant().toLongLong();
            if (frame < 0 || !frameSizes.contains(frame)) {
                continue;
            }
            const QRectF box = exactOrApproximateTrackBox(detection, frameSizes.value(frame));
            if (!box.isValid() || box.isEmpty()) {
                continue;
            }
            (*tracksByFrameOut)[frame].push_back(TrackObservation{
                trackId,
                frame,
                box,
                detection.value(QStringLiteral("score")).toDouble()});
        }
    }

    if (detectionsByFrameOut->isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("No detection observations were loaded.");
        }
        return;
    }
    if (errorOut) {
        errorOut->clear();
    }
}

FrameCoverageStats evaluateFrameCoverage(qint64 frame,
                                         const QVector<DetectionObservation>& detections,
                                         const QVector<TrackObservation>& tracks)
{
    QVector<MatchCandidate> candidates;
    for (int detectionIndex = 0; detectionIndex < detections.size(); ++detectionIndex) {
        const DetectionObservation& detection = detections.at(detectionIndex);
        for (int trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
            const TrackObservation& track = tracks.at(trackIndex);
            const double iou = jcut::facestream::continuityIou(detection.box, track.box);
            const double centerDistanceRatio =
                normalizedCenterDistance(detection.box, track.box);
            if (iou < 0.10 || centerDistanceRatio > 0.75) {
                continue;
            }
            candidates.push_back(MatchCandidate{
                detectionIndex,
                trackIndex,
                iou,
                centerDistanceRatio,
                (iou * 0.85) + ((1.0 - centerDistanceRatio) * 0.15)});
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const MatchCandidate& a, const MatchCandidate& b) {
        if (a.score == b.score) {
            if (a.detectionIndex == b.detectionIndex) {
                return a.trackIndex < b.trackIndex;
            }
            return a.detectionIndex < b.detectionIndex;
        }
        return a.score > b.score;
    });

    QVector<bool> detectionMatched(detections.size(), false);
    QVector<bool> trackMatched(tracks.size(), false);
    int matchedCount = 0;
    for (const MatchCandidate& candidate : candidates) {
        if (detectionMatched.at(candidate.detectionIndex) ||
            trackMatched.at(candidate.trackIndex)) {
            continue;
        }
        detectionMatched[candidate.detectionIndex] = true;
        trackMatched[candidate.trackIndex] = true;
        ++matchedCount;
    }

    FrameCoverageStats stats;
    stats.frame = frame;
    stats.detectionCount = detections.size();
    stats.trackCount = tracks.size();
    stats.matchedCount = matchedCount;
    stats.detectionRecall =
        detections.isEmpty() ? 1.0 : static_cast<double>(matchedCount) / detections.size();
    stats.trackPrecision =
        tracks.isEmpty() ? 1.0 : static_cast<double>(matchedCount) / tracks.size();
    return stats;
}

QHash<qint64, QVector<DetectionObservation>> filterEligibleDetections(
    const QHash<qint64, QVector<DetectionObservation>>& detectionsByFrame)
{
    QHash<qint64, QVector<DetectionObservation>> eligibleByFrame;
    QVector<qint64> frames = detectionsByFrame.keys().toVector();
    std::sort(frames.begin(), frames.end());
    for (int frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
        const qint64 frame = frames.at(frameIndex);
        const QVector<DetectionObservation>& detections = detectionsByFrame.value(frame);
        QVector<DetectionObservation> eligible;
        if (frameIndex == 0 || frameIndex == (frames.size() - 1)) {
            continue;
        }
        const QVector<DetectionObservation>& previousDetections =
            detectionsByFrame.value(frames.at(frameIndex - 1));
        const QVector<DetectionObservation>& nextDetections =
            detectionsByFrame.value(frames.at(frameIndex + 1));
        for (const DetectionObservation& detection : detections) {
            bool hasPreviousMatch = false;
            for (const DetectionObservation& previous : previousDetections) {
                if (detectionsLookTrackableTogether(detection.box, previous.box)) {
                    hasPreviousMatch = true;
                    break;
                }
            }
            if (!hasPreviousMatch) {
                continue;
            }
            bool hasNextMatch = false;
            for (const DetectionObservation& next : nextDetections) {
                if (detectionsLookTrackableTogether(detection.box, next.box)) {
                    hasNextMatch = true;
                    break;
                }
            }
            if (hasNextMatch) {
                eligible.push_back(detection);
            }
        }
        if (!eligible.isEmpty()) {
            eligibleByFrame.insert(frame, eligible);
        }
    }
    return eligibleByFrame;
}

int longestDetectionChainLength(
    const QHash<qint64, QVector<DetectionObservation>>& detectionsByFrame)
{
    QVector<qint64> frames = detectionsByFrame.keys().toVector();
    std::sort(frames.begin(), frames.end());
    QVector<QVector<int>> lengths;
    lengths.reserve(frames.size());
    int longest = 0;
    for (int frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
        const QVector<DetectionObservation>& detections =
            detectionsByFrame.value(frames.at(frameIndex));
        QVector<int> frameLengths(detections.size(), 1);
        if (frameIndex > 0) {
            const QVector<DetectionObservation>& previousDetections =
                detectionsByFrame.value(frames.at(frameIndex - 1));
            const QVector<int>& previousLengths = lengths.at(frameIndex - 1);
            for (int detectionIndex = 0; detectionIndex < detections.size(); ++detectionIndex) {
                for (int previousIndex = 0; previousIndex < previousDetections.size(); ++previousIndex) {
                    if (!detectionsLookTrackableTogether(detections.at(detectionIndex).box,
                                                         previousDetections.at(previousIndex).box)) {
                        continue;
                    }
                    frameLengths[detectionIndex] =
                        qMax(frameLengths[detectionIndex], previousLengths.at(previousIndex) + 1);
                }
            }
        }
        for (int value : frameLengths) {
            longest = qMax(longest, value);
        }
        lengths.push_back(frameLengths);
    }
    return longest;
}

CoverageStats evaluateCoverage(const QHash<qint64, QVector<DetectionObservation>>& rawDetectionsByFrame,
                               const QHash<qint64, QVector<TrackObservation>>& tracksByFrame)
{
    CoverageStats total;
    const QHash<qint64, QVector<DetectionObservation>> detectionsByFrame =
        filterEligibleDetections(rawDetectionsByFrame);
    total.longestDetectionChain = longestDetectionChainLength(rawDetectionsByFrame);
    QVector<qint64> allFrames = rawDetectionsByFrame.keys().toVector();
    std::sort(allFrames.begin(), allFrames.end());
    for (qint64 frame : allFrames) {
        total.totalDetections += rawDetectionsByFrame.value(frame).size();
    }

    QVector<qint64> frames = detectionsByFrame.keys().toVector();
    std::sort(frames.begin(), frames.end());

    QVector<FrameCoverageStats> perFrame;
    perFrame.reserve(frames.size());
    for (qint64 frame : frames) {
        const QVector<DetectionObservation> detections = detectionsByFrame.value(frame);
        const QVector<TrackObservation> tracks = tracksByFrame.value(frame);
        const FrameCoverageStats frameStats =
            evaluateFrameCoverage(frame, detections, tracks);
        perFrame.push_back(frameStats);

        total.eligibleDetections += frameStats.detectionCount;
        total.matchedDetections += frameStats.matchedCount;
        total.totalTrackObservations += frameStats.trackCount;
        total.matchedTrackObservations += frameStats.matchedCount;
        if (frameStats.detectionCount > 0) {
            ++total.framesWithEligibleDetections;
            total.worstFrameRecall = qMin(total.worstFrameRecall, frameStats.detectionRecall);
            if (frameStats.detectionRecall < 0.50) {
                ++total.lowRecallFrames;
            }
        }
    }

    total.framesWithDetections = rawDetectionsByFrame.size();
    total.detectionRecall = total.eligibleDetections > 0
        ? static_cast<double>(total.matchedDetections) / total.eligibleDetections
        : 1.0;
    total.trackPrecision = total.totalTrackObservations > 0
        ? static_cast<double>(total.matchedTrackObservations) / total.totalTrackObservations
        : 1.0;
    total.lowRecallFrameRatio = total.framesWithEligibleDetections > 0
        ? static_cast<double>(total.lowRecallFrames) / total.framesWithEligibleDetections
        : 0.0;

    std::sort(perFrame.begin(), perFrame.end(), [](const FrameCoverageStats& a, const FrameCoverageStats& b) {
        if (a.detectionRecall == b.detectionRecall) {
            return a.frame < b.frame;
        }
        return a.detectionRecall < b.detectionRecall;
    });
    for (int i = 0; i < qMin(5, perFrame.size()); ++i) {
        total.worstFrames.push_back(perFrame.at(i));
    }
    return total;
}

bool runRunner(const Options& options,
               const ClipSelection& selection,
               const QString& outputDir,
               QString* errorOut)
{
    if (!QFileInfo::exists(options.runnerPath)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Runner not found: %1").arg(options.runnerPath);
        }
        return false;
    }

    QStringList args{
        selection.filePath,
        QStringLiteral("--detector"), options.detector,
        QStringLiteral("--start-frame"), QString::number(qMax(0, options.startFrame)),
        QStringLiteral("--out-dir"), outputDir,
        QStringLiteral("--no-preview-window"),
        QStringLiteral("--no-preview-files"),
        QStringLiteral("--quiet"),
        QStringLiteral("--log-interval"), QStringLiteral("120"),
    };
    if (options.maxFrames > 0) {
        args << QStringLiteral("--max-frames") << QString::number(options.maxFrames);
    }
    args << options.passthroughArgs;

    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LD_LIBRARY_PATH"),
               QStringLiteral(EDITOR_FFMPEG_PREFIX) + QStringLiteral("/lib:") +
                   env.value(QStringLiteral("LD_LIBRARY_PATH")));
    process.setProcessEnvironment(env);
    process.setProgram(options.runnerPath);
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::ForwardedChannels);

    QElapsedTimer timer;
    timer.start();
    process.start();
    if (!process.waitForStarted(10000)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to start runner: %1").arg(process.errorString());
        }
        return false;
    }
    if (!process.waitForFinished(-1)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Runner did not finish cleanly.");
        }
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errorOut) {
            *errorOut =
                QStringLiteral("Runner failed with exit code %1 after %.2fs.")
                    .arg(process.exitCode())
                    .arg(static_cast<double>(timer.nsecsElapsed()) / 1'000'000'000.0, 0, 'f', 2);
        }
        return false;
    }
    return true;
}

QString transcriptPathForClip(const ClipSelection& selection)
{
    const QFileInfo clipInfo(selection.filePath);
    return clipInfo.dir().filePath(clipInfo.completeBaseName() + QStringLiteral(".json"));
}

bool loadBinaryObject(const QString& path, QJsonObject* rootOut)
{
    QString error;
    return jcut::jsonio::readBinaryJsonObject(path, rootOut, 0x4A435554, 1, &error);
}

bool writeBinaryObject(const QString& path, const QJsonObject& root)
{
    QString error;
    return jcut::jsonio::writeBinaryJsonObject(path, root, 0x4A435554, 1, &error);
}

jcut::facestream::ContinuityTrackingTuning trackingTuningFromSummary(const QJsonObject& summary)
{
    jcut::facestream::ContinuityTrackingTuning tuning;
    if (summary.isEmpty()) {
        return tuning;
    }
    tuning.trackMatchIouThreshold =
        static_cast<float>(summary.value(QStringLiteral("runtime_track_match_iou_threshold"))
                               .toDouble(tuning.trackMatchIouThreshold));
    tuning.newTrackMinConfidence =
        static_cast<float>(summary.value(QStringLiteral("runtime_new_track_min_confidence"))
                               .toDouble(tuning.newTrackMinConfidence));
    tuning.primaryFaceOnly =
        summary.value(QStringLiteral("runtime_primary_face_only")).toBool(tuning.primaryFaceOnly);
    return tuning;
}

bool rebuildTracksFromDetections(const QString& outputDir, QString* errorOut)
{
    const QString detectionsPath = QDir(outputDir).filePath(QStringLiteral("detections.bin"));
    const QString tracksPath = QDir(outputDir).filePath(QStringLiteral("tracks.bin"));
    const QString continuityPath = QDir(outputDir).filePath(QStringLiteral("continuity_facestream.bin"));
    const QString summaryPath = QDir(outputDir).filePath(QStringLiteral("summary.json"));

    QJsonObject detectionsRoot;
    if (!loadBinaryObject(detectionsPath, &detectionsRoot)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to load detections artifact: %1").arg(detectionsPath);
        }
        return false;
    }

    QJsonObject summaryRoot;
    jcut::jsonio::readJsonFile(summaryPath, &summaryRoot, nullptr);

    const QJsonArray rawFrames = detectionsRoot.value(QStringLiteral("frames")).toArray();
    const auto tuning = trackingTuningFromSummary(summaryRoot);
    QVector<jcut::facestream::ContinuityTrack> runtimeTracks;
    struct PersistedTrackAccumulator {
        int trackId = -1;
        int firstFrame = std::numeric_limits<int>::max();
        int lastFrame = std::numeric_limits<int>::min();
        int hits = 0;
        int misses = 0;
        QString state;
        QJsonArray detections;
    };
    std::map<int, PersistedTrackAccumulator> persistedTracksById;
    for (const QJsonValue& frameValue : rawFrames) {
        const QJsonObject frameObject = frameValue.toObject();
        const int frameNumber = frameObject.value(QStringLiteral("frame")).toInt(-1);
        const QSize frameSize(frameObject.value(QStringLiteral("frame_width")).toInt(0),
                              frameObject.value(QStringLiteral("frame_height")).toInt(0));
        if (frameNumber < 0 || frameSize.width() <= 0 || frameSize.height() <= 0) {
            continue;
        }
        QVector<jcut::facestream::Detection> detections;
        const QJsonArray detectionRows = frameObject.value(QStringLiteral("detections")).toArray();
        detections.reserve(detectionRows.size());
        for (const QJsonValue& detectionValue : detectionRows) {
            const QJsonObject detectionObject = detectionValue.toObject();
            jcut::facestream::Detection detection;
            detection.box = QRectF(detectionObject.value(QStringLiteral("x")).toDouble(),
                                   detectionObject.value(QStringLiteral("y")).toDouble(),
                                   detectionObject.value(QStringLiteral("w")).toDouble(),
                                   detectionObject.value(QStringLiteral("h")).toDouble());
            detection.confidence =
                static_cast<float>(detectionObject.value(QStringLiteral("confidence")).toDouble());
            if (detection.box.isValid() && !detection.box.isEmpty()) {
                detections.push_back(detection);
            }
        }
        jcut::facestream::updateContinuityTracks(&runtimeTracks, detections, frameNumber, frameSize, tuning);
        const QJsonArray frameTrackRows =
            jcut::facestream::frameTrackDetections(runtimeTracks, frameNumber);
        for (const QJsonValue& trackValue : frameTrackRows) {
            const QJsonObject trackObject = trackValue.toObject();
            const int trackId = trackObject.value(QStringLiteral("track_id")).toInt(-1);
            if (trackId < 0) {
                continue;
            }
            PersistedTrackAccumulator& accumulator = persistedTracksById[trackId];
            accumulator.trackId = trackId;
            accumulator.firstFrame = qMin(accumulator.firstFrame,
                                          trackObject.value(QStringLiteral("first_frame")).toInt(frameNumber));
            accumulator.lastFrame = qMax(accumulator.lastFrame,
                                         trackObject.value(QStringLiteral("last_frame")).toInt(frameNumber));
            accumulator.hits = qMax(accumulator.hits, trackObject.value(QStringLiteral("hits")).toInt(0));
            accumulator.misses = trackObject.value(QStringLiteral("misses")).toInt(accumulator.misses);
            accumulator.state = trackObject.value(QStringLiteral("track_state")).toString(accumulator.state);
            accumulator.detections.append(QJsonObject{
                {QStringLiteral("frame"), trackObject.value(QStringLiteral("frame")).toInt(frameNumber)},
                {QStringLiteral("x"), trackObject.value(QStringLiteral("x")).toDouble()},
                {QStringLiteral("y"), trackObject.value(QStringLiteral("y")).toDouble()},
                {QStringLiteral("box"), trackObject.value(QStringLiteral("box")).toDouble()},
                {QStringLiteral("score"), trackObject.value(QStringLiteral("score")).toDouble()}
            });
        }
    }

    QJsonArray trackRows;
    for (const auto& [trackId, accumulator] : persistedTracksById) {
        if (trackId < 0 || accumulator.detections.isEmpty()) {
            continue;
        }
        trackRows.append(QJsonObject{
            {QStringLiteral("track_id"), trackId},
            {QStringLiteral("first_frame"), accumulator.firstFrame == std::numeric_limits<int>::max() ? -1 : accumulator.firstFrame},
            {QStringLiteral("last_frame"), accumulator.lastFrame == std::numeric_limits<int>::min() ? -1 : accumulator.lastFrame},
            {QStringLiteral("length"), accumulator.detections.size()},
            {QStringLiteral("hits"), accumulator.hits},
            {QStringLiteral("misses"), accumulator.misses},
            {QStringLiteral("state"), accumulator.state},
            {QStringLiteral("detections"), accumulator.detections}
        });
    }

    const QString backend =
        detectionsRoot.value(QStringLiteral("backend")).toString(
            summaryRoot.value(QStringLiteral("backend")).toString());
    const QString frameDomain =
        detectionsRoot.value(QStringLiteral("frame_domain")).toString(QStringLiteral("source_absolute"));

    if (!writeBinaryObject(tracksPath, QJsonObject{
            {QStringLiteral("schema"), QStringLiteral("jcut_facestream_offscreen_tracks_v1")},
            {QStringLiteral("video"), detectionsRoot.value(QStringLiteral("video")).toString()},
            {QStringLiteral("backend"), backend},
            {QStringLiteral("frame_domain"), frameDomain},
            {QStringLiteral("tracks"), trackRows},
            {QStringLiteral("frame_summaries"), QJsonArray{}}
        })) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to write rebuilt tracks artifact: %1").arg(tracksPath);
        }
        return false;
    }

    const qint64 scanStart =
        rawFrames.isEmpty() ? 0 : rawFrames.first().toObject().value(QStringLiteral("frame")).toVariant().toLongLong();
    const qint64 scanEnd =
        rawFrames.isEmpty() ? 0 : rawFrames.last().toObject().value(QStringLiteral("frame")).toVariant().toLongLong();
    const QJsonObject continuityRoot = QJsonObject{
        {QStringLiteral("run_id"), QStringLiteral("retracked_%1")
             .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmss")))},
        {QStringLiteral("updated_at_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("only_dialogue"), false},
        {QStringLiteral("scan_start_frame"), scanStart},
        {QStringLiteral("scan_end_frame"), scanEnd},
        {QStringLiteral("raw_tracks"), trackRows},
        {QStringLiteral("raw_tracks_frame_domain"), frameDomain},
        {QStringLiteral("raw_frames"), rawFrames},
        {QStringLiteral("raw_frames_frame_domain"), frameDomain},
        {QStringLiteral("detector_mode"), backend}
    };
    writeBinaryObject(continuityPath, QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("jcut_facestream_v1")},
        {QStringLiteral("continuity_facestreams_by_clip"), QJsonObject{
            {QStringLiteral("facestream-offscreen-source"), continuityRoot}
        }}
    });
    return true;
}

bool buildInputsFromContinuityRoot(const QJsonObject& continuityRoot,
                                   ArtifactInputs* inputsOut)
{
    if (!inputsOut) {
        return false;
    }
    const QJsonArray rawFrames = continuityRoot.value(QStringLiteral("raw_frames")).toArray();
    const QJsonArray rawTracks = continuityRoot.value(QStringLiteral("raw_tracks")).toArray();
    if (rawFrames.isEmpty() || rawTracks.isEmpty()) {
        return false;
    }
    inputsOut->detectionsRoot = QJsonObject{
        {QStringLiteral("frames"), rawFrames}
    };
    inputsOut->tracksRoot = QJsonObject{
        {QStringLiteral("tracks"), rawTracks}
    };
    return true;
}

bool loadInputsFromExplicitOutputDir(const QString& outputDir, ArtifactInputs* inputsOut)
{
    if (!inputsOut || outputDir.trimmed().isEmpty()) {
        return false;
    }
    const QDir artifactDir(outputDir);
    QJsonObject summaryRoot;
    jcut::jsonio::readJsonFile(artifactDir.filePath(QStringLiteral("summary.json")), &summaryRoot, nullptr);
    const QString summaryVideo = summaryRoot.value(QStringLiteral("video")).toString().trimmed();
    if (!summaryVideo.isEmpty()) {
        inputsOut->videoPathOverride = summaryVideo;
    }
    const QStringList requestFiles = artifactDir.entryList(
        QStringList{QStringLiteral("*_facestream_request.json")}, QDir::Files, QDir::Name);
    if (!requestFiles.isEmpty()) {
        QJsonObject requestRoot;
        if (jcut::jsonio::readJsonFile(artifactDir.filePath(requestFiles.constFirst()), &requestRoot, nullptr)) {
            const QString requestVideo = requestRoot.value(QStringLiteral("media_path")).toString().trimmed();
            if (!requestVideo.isEmpty()) {
                inputsOut->videoPathOverride = requestVideo;
            }
            const QString requestClipId = requestRoot.value(QStringLiteral("clip_id")).toString().trimmed();
            if (!requestClipId.isEmpty()) {
                inputsOut->clipIdOverride = requestClipId;
            }
        }
    }
    const QString ndjsonPath = QDir(outputDir).filePath(QStringLiteral("facestream.ndjson"));
    if (QFileInfo::exists(ndjsonPath)) {
        QFile file(ndjsonPath);
        if (file.open(QIODevice::ReadOnly)) {
            QHash<qint64, QJsonObject> framesByNumber;
            QHash<int, QJsonObject> tracksById;
            while (!file.atEnd()) {
                const QByteArray line = file.readLine().trimmed();
                if (line.isEmpty()) {
                    continue;
                }
                QJsonObject object;
                QString error;
                if (!jcut::jsonio::parseObjectBytes(line, &object, &error)) {
                    continue;
                }
                if (object.value(QStringLiteral("type")).toString() != QStringLiteral("frame")) {
                    continue;
                }
                const qint64 frame = object.value(QStringLiteral("frame")).toVariant().toLongLong();
                if (frame < 0) {
                    continue;
                }
                const QJsonArray detectionBoxes = object.value(QStringLiteral("detection_boxes")).toArray();
                if (!detectionBoxes.isEmpty()) {
                    QJsonObject frameRoot;
                    frameRoot[QStringLiteral("frame")] = frame;
                    frameRoot[QStringLiteral("frame_width")] =
                        detectionBoxes.first().toObject().value(QStringLiteral("frame_width")).toInt(0);
                    frameRoot[QStringLiteral("frame_height")] =
                        detectionBoxes.first().toObject().value(QStringLiteral("frame_height")).toInt(0);
                    frameRoot[QStringLiteral("detections")] = detectionBoxes;
                    framesByNumber.insert(frame, frameRoot);
                }
                const QJsonArray trackDetections = object.value(QStringLiteral("track_detections")).toArray();
                for (const QJsonValue& value : trackDetections) {
                    const QJsonObject trackDetection = value.toObject();
                    const int trackId = trackDetection.value(QStringLiteral("track_id")).toInt(-1);
                    if (trackId < 0) {
                        continue;
                    }
                    QJsonObject trackRoot = tracksById.value(trackId);
                    if (trackRoot.isEmpty()) {
                        trackRoot[QStringLiteral("track_id")] = trackId;
                        trackRoot[QStringLiteral("first_frame")] = frame;
                        trackRoot[QStringLiteral("last_frame")] = frame;
                        trackRoot[QStringLiteral("detections")] = QJsonArray{};
                    } else {
                        trackRoot[QStringLiteral("first_frame")] =
                            qMin(trackRoot.value(QStringLiteral("first_frame")).toInt(frame), static_cast<int>(frame));
                        trackRoot[QStringLiteral("last_frame")] =
                            qMax(trackRoot.value(QStringLiteral("last_frame")).toInt(frame), static_cast<int>(frame));
                    }
                    QJsonArray detections = trackRoot.value(QStringLiteral("detections")).toArray();
                    detections.append(trackDetection);
                    trackRoot[QStringLiteral("detections")] = detections;
                    trackRoot[QStringLiteral("length")] = detections.size();
                    tracksById.insert(trackId, trackRoot);
                }
            }
            if (!framesByNumber.isEmpty()) {
                QList<qint64> frameNumbers = framesByNumber.keys();
                std::sort(frameNumbers.begin(), frameNumbers.end());
                QJsonArray frames;
                for (qint64 frame : frameNumbers) {
                    frames.append(framesByNumber.value(frame));
                }
                QList<int> trackIds = tracksById.keys();
                std::sort(trackIds.begin(), trackIds.end());
                QJsonArray tracks;
                for (int trackId : trackIds) {
                    tracks.append(tracksById.value(trackId));
                }
                inputsOut->sourceDescription =
                    QStringLiteral("facestream.ndjson in %1").arg(QDir(outputDir).absolutePath());
                inputsOut->detectionsRoot = QJsonObject{{QStringLiteral("frames"), frames}};
                inputsOut->tracksRoot = QJsonObject{{QStringLiteral("tracks"), tracks}};
                return true;
            }
        }
    }
    QJsonObject detectionsRoot;
    QJsonObject tracksRoot;
    if (!loadBinaryObject(QDir(outputDir).filePath(QStringLiteral("detections.bin")), &detectionsRoot) ||
        !loadBinaryObject(QDir(outputDir).filePath(QStringLiteral("tracks.bin")), &tracksRoot)) {
        return false;
    }
    inputsOut->sourceDescription =
        QStringLiteral("existing runner outputs in %1").arg(QDir(outputDir).absolutePath());
    inputsOut->detectionsRoot = detectionsRoot;
    inputsOut->tracksRoot = tracksRoot;
    return true;
}

bool loadInputsFromTranscriptArtifact(const ClipSelection& selection, ArtifactInputs* inputsOut)
{
    if (!inputsOut) {
        return false;
    }
    const QString transcriptPath = transcriptPathForClip(selection);
    QJsonObject artifactRoot;
    if (!loadBinaryObject(QFileInfo(transcriptPath).dir().filePath(
            QFileInfo(transcriptPath).completeBaseName() + QStringLiteral("_facestream.bin")),
            &artifactRoot)) {
        return false;
    }
    const QJsonObject continuityRoot = continuityRootForClip(artifactRoot, selection.clipId);
    if (!buildInputsFromContinuityRoot(continuityRoot, inputsOut)) {
        return false;
    }
    inputsOut->sourceDescription =
        QStringLiteral("persisted facestream sidecar %1").arg(
            QFileInfo(transcriptPath).dir().filePath(
                QFileInfo(transcriptPath).completeBaseName() + QStringLiteral("_facestream.bin")));
    return true;
}

bool loadInputsFromLatestDebugRun(const Options& options,
                                  const ClipSelection& selection,
                                  ArtifactInputs* inputsOut)
{
    if (!inputsOut) {
        return false;
    }
    const QDir projectDir(QFileInfo(options.projectStatePath).dir());
    const QDir clipDebugRoot(projectDir.filePath(
        QStringLiteral("debug/speaker_flow/%1").arg(selection.clipId)));
    if (!clipDebugRoot.exists()) {
        return false;
    }
    const QStringList runs =
        clipDebugRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    for (const QString& runId : runs) {
        const QDir artifactDir(clipDebugRoot.filePath(runId + QStringLiteral("/facestream_artifact")));
        QJsonObject detectionsRoot;
        QJsonObject tracksRoot;
        if (loadBinaryObject(artifactDir.filePath(QStringLiteral("detections.bin")), &detectionsRoot) &&
            loadBinaryObject(artifactDir.filePath(QStringLiteral("tracks.bin")), &tracksRoot)) {
            inputsOut->sourceDescription =
                QStringLiteral("debug run %1").arg(artifactDir.absolutePath());
            inputsOut->detectionsRoot = detectionsRoot;
            inputsOut->tracksRoot = tracksRoot;
            return true;
        }
        QJsonObject continuityArtifact;
        if (loadBinaryObject(artifactDir.filePath(QStringLiteral("continuity_facestream.bin")), &continuityArtifact)) {
            const QJsonObject continuityRoot =
                continuityRootForClip(continuityArtifact, QStringLiteral("facestream-offscreen-source"));
            if (buildInputsFromContinuityRoot(continuityRoot, inputsOut)) {
                inputsOut->sourceDescription =
                    QStringLiteral("debug continuity artifact %1").arg(artifactDir.absolutePath());
                return true;
            }
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Options options;
    if (!parseArgs(app.arguments(), &options)) {
        return 2;
    }

    ClipSelection selection;
    QString error;
    if (!resolveClipSelection(options.projectStatePath, options.clipId, &selection, &error)) {
        std::cerr << error.toStdString() << "\n";
        return 2;
    }
    if (!QFileInfo::exists(selection.filePath)) {
        std::cerr << "Clip media not found: " << selection.filePath.toStdString() << "\n";
        return 2;
    }

    if (options.retrack) {
        if (options.outputDir.trimmed().isEmpty()) {
            std::cerr << "--retrack requires --output-dir pointing at an existing detections artifact directory.\n";
            return 2;
        }
        QString retrackError;
        if (!rebuildTracksFromDetections(options.outputDir, &retrackError)) {
            std::cerr << retrackError.toStdString() << "\n";
            return 1;
        }
    }

    std::unique_ptr<QTemporaryDir> tempDir;
    QString outputDir = options.outputDir.trimmed();
    ArtifactInputs inputs;
    bool loadedInputs = false;

    if (!outputDir.isEmpty()) {
        loadedInputs = loadInputsFromExplicitOutputDir(outputDir, &inputs);
    }
    if (!loadedInputs) {
        loadedInputs = loadInputsFromTranscriptArtifact(selection, &inputs);
    }
    if (!loadedInputs) {
        loadedInputs = loadInputsFromLatestDebugRun(options, selection, &inputs);
    }

    if (!loadedInputs && !options.rerun) {
        std::cerr
            << "No persisted clip artifacts were found for parity checking.\n"
            << "Looked for existing detections/tracks in:\n"
            << "  1. --output-dir (if provided)\n"
            << "  2. sibling transcript facestream sidecar for " << selection.filePath.toStdString() << "\n"
            << "  3. latest debug/speaker_flow run for clip " << selection.clipId.toStdString() << "\n"
            << "Pass --rerun to recompute instead of reusing persisted artifacts.\n";
        return 1;
    }

    if (!loadedInputs && options.rerun) {
        if (outputDir.isEmpty()) {
            tempDir = std::make_unique<QTemporaryDir>();
            if (!tempDir->isValid()) {
                std::cerr << "Failed to create a temporary output directory.\n";
                return 2;
            }
            tempDir->setAutoRemove(!options.keepOutput);
            outputDir = tempDir->path();
        } else {
            QDir().mkpath(outputDir);
        }
        const QString clipIdForReport =
            inputs.clipIdOverride.trimmed().isEmpty() ? selection.clipId : inputs.clipIdOverride.trimmed();
        const QString videoForReport =
            inputs.videoPathOverride.trimmed().isEmpty() ? selection.filePath : inputs.videoPathOverride.trimmed();
        std::cout << "clip_id=" << clipIdForReport.toStdString()
                  << " video=" << videoForReport.toStdString()
                  << " duration_frames=" << selection.durationFrames
                  << " output_dir=" << outputDir.toStdString() << "\n";
        if (!runRunner(options, selection, outputDir, &error)) {
            if (tempDir) {
                tempDir->setAutoRemove(false);
            }
            std::cerr << error.toStdString() << "\n";
            std::cerr << "retained_output_dir=" << outputDir.toStdString() << "\n";
            return 1;
        }
        if (!loadInputsFromExplicitOutputDir(outputDir, &inputs)) {
            if (tempDir) {
                tempDir->setAutoRemove(false);
            }
            std::cerr << "Runner completed but detections.bin/tracks.bin could not be reloaded.\n";
            std::cerr << "retained_output_dir=" << outputDir.toStdString() << "\n";
            return 1;
        }
    }

    const QString clipIdForReport =
        inputs.clipIdOverride.trimmed().isEmpty() ? selection.clipId : inputs.clipIdOverride.trimmed();
    const QString videoForReport =
        inputs.videoPathOverride.trimmed().isEmpty() ? selection.filePath : inputs.videoPathOverride.trimmed();
    std::cout << "clip_id=" << clipIdForReport.toStdString()
              << " video=" << videoForReport.toStdString()
              << " duration_frames=" << selection.durationFrames
              << " input_source=" << inputs.sourceDescription.toStdString() << "\n";

    QHash<qint64, QVector<DetectionObservation>> detectionsByFrame;
    QHash<qint64, QVector<TrackObservation>> tracksByFrame;
    collectObservations(inputs.detectionsRoot, inputs.tracksRoot, &detectionsByFrame, &tracksByFrame, &error);
    if (!error.isEmpty()) {
        if (tempDir) {
            tempDir->setAutoRemove(false);
        }
        std::cerr << error.toStdString() << "\n";
        std::cerr << "retained_output_dir=" << outputDir.toStdString() << "\n";
        return 1;
    }

    const CoverageStats stats = evaluateCoverage(detectionsByFrame, tracksByFrame);
    std::cout << "total_detections=" << stats.totalDetections
              << " eligible_detections=" << stats.eligibleDetections
              << " matched_detections=" << stats.matchedDetections
              << " detection_recall=" << stats.detectionRecall
              << " total_track_observations=" << stats.totalTrackObservations
              << " matched_track_observations=" << stats.matchedTrackObservations
              << " track_precision=" << stats.trackPrecision
              << " longest_detection_chain=" << stats.longestDetectionChain
              << " low_recall_frames=" << stats.lowRecallFrames
              << " low_recall_frame_ratio=" << stats.lowRecallFrameRatio
              << " worst_frame_recall=" << stats.worstFrameRecall
              << "\n";
    for (const FrameCoverageStats& frame : stats.worstFrames) {
        std::cout << "frame=" << frame.frame
                  << " detections=" << frame.detectionCount
                  << " tracks=" << frame.trackCount
                  << " matched=" << frame.matchedCount
                  << " recall=" << frame.detectionRecall
                  << " precision=" << frame.trackPrecision
                  << "\n";
    }

    const bool zeroTracksAcceptable =
        stats.totalTrackObservations == 0 &&
        stats.longestDetectionChain < options.minTrackWorthyChain;
    const bool recallOk = zeroTracksAcceptable || stats.detectionRecall >= options.minDetectionRecall;
    const bool precisionOk = zeroTracksAcceptable || stats.trackPrecision >= options.minTrackPrecision;
    const bool worstFrameOk =
        zeroTracksAcceptable ||
        stats.worstFrameRecall >= options.minWorstFrameRecall ||
        stats.lowRecallFrameRatio <= options.maxLowRecallFrameRatio;
    if (!recallOk || !precisionOk || !worstFrameOk) {
        if (tempDir) {
            tempDir->setAutoRemove(false);
        }
        std::cerr << "coverage check failed"
                  << " min_detection_recall=" << options.minDetectionRecall
                  << " min_track_precision=" << options.minTrackPrecision
                  << " min_worst_frame_recall=" << options.minWorstFrameRecall
                  << " max_low_recall_frame_ratio=" << options.maxLowRecallFrameRatio
                  << " min_track_worthy_chain=" << options.minTrackWorthyChain
                  << " retained_output_dir=" << outputDir.toStdString()
                  << "\n";
        return 1;
    }

    return 0;
}
