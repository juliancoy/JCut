#include "editor.h"
#include "debug_controls.h"
#include "decoder_context.h"
#include "speaker_export_harness.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetrics>
#include <QLockFile>
#include <QSettings>
#include <QToolTip>

#include <limits>
#include <memory>
#include <cstring>

namespace {

int runDecoderCli(const QString& videoPath) {
    const QFileInfo source(videoPath);
    if (!source.isFile()) {
        qCritical().noquote() << QStringLiteral("Decoder test input is not a file: %1")
                                     .arg(videoPath);
        return 2;
    }

    editor::DecoderContext decoder(source.absoluteFilePath());
    if (!decoder.initialize()) {
        qCritical().noquote() << QStringLiteral("Decoder initialization failed: %1")
                                     .arg(source.absoluteFilePath());
        return 3;
    }

    const editor::VideoStreamInfo& info = decoder.info();
    qInfo().noquote()
        << QStringLiteral("decoder video=%1 codec=%2 mode=%3 path=%4 interop=%5 "
                          "hardware=%6 size=%7x%8 fps=%9 frames=%10")
               .arg(source.absoluteFilePath(),
                    info.codecName,
                    info.requestedDecodeMode,
                    info.decodePath,
                    info.interopPath,
                    info.hardwareAccelerated ? QStringLiteral("true") : QStringLiteral("false"))
               .arg(info.frameSize.width())
               .arg(info.frameSize.height())
               .arg(info.fps, 0, 'f', 3)
               .arg(info.durationFrames);

    const int64_t sampleCount =
        qBound<int64_t>(int64_t{1}, info.durationFrames, int64_t{30});
    int decodedCount = 0;
    int hardwareFrameCount = 0;
    QElapsedTimer timer;
    timer.start();
    for (int64_t frameNumber = 0; frameNumber < sampleCount; ++frameNumber) {
        const editor::FrameHandle frame = decoder.decodeFrame(frameNumber);
        if (frame.isNull()) {
            qCritical().noquote()
                << QStringLiteral("decoder frame=%1 result=null").arg(frameNumber);
            break;
        }
        ++decodedCount;
        if (frame.hasHardwareFrame()) {
            ++hardwareFrameCount;
        }
    }

    qInfo().noquote()
        << QStringLiteral("decoder result=%1 decoded=%2/%3 hardware_frames=%4 elapsed_ms=%5")
               .arg(decodedCount == sampleCount ? QStringLiteral("pass") : QStringLiteral("fail"))
               .arg(decodedCount)
               .arg(sampleCount)
               .arg(hardwareFrameCount)
               .arg(timer.elapsed());
    return decodedCount == sampleCount ? 0 : 4;
}

bool zeroCopyPreferredEnvironmentDetected() {
    return zeroCopyInteropEnvironmentDetected();
}

bool fontSupportsUiText(const QFont& font)
{
    const QFontMetrics metrics(font);
    const QString probe = QStringLiteral("JCut Export 0123456789");
    for (const QChar ch : probe) {
        if (!metrics.inFontUcs4(ch.unicode())) {
            return false;
        }
    }
    return true;
}

QString firstLoadedFontFamily(const QString& resourcePath)
{
    const int fontId = QFontDatabase::addApplicationFont(resourcePath);
    if (fontId < 0) {
        return {};
    }
    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    return families.isEmpty() ? QString() : families.first();
}

QString bundledUiFontFamily()
{
    const QStringList bundledFonts = {
        QStringLiteral(":/fonts/Karla-Regular.ttf"),
        QStringLiteral(":/fonts/Roboto-Medium.ttf"),
    };
    for (const QString& path : bundledFonts) {
        const QString family = firstLoadedFontFamily(path);
        if (!family.isEmpty()) {
            return family;
        }
    }
    return {};
}

constexpr int kDefaultApplicationFontPointSize = 10;
constexpr int kMinApplicationFontPointSize = 8;
constexpr int kMaxApplicationFontPointSize = 96;

int configuredApplicationFontPointSize(const QFont& fallbackFont)
{
    QSettings settings(QStringLiteral("PanelTalkEditor"), QStringLiteral("JCut"));
    const int fallbackPointSize = fallbackFont.pointSize() > 0
        ? fallbackFont.pointSize()
        : QFontInfo(fallbackFont).pointSize();
    const int pointSize = settings.value(
        QStringLiteral("ui/fontPointSize"),
        fallbackPointSize > 0 ? fallbackPointSize : kDefaultApplicationFontPointSize).toInt();
    return qBound(kMinApplicationFontPointSize, pointSize, kMaxApplicationFontPointSize);
}

void applyPreferredApplicationFont()
{
    const QFont currentFont = QApplication::font();
    const QString family = bundledUiFontFamily();
    const bool useBundledFont = !family.isEmpty();
    if (!useBundledFont && !fontSupportsUiText(currentFont)) {
        qWarning().noquote()
            << QStringLiteral("[STARTUP][WARN] Qt could not resolve a usable UI font, and bundled fonts failed to load.");
        return;
    }

    QFont readableFont = useBundledFont ? QFont(family) : currentFont;
    readableFont.setPointSize(configuredApplicationFontPointSize(currentFont));
    QApplication::setFont(readableFont);
    QToolTip::setFont(readableFont);

    if (useBundledFont) {
        qInfo().noquote()
            << QStringLiteral("[STARTUP] Using bundled UI font '%1'.").arg(family);
    }
}

}

int main(int argc, char **argv)
{
    bool runHeadlessSpeakerHarness = false;
    bool runDecoderTest = false;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--speaker-export-harness") == 0) {
            runHeadlessSpeakerHarness = true;
        }
        if (qstrcmp(argv[i], "--video") == 0 ||
            qstrncmp(argv[i], "--video=", 8) == 0) {
            runDecoderTest = true;
        }
    }
    if ((runHeadlessSpeakerHarness || runDecoderTest) &&
        qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") &&
        qEnvironmentVariableIsEmpty("DISPLAY") &&
        qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    if (runDecoderTest) {
        qputenv("JCUT_ALLOW_HEADLESS_HARDWARE_DECODE", "1");
    }

    QApplication app(argc, argv);
    Q_INIT_RESOURCE(ui_icons);
    QApplication::setApplicationName(QStringLiteral("PanelTalkEditor"));
    applyPreferredApplicationFont();
    qRegisterMetaType<editor::FrameHandle>();

    std::unique_ptr<QLockFile> lockFile;
    const bool runUiAutomationHarness = qEnvironmentVariableIsSet("JCUT_UI_AUTOMATION");
    if (!runHeadlessSpeakerHarness && !runDecoderTest && !runUiAutomationHarness) {
        // Single instance enforcement is for the interactive editor only.
        // Headless and UI automation harnesses must run alongside the UI and each other.
        const QString lockPath = QDir::tempPath() + QStringLiteral("/PanelTalkEditor.lock");
        lockFile = std::make_unique<QLockFile>(lockPath);
        lockFile->setStaleLockTime(0);
        if (!lockFile->tryLock(100)) {
            qint64 pid = 0;
            QString hostname, appname;
            lockFile->getLockInfo(&pid, &hostname, &appname);
            fprintf(stderr, "Another instance is already running (pid %lld). Exiting.\n",
                    static_cast<long long>(pid));
            return 1;
        }
    }

    if (!zeroCopyPreferredEnvironmentDetected()) {
        qWarning().noquote() << QStringLiteral(
            "[STARTUP][WARN] Preferred zero-copy decode path requires Linux GPU interop "
            "(CUDA or VAAPI render node); falling back to hardware CPU-upload or software decode.");
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("PanelVid2TikTok editor"));
    parser.addHelpOption();
    QCommandLineOption debugPlaybackOption(QStringLiteral("debug-playback"),
                                           QStringLiteral("Enable playback debug logging"));
    QCommandLineOption debugCacheOption(QStringLiteral("debug-cache"),
                                        QStringLiteral("Enable cache debug logging"));
    QCommandLineOption debugDecodeOption(QStringLiteral("debug-decode"),
                                         QStringLiteral("Enable decode debug logging"));
    QCommandLineOption debugAllOption(QStringLiteral("debug-all"),
                                      QStringLiteral("Enable all debug logging"));
    QCommandLineOption videoOption(
        QStringList{QStringLiteral("video")},
        QStringLiteral("Test the decoder in isolation using the selected video, then exit."),
        QStringLiteral("path"));
    QCommandLineOption controlPortOption(
        QStringList{QStringLiteral("control-port")},
        QStringLiteral("Control server port."),
        QStringLiteral("port"));
    QCommandLineOption noRestOption(
        QStringList{QStringLiteral("no-rest")},
        QStringLiteral("Disable the local REST/control server."));
    QCommandLineOption restVulkanDiagnosticsOption(
        QStringList{QStringLiteral("rest-vulkan-diagnostics")},
        QStringLiteral("Force Vulkan preview and skip startup project/state loading for REST screenshot diagnostics."));
    QCommandLineOption speakerHarnessOption(
        QStringList{QStringLiteral("speaker-export-harness")},
        QStringLiteral("Run speaker export harness without showing the main window."));
    QCommandLineOption stateOption(
        QStringList{QStringLiteral("state")},
        QStringLiteral("Path to state JSON for harness mode."),
        QStringLiteral("path"));
    QCommandLineOption outputOption(
        QStringList{QStringLiteral("output")},
        QStringLiteral("Output path for harness mode."),
        QStringLiteral("path"));
    QCommandLineOption speakerOption(
        QStringList{QStringLiteral("speaker-id")},
        QStringLiteral("Speaker id(s) for harness mode. Repeat or use comma-separated values."),
        QStringLiteral("id"));
    QCommandLineOption clipOption(
        QStringList{QStringLiteral("clip-id")},
        QStringLiteral("Clip id override for harness mode."),
        QStringLiteral("id"));
    QCommandLineOption formatOption(
        QStringList{QStringLiteral("format")},
        QStringLiteral("Output format override for harness mode."),
        QStringLiteral("format"));
    QCommandLineOption widthOption(
        QStringList{QStringLiteral("width")},
        QStringLiteral("Output width override for harness mode."),
        QStringLiteral("pixels"));
    QCommandLineOption heightOption(
        QStringList{QStringLiteral("height")},
        QStringLiteral("Output height override for harness mode."),
        QStringLiteral("pixels"));
    QCommandLineOption useProxyOption(
        QStringList{QStringLiteral("use-proxy")},
        QStringLiteral("Force proxy rendering in harness mode."));
    QCommandLineOption noProxyOption(
        QStringList{QStringLiteral("no-proxy")},
        QStringLiteral("Disable proxy rendering in harness mode."));
    parser.addOption(debugPlaybackOption);
    parser.addOption(debugCacheOption);
    parser.addOption(debugDecodeOption);
    parser.addOption(debugAllOption);
    parser.addOption(videoOption);
    parser.addOption(controlPortOption);
    parser.addOption(noRestOption);
    parser.addOption(restVulkanDiagnosticsOption);
    parser.addOption(speakerHarnessOption);
    parser.addOption(stateOption);
    parser.addOption(outputOption);
    parser.addOption(speakerOption);
    parser.addOption(clipOption);
    parser.addOption(formatOption);
    parser.addOption(widthOption);
    parser.addOption(heightOption);
    parser.addOption(useProxyOption);
    parser.addOption(noProxyOption);
    parser.process(app);

    if (parser.isSet(restVulkanDiagnosticsOption)) {
        qputenv("JCUT_REST_VULKAN_DIAGNOSTICS", "1");
        qputenv("JCUT_PREVIEW_BACKEND", "vulkan");
        qputenv("JCUT_VULKAN_PREVIEW_PRESENTER", "direct");
        qputenv("JCUT_RENDER_BACKEND", "vulkan");
    }

    if (parser.isSet(debugAllOption)) {
        editor::setDebugPlaybackEnabled(true);
        editor::setDebugCacheEnabled(true);
        editor::setDebugDecodeEnabled(true);
    } else {
        if (parser.isSet(debugPlaybackOption)) {
            editor::setDebugPlaybackEnabled(true);
        }
        if (parser.isSet(debugCacheOption)) {
            editor::setDebugCacheEnabled(true);
        }
        if (parser.isSet(debugDecodeOption)) {
            editor::setDebugDecodeEnabled(true);
        }
    }

    if (parser.isSet(videoOption)) {
        return runDecoderCli(parser.value(videoOption));
    }

    bool portOk = false;
    quint16 controlPort = 40130;
    const QString optionValue = parser.value(controlPortOption);
    if (!optionValue.isEmpty()) {
        const uint parsed = optionValue.toUInt(&portOk);
        if (portOk && parsed <= std::numeric_limits<quint16>::max()) {
            controlPort = static_cast<quint16>(parsed);
        }
    } else {
        const QString envValue = qEnvironmentVariable("EDITOR_CONTROL_PORT");
        const uint parsed = envValue.toUInt(&portOk);
        if (portOk && parsed <= std::numeric_limits<quint16>::max()) {
            controlPort = static_cast<quint16>(parsed);
        }
    }

    if (parser.isSet(noRestOption)) {
        controlPort = 0;
    }

    if (parser.isSet(speakerHarnessOption)) {
        editor::SpeakerExportHarnessConfig config;
        config.statePath = parser.value(stateOption);
        config.outputPath = parser.value(outputOption);
        config.outputFormat = parser.value(formatOption);
        config.clipId = parser.value(clipOption);
        config.speakerIds = parser.values(speakerOption);
        if (parser.isSet(widthOption) || parser.isSet(heightOption)) {
            bool widthOk = false;
            bool heightOk = false;
            const int parsedWidth = parser.value(widthOption).toInt(&widthOk);
            const int parsedHeight = parser.value(heightOption).toInt(&heightOk);
            config.outputSize = QSize(widthOk ? parsedWidth : 1080,
                                      heightOk ? parsedHeight : 1920);
            config.outputSizeOverride = true;
        }
        if (parser.isSet(useProxyOption) || parser.isSet(noProxyOption)) {
            config.useProxyOverride = true;
            config.useProxyMedia = parser.isSet(useProxyOption) && !parser.isSet(noProxyOption);
        }
        return editor::runSpeakerExportHarness(config);
    }

    editor::EditorWindow window(controlPort);
    window.show();
    return app.exec();
}
