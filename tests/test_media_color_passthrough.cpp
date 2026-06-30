#include <QtTest/QtTest>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include "../decoder_context.h"
#include "../decoder_image_io.h"

using namespace editor;

namespace {

constexpr int kWidth = 160;
constexpr int kHeight = 160;
const QColor kReferenceColor(74, 143, 211);

QString ffmpegProgram()
{
    return QStringLiteral("ffmpeg");
}

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
    ffmpeg.start(ffmpegProgram(), args);
    if (!ffmpeg.waitForFinished(30000)) {
        if (errorOut) {
            *errorOut = QStringLiteral("ffmpeg timed out: %1").arg(args.join(QLatin1Char(' ')));
        }
        ffmpeg.kill();
        ffmpeg.waitForFinished();
        return false;
    }
    if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0 || args.isEmpty()) {
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

QString colorSource()
{
    return QStringLiteral("color=c=0x%1%2%3:s=%4x%5:d=1:r=1")
        .arg(kReferenceColor.red(), 2, 16, QLatin1Char('0'))
        .arg(kReferenceColor.green(), 2, 16, QLatin1Char('0'))
        .arg(kReferenceColor.blue(), 2, 16, QLatin1Char('0'))
        .arg(kWidth)
        .arg(kHeight);
}

QColor decodedCenterColor(const QString& path, QString* errorOut)
{
    DecoderContext decoder(path);
    if (!decoder.initialize()) {
        if (errorOut) {
            *errorOut = QStringLiteral("DecoderContext failed to initialize for %1").arg(path);
        }
        return {};
    }

    const FrameHandle frame = decoder.decodeFrame(0);
    if (frame.isNull() || !frame.hasCpuImage()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Decoder returned no CPU image for %1").arg(path);
        }
        return {};
    }

    const QImage image = frame.cpuImage().convertToFormat(QImage::Format_RGBA8888);
    return image.pixelColor(image.width() / 2, image.height() / 2);
}

int maxChannelDelta(const QColor& a, const QColor& b)
{
    return qMax(qAbs(a.red() - b.red()),
                qMax(qAbs(a.green() - b.green()), qAbs(a.blue() - b.blue())));
}

} // namespace

class TestMediaColorPassthrough : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void testGeneratedMediaColorPassthrough_data();
    void testGeneratedMediaColorPassthrough();
    void testTransparentPngPreservesStraightRgb();
};

void TestMediaColorPassthrough::initTestCase()
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qunsetenv("JCUT_ALLOW_HEADLESS_HARDWARE_DECODE");
    if (!commandAvailable(ffmpegProgram())) {
        QSKIP("ffmpeg is not available.");
    }
}

void TestMediaColorPassthrough::testGeneratedMediaColorPassthrough_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QStringList>("ffmpegArgs");
    QTest::addColumn<bool>("writeWithQImage");
    QTest::addColumn<int>("tolerance");

    QTest::newRow("png-rgba")
        << QStringLiteral("solid.png")
        << QStringList{}
        << true
        << 1;

    QTest::newRow("jpeg-full-range-still")
        << QStringLiteral("solid.jpg")
        << QStringList{}
        << true
        << 8;

    QTest::newRow("h264-mp4-bt709-limited")
        << QStringLiteral("h264_bt709_limited.mp4")
        << (QStringList{}
            << QStringLiteral("-hide_banner") << QStringLiteral("-loglevel") << QStringLiteral("error")
            << QStringLiteral("-f") << QStringLiteral("lavfi")
            << QStringLiteral("-i") << colorSource()
            << QStringLiteral("-frames:v") << QStringLiteral("1")
            << QStringLiteral("-c:v") << QStringLiteral("libx264")
            << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
            << QStringLiteral("-color_range") << QStringLiteral("tv")
            << QStringLiteral("-colorspace") << QStringLiteral("bt709")
            << QStringLiteral("-color_primaries") << QStringLiteral("bt709")
            << QStringLiteral("-color_trc") << QStringLiteral("bt709")
            << QStringLiteral("-y"))
        << false
        << 12;

    QTest::newRow("hevc-mp4-smpte170m-full-range")
        << QStringLiteral("hevc_smpte170m_full.mp4")
        << (QStringList{}
            << QStringLiteral("-hide_banner") << QStringLiteral("-loglevel") << QStringLiteral("error")
            << QStringLiteral("-f") << QStringLiteral("lavfi")
            << QStringLiteral("-i") << colorSource()
            << QStringLiteral("-frames:v") << QStringLiteral("1")
            << QStringLiteral("-c:v") << QStringLiteral("libx265")
            << QStringLiteral("-pix_fmt") << QStringLiteral("yuvj420p")
            << QStringLiteral("-color_range") << QStringLiteral("pc")
            << QStringLiteral("-colorspace") << QStringLiteral("smpte170m")
            << QStringLiteral("-color_primaries") << QStringLiteral("bt470bg")
            << QStringLiteral("-color_trc") << QStringLiteral("smpte170m")
            << QStringLiteral("-y"))
        << false
        << 16;

    QTest::newRow("vp9-webm-bt709-limited")
        << QStringLiteral("vp9_bt709_limited.webm")
        << (QStringList{}
            << QStringLiteral("-hide_banner") << QStringLiteral("-loglevel") << QStringLiteral("error")
            << QStringLiteral("-f") << QStringLiteral("lavfi")
            << QStringLiteral("-i") << colorSource()
            << QStringLiteral("-frames:v") << QStringLiteral("1")
            << QStringLiteral("-c:v") << QStringLiteral("libvpx-vp9")
            << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
            << QStringLiteral("-color_range") << QStringLiteral("tv")
            << QStringLiteral("-colorspace") << QStringLiteral("bt709")
            << QStringLiteral("-color_primaries") << QStringLiteral("bt709")
            << QStringLiteral("-color_trc") << QStringLiteral("bt709")
            << QStringLiteral("-y"))
        << false
        << 14;

    QTest::newRow("prores-mov-yuv422p10")
        << QStringLiteral("prores_yuv422p10.mov")
        << (QStringList{}
            << QStringLiteral("-hide_banner") << QStringLiteral("-loglevel") << QStringLiteral("error")
            << QStringLiteral("-f") << QStringLiteral("lavfi")
            << QStringLiteral("-i") << colorSource()
            << QStringLiteral("-frames:v") << QStringLiteral("1")
            << QStringLiteral("-c:v") << QStringLiteral("prores_ks")
            << QStringLiteral("-pix_fmt") << QStringLiteral("yuv422p10le")
            << QStringLiteral("-color_range") << QStringLiteral("tv")
            << QStringLiteral("-colorspace") << QStringLiteral("bt709")
            << QStringLiteral("-color_primaries") << QStringLiteral("bt709")
            << QStringLiteral("-color_trc") << QStringLiteral("bt709")
            << QStringLiteral("-y"))
        << false
        << 10;

    QTest::newRow("mpeg4-avi-yuv420")
        << QStringLiteral("mpeg4_yuv420.avi")
        << (QStringList{}
            << QStringLiteral("-hide_banner") << QStringLiteral("-loglevel") << QStringLiteral("error")
            << QStringLiteral("-f") << QStringLiteral("lavfi")
            << QStringLiteral("-i") << colorSource()
            << QStringLiteral("-frames:v") << QStringLiteral("1")
            << QStringLiteral("-c:v") << QStringLiteral("mpeg4")
            << QStringLiteral("-q:v") << QStringLiteral("2")
            << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
            << QStringLiteral("-y"))
        << false
        << 16;
}

void TestMediaColorPassthrough::testGeneratedMediaColorPassthrough()
{
    QFETCH(QString, fileName);
    QFETCH(QStringList, ffmpegArgs);
    QFETCH(bool, writeWithQImage);
    QFETCH(int, tolerance);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(fileName);

    if (writeWithQImage) {
        QImage image(kWidth, kHeight, QImage::Format_RGBA8888);
        image.fill(kReferenceColor);
        QVERIFY2(image.save(path), qPrintable(QStringLiteral("Failed to write %1").arg(path)));
    } else {
        QStringList args = ffmpegArgs;
        args << path;
        QString ffmpegError;
        if (!runFfmpeg(args, &ffmpegError)) {
            QSKIP(qPrintable(QStringLiteral("Could not generate %1: %2").arg(fileName, ffmpegError)));
        }
    }

    QString decodeError;
    const QColor decoded = decodedCenterColor(path, &decodeError);
    QVERIFY2(decoded.isValid(), qPrintable(decodeError));
    const int delta = maxChannelDelta(decoded, kReferenceColor);
    QVERIFY2(delta <= tolerance,
             qPrintable(QStringLiteral("%1 decoded as rgb(%2,%3,%4), expected rgb(%5,%6,%7), max delta %8 > %9")
                            .arg(fileName)
                            .arg(decoded.red())
                            .arg(decoded.green())
                            .arg(decoded.blue())
                            .arg(kReferenceColor.red())
                            .arg(kReferenceColor.green())
                            .arg(kReferenceColor.blue())
                            .arg(delta)
                            .arg(tolerance)));
}

void TestMediaColorPassthrough::testTransparentPngPreservesStraightRgb()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("transparent_color.png"));

    QImage image(2, 1, QImage::Format_RGBA8888);
    image.setPixelColor(0, 0, QColor(240, 80, 32, 0));
    image.setPixelColor(1, 0, QColor(24, 200, 128, 96));
    QVERIFY2(image.save(path), qPrintable(QStringLiteral("Failed to write %1").arg(path)));

    const QImage decoded = loadSingleImageFile(path).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(!decoded.isNull());
    QCOMPARE(decoded.pixelColor(0, 0), QColor(240, 80, 32, 0));
    QCOMPARE(decoded.pixelColor(1, 0), QColor(24, 200, 128, 96));
}

QTEST_MAIN(TestMediaColorPassthrough)
#include "test_media_color_passthrough.moc"
