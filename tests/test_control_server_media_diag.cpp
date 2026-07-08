#include <QtTest/QtTest>

#include <QColor>
#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include "../clip_serialization.h"
#include "../control_server_media_diag.h"
#include "../debug_controls.h"
#include "../decoder_context.h"
#include "../editor_shared_media.h"

using namespace editor;

namespace {

QProcessEnvironment cleanProcessEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("LD_LIBRARY_PATH"));
    return env;
}

bool runFfmpeg(const QStringList& args, QString* errorOut)
{
    QProcess ffmpeg;
    ffmpeg.setProcessEnvironment(cleanProcessEnvironment());
    ffmpeg.start(QStringLiteral("ffmpeg"), args);
    if (!ffmpeg.waitForFinished(30000)) {
        ffmpeg.kill();
        ffmpeg.waitForFinished();
        if (errorOut) {
            *errorOut = QStringLiteral("ffmpeg timed out");
        }
        return false;
    }
    if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0) {
        if (errorOut) {
            *errorOut = QString::fromUtf8(ffmpeg.readAllStandardError()).trimmed();
        }
        return false;
    }
    return true;
}

bool commandAvailable(const QString& command)
{
    QProcess process;
    process.setProcessEnvironment(cleanProcessEnvironment());
    process.start(command, QStringList{QStringLiteral("-version")});
    return process.waitForFinished(3000) && process.exitCode() == 0;
}

bool writeSequenceFrame(const QString& path, int index)
{
    QImage image(64, 36, QImage::Format_RGB32);
    image.fill(QColor(40 + index * 20, 80 + index * 10, 130 + index * 5));
    return image.save(path);
}

TimelineClip makeVideoClip()
{
    TimelineClip clip;
    clip.id = QStringLiteral("relative-media");
    clip.label = QStringLiteral("relative.mp4");
    clip.filePath = QStringLiteral("relative.mp4");
    clip.proxyPath = QStringLiteral("relative.proxy");
    clip.useProxy = true;
    clip.mediaType = ClipMediaType::Video;
    clip.sourceKind = MediaSourceKind::File;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.sourceFps = 30.0;
    clip.sourceDurationFrames = 6;
    clip.durationFrames = 6;
    clip.startFrame = 0;
    clip.playbackRate = 1.0;
    return clip;
}

TimelineClip makeNoProxyVideoClip()
{
    TimelineClip clip = makeVideoClip();
    clip.id = QStringLiteral("relative-source-only");
    clip.label = QStringLiteral("source_only.mp4");
    clip.filePath = QStringLiteral("source_only.mp4");
    clip.proxyPath.clear();
    clip.useProxy = false;
    clip.sourceDurationFrames = 30;
    clip.durationFrames = 30;
    return clip;
}

QJsonObject makeState(const QString& mediaRoot)
{
    QJsonArray timeline;
    timeline.append(clipToJson(makeVideoClip()));
    return QJsonObject{
        {QStringLiteral("mediaRoot"), mediaRoot},
        {QStringLiteral("timeline"), timeline}
    };
}

QJsonObject makeNoProxyState(const QString& mediaRoot)
{
    QJsonArray timeline;
    timeline.append(clipToJson(makeNoProxyVideoClip()));
    return QJsonObject{
        {QStringLiteral("mediaRoot"), mediaRoot},
        {QStringLiteral("timeline"), timeline}
    };
}

QString createTinyVideo(const QString& path, QString* errorOut)
{
    if (!commandAvailable(QStringLiteral("ffmpeg"))) {
        if (errorOut) {
            *errorOut = QStringLiteral("ffmpeg unavailable");
        }
        return {};
    }
    const QStringList args{
        QStringLiteral("-y"),
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("testsrc2=size=96x64:rate=30:duration=1"),
        QStringLiteral("-an"),
        QStringLiteral("-c:v"), QStringLiteral("mpeg4"),
        QStringLiteral("-q:v"), QStringLiteral("4"),
        path
    };
    if (!runFfmpeg(args, errorOut)) {
        return {};
    }
    return path;
}

class ScopedDecodePreference {
public:
    explicit ScopedDecodePreference(DecodePreference preference)
        : previous(debugDecodePreference())
    {
        setDebugDecodePreference(preference);
    }

    ~ScopedDecodePreference()
    {
        setDebugDecodePreference(previous);
    }

private:
    DecodePreference previous;
};

} // namespace

class TestControlServerMediaDiag : public QObject {
    Q_OBJECT

private slots:
    void relativeProxyDecodeBenchmarksResolveAgainstMediaRoot();
    void relativeSourceDecodeBenchmarksResolveAgainstMediaRootWithoutProxy();
    void decoderPreferencesDecodeGeneratedVideoOnThisMachine();
};

void TestControlServerMediaDiag::relativeProxyDecodeBenchmarksResolveAgainstMediaRoot()
{
    QTemporaryDir root;
    QVERIFY2(root.isValid(), "temporary media root must be available");
    QDir rootDir(root.path());
    QVERIFY(rootDir.mkpath(QStringLiteral("relative.proxy")));
    QFile source(rootDir.filePath(QStringLiteral("relative.mp4")));
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("placeholder");
    source.close();

    QDir proxyDir(rootDir.filePath(QStringLiteral("relative.proxy")));
    for (int i = 1; i <= 6; ++i) {
        QVERIFY(writeSequenceFrame(proxyDir.filePath(QStringLiteral("frame_%1.jpg").arg(i, 6, 10, QLatin1Char('0'))), i));
    }

    const QJsonObject state = makeState(root.path());
    const QJsonObject rates = control_server::benchmarkDecodeRatesForState(state);
    QCOMPARE(rates.value(QStringLiteral("success_count")).toInt(), 1);
    QCOMPARE(rates.value(QStringLiteral("error_count")).toInt(), 0);
    const QJsonObject rateResult = rates.value(QStringLiteral("results")).toArray().at(0).toObject();
    QCOMPARE(rateResult.value(QStringLiteral("success")).toBool(), true);
    QCOMPARE(rateResult.value(QStringLiteral("codec")).toString(), QStringLiteral("image_sequence"));
    QCOMPARE(rateResult.value(QStringLiteral("decode_path")).toString(),
             QDir::toNativeSeparators(proxyDir.absolutePath()));
    QCOMPARE(rateResult.value(QStringLiteral("source_path")).toString(),
             QDir::toNativeSeparators(rootDir.absoluteFilePath(QStringLiteral("relative.mp4"))));
    QCOMPARE(rateResult.value(QStringLiteral("null_frames")).toInt(), 0);
    QVERIFY(rateResult.value(QStringLiteral("frames_decoded")).toInt() > 0);

    const QJsonObject seeks = control_server::benchmarkSeekRatesForState(state);
    QCOMPARE(seeks.value(QStringLiteral("success_count")).toInt(), 1);
    QCOMPARE(seeks.value(QStringLiteral("error_count")).toInt(), 0);
    const QJsonObject seekResult = seeks.value(QStringLiteral("results")).toArray().at(0).toObject();
    QCOMPARE(seekResult.value(QStringLiteral("success")).toBool(), true);
    QCOMPARE(seekResult.value(QStringLiteral("null_seeks")).toInt(), 0);
    QCOMPARE(seekResult.value(QStringLiteral("successful_seeks")).toInt(),
             seekResult.value(QStringLiteral("seek_count")).toInt());
}

void TestControlServerMediaDiag::relativeSourceDecodeBenchmarksResolveAgainstMediaRootWithoutProxy()
{
    QTemporaryDir root;
    QVERIFY2(root.isValid(), "temporary media root must be available");
    QDir rootDir(root.path());
    QString error;
    const QString videoPath = createTinyVideo(rootDir.filePath(QStringLiteral("source_only.mp4")), &error);
    if (videoPath.isEmpty()) {
        QSKIP(qPrintable(QStringLiteral("cannot create generated no-proxy decode fixture: %1").arg(error)));
    }

    const QJsonObject state = makeNoProxyState(root.path());
    const QJsonObject rates = control_server::benchmarkDecodeRatesForState(state);
    QCOMPARE(rates.value(QStringLiteral("success_count")).toInt(), 1);
    QCOMPARE(rates.value(QStringLiteral("error_count")).toInt(), 0);
    const QJsonObject rateResult = rates.value(QStringLiteral("results")).toArray().at(0).toObject();
    QCOMPARE(rateResult.value(QStringLiteral("success")).toBool(), true);
    QCOMPARE(rateResult.value(QStringLiteral("decode_path")).toString(),
             QDir::toNativeSeparators(videoPath));
    QCOMPARE(rateResult.value(QStringLiteral("source_path")).toString(),
             QDir::toNativeSeparators(videoPath));
    QCOMPARE(rateResult.value(QStringLiteral("null_frames")).toInt(), 0);
    QVERIFY(rateResult.value(QStringLiteral("frames_decoded")).toInt() > 0);

    const QJsonObject seeks = control_server::benchmarkSeekRatesForState(state);
    QCOMPARE(seeks.value(QStringLiteral("success_count")).toInt(), 1);
    QCOMPARE(seeks.value(QStringLiteral("error_count")).toInt(), 0);
    const QJsonObject seekResult = seeks.value(QStringLiteral("results")).toArray().at(0).toObject();
    QCOMPARE(seekResult.value(QStringLiteral("success")).toBool(), true);
    QCOMPARE(seekResult.value(QStringLiteral("decode_path")).toString(),
             QDir::toNativeSeparators(videoPath));
    QCOMPARE(seekResult.value(QStringLiteral("source_path")).toString(),
             QDir::toNativeSeparators(videoPath));
    QCOMPARE(seekResult.value(QStringLiteral("null_seeks")).toInt(), 0);
    QCOMPARE(seekResult.value(QStringLiteral("successful_seeks")).toInt(),
             seekResult.value(QStringLiteral("seek_count")).toInt());
}

void TestControlServerMediaDiag::decoderPreferencesDecodeGeneratedVideoOnThisMachine()
{
    QTemporaryDir root;
    QVERIFY2(root.isValid(), "temporary media root must be available");
    QString error;
    const QString videoPath = createTinyVideo(QDir(root.path()).filePath(QStringLiteral("decode_modes.mp4")), &error);
    if (videoPath.isEmpty()) {
        QSKIP(qPrintable(QStringLiteral("cannot create generated decode fixture: %1").arg(error)));
    }

    struct Mode {
        const char* label;
        DecodePreference preference;
        bool forceSoftware;
    };
    const QVector<Mode> modes{
        {"force_software", DecodePreference::Hardware, true},
        {"auto", DecodePreference::Auto, false},
        {"hardware", DecodePreference::Hardware, false},
        {"hardware_zero_copy", DecodePreference::HardwareZeroCopy, false},
    };

    for (const Mode& mode : modes) {
        ScopedDecodePreference scoped(mode.preference);
        DecoderContext decoder(videoPath, nullptr, mode.forceSoftware);
        QVERIFY2(decoder.initialize(), qPrintable(QStringLiteral("initialize failed for %1").arg(mode.label)));
        const FrameHandle frame = decoder.decodeFrame(0);
        QVERIFY2(!frame.isNull(), qPrintable(QStringLiteral("decodeFrame returned null for %1").arg(mode.label)));
        QVERIFY2(frame.hasCpuImage() || frame.hasHardwareFrame() || frame.hasGpuTexture(),
                 qPrintable(QStringLiteral("decoded frame had no payload for %1").arg(mode.label)));
    }
}

QTEST_MAIN(TestControlServerMediaDiag)
#include "test_control_server_media_diag.moc"
