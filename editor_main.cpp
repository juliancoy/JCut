#include "editor.h"
#include "debug_controls.h"
#include "speaker_export_harness.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QLockFile>

#include <limits>
#include <cstring>

namespace {

bool zeroCopyPreferredEnvironmentDetected() {
#if defined(Q_OS_LINUX)
    return QFile::exists(QStringLiteral("/proc/driver/nvidia/version")) ||
           QFile::exists(QStringLiteral("/sys/module/nvidia")) ||
           QFile::exists(QStringLiteral("/dev/dri/renderD128"));
#else
    return false;
#endif
}

}

int main(int argc, char **argv)
{
    bool runHeadlessSpeakerHarness = false;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--speaker-export-harness") == 0) {
            runHeadlessSpeakerHarness = true;
            break;
        }
    }
    if (runHeadlessSpeakerHarness &&
        qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") &&
        qEnvironmentVariableIsEmpty("DISPLAY") &&
        qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PanelTalkEditor"));
    qRegisterMetaType<editor::FrameHandle>();

    // Single instance enforcement via lock file
    const QString lockPath = QDir::tempPath() + QStringLiteral("/PanelTalkEditor.lock");
    QLockFile lockFile(lockPath);
    lockFile.setStaleLockTime(0);
    if (!lockFile.tryLock(100)) {
        qint64 pid = 0;
        QString hostname, appname;
        lockFile.getLockInfo(&pid, &hostname, &appname);
        fprintf(stderr, "Another instance is already running (pid %lld). Exiting.\n",
                static_cast<long long>(pid));
        return 1;
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
    QCommandLineOption controlPortOption(
        QStringList{QStringLiteral("control-port")},
        QStringLiteral("Control server port."),
        QStringLiteral("port"));
    QCommandLineOption noRestOption(
        QStringList{QStringLiteral("no-rest")},
        QStringLiteral("Disable the local REST/control server."));
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
    parser.addOption(controlPortOption);
    parser.addOption(noRestOption);
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
        SpeakerExportHarnessConfig config;
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
        return runSpeakerExportHarness(config);
    }

    EditorWindow window(controlPort);
    window.show();
    return app.exec();
}
