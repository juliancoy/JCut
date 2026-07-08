#include <QtTest/QtTest>

#include "../editor_effect_presets.h"
#include "../titles.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QVector>

class TestSpeakerTitleFlyInVideo : public QObject {
    Q_OBJECT

private slots:
    void rendersSourceClipSpeakerTitleFlyInVideo();
};

namespace {

void drawSpeakerMask(QPainter& painter, const QSize& size)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(220, 230, 235));
    painter.drawEllipse(QPoint(size.width() / 2, size.height() / 2), 44, 58);
}

QImage composeFrame(const QSize& size, const EvaluatedTitle& title, int frame, bool speakerMaskOnTop)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    const int blue = qBound(28, 46 + frame, 120);
    image.fill(QColor(18, 28, blue));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(45, 84, 122));
    painter.drawRect(QRect(0, size.height() * 2 / 3, size.width(), size.height() / 3));
    if (!speakerMaskOnTop) {
        drawSpeakerMask(painter, size);
    }
    painter.end();

    const render_detail::OverlayImage overlay = renderTitleOverlay(size, title, size);
    if (!overlay.isNull()) {
        QPainter overlayPainter(&image);
        overlayPainter.drawImage(0, 0, overlay.asQImageView());
    }
    if (speakerMaskOnTop) {
        QPainter maskPainter(&image);
        maskPainter.setRenderHint(QPainter::Antialiasing, true);
        drawSpeakerMask(maskPainter, size);
    }
    return image;
}

bool runFfmpeg(const QStringList& args, QString* errorOut)
{
    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("LD_LIBRARY_PATH"));
    process.setProcessEnvironment(env);
    process.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")), args);
    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished();
        if (errorOut) {
            *errorOut = QStringLiteral("ffmpeg timed out");
        }
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(process.readAllStandardError()).trimmed();
        }
        return false;
    }
    return true;
}

struct FlyInVideoCase {
    QString slug;
    SpeakerTitleFlyInStyle style = SpeakerTitleFlyInStyle::SlideFromLeft;
    int flyInFrames = 15;
    qreal wrapRadius = 0.72;
    qreal wrapDepth = 0.70;
};

TimelineClip makeSourceClip()
{
    TimelineClip source;
    source.id = QStringLiteral("source-clip");
    source.mediaType = ClipMediaType::Video;
    source.sourceInFrame = 0;
    source.startFrame = 0;
    source.durationFrames = 120;
    source.playbackRate = 1.0;
    return source;
}

} // namespace

void TestSpeakerTitleFlyInVideo::rendersSourceClipSpeakerTitleFlyInVideo()
{
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
        QSKIP("ffmpeg is not available.");
    }

    const QDir artifactRoot(QStringLiteral(JCUT_BINARY_DIR "/artifacts"));
    QVERIFY2(QDir().mkpath(artifactRoot.path()), "artifact directory must be creatable");
    const QString transcriptPath = artifactRoot.filePath(QStringLiteral("speaker_title_fly_in.transcript.json"));
    QFile transcriptFile(transcriptPath);
    QVERIFY(transcriptFile.open(QIODevice::WriteOnly));
    transcriptFile.write(R"({
        "speaker_profiles": {
            "S1": {"name": "Jane Doe", "organization": "Director"}
        },
        "segments": []
    })");
    transcriptFile.close();

    TranscriptSection section;
    section.startFrame = 0;
    section.endFrame = 80;
    section.words.push_back(TranscriptWord{0, 8, QStringLiteral("S1"), QStringLiteral("hello"), false});

    const QVector<FlyInVideoCase> cases{
        {QStringLiteral("slide_left"), SpeakerTitleFlyInStyle::SlideFromLeft, 15, 0.72, 0.70},
        {QStringLiteral("slide_right"), SpeakerTitleFlyInStyle::SlideFromRight, 15, 0.72, 0.70},
        {QStringLiteral("rise_bottom"), SpeakerTitleFlyInStyle::RiseFromBottom, 15, 0.72, 0.70},
        {QStringLiteral("drop_top"), SpeakerTitleFlyInStyle::DropFromTop, 15, 0.72, 0.70},
        {QStringLiteral("wrap_around"), SpeakerTitleFlyInStyle::WrapAroundSpeaker, 30, 1.18, 0.88},
    };

    const QSize outputSize(640, 360);
    for (const FlyInVideoCase& videoCase : cases) {
        const QString framesPath = artifactRoot.filePath(
            QStringLiteral("speaker_title_%1_frames").arg(videoCase.slug));
        QDir framesDir(framesPath);
        if (framesDir.exists()) {
            QVERIFY(framesDir.removeRecursively());
        }
        QVERIFY(QDir().mkpath(framesPath));

        TimelineClip source = makeSourceClip();
        SpeakerTitleFlyInSettings settings;
        settings.style = videoCase.style;
        settings.titleStartDelayFrames = 0;
        settings.titleDurationFrames = 90;
        settings.flyInFrames = videoCase.flyInFrames;
        settings.flyOutFrames = 15;
        settings.wrapRadius = videoCase.wrapRadius;
        settings.wrapDepth = videoCase.wrapDepth;
        if (videoCase.style == SpeakerTitleFlyInStyle::WrapAroundSpeaker) {
            settings.wrapStartAngleDegrees = -120.0;
            settings.wrapEndAngleDegrees = 125.0;
            settings.wrapPitchDegrees = 14.0;
            settings.wrapRollDegrees = -10.0;
        }
        QCOMPARE(applySpeakerTitleFlyInsToSourceClip(source, transcriptPath, {section}, settings), 1);
        QCOMPARE(source.mediaType, ClipMediaType::Video);
        QVERIFY(!source.titleKeyframes.isEmpty());

        const EvaluatedTitle hidden = evaluateTitleAtLocalFrame(source, 0);
        const EvaluatedTitle arrived = evaluateTitleAtLocalFrame(source, videoCase.flyInFrames);
        const EvaluatedTitle hold = evaluateTitleAtLocalFrame(source, 60);
        const EvaluatedTitle exiting = evaluateTitleAtLocalFrame(source, 88);
        QVERIFY(hidden.valid);
        QVERIFY(arrived.valid);
        if (videoCase.style == SpeakerTitleFlyInStyle::WrapAroundSpeaker) {
            QCOMPARE(arrived.text, QStringLiteral("Jane Doe - Director"));
        } else {
            QCOMPARE(arrived.text, QStringLiteral("Jane Doe - Director"));
        }
        QVERIFY(hidden.opacity < arrived.opacity);
        QCOMPARE(hold.opacity, 1.0);
        QVERIFY(exiting.opacity < hold.opacity);

        for (int frame = 0; frame < 90; ++frame) {
            const EvaluatedTitle title = evaluateTitleAtLocalFrame(source, frame);
            const QImage image = composeFrame(
                outputSize,
                title,
                frame,
                videoCase.style == SpeakerTitleFlyInStyle::WrapAroundSpeaker);
            const QString framePath = framesDir.filePath(QStringLiteral("frame_%1.png").arg(frame, 3, 10, QChar('0')));
            QVERIFY2(image.save(framePath), qPrintable(QStringLiteral("failed to save %1").arg(framePath)));
        }

        const QString outputPath = artifactRoot.filePath(
            QStringLiteral("speaker_title_%1_output.mp4").arg(videoCase.slug));
        QFile::remove(outputPath);
        QString ffmpegError;
        QVERIFY2(runFfmpeg({
                     QStringLiteral("-y"),
                     QStringLiteral("-framerate"), QStringLiteral("30"),
                     QStringLiteral("-i"), framesDir.filePath(QStringLiteral("frame_%03d.png")),
                     QStringLiteral("-c:v"), QStringLiteral("libx264"),
                     QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                     outputPath,
                 }, &ffmpegError),
                 qPrintable(ffmpegError));
        QVERIFY2(QFileInfo(outputPath).exists(), "speaker title fly-in output video must exist");
        QVERIFY2(QFileInfo(outputPath).size() > 0, "speaker title fly-in output video must be non-empty");
        qInfo().noquote() << QStringLiteral("speaker_title_%1_output=%2").arg(videoCase.slug, outputPath);

        if (videoCase.slug == QStringLiteral("slide_left")) {
            const QString legacyOutputPath = artifactRoot.filePath(QStringLiteral("speaker_title_fly_in_output.mp4"));
            QFile::remove(legacyOutputPath);
            QVERIFY(QFile::copy(outputPath, legacyOutputPath));
            qInfo().noquote() << QStringLiteral("speaker_title_fly_in_output=%1").arg(legacyOutputPath);
        }
    }
}

QTEST_MAIN(TestSpeakerTitleFlyInVideo)
#include "test_speaker_title_fly_in_video.moc"
