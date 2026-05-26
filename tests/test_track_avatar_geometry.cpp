#include "track_avatar_utils.h"

#include "decoder_context.h"
#include "facedetections_artifact_utils.h"
#include "facedetections_runtime.h"
#include "startup_project_state.h"
#include "transcript_engine.h"
#include "editor_shared_render_sync.h"
#include "editor_shared_transcript.h"

#include <QDir>
#include <QDebug>
#include <QImage>
#include <QFileInfo>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QtTest/QtTest>

#include <cmath>

namespace {

constexpr int kAvatarSize = 160;
constexpr int kRandomTrackSampleCount = 25;
constexpr int kMinTrackKeyframesForAvatarSampling = 10;

struct TrackAvatarSample {
    int trackId = -1;
    int keyframeIndex = -1;
    int keyframeCount = 0;
    int64_t storedFrame = -1;
    FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
    QJsonObject keyframe;
};

struct ActiveProjectTrackContext {
    TimelineClip clip;
    QString description;
    QVector<RenderSyncMarker> renderSyncMarkers;
    QVector<TrackAvatarSample> samples;
};

QVector<RenderSyncMarker> renderSyncMarkersFromStartupRoot(const QJsonObject& root)
{
    QVector<RenderSyncMarker> markers;
    const QJsonArray markerArray = root.value(QStringLiteral("renderSyncMarkers")).toArray();
    markers.reserve(markerArray.size());
    for (const QJsonValue& value : markerArray) {
        const QJsonObject obj = value.toObject();
        if (obj.isEmpty()) {
            continue;
        }
        RenderSyncMarker marker;
        marker.clipId = obj.value(QStringLiteral("clipId")).toString();
        marker.frame = qMax<int64_t>(0, obj.value(QStringLiteral("frame")).toVariant().toLongLong());
        marker.action = renderSyncActionFromString(obj.value(QStringLiteral("action")).toString());
        marker.count = qMax(1, obj.value(QStringLiteral("count")).toInt(1));
        markers.push_back(marker);
    }
    return markers;
}

qreal pixelFractionMatching(const QImage& image,
                            const QRect& rect,
                            const std::function<bool(const QColor&)>& predicate)
{
    const QRect bounded = rect.intersected(image.rect());
    if (bounded.isEmpty()) {
        return 0.0;
    }

    int matches = 0;
    int total = 0;
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() <= 0) {
                continue;
            }
            ++total;
            if (predicate(color)) {
                ++matches;
            }
        }
    }
    if (total <= 0) {
        return 0.0;
    }
    return static_cast<qreal>(matches) / static_cast<qreal>(total);
}

struct RegionLumaStats {
    qreal mean = 0.0;
    qreal stddev = 0.0;
    qreal min = 0.0;
    qreal max = 0.0;
};

RegionLumaStats lumaStats(const QImage& image, const QRect& rect)
{
    const QRect bounded = rect.intersected(image.rect());
    RegionLumaStats stats;
    if (bounded.isEmpty()) {
        return stats;
    }

    qreal sum = 0.0;
    qreal sumSquares = 0.0;
    qreal minLuma = 255.0;
    qreal maxLuma = 0.0;
    int total = 0;
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() <= 0) {
                continue;
            }
            const qreal luma =
                (0.2126 * color.red()) + (0.7152 * color.green()) + (0.0722 * color.blue());
            sum += luma;
            sumSquares += luma * luma;
            minLuma = qMin(minLuma, luma);
            maxLuma = qMax(maxLuma, luma);
            ++total;
        }
    }
    if (total <= 0) {
        return stats;
    }

    stats.mean = sum / static_cast<qreal>(total);
    const qreal variance =
        qMax<qreal>(0.0, (sumSquares / static_cast<qreal>(total)) - (stats.mean * stats.mean));
    stats.stddev = std::sqrt(variance);
    stats.min = minLuma;
    stats.max = maxLuma;
    return stats;
}

bool avatarLooksFaceLikeForProjectFallback(const QImage& image, QString* reasonOut)
{
    const RegionLumaStats center = lumaStats(image, QRect(32, 28, 96, 98));
    const RegionLumaStats full = lumaStats(image, image.rect());
    const qreal upperDarkFraction = pixelFractionMatching(
        image,
        QRect(36, 30, 88, 50),
        [](const QColor& color) {
            const int darkest = qMin(color.red(), qMin(color.green(), color.blue()));
            return darkest < 70;
        });

    const bool plausible =
        center.stddev >= 18.0 &&
        (center.max - center.min) >= 70.0 &&
        full.stddev >= 14.0 &&
        upperDarkFraction >= 0.01 &&
        upperDarkFraction <= 0.45;
    if (!plausible && reasonOut) {
        *reasonOut = QStringLiteral(
            "Rendered avatar did not look face-like enough for project-backed validation "
            "(center stddev=%1, center range=%2, full stddev=%3, upper dark=%4).")
                .arg(center.stddev, 0, 'f', 1)
                .arg(center.max - center.min, 0, 'f', 1)
                .arg(full.stddev, 0, 'f', 1)
                .arg(upperDarkFraction, 0, 'f', 3);
    }
    return plausible;
}

QImage renderAvatarForRepresentativeTrack(const TimelineClip& clip,
                                          editor::DecoderContext& decoder,
                                          const QJsonObject& representativeKeyframe,
                                          QString* errorOut)
{
    if (clip.filePath.trimmed().isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Selected clip has an empty media path.");
        }
        return {};
    }

    const int64_t sourceFrame =
        qMax<int64_t>(0, representativeKeyframe.value(QStringLiteral("frame")).toVariant().toLongLong());
    const int64_t decodeFrame = sourceFrame;
    const editor::FrameHandle frame = decoder.decodeFrame(decodeFrame);
    if (!frame.hasCpuImage()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Decoder returned no CPU image for frame %1").arg(decodeFrame);
        }
        return {};
    }

    const QImage avatar = renderTrackAvatarImage(frame.cpuImage(), representativeKeyframe, kAvatarSize);
    if (avatar.isNull() && errorOut) {
        *errorOut = QStringLiteral("Avatar crop rendered a null image.");
    }
    return avatar;
}

bool loadActiveProjectTrackContext(ActiveProjectTrackContext* contextOut, QString* errorOut)
{
    if (contextOut) {
        *contextOut = ActiveProjectTrackContext{};
    }

    QString projectId;
    QString statePath;
    const QJsonObject startupPayload =
        editor_startup::loadActiveProjectStartupStatePayload(&projectId, &statePath, nullptr, nullptr);
    const QJsonObject root = startupPayload.value(QStringLiteral("root")).toObject();
    if (root.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Active project state is empty.");
        }
        return false;
    }

    ActiveProjectTrackContext context;
    if (!editor_startup::startupSelectedClip(root, &context.clip)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Active project has no selected clip.");
        }
        return false;
    }
    context.renderSyncMarkers = renderSyncMarkersFromStartupRoot(root);

    const QString transcriptPath = transcriptPathForRuntimeSidecarForClipFile(context.clip.filePath);
    if (transcriptPath.trimmed().isEmpty() || !QFileInfo::exists(transcriptPath)) {
        if (errorOut) {
            *errorOut = QStringLiteral("No transcript sidecar found for selected clip %1").arg(context.clip.filePath);
        }
        return false;
    }

    editor::TranscriptEngine engine;
    QJsonDocument transcriptDocument;
    if (!engine.loadTranscriptJson(transcriptPath, &transcriptDocument)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to load transcript sidecar %1").arg(transcriptPath);
        }
        return false;
    }

    QJsonObject facedetectionsArtifact;
    if (!engine.loadFacestreamArtifact(transcriptPath, &facedetectionsArtifact)) {
        if (errorOut) {
            *errorOut = QStringLiteral("No facedetections artifact for transcript %1").arg(transcriptPath);
        }
        return false;
    }

    const QJsonObject continuityRoot = continuityRootForClip(facedetectionsArtifact, context.clip.id);
    if (continuityRoot.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("No continuity payload for selected clip %1").arg(context.clip.id);
        }
        return false;
    }

    const QJsonArray streams =
        jcut::facedetections::continuityStreamsForRoot(continuityRoot, transcriptDocument.object());
    if (streams.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Continuity payload has no tracks for selected clip %1").arg(context.clip.id);
        }
        return false;
    }

    QVector<QJsonObject> eligibleStreams;
    eligibleStreams.reserve(streams.size());
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject stream = streamValue.toObject();
        if (stream.value(QStringLiteral("keyframes")).toArray().size() >= kMinTrackKeyframesForAvatarSampling) {
            eligibleStreams.push_back(stream);
        }
    }
    if (eligibleStreams.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("No continuity tracks had at least %1 keyframes.")
                            .arg(kMinTrackKeyframesForAvatarSampling);
        }
        return false;
    }

    QRandomGenerator rng(0x4a435554u);
    context.samples.reserve(kRandomTrackSampleCount);
    for (int i = 0; i < kRandomTrackSampleCount; ++i) {
        const QJsonObject stream = eligibleStreams.at(rng.bounded(eligibleStreams.size()));
        const QJsonArray keyframes = stream.value(QStringLiteral("keyframes")).toArray();
        const int mid = keyframes.size() / 2;
        const int halfWindow = qMax(1, keyframes.size() / 4);
        const int minIndex = qMax(0, mid - halfWindow);
        const int maxExclusive = qMin(keyframes.size(), mid + halfWindow + 1);
        const int keyframeIndex = minIndex + rng.bounded(qMax(1, maxExclusive - minIndex));
        QJsonObject keyframe = keyframes.at(keyframeIndex).toObject();
        if (keyframe.isEmpty()) {
            --i;
            continue;
        }
        FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
        if (!parseFacestreamFrameDomainString(
                stream.value(QStringLiteral("frame_domain")).toString().trimmed(),
                &frameDomain)) {
            frameDomain = FacestreamFrameDomain::SourceRelative;
        }
        const int64_t storedFrame = keyframe.value(QStringLiteral("frame")).toVariant().toLongLong();
        keyframe[QStringLiteral("frame")] = static_cast<qint64>(
            qMax<int64_t>(0, mapFacestreamFrameToSourceFrame(
                                 context.clip,
                                 storedFrame,
                                 frameDomain,
                                 context.renderSyncMarkers)));

        TrackAvatarSample sample;
        sample.trackId = stream.value(QStringLiteral("track_id")).toInt(-1);
        sample.keyframeIndex = keyframeIndex;
        sample.keyframeCount = keyframes.size();
        sample.storedFrame = storedFrame;
        sample.frameDomain = frameDomain;
        sample.keyframe = keyframe;
        context.samples.push_back(sample);
    }

    context.description =
        QStringLiteral("active project %1, clip %2, state %3")
            .arg(projectId, context.clip.id, statePath);
    if (contextOut) {
        *contextOut = context;
    }
    return true;
}

} // namespace

class TrackAvatarGeometryTest : public QObject {
    Q_OBJECT

private slots:
    void avatarPngRetainsVisibleFace()
    {
        ActiveProjectTrackContext context;
        QString activeProjectError;
        QVERIFY2(loadActiveProjectTrackContext(&context, &activeProjectError),
                 qPrintable(QStringLiteral(
                     "Expected the normal startup project and selected clip to be available to this test: %1")
                                .arg(activeProjectError)));
        QCOMPARE(context.samples.size(), kRandomTrackSampleCount);
        qInfo().noquote()
            << QStringLiteral("[track-avatar-test] Using startup-selected project clip: %1")
                   .arg(context.description);

        editor::DecoderContext decoder(context.clip.filePath);
        QVERIFY2(decoder.initialize(),
                 qPrintable(QStringLiteral("Decoder failed to initialize for %1").arg(context.clip.filePath)));

        const QDir outputDir(QStringLiteral(QT_TESTCASE_BUILDDIR));
        for (const QString& staleFile : outputDir.entryList(
                 QStringList{QStringLiteral("track_avatar_random_*.png")},
                 QDir::Files)) {
            QFile::remove(outputDir.filePath(staleFile));
        }

        QStringList failures;
        for (int i = 0; i < context.samples.size(); ++i) {
            const TrackAvatarSample& sample = context.samples.at(i);
            qInfo().noquote()
                << QStringLiteral("[track-avatar-test] sample %1 track %2 keyframe %3/%4 stored_frame %5 mapped_source_frame %6 x %7 y %8 box %9")
                       .arg(i)
                       .arg(sample.trackId)
                       .arg(sample.keyframeIndex)
                       .arg(sample.keyframeCount)
                       .arg(sample.storedFrame)
                       .arg(sample.keyframe.value(QStringLiteral("frame")).toVariant().toLongLong())
                       .arg(sample.keyframe.value(QStringLiteral("x")).toDouble(), 0, 'f', 4)
                       .arg(sample.keyframe.value(QStringLiteral("y")).toDouble(), 0, 'f', 4)
                       .arg(sample.keyframe.value(QStringLiteral("box_size")).toDouble(
                                sample.keyframe.value(QStringLiteral("box")).toDouble()), 0, 'f', 4);
            QString renderError;
            const QImage avatar =
                renderAvatarForRepresentativeTrack(context.clip, decoder, sample.keyframe, &renderError);
            if (avatar.isNull()) {
                failures.push_back(QStringLiteral("sample %1 track %2 keyframe %3/%4: null avatar: %5")
                                       .arg(i)
                                       .arg(sample.trackId)
                                       .arg(sample.keyframeIndex)
                                       .arg(sample.keyframeCount)
                                       .arg(renderError));
                continue;
            }

            const QString avatarPath =
                outputDir.filePath(QStringLiteral("track_avatar_random_%1_track_%2.png")
                                       .arg(i, 2, 10, QLatin1Char('0'))
                                       .arg(sample.trackId));
            qInfo().noquote()
                << QStringLiteral("[track-avatar-test] Wrote avatar PNG: %1").arg(avatarPath);
            QVERIFY2(avatar.save(avatarPath), "Failed to save rendered avatar PNG.");

            QImage reloaded(avatarPath);
            if (reloaded.isNull()) {
                failures.push_back(QStringLiteral("sample %1 track %2 keyframe %3/%4: failed to read back %5")
                                       .arg(i)
                                       .arg(sample.trackId)
                                       .arg(sample.keyframeIndex)
                                       .arg(sample.keyframeCount)
                                       .arg(avatarPath));
                continue;
            }
            if (reloaded.size() != QSize(kAvatarSize, kAvatarSize)) {
                failures.push_back(QStringLiteral("sample %1 track %2 keyframe %3/%4: wrong PNG size %5x%6")
                                       .arg(i)
                                       .arg(sample.trackId)
                                       .arg(sample.keyframeIndex)
                                       .arg(sample.keyframeCount)
                                       .arg(reloaded.width())
                                       .arg(reloaded.height()));
                continue;
            }
            QString reason;
            if (!avatarLooksFaceLikeForProjectFallback(reloaded, &reason)) {
                failures.push_back(QStringLiteral("sample %1 track %2 keyframe %3/%4: %5 (%6)")
                                       .arg(i)
                                       .arg(sample.trackId)
                                       .arg(sample.keyframeIndex)
                                       .arg(sample.keyframeCount)
                                       .arg(reason)
                                       .arg(avatarPath));
            }
        }
        QVERIFY2(failures.isEmpty(),
                 qPrintable(QStringLiteral("One or more random track avatars did not contain a visible face:\n%1")
                                .arg(failures.join(QLatin1Char('\n')))));
    }
};

QTEST_MAIN(TrackAvatarGeometryTest)

#include "test_track_avatar_geometry.moc"
