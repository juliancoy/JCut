#include "decode_trace.h"

#include "debug_controls.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMetaObject>

#include <limits>
#include <mutex>

namespace editor {

QElapsedTimer& decodeTraceTimer() {
    static QElapsedTimer timer = []() {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return timer;
}

qint64 decodeTraceMs() {
    return decodeTraceTimer().elapsed();
}

QString shortPath(const QString& path) {
    return QFileInfo(path).fileName();
}

bool linuxNvidiaDetected() {
#if defined(Q_OS_LINUX)
    return QFile::exists(QStringLiteral("/proc/driver/nvidia/version")) ||
           QFile::exists(QStringLiteral("/sys/module/nvidia"));
#else
    return false;
#endif
}

bool zeroCopyInteropSupportedForCurrentBuild() {
#if defined(Q_OS_LINUX)
    return linuxNvidiaDetected();
#else
    return false;
#endif
}

void decodeTrace(const QString& stage, const QString& detail) {
    if (debugDecodeLevel() < DebugLogLevel::Info) {
        return;
    }

    static std::mutex logMutex;
    static QHash<QString, qint64> lastLogByStage;

    const qint64 now = decodeTraceMs();
    if (!debugDecodeVerboseEnabled()) {
        // Rate-limit high-frequency decode operations
        const bool isHighFreqStage =
            stage.startsWith(QStringLiteral("AsyncDecoder::requestFrame")) ||
            stage.startsWith(QStringLiteral("AsyncDecoder::runLane.begin")) ||
            stage.startsWith(QStringLiteral("AsyncDecoder::runLane.end")) ||
            stage.startsWith(QStringLiteral("DecoderWorker::processRequest.begin")) ||
            stage.startsWith(QStringLiteral("DecoderContext::decodeFrame.begin")) ||
            stage.startsWith(QStringLiteral("DecoderContext::decodeFrame.seek")) ||
            stage.startsWith(QStringLiteral("DecoderContext::decodeFrame.end")) ||
            stage.startsWith(QStringLiteral("DecoderContext::seekAndDecode.begin")) ||
            stage.startsWith(QStringLiteral("DecoderContext::seekAndDecode.end"));
        if (isHighFreqStage) {
            std::lock_guard<std::mutex> lock(logMutex);
            const qint64 last = lastLogByStage.value(stage, std::numeric_limits<qint64>::min());
            if (now - last < 500) {  // Increased to 500ms for less spam
                return;
            }
            lastLogByStage.insert(stage, now);
        }
    }

    qDebug().noquote() << QStringLiteral("[DECODE %1 ms] %2%3")
                              .arg(now, 6)
                              .arg(stage)
                              .arg(detail.isEmpty() ? QString() : QStringLiteral(" | ") + detail);
}

void invokeRequestCallback(std::function<void(FrameHandle)> callback, FrameHandle frame) {
    if (!callback) {
        return;
    }

    QCoreApplication* app = QCoreApplication::instance();
    if (!app) {
        callback(frame);
        return;
    }

    QMetaObject::invokeMethod(app,
                              [callback = std::move(callback), frame]() mutable {
                                  if (callback) {
                                      callback(frame);
                                  }
                              },
                              Qt::QueuedConnection);
}

} // namespace editor
