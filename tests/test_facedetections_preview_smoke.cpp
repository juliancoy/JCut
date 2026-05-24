#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

class FaceDetectionsPreviewSmokeTest : public QObject {
    Q_OBJECT

private:
    static bool commandAvailable(const QString& program)
    {
        QProcess probe;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("LD_LIBRARY_PATH"));
        probe.setProcessEnvironment(env);
        probe.start(program, {QStringLiteral("-version")});
        return probe.waitForFinished(3000) && probe.exitCode() == 0;
    }

    static bool generateTestVideo(const QString& path)
    {
        QStringList args;
        args << QStringLiteral("-f")
             << QStringLiteral("lavfi")
             << QStringLiteral("-i")
             << QStringLiteral("testsrc=duration=1.5:size=320x240:rate=24")
             << QStringLiteral("-c:v")
             << QStringLiteral("mpeg4")
             << QStringLiteral("-q:v")
             << QStringLiteral("5")
             << QStringLiteral("-pix_fmt")
             << QStringLiteral("yuv420p")
             << QStringLiteral("-y")
             << path;

        QProcess ffmpeg;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("LD_LIBRARY_PATH"));
        ffmpeg.setProcessEnvironment(env);
        ffmpeg.start(QStringLiteral("ffmpeg"), args);
        return ffmpeg.waitForFinished(30000) && ffmpeg.exitCode() == 0 && QFile::exists(path);
    }

private slots:
    void previewWindowRunsAndExitsCleanly()
    {
        const QProcessEnvironment sysEnv = QProcessEnvironment::systemEnvironment();
        if (sysEnv.value(QStringLiteral("JCUT_RUN_VULKAN_FACESTREAM_PREVIEW_SMOKE")).trimmed() != QStringLiteral("1")) {
            QSKIP("Set JCUT_RUN_VULKAN_FACESTREAM_PREVIEW_SMOKE=1 to run Vulkan FaceDetections preview smoke test.");
        }
        const bool hasDisplay =
            !sysEnv.value(QStringLiteral("DISPLAY")).trimmed().isEmpty() ||
            !sysEnv.value(QStringLiteral("WAYLAND_DISPLAY")).trimmed().isEmpty();
        if (!hasDisplay) {
            QSKIP("No GUI display is available for preview-window smoke test.");
        }
        if (!commandAvailable(QStringLiteral("ffmpeg"))) {
            QSKIP("ffmpeg is not available.");
        }

        const QString binaryPath = QDir(QStringLiteral(JCUT_BINARY_DIR))
                                       .filePath(QStringLiteral("jcut_vulkan_facedetections_offscreen"));
        if (!QFileInfo::exists(binaryPath)) {
            QSKIP("Standalone FaceDetections generator binary is not available.");
        }

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString videoPath = tempDir.filePath(QStringLiteral("preview_smoke.mp4"));
        QVERIFY2(generateTestVideo(videoPath), "Failed to generate smoke-test video.");

        QProcess process;
        QProcessEnvironment env = sysEnv;
        env.insert(QStringLiteral("LD_LIBRARY_PATH"),
                   QStringLiteral(EDITOR_FFMPEG_PREFIX) + QStringLiteral("/lib:") +
                       sysEnv.value(QStringLiteral("LD_LIBRARY_PATH")));
        process.setProcessEnvironment(env);

        const QString outputDir = tempDir.filePath(QStringLiteral("facedetections_out"));
        const QStringList args{
            videoPath,
            QStringLiteral("--detector"), QStringLiteral("scrfd-ncnn-vulkan"),
            QStringLiteral("--stride"), QStringLiteral("12"),
            QStringLiteral("--threshold"), QStringLiteral("0.25"),
            QStringLiteral("--nms-iou"), QStringLiteral("0.14"),
            QStringLiteral("--track-match-iou"), QStringLiteral("0.22"),
            QStringLiteral("--new-track-min-confidence"), QStringLiteral("0.35"),
            QStringLiteral("--max-faces-per-frame"), QStringLiteral("4"),
            QStringLiteral("--scrfd-target-size"), QStringLiteral("640"),
            QStringLiteral("--start-frame"), QStringLiteral("0"),
            QStringLiteral("--out-dir"), outputDir,
            QStringLiteral("--multi-face"),
            QStringLiteral("--require-zero-copy"),
            QStringLiteral("--preview-window"),
            QStringLiteral("--no-preview-files"),
            QStringLiteral("--max-frames"), QStringLiteral("48"),
            QStringLiteral("--quiet"),
            QStringLiteral("--progress")
        };

        process.start(binaryPath, args);
        QVERIFY2(process.waitForStarted(10000), qPrintable(process.errorString()));
        QVERIFY2(process.waitForFinished(60000), "Standalone FaceDetections preview smoke run timed out.");

        const QByteArray stderrText = process.readAllStandardError();
        const QByteArray stdoutText = process.readAllStandardOutput();
        const QByteArray merged = stdoutText + QByteArrayLiteral("\n") + stderrText;

        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        QVERIFY2(merged.contains("direct presenter device="),
                 "Preview smoke run did not initialize the direct Vulkan presenter.");
    }
};

QTEST_MAIN(FaceDetectionsPreviewSmokeTest)
#include "test_facedetections_preview_smoke.moc"
