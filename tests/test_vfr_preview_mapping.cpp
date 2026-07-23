#include <QtTest/QtTest>

#include "../decoder_context.h"
#include "../editor_shared_render_sync.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>

#include <cmath>

namespace {

struct FrameMapEntry {
    int64_t sourceFrame = -1;
    int64_t decodedOrdinal = -1;
};

QProcessEnvironment externalToolEnvironment()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // CTest supplies JCut's bundled FFmpeg libraries for the test binary. A
    // host ffmpeg/python process must resolve its own compatible libraries.
    environment.remove(QStringLiteral("LD_LIBRARY_PATH"));
    return environment;
}

bool runExternalTool(const QString& program,
                     const QStringList& arguments,
                     QByteArray* standardOutput,
                     QString* errorOut)
{
    QProcess process;
    process.setProcessEnvironment(externalToolEnvironment());
    process.start(program, arguments);
    if (!process.waitForStarted(5000)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Could not start %1: %2")
                            .arg(program, process.errorString());
        }
        return false;
    }
    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished();
        if (errorOut) {
            *errorOut = QStringLiteral("%1 timed out").arg(program);
        }
        return false;
    }
    if (standardOutput) {
        *standardOutput = process.readAllStandardOutput();
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("%1 exited with code %2: %3")
                            .arg(program)
                            .arg(process.exitCode())
                            .arg(QString::fromUtf8(process.readAllStandardError()).trimmed());
        }
        return false;
    }
    return true;
}

QVector<FrameMapEntry> readFrameMap(const QString& path, QString* errorOut)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = file.errorString();
        return {};
    }

    QVector<FrameMapEntry> entries;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const QStringList columns = line.split(QLatin1Char('\t'));
        if (columns.size() != 2) {
            if (errorOut) *errorOut = QStringLiteral("Malformed map row: %1").arg(line);
            return {};
        }
        bool sourceOk = false;
        bool ordinalOk = false;
        const int64_t sourceFrame = columns.at(0).toLongLong(&sourceOk);
        const int64_t ordinal = columns.at(1).toLongLong(&ordinalOk);
        if (!sourceOk || !ordinalOk || ordinal != entries.size()) {
            if (errorOut) *errorOut = QStringLiteral("Invalid map row: %1").arg(line);
            return {};
        }
        entries.push_back({sourceFrame, ordinal});
    }
    return entries;
}

int64_t previewDecodedOrdinal(const QString& python,
                              const QString& helperPath,
                              const QString& sourcePath,
                              int64_t requestedSourceFrame,
                              QString* errorOut)
{
    QByteArray output;
    if (!runExternalTool(
            python,
            {helperPath,
             QStringLiteral("--input"), sourcePath,
             QStringLiteral("--lookup-source-frame"),
             QString::number(requestedSourceFrame)},
            &output,
            errorOut)) {
        return -1;
    }
    bool ok = false;
    const int64_t oneBasedOrdinal = QString::fromUtf8(output).trimmed().toLongLong(&ok);
    if (!ok || oneBasedOrdinal <= 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("Invalid ordinal output: %1")
                            .arg(QString::fromUtf8(output).trimmed());
        }
        return -1;
    }
    return oneBasedOrdinal - 1;
}

TimelineClip makeCut(const QString& id,
                     const QString& sourcePath,
                     int64_t startFrame,
                     int64_t sourceInFrame,
                     qreal playbackRate,
                     qreal sourceFps,
                     int64_t sourceDurationFrames)
{
    TimelineClip clip;
    clip.id = id;
    clip.mediaType = ClipMediaType::Video;
    clip.filePath = sourcePath;
    clip.startFrame = startFrame;
    clip.durationFrames = 30;
    clip.sourceInFrame = sourceInFrame;
    clip.sourceDurationFrames = sourceDurationFrames;
    clip.sourceFps = sourceFps;
    clip.playbackRate = playbackRate;
    return clip;
}

} // namespace

class TestVfrPreviewMapping : public QObject {
    Q_OBJECT

private slots:
    void syntheticVfrStaysAlignedAcrossCutsRatesAndMarkers();
};

void TestVfrPreviewMapping::syntheticVfrStaysAlignedAcrossCutsRatesAndMarkers()
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (ffmpeg.isEmpty() || python.isEmpty()) {
        QSKIP("ffmpeg and python3 are required for the synthetic VFR integration test");
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString sourcePath = temporary.filePath(QStringLiteral("synthetic-vfr.mkv"));
    const QString mapPath = temporary.filePath(QStringLiteral("jcut_frame_map.tsv"));
    const QString mapMetadataPath = temporary.filePath(QStringLiteral("jcut_frame_map.json"));
    const QString helperPath =
        QDir(QStringLiteral(JCUT_SOURCE_DIR)).filePath(QStringLiteral("jcut_frame_index_map.py"));

    QString error;
    const QString vfrPts = QStringLiteral(
        "setpts='if(lt(N,3),N/10/TB,"
        "if(lt(N,6),(0.3+(N-3)/5)/TB,(0.9+(N-6)/10)/TB))'");
    QVERIFY2(runExternalTool(
                 ffmpeg,
                 {QStringLiteral("-hide_banner"),
                  QStringLiteral("-loglevel"), QStringLiteral("error"),
                  QStringLiteral("-y"),
                  QStringLiteral("-f"), QStringLiteral("lavfi"),
                  QStringLiteral("-i"),
                  QStringLiteral("testsrc=size=32x24:rate=10:duration=1"),
                  QStringLiteral("-vf"), vfrPts,
                  QStringLiteral("-fps_mode"), QStringLiteral("vfr"),
                  QStringLiteral("-c:v"), QStringLiteral("ffv1"),
                  sourcePath},
                 nullptr,
                 &error),
             qPrintable(error));

    QVERIFY2(runExternalTool(
                 python,
                 {helperPath,
                  QStringLiteral("--input"), sourcePath,
                  QStringLiteral("--output"), mapPath},
                 nullptr,
                 &error),
             qPrintable(error));

    const QVector<FrameMapEntry> frameMap = readFrameMap(mapPath, &error);
    QVERIFY2(!frameMap.isEmpty(), qPrintable(error));
    QCOMPARE(frameMap.size(), 10);
    bool hasUnitStep = false;
    bool hasVfrGap = false;
    for (qsizetype index = 1; index < frameMap.size(); ++index) {
        const int64_t delta =
            frameMap.at(index).sourceFrame - frameMap.at(index - 1).sourceFrame;
        hasUnitStep = hasUnitStep || delta == 1;
        hasVfrGap = hasVfrGap || delta > 1;
    }
    QVERIFY2(hasUnitStep && hasVfrGap,
             "The fixture must contain genuinely non-uniform presentation timestamps");

    QFile metadataFile(mapMetadataPath);
    QVERIFY(metadataFile.open(QIODevice::ReadOnly));
    const QJsonObject metadata =
        QJsonDocument::fromJson(metadataFile.readAll()).object();
    QCOMPARE(metadata.value(QStringLiteral("schema")).toString(),
             QStringLiteral("jcut_frame_index_map_v2"));
    QCOMPARE(metadata.value(QStringLiteral("mapped_frame_count")).toInteger(),
             qint64(frameMap.size()));

    editor::DecoderContext sourceProbe(sourcePath, nullptr, true);
    QVERIFY(sourceProbe.initialize());
    const qreal sourceFps = sourceProbe.info().fps;
    const int64_t sourceDurationFrames = sourceProbe.info().durationFrames;
    QVERIFY(sourceFps > 0.0);
    QVERIFY(sourceDurationFrames > 0);
    QVERIFY2(std::abs(sourceFps -
                      metadata.value(QStringLiteral("source_frame_rate")).toDouble()) < 0.000001,
             "DecoderContext and the ordinal-map generator must share one source-frame rate");

    const TimelineClip firstCut = makeCut(
        QStringLiteral("vfr-cut-a"), sourcePath,
        100, 2, 1.0, sourceFps, sourceDurationFrames);
    TimelineClip fastCut = firstCut;
    fastCut.id = QStringLiteral("vfr-cut-fast");
    fastCut.playbackRate = 1.5;
    const TimelineClip slowCut = makeCut(
        QStringLiteral("vfr-cut-b"), sourcePath,
        200, 6, 0.5, sourceFps, sourceDurationFrames);

    RenderSyncMarker skipMarker;
    skipMarker.clipId = fastCut.id;
    skipMarker.frame = fastCut.startFrame + 3;
    skipMarker.action = RenderSyncAction::SkipFrame;
    skipMarker.count = 2;

    struct Scenario {
        const char* name;
        TimelineClip clip;
        qreal timelineFrame = 0.0;
        QVector<RenderSyncMarker> markers;
        int64_t expectedRequestedFrame = -1;
        int64_t expectedPresentedFrame = -1;
        int64_t expectedDecodedOrdinal = -1;
    };
    const QVector<Scenario> scenarios{
        {"cut-source-in", firstCut, 100.0, {}, 2, 2, 2},
        {"cut-enters-vfr-gap", firstCut, 106.0, {}, 4, 5, 4},
        {"fast-rate-enters-vfr-gap", fastCut, 104.0, {}, 4, 5, 4},
        {"fast-rate-with-skip-marker", fastCut, 104.0, {skipMarker}, 5, 5, 4},
        {"second-cut-slow-rate", slowCut, 206.0, {}, 7, 9, 6},
    };

    for (const Scenario& scenario : scenarios) {
        const QByteArray context = QByteArray("scenario=") + scenario.name;
        const QVector<TimelineClip> clips{scenario.clip};
        const int64_t requestedSourceFrame =
            requestedSourceFrameForGeneratedMaskPreview(
                scenario.clip,
                clips,
                scenario.timelineFrame,
                scenario.markers);
        QCOMPARE(requestedSourceFrame, scenario.expectedRequestedFrame);

        const int64_t ordinal = previewDecodedOrdinal(
            python, helperPath, scenario.clip.filePath, requestedSourceFrame, &error);
        const QByteArray lookupFailure = context + ": " + error.toUtf8();
        QVERIFY2(ordinal >= 0, lookupFailure.constData());
        QCOMPARE(ordinal, scenario.expectedDecodedOrdinal);
        QVERIFY(ordinal < frameMap.size());
        QCOMPARE(frameMap.at(ordinal).sourceFrame, scenario.expectedPresentedFrame);

        editor::DecoderContext decoder(scenario.clip.filePath, nullptr, true);
        QVERIFY2(decoder.initialize(), context.constData());
        const editor::FrameHandle decoded = decoder.decodeFrame(requestedSourceFrame);
        QVERIFY2(!decoded.isNull(), context.constData());
        QCOMPARE(decoded.frameNumber(), scenario.expectedPresentedFrame);
        QCOMPARE(decoded.frameNumber(), frameMap.at(ordinal).sourceFrame);
    }

    // The preview resolver must ignore stale generated-child timing caches and
    // evaluate the same parent clock and marker identity used by rendering.
    TimelineClip generatedChild = fastCut;
    generatedChild.id = QStringLiteral("vfr-generated-mask");
    generatedChild.clipRole = ClipRole::MaskMatte;
    generatedChild.linkedSourceClipId = fastCut.id;
    generatedChild.syncLockedToSource = false;
    generatedChild.startFrame = 0;
    generatedChild.sourceInFrame = 0;
    generatedChild.sourceFps = 60.0;
    generatedChild.playbackRate = 0.25;
    const QVector<TimelineClip> aggregate{fastCut, generatedChild};
    QCOMPARE(requestedSourceFrameForGeneratedMaskPreview(
                 aggregate.constLast(), aggregate, 104.0, {skipMarker}),
             int64_t(5));
}

QTEST_GUILESS_MAIN(TestVfrPreviewMapping)
#include "test_vfr_preview_mapping.moc"
