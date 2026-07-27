#include <QtTest/QtTest>

#include <QFileInfo>
#include <QDataStream>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "../render.h"

#include <cmath>

namespace {

bool writeToneWave(const QString& path, int sampleFrames)
{
    constexpr int sampleRate = 48000;
    constexpr int channels = 2;
    constexpr int bytesPerSample = 2;
    const quint32 payloadBytes =
        static_cast<quint32>(sampleFrames * channels * bytesPerSample);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << quint32{36 + payloadBytes};
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32{16} << quint16{1} << quint16{channels}
           << quint32{sampleRate}
           << quint32{sampleRate * channels * bytesPerSample}
           << quint16{channels * bytesPerSample} << quint16{16};
    stream.writeRawData("data", 4);
    stream << payloadBytes;
    for (int frame = 0; frame < sampleFrames; ++frame) {
        const qint16 sample =
            (frame / 240) % 2 == 0 ? qint16{4000} : qint16{-4000};
        stream << sample << sample;
    }
    return stream.status() == QDataStream::Ok && file.commit();
}

} // namespace

class TestIncrementalRender : public QObject {
    Q_OBJECT

private slots:
    void resumesCompletedGpuChunksAndPublishesExactFrameCount();
};

void TestIncrementalRender::
    resumesCompletedGpuChunksAndPublishesExactFrameCount()
{
    qputenv("JCUT_RENDER_BACKEND", "vulkan");

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString audioPath =
        directory.filePath(QStringLiteral("tone.wav"));
    QVERIFY(writeToneWave(audioPath, 3 * 48000));

    RenderRequest request;
    request.outputPath = directory.filePath(QStringLiteral("incremental.mp4"));
    request.outputFormat = QStringLiteral("mp4");
    request.outputSize = QSize(256, 256);
    request.outputFps = 30.0;
    request.exportStartFrame = 0;
    request.exportEndFrame = 61;
    request.exportRanges = {ExportRangeSegment{0, 61}};
    request.incrementalExport = true;
    request.incrementalChunkFrames = 60;
    TimelineClip audioClip;
    audioClip.id = QStringLiteral("incremental-audio");
    audioClip.filePath = audioPath;
    audioClip.audioSourcePath = audioPath;
    audioClip.mediaType = ClipMediaType::Audio;
    audioClip.videoEnabled = false;
    audioClip.audioEnabled = true;
    audioClip.hasAudio = true;
    audioClip.startFrame = 0;
    audioClip.durationFrames = 62;
    audioClip.sourceDurationFrames = 90;
    audioClip.sourceFps = 30.0;
    audioClip.trackIndex = 0;
    request.clips = {audioClip};

    const RenderResult interrupted = renderTimelineToFile(
        request,
        [](const RenderProgress& progress) {
            return progress.incrementalChunksCompleted < 1;
        });
    QVERIFY2(interrupted.cancelled, qPrintable(interrupted.message));
    QCOMPARE(interrupted.incrementalChunksCompleted, 1);
    QVERIFY(QFileInfo(interrupted.incrementalCachePath).isDir());

    const RenderResult resumed = renderTimelineToFile(request);
    QVERIFY2(resumed.success, qPrintable(resumed.message));
    QVERIFY2(resumed.usedGpu, qPrintable(resumed.message));
    QVERIFY2(resumed.usedHardwareEncode, qPrintable(resumed.message));
    QVERIFY2(!resumed.cudaExternalTransfer, qPrintable(resumed.message));
    QCOMPARE(resumed.framesRendered, int64_t{62});
    QCOMPARE(resumed.incrementalChunksCompleted, 2);
    QCOMPARE(resumed.incrementalChunksTotal, 2);
    QCOMPARE(resumed.incrementalFramesReused, int64_t{60});
    QVERIFY(QFileInfo(request.outputPath).size() > 1024);
    const QDir checkpoint(resumed.incrementalCachePath);
    const QStringList checkpointDirs = checkpoint.entryList(
        {QStringLiteral("chunk_*_frames")}, QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(checkpointDirs.size(), 2);
    QCOMPARE(
        QDir(checkpoint.filePath(checkpointDirs.constFirst()))
            .entryList({QStringLiteral("frame_*.jpg")}, QDir::Files)
            .size(),
        60);

    const RenderResult fullyReused = renderTimelineToFile(request);
    QVERIFY2(fullyReused.success, qPrintable(fullyReused.message));
    QVERIFY(fullyReused.usedGpu);
    QVERIFY(fullyReused.usedHardwareEncode);
    QCOMPARE(fullyReused.incrementalFramesReused, int64_t{62});

    RenderRequest reverifiedRequest = request;
    reverifiedRequest.clips[0].audioSourceLastVerifiedMs += 1000;
    const RenderResult reverified = renderTimelineToFile(reverifiedRequest);
    QVERIFY2(reverified.success, qPrintable(reverified.message));
    QCOMPARE(reverified.incrementalCachePath,
             fullyReused.incrementalCachePath);
    QCOMPARE(reverified.incrementalFramesReused, int64_t{62});

    const QString ffprobe =
        QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    QVERIFY2(!ffprobe.isEmpty(), "ffprobe is required for render acceptance");
    QProcess probe;
    probe.start(
        ffprobe,
        {QStringLiteral("-v"), QStringLiteral("error"),
         QStringLiteral("-count_frames"),
         QStringLiteral("-select_streams"), QStringLiteral("v:0"),
         QStringLiteral("-show_entries"),
         QStringLiteral("stream=nb_read_frames"),
         QStringLiteral("-of"), QStringLiteral("default=nw=1:nk=1"),
         request.outputPath});
    QVERIFY(probe.waitForFinished(30000));
    QCOMPARE(probe.exitCode(), 0);
    QCOMPARE(QString::fromUtf8(probe.readAllStandardOutput()).trimmed(),
             QStringLiteral("62"));

    QProcess audioProbe;
    audioProbe.start(
        ffprobe,
        {QStringLiteral("-v"), QStringLiteral("error"),
         QStringLiteral("-select_streams"), QStringLiteral("a:0"),
         QStringLiteral("-show_entries"), QStringLiteral("stream=duration"),
         QStringLiteral("-of"), QStringLiteral("default=nw=1:nk=1"),
         request.outputPath});
    QVERIFY(audioProbe.waitForFinished(30000));
    QCOMPARE(audioProbe.exitCode(), 0);
    bool durationOk = false;
    const double audioDuration =
        QString::fromUtf8(audioProbe.readAllStandardOutput())
            .trimmed()
            .toDouble(&durationOk);
    QVERIFY(durationOk);
    QVERIFY2(std::abs(audioDuration - (62.0 / 30.0)) < 0.05,
             qPrintable(QStringLiteral("Unexpected audio duration %1")
                            .arg(audioDuration, 0, 'f', 6)));
}

QTEST_GUILESS_MAIN(TestIncrementalRender)
#include "test_incremental_render.moc"
