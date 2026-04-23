#include "control_server.h"
#include "build_info.h"
#include "clip_serialization.h"
#include "decoder_context.h"
#include "debug_controls.h"
#include "editor_shared.h"

#include <QAbstractButton>
#include <QApplication>
#include <QBuffer>
#include <QCoreApplication>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFutureWatcher>
#include <QHash>
#include <QJsonArray>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPoint>
#include <QPointer>
#include <QAction>
#include <QProcess>
#include <QSemaphore>
#include <QSlider>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>
#include <optional>
#include <atomic>

namespace {

constexpr int kUiInvokeTimeoutMs = 500;
constexpr int kUiBackgroundInvokeTimeoutMs = 200;
constexpr qint64 kUiHeartbeatStaleMs = 1000;
constexpr qint64 kProfileCacheFreshMs = 250;
constexpr qint64 kBackgroundRefreshTickMs = 50;
constexpr qint64 kProfileRefreshIntervalMs = 100;
constexpr qint64 kProfileDemandWindowMs = 15000;
constexpr qint64 kSnapshotDemandWindowMs = 15000;
constexpr qint64 kStateRefreshIntervalMs = 100;
constexpr qint64 kProjectRefreshIntervalMs = 750;
constexpr qint64 kHistoryRefreshIntervalMs = 400;
constexpr qint64 kUiTreeRefreshIntervalMs = 250;
constexpr qint64 kIdleStateRefreshIntervalMs = 5000;
constexpr qint64 kIdleProjectRefreshIntervalMs = 8000;
constexpr qint64 kIdleHistoryRefreshIntervalMs = 8000;
constexpr qint64 kIdleUiTreeRefreshIntervalMs = 12000;
constexpr qint64 kScreenshotMinIntervalMs = 250;
constexpr qint64 kUiRefreshCooldownAfterTimeoutMs = 750;
constexpr qint64 kFrameTraceSampleCap = 300;
constexpr qint64 kFreezeEventCap = 64;
constexpr qint64 kStallDetectHeartbeatMs = 300;
constexpr int kStallConsecutiveThreshold = 3;

bool queryBool(const QUrlQuery& query, const QString& key) {
    const QString value = query.queryItemValue(key).trimmed().toLower();
    return value == QStringLiteral("1") || value == QStringLiteral("true") ||
           value == QStringLiteral("yes") || value == QStringLiteral("on");
}

QString reasonPhrase(int statusCode) {
    switch (statusCode) {
    case 200: return QStringLiteral("OK");
    case 400: return QStringLiteral("Bad Request");
    case 404: return QStringLiteral("Not Found");
    case 405: return QStringLiteral("Method Not Allowed");
    case 429: return QStringLiteral("Too Many Requests");
    case 408: return QStringLiteral("Request Timeout");
    case 500: return QStringLiteral("Internal Server Error");
    case 503: return QStringLiteral("Service Unavailable");
    default: return QStringLiteral("Status");
    }
}

QByteArray jsonBytes(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject parseJsonObject(const QByteArray& body, QString* error) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = parseError.error != QJsonParseError::NoError
                ? parseError.errorString()
                : QStringLiteral("request body must be a JSON object");
        }
        return {};
    }
    return document.object();
}

QJsonObject enrichClipForApi(const QJsonObject& clipObject) {
    QJsonObject enriched = clipObject;
    const TimelineClip clip = editor::clipFromJson(clipObject);
    const QString playbackPath = playbackMediaPathForClip(clip);
    const QString audioPath = playbackAudioPathForClip(clip);
    const QString detectedProxyPath = playbackProxyPathForClip(clip);
    const QString transcriptPath = transcriptWorkingPathForClipFile(clip.filePath);
    const qint64 startFrame = clip.startFrame;
    const qint64 durationFrames = clip.durationFrames;

    enriched[QStringLiteral("endFrame")] = startFrame + durationFrames;
    enriched[QStringLiteral("playbackMediaPath")] = QDir::toNativeSeparators(playbackPath);
    enriched[QStringLiteral("playbackAudioPath")] = QDir::toNativeSeparators(audioPath);
    enriched[QStringLiteral("detectedProxyPath")] = QDir::toNativeSeparators(detectedProxyPath);
    enriched[QStringLiteral("proxyVideoAvailable")] = !detectedProxyPath.isEmpty();
    enriched[QStringLiteral("proxyVideoActive")] = !playbackPath.isEmpty() &&
        QFileInfo(playbackPath).absoluteFilePath() != QFileInfo(clip.filePath).absoluteFilePath();
    enriched[QStringLiteral("proxyAudioActive")] = playbackUsesAlternateAudioSource(clip);
    enriched[QStringLiteral("transcriptPath")] = QDir::toNativeSeparators(transcriptPath);
    enriched[QStringLiteral("transcriptAvailable")] =
        !transcriptPath.isEmpty() && QFileInfo::exists(transcriptPath);
    enriched[QStringLiteral("audioSourceModeResolved")] = clip.audioSourceMode;
    enriched[QStringLiteral("audioSourceStatusResolved")] = clip.audioSourceStatus;
    return enriched;
}

template <typename Result, typename Fn>
bool invokeOnUiThread(QWidget* window, int timeoutMs, Result* out, Fn&& fn) {
    if (!window || !out) {
        return false;
    }

    struct SharedState {
        QSemaphore semaphore;
        Result result{};
        bool invoked = false;
        std::atomic_bool canceled{false};
    };

    auto state = std::make_shared<SharedState>();
    const bool scheduled = QMetaObject::invokeMethod(
        window,
        [state, fn = std::forward<Fn>(fn)]() mutable {
            if (state->canceled.load(std::memory_order_acquire)) {
                state->semaphore.release();
                return;
            }
            state->result = fn();
            state->invoked = true;
            state->semaphore.release();
        },
        Qt::QueuedConnection);

    if (!scheduled) {
        return false;
    }
    if (!state->semaphore.tryAcquire(1, timeoutMs)) {
        state->canceled.store(true, std::memory_order_release);
        return false;
    }

    *out = state->result;
    return state->invoked;
}

QJsonObject widgetSnapshot(QWidget* widget) {
    QJsonObject object{
        {QStringLiteral("class"), QString::fromLatin1(widget->metaObject()->className())},
        {QStringLiteral("id"), widget->objectName()},
        {QStringLiteral("visible"), widget->isVisible()},
        {QStringLiteral("enabled"), widget->isEnabled()},
        {QStringLiteral("x"), widget->x()},
        {QStringLiteral("y"), widget->y()},
        {QStringLiteral("width"), widget->width()},
        {QStringLiteral("height"), widget->height()}
    };

    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        object[QStringLiteral("text")] = button->text();
        object[QStringLiteral("clickable")] = true;
        object[QStringLiteral("checked")] = button->isChecked();
    } else if (auto* slider = qobject_cast<QSlider*>(widget)) {
        object[QStringLiteral("clickable")] = true;
        object[QStringLiteral("value")] = slider->value();
        object[QStringLiteral("minimum")] = slider->minimum();
        object[QStringLiteral("maximum")] = slider->maximum();
    } else {
        object[QStringLiteral("clickable")] = false;
    }

    QJsonArray children;
    const auto childWidgets = widget->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* child : childWidgets) {
        children.append(widgetSnapshot(child));
    }
    object[QStringLiteral("children")] = children;
    return object;
}

QJsonObject topLevelWindowSnapshot(QWidget* widget) {
    if (!widget) {
        return {};
    }

    const QRect geometry = widget->geometry();
    const QRect frameGeometry = widget->frameGeometry();
    return QJsonObject{
        {QStringLiteral("class"), QString::fromLatin1(widget->metaObject()->className())},
        {QStringLiteral("id"), widget->objectName()},
        {QStringLiteral("title"), widget->windowTitle()},
        {QStringLiteral("visible"), widget->isVisible()},
        {QStringLiteral("active"), widget->isActiveWindow()},
        {QStringLiteral("x"), geometry.x()},
        {QStringLiteral("y"), geometry.y()},
        {QStringLiteral("width"), geometry.width()},
        {QStringLiteral("height"), geometry.height()},
        {QStringLiteral("frame_x"), frameGeometry.x()},
        {QStringLiteral("frame_y"), frameGeometry.y()},
        {QStringLiteral("frame_width"), frameGeometry.width()},
        {QStringLiteral("frame_height"), frameGeometry.height()}
    };
}

QJsonObject benchmarkDecodeRatesForState(const QJsonObject& state) {
    struct BenchmarkTarget {
        TimelineClip clip;
        QString decodePath;
        QString sourcePath;
    };

    QHash<QString, BenchmarkTarget> targetsByDecodePath;
    const QJsonArray timeline = state.value(QStringLiteral("timeline")).toArray();
    for (const QJsonValue& value : timeline) {
        if (!value.isObject()) {
            continue;
        }
        const TimelineClip clip = editor::clipFromJson(value.toObject());
        if (clip.filePath.isEmpty()) {
            continue;
        }
        const QString decodePath = interactivePreviewMediaPathForClip(clip);
        const QString dedupeKey = decodePath.isEmpty() ? clip.filePath : decodePath;
        if (dedupeKey.isEmpty() || targetsByDecodePath.contains(dedupeKey)) {
            continue;
        }
        targetsByDecodePath.insert(dedupeKey, BenchmarkTarget{clip, decodePath, clip.filePath});
    }

    QJsonArray results;
    int successCount = 0;
    int skippedCount = 0;
    int errorCount = 0;
    double fpsSum = 0.0;

    for (auto it = targetsByDecodePath.cbegin(); it != targetsByDecodePath.cend(); ++it) {
        const BenchmarkTarget& target = it.value();
        QJsonObject result{
            {QStringLiteral("clip_id"), target.clip.id},
            {QStringLiteral("label"), target.clip.label},
            {QStringLiteral("source_path"), QDir::toNativeSeparators(target.sourcePath)},
            {QStringLiteral("decode_path"), QDir::toNativeSeparators(target.decodePath)},
            {QStringLiteral("media_type"), clipMediaTypeLabel(target.clip.mediaType)},
            {QStringLiteral("source_kind"), mediaSourceKindLabel(target.clip.sourceKind)}
        };

        if (!clipHasVisuals(target.clip)) {
            result[QStringLiteral("success")] = false;
            result[QStringLiteral("skipped")] = true;
            result[QStringLiteral("reason")] = QStringLiteral("clip has no visual decode path");
            results.append(result);
            ++skippedCount;
            continue;
        }

        if (target.decodePath.isEmpty()) {
            result[QStringLiteral("success")] = false;
            result[QStringLiteral("skipped")] = true;
            result[QStringLiteral("reason")] = QStringLiteral("no interactive preview media path");
            results.append(result);
            ++skippedCount;
            continue;
        }

        editor::DecoderContext ctx(target.decodePath);
        if (!ctx.initialize()) {
            result[QStringLiteral("success")] = false;
            result[QStringLiteral("error")] = QStringLiteral("failed to initialize decoder context");
            results.append(result);
            ++errorCount;
            continue;
        }

        const editor::VideoStreamInfo info = ctx.info();
        const int64_t durationFrames = qMax<int64_t>(1, info.durationFrames);
        const int framesToBenchmark = static_cast<int>(qMin<int64_t>(90, durationFrames));
        int decodedFrames = 0;
        int nullFrames = 0;

        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < framesToBenchmark; ++i) {
            editor::FrameHandle frame = ctx.decodeFrame(i);
            if (frame.isNull()) {
                ++nullFrames;
            } else {
                ++decodedFrames;
            }
        }
        const qint64 elapsedMs = qMax<qint64>(1, timer.elapsed());
        const double fps = (1000.0 * decodedFrames) / static_cast<double>(elapsedMs);

        result[QStringLiteral("success")] = true;
        result[QStringLiteral("codec")] = info.codecName;
        result[QStringLiteral("frames_benchmarked")] = framesToBenchmark;
        result[QStringLiteral("frames_decoded")] = decodedFrames;
        result[QStringLiteral("null_frames")] = nullFrames;
        result[QStringLiteral("elapsed_ms")] = elapsedMs;
        result[QStringLiteral("fps")] = fps;
        result[QStringLiteral("hardware_accelerated")] = ctx.isHardwareAccelerated();
        result[QStringLiteral("frame_width")] = info.frameSize.width();
        result[QStringLiteral("frame_height")] = info.frameSize.height();
        results.append(result);
        ++successCount;
        fpsSum += fps;
    }

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("file_count"), results.size()},
        {QStringLiteral("success_count"), successCount},
        {QStringLiteral("skipped_count"), skippedCount},
        {QStringLiteral("error_count"), errorCount},
        {QStringLiteral("avg_fps"), successCount > 0 ? fpsSum / static_cast<double>(successCount) : 0.0},
        {QStringLiteral("results"), results}
    };
}

QJsonObject benchmarkSeekRatesForState(const QJsonObject& state) {
    struct BenchmarkTarget {
        TimelineClip clip;
        QString decodePath;
        QString sourcePath;
    };

    QHash<QString, BenchmarkTarget> targetsByDecodePath;
    const QJsonArray timeline = state.value(QStringLiteral("timeline")).toArray();
    for (const QJsonValue& value : timeline) {
        if (!value.isObject()) {
            continue;
        }
        const TimelineClip clip = editor::clipFromJson(value.toObject());
        if (clip.filePath.isEmpty()) {
            continue;
        }
        const QString decodePath = interactivePreviewMediaPathForClip(clip);
        const QString dedupeKey = decodePath.isEmpty() ? clip.filePath : decodePath;
        if (dedupeKey.isEmpty() || targetsByDecodePath.contains(dedupeKey)) {
            continue;
        }
        targetsByDecodePath.insert(dedupeKey, BenchmarkTarget{clip, decodePath, clip.filePath});
    }

    QJsonArray results;
    int successCount = 0;
    int skippedCount = 0;
    int errorCount = 0;
    double avgSeekMsSum = 0.0;

    for (auto it = targetsByDecodePath.cbegin(); it != targetsByDecodePath.cend(); ++it) {
        const BenchmarkTarget& target = it.value();
        QJsonObject result{
            {QStringLiteral("clip_id"), target.clip.id},
            {QStringLiteral("label"), target.clip.label},
            {QStringLiteral("source_path"), QDir::toNativeSeparators(target.sourcePath)},
            {QStringLiteral("decode_path"), QDir::toNativeSeparators(target.decodePath)},
            {QStringLiteral("media_type"), clipMediaTypeLabel(target.clip.mediaType)},
            {QStringLiteral("source_kind"), mediaSourceKindLabel(target.clip.sourceKind)}
        };

        if (!clipHasVisuals(target.clip)) {
            result[QStringLiteral("success")] = false;
            result[QStringLiteral("skipped")] = true;
            result[QStringLiteral("reason")] = QStringLiteral("clip has no visual decode path");
            results.append(result);
            ++skippedCount;
            continue;
        }

        if (target.decodePath.isEmpty()) {
            result[QStringLiteral("success")] = false;
            result[QStringLiteral("skipped")] = true;
            result[QStringLiteral("reason")] = QStringLiteral("no interactive preview media path");
            results.append(result);
            ++skippedCount;
            continue;
        }

        editor::DecoderContext ctx(target.decodePath);
        if (!ctx.initialize()) {
            result[QStringLiteral("success")] = false;
            result[QStringLiteral("error")] = QStringLiteral("failed to initialize decoder context");
            results.append(result);
            ++errorCount;
            continue;
        }

        const editor::VideoStreamInfo info = ctx.info();
        const int64_t durationFrames = qMax<int64_t>(1, info.durationFrames);
        const QVector<int64_t> seekTargets = {
            0,
            qMin<int64_t>(durationFrames - 1, durationFrames / 4),
            qMin<int64_t>(durationFrames - 1, durationFrames / 2),
            qMin<int64_t>(durationFrames - 1, (durationFrames * 3) / 4),
            qMax<int64_t>(0, durationFrames - 1)
        };

        QJsonArray samples;
        int successfulSeeks = 0;
        int nullSeeks = 0;
        qint64 totalSeekMs = 0;
        qint64 maxSeekMs = 0;

        for (int64_t targetFrame : seekTargets) {
            QElapsedTimer timer;
            timer.start();
            editor::FrameHandle frame = ctx.decodeFrame(targetFrame);
            const qint64 elapsedMs = qMax<qint64>(1, timer.elapsed());
            totalSeekMs += elapsedMs;
            maxSeekMs = qMax(maxSeekMs, elapsedMs);
            if (frame.isNull()) {
                ++nullSeeks;
            } else {
                ++successfulSeeks;
            }
            samples.append(QJsonObject{
                {QStringLiteral("target_frame"), static_cast<qint64>(targetFrame)},
                {QStringLiteral("elapsed_ms"), elapsedMs},
                {QStringLiteral("success"), !frame.isNull()},
                {QStringLiteral("decoded_frame"), frame.isNull() ? static_cast<qint64>(-1)
                                                                 : static_cast<qint64>(frame.frameNumber())}
            });
        }

        const double avgSeekMs =
            seekTargets.isEmpty() ? 0.0 : static_cast<double>(totalSeekMs) / static_cast<double>(seekTargets.size());

        result[QStringLiteral("success")] = true;
        result[QStringLiteral("codec")] = info.codecName;
        result[QStringLiteral("hardware_accelerated")] = ctx.isHardwareAccelerated();
        result[QStringLiteral("seek_count")] = seekTargets.size();
        result[QStringLiteral("successful_seeks")] = successfulSeeks;
        result[QStringLiteral("null_seeks")] = nullSeeks;
        result[QStringLiteral("avg_seek_ms")] = avgSeekMs;
        result[QStringLiteral("max_seek_ms")] = maxSeekMs;
        result[QStringLiteral("frame_width")] = info.frameSize.width();
        result[QStringLiteral("frame_height")] = info.frameSize.height();
        result[QStringLiteral("samples")] = samples;
        results.append(result);
        ++successCount;
        avgSeekMsSum += avgSeekMs;
    }

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("file_count"), results.size()},
        {QStringLiteral("success_count"), successCount},
        {QStringLiteral("skipped_count"), skippedCount},
        {QStringLiteral("error_count"), errorCount},
        {QStringLiteral("avg_seek_ms"), successCount > 0 ? avgSeekMsSum / static_cast<double>(successCount) : 0.0},
        {QStringLiteral("results"), results}
    };
}

QJsonArray topLevelWindowsSnapshot() {
    QJsonArray windows;
    const auto widgets = QApplication::topLevelWidgets();
    for (QWidget* widget : widgets) {
        if (!widget) {
            continue;
        }
        windows.append(topLevelWindowSnapshot(widget));
    }
    return windows;
}

QWidget* findWidgetByObjectName(QWidget* root, const QString& objectName) {
    if (!root || objectName.isEmpty()) {
        return nullptr;
    }
    if (root->objectName() == objectName) {
        return root;
    }
    const auto matches = root->findChildren<QWidget*>(objectName, Qt::FindChildrenRecursively);
    return matches.isEmpty() ? nullptr : matches.constFirst();
}

Qt::MouseButton parseMouseButton(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("right")) {
        return Qt::RightButton;
    }
    if (normalized == QStringLiteral("middle")) {
        return Qt::MiddleButton;
    }
    return Qt::LeftButton;
}

bool sendSyntheticClick(QWidget* window, const QPoint& pos, Qt::MouseButton button) {
    if (!window) {
        return false;
    }

    QWidget* target = window->childAt(pos);
    if (!target) {
        target = window;
    }

    const QPoint localPos = target->mapFrom(window, pos);
    const QPoint globalPos = target->mapToGlobal(localPos);
    QMouseEvent pressEvent(
        QEvent::MouseButtonPress,
        localPos,
        globalPos,
        button,
        button,
        Qt::NoModifier);
    QMouseEvent releaseEvent(
        QEvent::MouseButtonRelease,
        localPos,
        globalPos,
        button,
        Qt::NoButton,
        Qt::NoModifier);

    const bool pressOk = QApplication::sendEvent(target, &pressEvent);
    const bool releaseOk = QApplication::sendEvent(target, &releaseEvent);
    bool contextOk = true;
    if (button == Qt::RightButton) {
        QContextMenuEvent contextEvent(QContextMenuEvent::Mouse, localPos, globalPos);
        contextOk = QApplication::sendEvent(target, &contextEvent);
    }
    return pressOk && releaseOk && contextOk;
}

bool sendSyntheticClick(QWidget* window, const QPoint& pos) {
    return sendSyntheticClick(window, pos, Qt::LeftButton);
}

QJsonObject menuSnapshot(QMenu* menu) {
    QJsonArray actions;
    if (menu) {
        const auto menuActions = menu->actions();
        for (QAction* action : menuActions) {
            if (!action || action->isSeparator()) {
                continue;
            }
            actions.append(QJsonObject{
                {QStringLiteral("text"), action->text()},
                {QStringLiteral("enabled"), action->isEnabled()}
            });
        }
    }
    return QJsonObject{
        {QStringLiteral("ok"), menu != nullptr},
        {QStringLiteral("visible"), menu && menu->isVisible()},
        {QStringLiteral("actions"), actions}
    };
}

QMenu* activePopupMenu() {
    QWidget* widget = QApplication::activePopupWidget();
    return qobject_cast<QMenu*>(widget);
}

struct Request {
    QString method;
    QString path;
    QUrl url;
    QByteArray body;
};

class ControlServerWorker final : public QObject {
    Q_OBJECT
public:
    ControlServerWorker(QWidget* window,
                        std::function<QJsonObject()> fastSnapshotCallback,
                        std::function<QJsonObject()> stateSnapshotCallback,
                        std::function<QJsonObject()> projectSnapshotCallback,
                        std::function<QJsonObject()> historySnapshotCallback,
                        std::function<QJsonObject()> profilingCallback,
                        std::function<void()> resetProfilingCallback,
                        std::function<void(int64_t)> setPlayheadCallback,
                        std::function<QJsonObject()> renderResultCallback)
        : m_window(window)
        , m_fastSnapshotCallback(std::move(fastSnapshotCallback))
        , m_stateSnapshotCallback(std::move(stateSnapshotCallback))
        , m_projectSnapshotCallback(std::move(projectSnapshotCallback))
        , m_historySnapshotCallback(std::move(historySnapshotCallback))
        , m_profilingCallback(std::move(profilingCallback))
        , m_resetProfilingCallback(std::move(resetProfilingCallback))
        , m_setPlayheadCallback(std::move(setPlayheadCallback))
        , m_renderResultCallback(std::move(renderResultCallback)) {}

    ~ControlServerWorker() override = default;

    bool startListening(quint16 port) {
        m_server = std::make_unique<QTcpServer>();
        m_server->setMaxPendingConnections(256);
        connect(m_server.get(), &QTcpServer::newConnection, this, &ControlServerWorker::onNewConnection);
        if (!m_server->listen(QHostAddress::LocalHost, port)) {
            return false;
        }
        m_listenPort = m_server->serverPort();
        fprintf(stderr, "ControlServer listening on http://127.0.0.1:%u\n", static_cast<unsigned>(m_listenPort));
        fflush(stderr);
        m_refreshTimer = std::make_unique<QTimer>();
        m_refreshTimer->setInterval(static_cast<int>(kBackgroundRefreshTickMs));
        connect(m_refreshTimer.get(), &QTimer::timeout, this, &ControlServerWorker::refreshBackgroundCaches);
        m_refreshTimer->start();
        m_decodeRatesWatcher = std::make_unique<QFutureWatcher<QJsonObject>>();
        m_decodeSeeksWatcher = std::make_unique<QFutureWatcher<QJsonObject>>();
        connect(m_decodeRatesWatcher.get(), &QFutureWatcher<QJsonObject>::finished, this,
                &ControlServerWorker::onDecodeRatesJobFinished);
        connect(m_decodeSeeksWatcher.get(), &QFutureWatcher<QJsonObject>::finished, this,
                &ControlServerWorker::onDecodeSeeksJobFinished);
        QMetaObject::invokeMethod(this, &ControlServerWorker::refreshBackgroundCaches, Qt::QueuedConnection);
        return true;
    }

    void stopListening() {
        if (m_refreshTimer) {
            m_refreshTimer->stop();
            m_refreshTimer.reset();
        }
        for (QTcpSocket* socket : m_buffers.keys()) {
            socket->disconnectFromHost();
            socket->deleteLater();
        }
        m_buffers.clear();
        if (m_server) {
            m_server->close();
        }
        if (m_decodeRatesWatcher) {
            m_decodeRatesWatcher->cancel();
            m_decodeRatesWatcher.reset();
        }
        if (m_decodeSeeksWatcher) {
            m_decodeSeeksWatcher->cancel();
            m_decodeSeeksWatcher.reset();
        }
    }

private slots:
    void onNewConnection() {
        if (!m_server) {
            return;
        }
        while (QTcpSocket* socket = m_server->nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                m_buffers.remove(socket);
                socket->deleteLater();
            });
        }
    }

    void refreshBackgroundCaches() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const QJsonObject snapshot = fastSnapshot();
        recordFrameTraceSample(snapshot, now);
        const bool uiResponsive = uiThreadResponsive(snapshot);
        const bool playbackActive = snapshot.value(QStringLiteral("playback_active")).toBool();

        // Best-effort isolation: avoid any UI-thread cache work while playback is active.
        if (playbackActive) {
            m_cacheRefreshPausedForPlayback = true;
            return;
        }
        m_cacheRefreshPausedForPlayback = false;

        // Backpressure after UI timeout to avoid piling UI invocations.
        if (m_lastUiRefreshTimeoutMs > 0 &&
            (now - m_lastUiRefreshTimeoutMs) < kUiRefreshCooldownAfterTimeoutMs) {
            return;
        }

        const bool profileInDemand = (now - m_lastProfileDemandMs) <= kProfileDemandWindowMs;
        const bool stateInDemand =
            m_lastStateSnapshot.isEmpty() || (now - m_lastStateDemandMs) <= kSnapshotDemandWindowMs;
        const bool projectInDemand =
            m_lastProjectSnapshot.isEmpty() || (now - m_lastProjectDemandMs) <= kSnapshotDemandWindowMs;
        const bool historyInDemand =
            m_lastHistorySnapshot.isEmpty() || (now - m_lastHistoryDemandMs) <= kSnapshotDemandWindowMs;
        const bool uiTreeInDemand =
            m_lastUiTreeSnapshot.isEmpty() || (now - m_lastUiTreeDemandMs) <= kSnapshotDemandWindowMs;
        const qint64 profileIntervalMs = uiResponsive ? kProfileRefreshIntervalMs : 1000;
        const qint64 stateIntervalMs = uiResponsive
            ? (stateInDemand ? kStateRefreshIntervalMs : kIdleStateRefreshIntervalMs)
            : 1000;
        const qint64 projectIntervalMs = uiResponsive
            ? (projectInDemand ? kProjectRefreshIntervalMs : kIdleProjectRefreshIntervalMs)
            : 2000;
        const qint64 historyIntervalMs = uiResponsive
            ? (historyInDemand ? kHistoryRefreshIntervalMs : kIdleHistoryRefreshIntervalMs)
            : 1500;
        const qint64 uiTreeIntervalMs = uiResponsive
            ? (uiTreeInDemand ? kUiTreeRefreshIntervalMs : kIdleUiTreeRefreshIntervalMs)
            : 2000;

        // Refresh one cache per tick with fair scheduling (round-robin among due tasks).
        for (int step = 0; step < 5; ++step) {
            const int index = (m_refreshCursor + step) % 5;
            if (index == 0 && profileInDemand && (now - m_lastProfileRefreshAttemptMs) >= profileIntervalMs) {
                m_lastProfileRefreshAttemptMs = now;
                QString error;
                if (refreshProfileCacheFromUi(kUiBackgroundInvokeTimeoutMs, &error)) {
                    m_lastProfileRefreshError.clear();
                } else {
                    m_lastProfileRefreshError = uiResponsive ? error : QStringLiteral("ui thread is unresponsive");
                    if (error.contains(QStringLiteral("timed out"), Qt::CaseInsensitive)) {
                        m_lastUiRefreshTimeoutMs = now;
                    }
                }
                m_refreshCursor = (index + 1) % 5;
                return;
            }
            if (index == 1 && stateInDemand && (now - m_lastStateRefreshAttemptMs) >= stateIntervalMs) {
                m_lastStateRefreshAttemptMs = now;
                QString error;
                if (refreshStateCacheFromUi(kUiBackgroundInvokeTimeoutMs, &error)) {
                    m_lastStateRefreshError.clear();
                } else {
                    m_lastStateRefreshError = uiResponsive ? error : QStringLiteral("ui thread is unresponsive");
                    if (error.contains(QStringLiteral("timed out"), Qt::CaseInsensitive)) {
                        m_lastUiRefreshTimeoutMs = now;
                    }
                }
                m_refreshCursor = (index + 1) % 5;
                return;
            }
            if (index == 2 && projectInDemand && (now - m_lastProjectRefreshAttemptMs) >= projectIntervalMs) {
                m_lastProjectRefreshAttemptMs = now;
                QString error;
                if (refreshProjectCacheFromUi(kUiBackgroundInvokeTimeoutMs, &error)) {
                    m_lastProjectRefreshError.clear();
                } else {
                    m_lastProjectRefreshError = uiResponsive ? error : QStringLiteral("ui thread is unresponsive");
                    if (error.contains(QStringLiteral("timed out"), Qt::CaseInsensitive)) {
                        m_lastUiRefreshTimeoutMs = now;
                    }
                }
                m_refreshCursor = (index + 1) % 5;
                return;
            }
            if (index == 3 && historyInDemand && (now - m_lastHistoryRefreshAttemptMs) >= historyIntervalMs) {
                m_lastHistoryRefreshAttemptMs = now;
                QString error;
                if (refreshHistoryCacheFromUi(kUiBackgroundInvokeTimeoutMs, &error)) {
                    m_lastHistoryRefreshError.clear();
                } else {
                    m_lastHistoryRefreshError = uiResponsive ? error : QStringLiteral("ui thread is unresponsive");
                    if (error.contains(QStringLiteral("timed out"), Qt::CaseInsensitive)) {
                        m_lastUiRefreshTimeoutMs = now;
                    }
                }
                m_refreshCursor = (index + 1) % 5;
                return;
            }
            if (index == 4 && uiTreeInDemand && (now - m_lastUiTreeRefreshAttemptMs) >= uiTreeIntervalMs) {
                m_lastUiTreeRefreshAttemptMs = now;
                QString error;
                if (refreshUiTreeCacheFromUi(kUiBackgroundInvokeTimeoutMs, &error)) {
                    m_lastUiTreeRefreshError.clear();
                } else {
                    m_lastUiTreeRefreshError = uiResponsive ? error : QStringLiteral("ui thread is unresponsive");
                    if (error.contains(QStringLiteral("timed out"), Qt::CaseInsensitive)) {
                        m_lastUiRefreshTimeoutMs = now;
                    }
                }
                m_refreshCursor = (index + 1) % 5;
                return;
            }
        }
    }

    void onDecodeRatesJobFinished() {
        m_decodeRatesJob.running = false;
        m_decodeRatesJob.lastFinishMs = QDateTime::currentMSecsSinceEpoch();
        ++m_decodeRatesJob.completedCount;
        if (m_decodeRatesWatcher && !m_decodeRatesWatcher->isCanceled()) {
            m_decodeRatesJob.lastResult = m_decodeRatesWatcher->result();
            m_decodeRatesJob.lastError.clear();
        } else {
            m_decodeRatesJob.lastResult = QJsonObject{};
            m_decodeRatesJob.lastError = QStringLiteral("decode rates job canceled");
            ++m_decodeRatesJob.errorCount;
        }
    }

    void onDecodeSeeksJobFinished() {
        m_decodeSeeksJob.running = false;
        m_decodeSeeksJob.lastFinishMs = QDateTime::currentMSecsSinceEpoch();
        ++m_decodeSeeksJob.completedCount;
        if (m_decodeSeeksWatcher && !m_decodeSeeksWatcher->isCanceled()) {
            m_decodeSeeksJob.lastResult = m_decodeSeeksWatcher->result();
            m_decodeSeeksJob.lastError.clear();
        } else {
            m_decodeSeeksJob.lastResult = QJsonObject{};
            m_decodeSeeksJob.lastError = QStringLiteral("decode seeks job canceled");
            ++m_decodeSeeksJob.errorCount;
        }
    }

private:
    void markStateDemand() {
        m_lastStateDemandMs = QDateTime::currentMSecsSinceEpoch();
    }

    void markProjectDemand() {
        m_lastProjectDemandMs = QDateTime::currentMSecsSinceEpoch();
    }

    void markHistoryDemand() {
        m_lastHistoryDemandMs = QDateTime::currentMSecsSinceEpoch();
    }

    void markUiTreeDemand() {
        m_lastUiTreeDemandMs = QDateTime::currentMSecsSinceEpoch();
    }

    struct DecodeJobState {
        bool running = false;
        qint64 lastStartMs = 0;
        qint64 lastFinishMs = 0;
        qint64 startCount = 0;
        qint64 completedCount = 0;
        qint64 errorCount = 0;
        QString lastError;
        QJsonObject lastResult;
        QJsonObject inputSnapshotMeta;
    };

    static QJsonObject decodeSnapshotMeta(const QJsonObject& stateSnapshot) {
        return QJsonObject{
            {QStringLiteral("currentFrame"), stateSnapshot.value(QStringLiteral("currentFrame")).toInteger()},
            {QStringLiteral("timeline_count"), stateSnapshot.value(QStringLiteral("timeline")).toArray().size()},
            {QStringLiteral("tracks_count"), stateSnapshot.value(QStringLiteral("tracks")).toArray().size()}
        };
    }

    QJsonObject decodeJobMeta(const DecodeJobState& job, const QString& name) const {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        return QJsonObject{
            {QStringLiteral("name"), name},
            {QStringLiteral("running"), job.running},
            {QStringLiteral("last_start_ms"), job.lastStartMs},
            {QStringLiteral("last_finish_ms"), job.lastFinishMs},
            {QStringLiteral("running_for_ms"), job.running ? (now - job.lastStartMs) : 0},
            {QStringLiteral("start_count"), job.startCount},
            {QStringLiteral("completed_count"), job.completedCount},
            {QStringLiteral("error_count"), job.errorCount},
            {QStringLiteral("last_error"), job.lastError},
            {QStringLiteral("input_snapshot"), job.inputSnapshotMeta}
        };
    }

    QJsonObject decodeJobResponse(const DecodeJobState& job, const QString& name) const {
        QJsonObject response = decodeJobMeta(job, name);
        response[QStringLiteral("ok")] = true;
        response[QStringLiteral("status")] =
            job.running ? QStringLiteral("running")
                        : (job.lastResult.isEmpty() ? QStringLiteral("idle") : QStringLiteral("completed"));
        if (!job.lastResult.isEmpty()) {
            response[QStringLiteral("result")] = job.lastResult;
        }
        return response;
    }

    bool startDecodeRatesJob(QString* errorOut) {
        if (m_decodeRatesJob.running) {
            if (errorOut) {
                *errorOut = QStringLiteral("decode rates job is already running");
            }
            return false;
        }
        if (!m_decodeRatesWatcher) {
            if (errorOut) {
                *errorOut = QStringLiteral("decode rates watcher unavailable");
            }
            return false;
        }

        if (m_lastStateSnapshot.isEmpty()) {
            QString error;
            if (!refreshStateCacheFromUi(kUiInvokeTimeoutMs, &error)) {
                if (errorOut) {
                    *errorOut = error.isEmpty() ? QStringLiteral("state snapshot unavailable") : error;
                }
                return false;
            }
        }

        const QJsonObject stateSnapshot = m_lastStateSnapshot;
        m_decodeRatesJob.running = true;
        m_decodeRatesJob.lastStartMs = QDateTime::currentMSecsSinceEpoch();
        ++m_decodeRatesJob.startCount;
        m_decodeRatesJob.inputSnapshotMeta = decodeSnapshotMeta(stateSnapshot);
        m_decodeRatesJob.lastError.clear();
        m_decodeRatesJob.lastResult = QJsonObject{};
        m_decodeRatesWatcher->setFuture(QtConcurrent::run([stateSnapshot]() {
            return benchmarkDecodeRatesForState(stateSnapshot);
        }));
        return true;
    }

    bool startDecodeSeeksJob(QString* errorOut) {
        if (m_decodeSeeksJob.running) {
            if (errorOut) {
                *errorOut = QStringLiteral("decode seeks job is already running");
            }
            return false;
        }
        if (!m_decodeSeeksWatcher) {
            if (errorOut) {
                *errorOut = QStringLiteral("decode seeks watcher unavailable");
            }
            return false;
        }

        if (m_lastStateSnapshot.isEmpty()) {
            QString error;
            if (!refreshStateCacheFromUi(kUiInvokeTimeoutMs, &error)) {
                if (errorOut) {
                    *errorOut = error.isEmpty() ? QStringLiteral("state snapshot unavailable") : error;
                }
                return false;
            }
        }

        const QJsonObject stateSnapshot = m_lastStateSnapshot;
        m_decodeSeeksJob.running = true;
        m_decodeSeeksJob.lastStartMs = QDateTime::currentMSecsSinceEpoch();
        ++m_decodeSeeksJob.startCount;
        m_decodeSeeksJob.inputSnapshotMeta = decodeSnapshotMeta(stateSnapshot);
        m_decodeSeeksJob.lastError.clear();
        m_decodeSeeksJob.lastResult = QJsonObject{};
        m_decodeSeeksWatcher->setFuture(QtConcurrent::run([stateSnapshot]() {
            return benchmarkSeekRatesForState(stateSnapshot);
        }));
        return true;
    }

    void appendFreezeEvent(QJsonObject event) {
        event[QStringLiteral("event_index")] = static_cast<qint64>(m_freezeEvents.size());
        m_freezeEvents.append(std::move(event));
        while (m_freezeEvents.size() > kFreezeEventCap) {
            m_freezeEvents.removeAt(0);
        }
    }

    void recordFrameTraceSample(const QJsonObject& snapshot, qint64 nowMs) {
        const qint64 heartbeatAgeMs =
            snapshot.value(QStringLiteral("main_thread_heartbeat_age_ms")).toInteger(-1);
        const qint64 playheadAdvanceAgeMs =
            snapshot.value(QStringLiteral("last_playhead_advance_age_ms")).toInteger(-1);
        const bool uiResponsive = heartbeatAgeMs >= 0 && heartbeatAgeMs <= kUiHeartbeatStaleMs;
        const bool playbackActive = snapshot.value(QStringLiteral("playback_active")).toBool();

        QJsonObject sample{
            {QStringLiteral("time_ms"), nowMs},
            {QStringLiteral("current_frame"), snapshot.value(QStringLiteral("current_frame")).toInteger()},
            {QStringLiteral("playback_active"), playbackActive},
            {QStringLiteral("main_thread_heartbeat_age_ms"), heartbeatAgeMs},
            {QStringLiteral("last_playhead_advance_age_ms"), playheadAdvanceAgeMs},
            {QStringLiteral("ui_thread_responsive"), uiResponsive},
            {QStringLiteral("state_snapshot_age_ms"), m_lastStateSnapshotMs > 0 ? nowMs - m_lastStateSnapshotMs : -1},
            {QStringLiteral("state_snapshot_timeout_count"), m_stateSnapshotTimeoutCount},
            {QStringLiteral("project_snapshot_age_ms"),
             m_lastProjectSnapshotMs > 0 ? nowMs - m_lastProjectSnapshotMs : -1},
            {QStringLiteral("history_snapshot_age_ms"),
             m_lastHistorySnapshotMs > 0 ? nowMs - m_lastHistorySnapshotMs : -1},
            {QStringLiteral("decode_rates_running"), m_decodeRatesJob.running},
            {QStringLiteral("decode_seeks_running"), m_decodeSeeksJob.running}
        };
        m_frameTraceSamples.append(sample);
        while (m_frameTraceSamples.size() > kFrameTraceSampleCap) {
            m_frameTraceSamples.removeAt(0);
        }

        if (heartbeatAgeMs >= kStallDetectHeartbeatMs) {
            ++m_stallConsecutiveOverThreshold;
            if (!m_stallActive && m_stallConsecutiveOverThreshold >= kStallConsecutiveThreshold) {
                m_stallActive = true;
                m_stallStartedMs = nowMs;
                appendFreezeEvent(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("stall_start")},
                    {QStringLiteral("time_ms"), nowMs},
                    {QStringLiteral("heartbeat_age_ms"), heartbeatAgeMs},
                    {QStringLiteral("playhead_advance_age_ms"), playheadAdvanceAgeMs},
                    {QStringLiteral("playback_active"), playbackActive},
                    {QStringLiteral("current_frame"),
                     snapshot.value(QStringLiteral("current_frame")).toInteger()},
                    {QStringLiteral("decode_rates_running"), m_decodeRatesJob.running},
                    {QStringLiteral("decode_seeks_running"), m_decodeSeeksJob.running}
                });
            }
        } else {
            if (m_stallActive) {
                appendFreezeEvent(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("stall_end")},
                    {QStringLiteral("time_ms"), nowMs},
                    {QStringLiteral("duration_ms"), nowMs - m_stallStartedMs},
                    {QStringLiteral("heartbeat_age_ms"), heartbeatAgeMs},
                    {QStringLiteral("playhead_advance_age_ms"), playheadAdvanceAgeMs},
                    {QStringLiteral("current_frame"),
                     snapshot.value(QStringLiteral("current_frame")).toInteger()}
                });
                m_stallActive = false;
                m_stallStartedMs = 0;
            }
            m_stallConsecutiveOverThreshold = 0;
        }
    }

    QJsonObject frameTraceSnapshot(const QUrlQuery& query) const {
        int limit = query.queryItemValue(QStringLiteral("limit")).toInt();
        if (limit <= 0) {
            limit = 120;
        }
        limit = qMin(limit, static_cast<int>(kFrameTraceSampleCap));
        const int start = qMax(0, m_frameTraceSamples.size() - limit);

        QJsonArray samples;
        for (int i = start; i < m_frameTraceSamples.size(); ++i) {
            samples.append(m_frameTraceSamples.at(i));
        }

        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("sample_limit"), limit},
            {QStringLiteral("sample_count"), samples.size()},
            {QStringLiteral("samples"), samples},
            {QStringLiteral("freeze_events"), m_freezeEvents},
            {QStringLiteral("stall_active"), m_stallActive},
            {QStringLiteral("stall_started_ms"), m_stallStartedMs},
            {QStringLiteral("stall_consecutive_over_threshold"), m_stallConsecutiveOverThreshold},
            {QStringLiteral("stall_threshold_ms"), kStallDetectHeartbeatMs},
            {QStringLiteral("stall_threshold_consecutive"), kStallConsecutiveThreshold},
            {QStringLiteral("decode_jobs"), QJsonObject{
                {QStringLiteral("rates"), decodeJobMeta(m_decodeRatesJob, QStringLiteral("decode/rates"))},
                {QStringLiteral("seeks"), decodeJobMeta(m_decodeSeeksJob, QStringLiteral("decode/seeks"))}
            }}
        };
    }

    bool refreshProfileCacheFromUi(int timeoutMs, QString* errorOut) {
        QJsonObject profile;
        if (!invokeOnUiThread(m_window, timeoutMs, &profile, [this]() {
                return m_profilingCallback ? m_profilingCallback() : QJsonObject{};
            })) {
            ++m_profileTimeoutCount;
            if (errorOut) {
                *errorOut = QStringLiteral("timed out waiting for ui-thread profile");
            }
            return false;
        }

        m_lastProfileSnapshot = profile;
        m_lastProfileSnapshotMs = QDateTime::currentMSecsSinceEpoch();
        ++m_profileSuccessCount;
        return true;
    }

    bool refreshStateCacheFromUi(int timeoutMs, QString* errorOut) {
        if (!m_stateSnapshotCallback) {
            if (errorOut) {
                *errorOut = QStringLiteral("state snapshot callback unavailable");
            }
            return false;
        }
        QJsonObject snapshot;
        if (!invokeOnUiThread(m_window, timeoutMs, &snapshot, [this]() { return m_stateSnapshotCallback(); })) {
            ++m_stateSnapshotTimeoutCount;
            if (errorOut) {
                *errorOut = QStringLiteral("timed out waiting for state snapshot");
            }
            return false;
        }
        m_lastStateSnapshot = snapshot;
        m_lastStateSnapshotMs = QDateTime::currentMSecsSinceEpoch();
        ++m_stateSnapshotSuccessCount;
        return true;
    }

    bool refreshProjectCacheFromUi(int timeoutMs, QString* errorOut) {
        if (!m_projectSnapshotCallback) {
            if (errorOut) {
                *errorOut = QStringLiteral("project snapshot callback unavailable");
            }
            return false;
        }
        QJsonObject project;
        if (!invokeOnUiThread(m_window, timeoutMs, &project, [this]() { return m_projectSnapshotCallback(); })) {
            ++m_projectSnapshotTimeoutCount;
            if (errorOut) {
                *errorOut = QStringLiteral("timed out waiting for project snapshot");
            }
            return false;
        }
        m_lastProjectSnapshot = project;
        m_lastProjectSnapshotMs = QDateTime::currentMSecsSinceEpoch();
        ++m_projectSnapshotSuccessCount;
        return true;
    }

    bool refreshHistoryCacheFromUi(int timeoutMs, QString* errorOut) {
        if (!m_historySnapshotCallback) {
            if (errorOut) {
                *errorOut = QStringLiteral("history snapshot callback unavailable");
            }
            return false;
        }
        QJsonObject history;
        if (!invokeOnUiThread(m_window, timeoutMs, &history, [this]() { return m_historySnapshotCallback(); })) {
            ++m_historySnapshotTimeoutCount;
            if (errorOut) {
                *errorOut = QStringLiteral("timed out waiting for history snapshot");
            }
            return false;
        }
        m_lastHistorySnapshot = history;
        m_lastHistorySnapshotMs = QDateTime::currentMSecsSinceEpoch();
        ++m_historySnapshotSuccessCount;
        return true;
    }

    bool refreshUiTreeCacheFromUi(int timeoutMs, QString* errorOut) {
        QJsonObject tree;
        if (!invokeOnUiThread(m_window, timeoutMs, &tree, [this]() { return widgetSnapshot(m_window); })) {
            ++m_uiTreeSnapshotTimeoutCount;
            if (errorOut) {
                *errorOut = QStringLiteral("timed out waiting for ui hierarchy");
            }
            return false;
        }
        m_lastUiTreeSnapshot = tree;
        m_lastUiTreeSnapshotMs = QDateTime::currentMSecsSinceEpoch();
        ++m_uiTreeSnapshotSuccessCount;
        return true;
    }

    QJsonObject profileCacheMeta() const {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        return QJsonObject{
            {QStringLiteral("has_snapshot"), !m_lastProfileSnapshot.isEmpty()},
            {QStringLiteral("last_snapshot_ms"), m_lastProfileSnapshotMs},
            {QStringLiteral("last_snapshot_age_ms"),
             m_lastProfileSnapshotMs > 0 ? now - m_lastProfileSnapshotMs : -1},
            {QStringLiteral("success_count"), m_profileSuccessCount},
            {QStringLiteral("timeout_count"), m_profileTimeoutCount},
            {QStringLiteral("served_cached_count"), m_profileServedCachedCount}};
    }

    QJsonObject stateCacheMeta() const {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        return QJsonObject{
            {QStringLiteral("has_snapshot"), !m_lastStateSnapshot.isEmpty()},
            {QStringLiteral("last_snapshot_ms"), m_lastStateSnapshotMs},
            {QStringLiteral("last_snapshot_age_ms"),
             m_lastStateSnapshotMs > 0 ? now - m_lastStateSnapshotMs : -1},
            {QStringLiteral("success_count"), m_stateSnapshotSuccessCount},
            {QStringLiteral("timeout_count"), m_stateSnapshotTimeoutCount},
            {QStringLiteral("served_cached_count"), m_stateSnapshotServedCachedCount}};
    }

    QJsonObject projectCacheMeta() const {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        return QJsonObject{
            {QStringLiteral("has_snapshot"), !m_lastProjectSnapshot.isEmpty()},
            {QStringLiteral("last_snapshot_ms"), m_lastProjectSnapshotMs},
            {QStringLiteral("last_snapshot_age_ms"),
             m_lastProjectSnapshotMs > 0 ? now - m_lastProjectSnapshotMs : -1},
            {QStringLiteral("success_count"), m_projectSnapshotSuccessCount},
            {QStringLiteral("timeout_count"), m_projectSnapshotTimeoutCount}
        };
    }

    QJsonObject historyCacheMeta() const {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        return QJsonObject{
            {QStringLiteral("has_snapshot"), !m_lastHistorySnapshot.isEmpty()},
            {QStringLiteral("last_snapshot_ms"), m_lastHistorySnapshotMs},
            {QStringLiteral("last_snapshot_age_ms"),
             m_lastHistorySnapshotMs > 0 ? now - m_lastHistorySnapshotMs : -1},
            {QStringLiteral("success_count"), m_historySnapshotSuccessCount},
            {QStringLiteral("timeout_count"), m_historySnapshotTimeoutCount}
        };
    }

    QJsonObject uiTreeCacheMeta() const {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        return QJsonObject{
            {QStringLiteral("has_snapshot"), !m_lastUiTreeSnapshot.isEmpty()},
            {QStringLiteral("last_snapshot_ms"), m_lastUiTreeSnapshotMs},
            {QStringLiteral("last_snapshot_age_ms"),
             m_lastUiTreeSnapshotMs > 0 ? now - m_lastUiTreeSnapshotMs : -1},
            {QStringLiteral("success_count"), m_uiTreeSnapshotSuccessCount},
            {QStringLiteral("timeout_count"), m_uiTreeSnapshotTimeoutCount}
        };
    }

    void onReadyRead(QTcpSocket* socket) {
        m_buffers[socket].append(socket->readAll());
        std::optional<Request> request = tryParseRequest(m_buffers[socket]);
        if (!request.has_value()) {
            return;
        }
        handleRequest(socket, *request);
        m_buffers.remove(socket);
    }

    std::optional<Request> tryParseRequest(const QByteArray& data) const {
        const int headerEnd = data.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return std::nullopt;
        }

        const QList<QByteArray> lines = data.left(headerEnd).split('\n');
        if (lines.isEmpty()) {
            return std::nullopt;
        }

        const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
        if (requestLine.size() < 2) {
            return std::nullopt;
        }

        int contentLength = 0;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const int colon = line.indexOf(':');
            if (colon <= 0) {
                continue;
            }
            const QByteArray key = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            if (key == "content-length") {
                contentLength = value.toInt();
            }
        }

        const int totalSize = headerEnd + 4 + contentLength;
        if (data.size() < totalSize) {
            return std::nullopt;
        }

        Request request;
        request.method = QString::fromLatin1(requestLine.at(0));
        request.path = QString::fromLatin1(requestLine.at(1));
        request.url = QUrl(QStringLiteral("http://127.0.0.1") + request.path);
        request.body = data.mid(headerEnd + 4, contentLength);
        return request;
    }

    QJsonObject fastSnapshot() const {
        return m_fastSnapshotCallback ? m_fastSnapshotCallback() : QJsonObject{};
    }

    static bool uiThreadResponsive(const QJsonObject& snapshot) {
        return snapshot.value(QStringLiteral("main_thread_heartbeat_age_ms")).toInteger(-1) <= kUiHeartbeatStaleMs;
    }

    void writeResponse(QTcpSocket* socket, int statusCode, const QByteArray& body, const QByteArray& contentType) {
        const QByteArray header =
            "HTTP/1.1 " + QByteArray::number(statusCode) + ' ' + reasonPhrase(statusCode).toUtf8() + "\r\n"
            "Content-Type: " + contentType + "\r\n"
            "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
            "Connection: close\r\n\r\n";
        socket->write(header);
        socket->write(body);
        socket->disconnectFromHost();
    }

    void writeJson(QTcpSocket* socket, int statusCode, const QJsonObject& object) {
        writeResponse(socket, statusCode, jsonBytes(object), "application/json");
    }

    void writeError(QTcpSocket* socket, int statusCode, const QString& error) {
        writeJson(socket, statusCode, QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), error}
        });
    }

    void handleRequest(QTcpSocket* socket, const Request& request) {
        const QString path = request.url.path();
        
        // Serve HTML dashboard at root
        if (request.method == QStringLiteral("GET") && (request.url.path() == QStringLiteral("/") || request.url.path() == QStringLiteral("/index.html"))) {
            // Read the HTML file
            QFile htmlFile("control_server_webpage.html");
            if (htmlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QByteArray htmlContent = htmlFile.readAll();
                htmlFile.close();
                writeResponse(socket, 200, htmlContent, "text/html; charset=utf-8");
            } else {
                // Fallback: serve a simple HTML page
                QByteArray simpleHtml = R"(
<!DOCTYPE html>
<html>
<head>
    <title>PanelVid2TikTok Editor Dashboard</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; background: #f0f0f0; }
        .container { max-width: 1200px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        h1 { color: #333; }
        .status { padding: 10px; margin: 10px 0; border-radius: 5px; }
        .connected { background: #d4edda; color: #155724; }
        .disconnected { background: #f8d7da; color: #721c24; }
        .endpoint { background: #e2e3e5; padding: 8px; margin: 5px 0; border-radius: 3px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>PanelVid2TikTok Editor Dashboard</h1>
        <p>Real-time monitoring dashboard for the video editor.</p>
        
        <div class="status connected" id="status">Connected to Control Server</div>
        
        <h2>Available API Endpoints:</h2>
        <div class="endpoint"><strong>GET /health</strong> - System health status</div>
        <div class="endpoint"><strong>GET /version</strong> - Build information</div>
        <div class="endpoint"><strong>GET /playhead</strong> - Current playback position</div>
        <div class="endpoint"><strong>GET /state</strong> - Editor state snapshot</div>
        <div class="endpoint"><strong>GET /profile</strong> - Performance profiling data</div>
        <div class="endpoint"><strong>GET /decode/rates</strong> - Decode benchmark for current project media</div>
        <div class="endpoint"><strong>GET /decode/seeks</strong> - Seek benchmark for current project media</div>
        <div class="endpoint"><strong>GET /diag/perf</strong> - Performance diagnostics</div>
        <div class="endpoint"><strong>GET /diag/frame-trace</strong> - Recent frame/heartbeat trace and freeze events</div>
        <div class="endpoint"><strong>GET /timeline</strong> - Timeline information</div>
        <div class="endpoint"><strong>GET /project</strong> - Project information</div>
        <div class="endpoint"><strong>GET /history</strong> - History snapshot</div>
        <div class="endpoint"><strong>GET /ui</strong> - UI hierarchy</div>
        
        <h2>Controls:</h2>
        <div class="endpoint"><strong>POST /playhead</strong> - Set playhead position</div>
        <div class="endpoint"><strong>POST /profile/reset</strong> - Reset profiling stats</div>
        
        <p>For the full interactive dashboard, ensure <code>control_server_webpage.html</code> exists in the working directory.</p>
    </div>
    
    <script>
        // Simple status check
        fetch('/health').then(r => r.json()).then(data => {
            document.getElementById('status').textContent = 
                `Connected - PID: ${data.pid}, Port: ${data.port}, Uptime: ${data.uptime_seconds}s`;
        }).catch(err => {
            document.getElementById('status').className = 'status disconnected';
            document.getElementById('status').textContent = 'Disconnected: ' + err.message;
        });
    </script>
</body>
</html>
)";
                writeResponse(socket, 200, simpleHtml, "text/html; charset=utf-8");
            }
            return;
        }
        
        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/health")) {
            QJsonObject snapshot = fastSnapshot();
            snapshot[QStringLiteral("ok")] = true;
            snapshot[QStringLiteral("port")] = static_cast<qint64>(m_listenPort);
            snapshot[QStringLiteral("pid")] = snapshot.value(QStringLiteral("pid")).toInteger(static_cast<qint64>(QCoreApplication::applicationPid()));
            writeJson(socket, 200, snapshot);
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/version")) {
            const qint64 uptimeMs = QDateTime::currentMSecsSinceEpoch() - m_startTimeMs;
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("build_time"), QStringLiteral(EDITOR_BUILD_TIME)},
                {QStringLiteral("commit"), QStringLiteral(EDITOR_GIT_COMMIT)},
                {QStringLiteral("dirty"), QJsonValue(static_cast<bool>(EDITOR_GIT_DIRTY))},
                {QStringLiteral("current_time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                {QStringLiteral("uptime_seconds"), uptimeMs / 1000},
                {QStringLiteral("pid"), static_cast<qint64>(QCoreApplication::applicationPid())}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/playhead")) {
            const QJsonObject snapshot = fastSnapshot();
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("pid"), snapshot.value(QStringLiteral("pid")).toInteger(static_cast<qint64>(QCoreApplication::applicationPid()))},
                {QStringLiteral("current_frame"), snapshot.value(QStringLiteral("current_frame")).toInteger()},
                {QStringLiteral("playback_active"), snapshot.value(QStringLiteral("playback_active")).toBool()},
                {QStringLiteral("main_thread_heartbeat_age_ms"), snapshot.value(QStringLiteral("main_thread_heartbeat_age_ms")).toInteger(-1)}
            });
            return;
        }

        const bool uiBoundRoute =
            (request.method == QStringLiteral("POST") && path == QStringLiteral("/playhead")) ||
            (request.method == QStringLiteral("GET") &&
             (path == QStringLiteral("/windows") ||
              path == QStringLiteral("/screenshot") || path == QStringLiteral("/menu"))) ||
            ((request.method == QStringLiteral("POST")) &&
             (path == QStringLiteral("/menu") || path == QStringLiteral("/click-item") ||
              path == QStringLiteral("/click") || path == QStringLiteral("/profile/reset"))) ||
            ((request.method == QStringLiteral("GET") || request.method == QStringLiteral("POST")) &&
             path == QStringLiteral("/click"));

        if (uiBoundRoute && !uiThreadResponsive(fastSnapshot())) {
            writeError(socket, 503, QStringLiteral("ui thread is unresponsive"));
            return;
        }

        const bool stateDemandRoute =
            request.method == QStringLiteral("GET") &&
            (path == QStringLiteral("/state") ||
             path == QStringLiteral("/timeline") ||
             path == QStringLiteral("/tracks") ||
             path == QStringLiteral("/clips") ||
             path == QStringLiteral("/clip") ||
             path == QStringLiteral("/keyframes"));
        if (stateDemandRoute) {
            markStateDemand();
        }
        if (request.method == QStringLiteral("GET") && path == QStringLiteral("/project")) {
            markProjectDemand();
        }
        if (request.method == QStringLiteral("GET") && path == QStringLiteral("/history")) {
            markHistoryDemand();
        }
        if (request.method == QStringLiteral("GET") && path == QStringLiteral("/ui")) {
            markUiTreeDemand();
        }

        if (request.method == QStringLiteral("POST") && request.url.path() == QStringLiteral("/playhead")) {
            const QJsonObject body = QJsonDocument::fromJson(request.body).object();
            const qint64 frame = body.value(QStringLiteral("frame")).toInteger(-1);
            if (frame < 0) {
                writeError(socket, 400, QStringLiteral("invalid frame number"));
                return;
            }
            bool success = false;
            if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &success, [this, frame]() {
                    if (m_setPlayheadCallback) {
                        m_setPlayheadCallback(frame);
                        return true;
                    }
                    return false;
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for playhead seek"));
                return;
            }
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), success},
                {QStringLiteral("frame"), frame}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/state")) {
            if (m_lastStateSnapshot.isEmpty()) {
                const QString error = m_lastStateRefreshError.isEmpty()
                    ? QStringLiteral("state snapshot unavailable; cache warming")
                    : m_lastStateRefreshError;
                writeError(socket, 503, error);
                return;
            }
            ++m_stateSnapshotServedCachedCount;
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("state"), m_lastStateSnapshot}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/timeline")) {
            if (m_lastStateSnapshot.isEmpty()) {
                const QString error = m_lastStateRefreshError.isEmpty()
                    ? QStringLiteral("state snapshot unavailable; cache warming")
                    : m_lastStateRefreshError;
                writeError(socket, 503, error);
                return;
            }
            ++m_stateSnapshotServedCachedCount;
            const QJsonObject& state = m_lastStateSnapshot;
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("currentFrame"), state.value(QStringLiteral("currentFrame")).toInteger()},
                {QStringLiteral("selectedClipId"), state.value(QStringLiteral("selectedClipId")).toString()},
                {QStringLiteral("timeline"), state.value(QStringLiteral("timeline")).toArray()},
                {QStringLiteral("tracks"), state.value(QStringLiteral("tracks")).toArray()},
                {QStringLiteral("renderSyncMarkers"), state.value(QStringLiteral("renderSyncMarkers")).toArray()},
                {QStringLiteral("timelineZoom"), state.value(QStringLiteral("timelineZoom")).toDouble(4.0)},
                {QStringLiteral("timelineVerticalScroll"), state.value(QStringLiteral("timelineVerticalScroll")).toInteger(0)}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/tracks")) {
            if (m_lastStateSnapshot.isEmpty()) {
                const QString error = m_lastStateRefreshError.isEmpty()
                    ? QStringLiteral("state snapshot unavailable; cache warming")
                    : m_lastStateRefreshError;
                writeError(socket, 503, error);
                return;
            }
            ++m_stateSnapshotServedCachedCount;
            const QJsonObject& state = m_lastStateSnapshot;
            const QJsonArray tracks = state.value(QStringLiteral("tracks")).toArray();
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("count"), tracks.size()},
                {QStringLiteral("tracks"), tracks}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/clips")) {
            if (m_lastStateSnapshot.isEmpty()) {
                const QString error = m_lastStateRefreshError.isEmpty()
                    ? QStringLiteral("state snapshot unavailable; cache warming")
                    : m_lastStateRefreshError;
                writeError(socket, 503, error);
                return;
            }
            ++m_stateSnapshotServedCachedCount;
            const QJsonObject& state = m_lastStateSnapshot;

            const QUrlQuery query(request.url);
            const QString idFilter = query.queryItemValue(QStringLiteral("id")).trimmed();
            const QString labelContains =
                query.queryItemValue(QStringLiteral("label_contains")).trimmed().toLower();
            const QString fileContains =
                query.queryItemValue(QStringLiteral("file_contains")).trimmed().toLower();
            const int trackIndexFilter = query.queryItemValue(QStringLiteral("trackIndex")).toInt();
            const bool hasTrackFilter = query.hasQueryItem(QStringLiteral("trackIndex"));

            QJsonArray filtered;
            const QJsonArray timeline = state.value(QStringLiteral("timeline")).toArray();
            for (const QJsonValue& value : timeline) {
                if (!value.isObject()) {
                    continue;
                }
                QJsonObject clip = value.toObject();
                if (!idFilter.isEmpty() && clip.value(QStringLiteral("id")).toString() != idFilter) {
                    continue;
                }
                if (hasTrackFilter && clip.value(QStringLiteral("trackIndex")).toInt() != trackIndexFilter) {
                    continue;
                }
                if (!labelContains.isEmpty() &&
                    !clip.value(QStringLiteral("label")).toString().toLower().contains(labelContains)) {
                    continue;
                }
                if (!fileContains.isEmpty() &&
                    !clip.value(QStringLiteral("filePath")).toString().toLower().contains(fileContains)) {
                    continue;
                }
                
                filtered.push_back(enrichClipForApi(clip));
            }

            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("count"), filtered.size()},
                {QStringLiteral("clips"), filtered}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/clip")) {
            if (m_lastStateSnapshot.isEmpty()) {
                const QString error = m_lastStateRefreshError.isEmpty()
                    ? QStringLiteral("state snapshot unavailable; cache warming")
                    : m_lastStateRefreshError;
                writeError(socket, 503, error);
                return;
            }
            ++m_stateSnapshotServedCachedCount;
            const QJsonObject& state = m_lastStateSnapshot;
            const QUrlQuery query(request.url);
            const QString clipId = query.queryItemValue(QStringLiteral("id")).trimmed();
            if (clipId.isEmpty()) {
                writeError(socket, 400, QStringLiteral("missing id"));
                return;
            }

            const QJsonArray timeline = state.value(QStringLiteral("timeline")).toArray();
            for (int i = 0; i < timeline.size(); ++i) {
                const QJsonObject clip = timeline.at(i).toObject();
                if (clip.value(QStringLiteral("id")).toString() == clipId) {
                    writeJson(socket, 200, QJsonObject{
                        {QStringLiteral("ok"), true},
                        {QStringLiteral("index"), i},
                        {QStringLiteral("clip"), enrichClipForApi(clip)}
                    });
                    return;
                }
            }
            writeError(socket, 404, QStringLiteral("clip not found"));
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/keyframes")) {
            if (m_lastStateSnapshot.isEmpty()) {
                const QString error = m_lastStateRefreshError.isEmpty()
                    ? QStringLiteral("state snapshot unavailable; cache warming")
                    : m_lastStateRefreshError;
                writeError(socket, 503, error);
                return;
            }
            ++m_stateSnapshotServedCachedCount;
            const QJsonObject& state = m_lastStateSnapshot;
            const QUrlQuery query(request.url);
            const QString clipId = query.queryItemValue(QStringLiteral("id")).trimmed();
            if (clipId.isEmpty()) {
                writeError(socket, 400, QStringLiteral("missing id"));
                return;
            }
            const QString type = query.queryItemValue(QStringLiteral("type")).trimmed().toLower();
            const qint64 minFrame = query.queryItemValue(QStringLiteral("minFrame")).toLongLong();
            const qint64 maxFrame = query.queryItemValue(QStringLiteral("maxFrame")).toLongLong();
            const bool hasMin = query.hasQueryItem(QStringLiteral("minFrame"));
            const bool hasMax = query.hasQueryItem(QStringLiteral("maxFrame"));

            const QJsonArray timeline = state.value(QStringLiteral("timeline")).toArray();
            for (const QJsonValue& value : timeline) {
                const QJsonObject clip = value.toObject();
                if (clip.value(QStringLiteral("id")).toString() != clipId) {
                    continue;
                }

                const QString key = type == QStringLiteral("grading")
                    ? QStringLiteral("gradingKeyframes")
                    : (type == QStringLiteral("opacity")
                           ? QStringLiteral("opacityKeyframes")
                           : (type == QStringLiteral("title")
                                  ? QStringLiteral("titleKeyframes")
                                  : QStringLiteral("transformKeyframes")));
                const QJsonArray keyframes = clip.value(key).toArray();
                QJsonArray filtered;
                for (const QJsonValue& keyframeValue : keyframes) {
                    if (!keyframeValue.isObject()) {
                        continue;
                    }
                    const QJsonObject keyframe = keyframeValue.toObject();
                    const qint64 frame = keyframe.value(QStringLiteral("frame")).toInteger();
                    if (hasMin && frame < minFrame) {
                        continue;
                    }
                    if (hasMax && frame > maxFrame) {
                        continue;
                    }
                    filtered.push_back(keyframe);
                }

                writeJson(socket, 200, QJsonObject{
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("id"), clipId},
                    {QStringLiteral("type"), key},
                    {QStringLiteral("count"), filtered.size()},
                    {QStringLiteral("keyframes"), filtered}
                });
                return;
            }

            writeError(socket, 404, QStringLiteral("clip not found"));
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/project")) {
            if (m_lastProjectSnapshot.isEmpty()) {
                const QString error = m_lastProjectRefreshError.isEmpty()
                    ? QStringLiteral("project snapshot unavailable; cache warming")
                    : m_lastProjectRefreshError;
                writeError(socket, 503, error);
                return;
            }
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("project"), m_lastProjectSnapshot}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/decode/rates")) {
            markStateDemand();
            const QUrlQuery query(request.url);
            const bool refresh = queryBool(query, QStringLiteral("refresh"));
            QString error;
            if ((refresh || m_decodeRatesJob.lastResult.isEmpty()) && !m_decodeRatesJob.running &&
                !startDecodeRatesJob(&error)) {
                writeError(socket, 503, error.isEmpty() ? QStringLiteral("failed to start decode rates job") : error);
                return;
            }
            writeJson(socket, 200, decodeJobResponse(m_decodeRatesJob, QStringLiteral("decode/rates")));
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/decode/seeks")) {
            markStateDemand();
            const QUrlQuery query(request.url);
            const bool refresh = queryBool(query, QStringLiteral("refresh"));
            QString error;
            if ((refresh || m_decodeSeeksJob.lastResult.isEmpty()) && !m_decodeSeeksJob.running &&
                !startDecodeSeeksJob(&error)) {
                writeError(socket, 503, error.isEmpty() ? QStringLiteral("failed to start decode seeks job") : error);
                return;
            }
            writeJson(socket, 200, decodeJobResponse(m_decodeSeeksJob, QStringLiteral("decode/seeks")));
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/history")) {
            if (m_lastHistorySnapshot.isEmpty()) {
                const QString error = m_lastHistoryRefreshError.isEmpty()
                    ? QStringLiteral("history snapshot unavailable; cache warming")
                    : m_lastHistoryRefreshError;
                writeError(socket, 503, error);
                return;
            }
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("history"), m_lastHistorySnapshot}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/profile")) {
            m_lastProfileDemandMs = QDateTime::currentMSecsSinceEpoch();
            if (m_lastProfileSnapshot.isEmpty()) {
                const QString error = m_lastProfileRefreshError.isEmpty()
                    ? QStringLiteral("profile snapshot unavailable; cache warming")
                    : m_lastProfileRefreshError;
                writeError(socket, 503, error);
                return;
            }
            ++m_profileServedCachedCount;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const qint64 ageMs = m_lastProfileSnapshotMs > 0 ? now - m_lastProfileSnapshotMs : -1;
            const bool live = ageMs >= 0 && ageMs <= kProfileCacheFreshMs;
            const QJsonObject snapshot = fastSnapshot();
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("live"), live},
                {QStringLiteral("ui_thread_responsive"),
                 snapshot.value(QStringLiteral("main_thread_heartbeat_age_ms")).toInteger(-1) <=
                     kUiHeartbeatStaleMs},
                {QStringLiteral("ui_error"), m_lastProfileRefreshError},
                {QStringLiteral("profile"), m_lastProfileSnapshot},
                {QStringLiteral("served_cached"), true},
                {QStringLiteral("cache"), profileCacheMeta()}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") &&
            request.url.path() == QStringLiteral("/profile/cached")) {
            if (m_lastProfileSnapshot.isEmpty()) {
                writeError(socket, 404, QStringLiteral("no cached profile snapshot"));
                return;
            }
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("profile"), m_lastProfileSnapshot},
                {QStringLiteral("cache"), profileCacheMeta()}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") &&
            request.url.path() == QStringLiteral("/diag/perf")) {
            const QJsonObject snapshot = fastSnapshot();
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("ui_thread_responsive"),
                 snapshot.value(QStringLiteral("main_thread_heartbeat_age_ms")).toInteger(-1) <=
                     kUiHeartbeatStaleMs},
                {QStringLiteral("fast_snapshot"), snapshot},
                {QStringLiteral("profile_cache"), profileCacheMeta()},
                {QStringLiteral("state_cache"), stateCacheMeta()},
                {QStringLiteral("project_cache"), projectCacheMeta()},
                {QStringLiteral("history_cache"), historyCacheMeta()},
                {QStringLiteral("ui_tree_cache"), uiTreeCacheMeta()},
                {QStringLiteral("rate_limit"), QJsonObject{
                    {QStringLiteral("screenshot_count"), m_screenshotRateLimitedCount},
                    {QStringLiteral("screenshot_min_interval_ms"), kScreenshotMinIntervalMs}
                }},
                {QStringLiteral("freeze_monitor"), QJsonObject{
                    {QStringLiteral("stall_threshold_ms"), kStallDetectHeartbeatMs},
                    {QStringLiteral("stall_threshold_consecutive"), kStallConsecutiveThreshold},
                    {QStringLiteral("stall_active"), m_stallActive},
                    {QStringLiteral("stall_started_ms"), m_stallStartedMs},
                    {QStringLiteral("stall_consecutive_over_threshold"), m_stallConsecutiveOverThreshold},
                    {QStringLiteral("frame_trace_sample_count"), m_frameTraceSamples.size()},
                    {QStringLiteral("freeze_event_count"), m_freezeEvents.size()},
                    {QStringLiteral("cache_refresh_paused_for_playback"), m_cacheRefreshPausedForPlayback},
                    {QStringLiteral("last_ui_refresh_timeout_ms"), m_lastUiRefreshTimeoutMs},
                    {QStringLiteral("ui_refresh_cooldown_remaining_ms"),
                     qMax<qint64>(0, kUiRefreshCooldownAfterTimeoutMs -
                                     (QDateTime::currentMSecsSinceEpoch() - m_lastUiRefreshTimeoutMs))}
                }},
                {QStringLiteral("decode_jobs"), QJsonObject{
                    {QStringLiteral("rates"), decodeJobMeta(m_decodeRatesJob, QStringLiteral("decode/rates"))},
                    {QStringLiteral("seeks"), decodeJobMeta(m_decodeSeeksJob, QStringLiteral("decode/seeks"))}
                }},
                {QStringLiteral("debug"), editor::debugControlsSnapshot()}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") &&
            request.url.path() == QStringLiteral("/diag/frame-trace")) {
            writeJson(socket, 200, frameTraceSnapshot(QUrlQuery(request.url)));
            return;
        }

        if (request.method == QStringLiteral("POST") && request.url.path() == QStringLiteral("/profile/reset")) {
            bool reset = false;
            if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &reset, [this]() {
                    if (m_resetProfilingCallback) {
                        m_resetProfilingCallback();
                        return true;
                    }
                    return false;
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for ui-thread profile reset"));
                return;
            }
            if (reset) {
                m_lastProfileSnapshot = QJsonObject{};
                m_lastProfileSnapshotMs = 0;
            }
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), reset},
                {QStringLiteral("message"), reset ? QStringLiteral("profiling stats reset") : QStringLiteral("no reset callback configured")}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/debug")) {
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("debug"), editor::debugControlsSnapshot()}
            });
            return;
        }

        // New endpoint: /render/status - Returns GPU rendering status
        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/render/status")) {
            QJsonObject renderResult;
            if (m_renderResultCallback) {
                renderResult = m_renderResultCallback();
            }
            // Return the usedGpu flag from render result if available
            const bool usedGpu = renderResult.value(QStringLiteral("usedGpu")).toBool(false);
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("usingGpu"), usedGpu},
                {QStringLiteral("path"), usedGpu ? QStringLiteral("gpu") : QStringLiteral("cpu_fallback")},
                {QStringLiteral("encoder"), renderResult.value(QStringLiteral("encoderLabel")).toString()},
                {QStringLiteral("usedHardwareEncode"), renderResult.value(QStringLiteral("usedHardwareEncode")).toBool(false)},
                {QStringLiteral("lastRenderResult"), renderResult}
            });
            return;
        }

        // New endpoint: /hardware - Returns hardware information about the current machine
        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/hardware")) {
            QJsonObject hardware;

            // CPU info
            QProcess cpuProc;
            cpuProc.start("lscpu");
            cpuProc.waitForFinished(2000);
            QString cpuInfo = QString::fromUtf8(cpuProc.readAllStandardOutput());
            QStringList cpuLines = cpuInfo.split('\n');
            QString cpuModel, cpuCores, cpuThreads, cpuMHz;
            for (const QString& line : cpuLines) {
                if (line.startsWith("Model name:")) {
                    cpuModel = line.mid(11).trimmed();
                } else if (line.startsWith("CPU(s):")) {
                    cpuCores = line.mid(7).trimmed();
                } else if (line.startsWith("Thread(s) per core:")) {
                    cpuThreads = line.mid(18).trimmed().remove(':');
                } else if (line.startsWith("CPU MHz:")) {
                    cpuMHz = line.mid(8).trimmed();
                }
            }
            hardware[QStringLiteral("cpu_model")] = cpuModel;
            hardware[QStringLiteral("cpu_cores")] = cpuCores;
            hardware[QStringLiteral("cpu_threads")] = cpuThreads;
            hardware[QStringLiteral("cpu_mhz")] = cpuMHz;

            // Memory info
            QProcess memProc;
            memProc.start("free", QStringList() << "-h");
            memProc.waitForFinished(2000);
            QString memInfo = QString::fromUtf8(memProc.readAllStandardOutput());
            QStringList memLines = memInfo.split('\n');
            for (const QString& line : memLines) {
                if (line.startsWith("Mem:")) {
                    QStringList parts = line.simplified().split(' ');
                    if (parts.size() >= 2) {
                        hardware[QStringLiteral("memory_total")] = parts.at(1);
                    }
                    if (parts.size() >= 3) {
                        hardware[QStringLiteral("memory_used")] = parts.at(2);
                    }
                    if (parts.size() >= 4) {
                        hardware[QStringLiteral("memory_free")] = parts.at(3);
                    }
                    break;
                }
            }

            // GPU info - try nvidia-smi first
            QProcess gpuProc;
            gpuProc.start("nvidia-smi", QStringList() << "--query-gpu=name,memory.total,memory.used,utilization.gpu" << "--format=csv,noheader");
            gpuProc.waitForFinished(2000);
            if (gpuProc.exitCode() == 0) {
                QString gpuInfo = QString::fromUtf8(gpuProc.readAllStandardOutput()).trimmed();
                if (!gpuInfo.isEmpty()) {
                    QStringList gpuParts = gpuInfo.split(',');
                    if (gpuParts.size() >= 1) {
                        hardware[QStringLiteral("nvidia_gpu")] = gpuParts.at(0).trimmed();
                    }
                    if (gpuParts.size() >= 2) {
                        hardware[QStringLiteral("nvidia_memory")] = gpuParts.at(1).trimmed();
                    }
                    if (gpuParts.size() >= 4) {
                        hardware[QStringLiteral("nvidia_utilization")] = gpuParts.at(3).trimmed();
                    }
                }
            }

            // Check for AMD GPU
            QProcess amdProc;
            amdProc.start("lspci", QStringList() << "-v" << "-mm");
            amdProc.waitForFinished(2000);
            QString pciInfo = QString::fromUtf8(amdProc.readAllStandardOutput());
            if (pciInfo.contains("AMD") || pciInfo.contains("Radeon")) {
                hardware[QStringLiteral("amd_gpu_detected")] = true;
            }

            // OpenGL info
            QProcess glProc;
            glProc.start("glxinfo");
            glProc.waitForFinished(2000);
            QString glInfo = QString::fromUtf8(glProc.readAllStandardOutput());
            QStringList glLines = glInfo.split('\n');
            QString glVendor, glRenderer, glVersion;
            for (const QString& line : glLines) {
                if (line.startsWith("OpenGL vendor string:")) {
                    glVendor = line.mid(20).trimmed();
                } else if (line.startsWith("OpenGL renderer string:")) {
                    glRenderer = line.mid(23).trimmed();
                } else if (line.startsWith("OpenGL version string:")) {
                    glVersion = line.mid(22).trimmed();
                }
            }
            hardware[QStringLiteral("opengl_vendor")] = glVendor;
            hardware[QStringLiteral("opengl_renderer")] = glRenderer;
            hardware[QStringLiteral("opengl_version")] = glVersion;

            // Check for CUDA
            QProcess cudaProc;
            cudaProc.start("nvcc", QStringList() << "--version");
            cudaProc.waitForFinished(2000);
            if (cudaProc.exitCode() == 0) {
                QString cudaVersion = QString::fromUtf8(cudaProc.readAllStandardOutput());
                if (cudaVersion.contains("release")) {
                    int start = cudaVersion.indexOf("release") + 8;
                    int end = cudaVersion.indexOf(',', start);
                    if (end > start) {
                        hardware[QStringLiteral("cuda_version")] = cudaVersion.mid(start, end - start).trimmed();
                    }
                }
            }

            // Check for VA-API (Video Acceleration API)
            QProcess vaapiProc;
            vaapiProc.start("vainfo");
            vaapiProc.waitForFinished(2000);
            if (vaapiProc.exitCode() == 0) {
                QString vaapiInfo = QString::fromUtf8(vaapiProc.readAllStandardOutput());
                if (vaapiInfo.contains("VAProfile")) {
                    hardware[QStringLiteral("vaapi_available")] = true;
                }
            }

            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("hardware"), hardware}
            });
            return;
        }

        if (request.method == QStringLiteral("POST") && request.url.path() == QStringLiteral("/debug")) {
            QString error;
            const QJsonObject body = parseJsonObject(request.body, &error);
            if (!error.isEmpty()) {
                writeError(socket, 400, error);
                return;
            }
            bool updatedAny = false;
            for (auto it = body.begin(); it != body.end(); ++it) {
                if (it.key() == QStringLiteral("ok")) {
                    continue;
                }
                if (it.value().isBool()) {
                    if (!editor::setDebugControl(it.key(), it.value().toBool())) {
                        writeError(socket, 400, QStringLiteral("invalid debug field: %1").arg(it.key()));
                        return;
                    }
                } else if (it.value().isString()) {
                    editor::DebugLogLevel level = editor::DebugLogLevel::Off;
                    if (editor::parseDebugLogLevel(it.value().toString(), &level)) {
                        // Valid log level string
                        if (!editor::setDebugControlLevel(it.key(), level)) {
                            writeError(socket, 400, QStringLiteral("invalid debug field: %1").arg(it.key()));
                            return;
                        }
                    } else {
                        // Not a log level, try as a regular string option
                        if (!editor::setDebugOption(it.key(), it.value())) {
                            writeError(socket, 400, QStringLiteral("invalid debug field: %1").arg(it.key()));
                            return;
                        }
                    }
                } else if (editor::setDebugOption(it.key(), it.value())) {
                    // handled
                } else {
                    writeError(socket, 400, QStringLiteral("invalid debug field: %1").arg(it.key()));
                    return;
                }
                updatedAny = true;
            }
            if (!updatedAny) {
                writeError(socket, 400, QStringLiteral("no debug fields supplied"));
                return;
            }
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("debug"), editor::debugControlsSnapshot()}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/ui")) {
            if (m_lastUiTreeSnapshot.isEmpty()) {
                const QString error = m_lastUiTreeRefreshError.isEmpty()
                    ? QStringLiteral("ui hierarchy unavailable; cache warming")
                    : m_lastUiTreeRefreshError;
                writeError(socket, 503, error);
                return;
            }
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("ui"), m_lastUiTreeSnapshot},
                {QStringLiteral("window"), m_lastUiTreeSnapshot}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/windows")) {
            QJsonArray windows;
            if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &windows, []() {
                    return topLevelWindowsSnapshot();
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for window sizes"));
                return;
            }
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("count"), windows.size()},
                {QStringLiteral("windows"), windows}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/screenshot")) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (m_lastScreenshotRequestMs > 0 && (now - m_lastScreenshotRequestMs) < kScreenshotMinIntervalMs) {
                ++m_screenshotRateLimitedCount;
                writeError(socket, 429, QStringLiteral("screenshot requests are rate-limited"));
                return;
            }
            m_lastScreenshotRequestMs = now;
            QByteArray pngBytes;
            if (!invokeOnUiThread(m_window, 500, &pngBytes, [this]() {
                    QByteArray bytes;
                    QBuffer buffer(&bytes);
                    buffer.open(QIODevice::WriteOnly);
                    m_window->grab().save(&buffer, "PNG");
                    return bytes;
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for screenshot"));
                return;
            }
            writeResponse(socket, 200, pngBytes, "image/png");
            return;
        }

        if ((request.method == QStringLiteral("GET") || request.method == QStringLiteral("POST")) &&
            request.url.path() == QStringLiteral("/click")) {
            const QUrlQuery query(request.url);
            int x = query.queryItemValue(QStringLiteral("x")).toInt();
            int y = query.queryItemValue(QStringLiteral("y")).toInt();
            QString buttonName = query.queryItemValue(QStringLiteral("button"));
            if (request.method == QStringLiteral("POST")) {
                QString error;
                const QJsonObject body = parseJsonObject(request.body, &error);
                if (!error.isEmpty()) {
                    writeError(socket, 400, error);
                    return;
                }
                x = body.value(QStringLiteral("x")).toInt(x);
                y = body.value(QStringLiteral("y")).toInt(y);
                buttonName = body.value(QStringLiteral("button")).toString(buttonName);
            }
            const Qt::MouseButton button = parseMouseButton(buttonName);

            QJsonObject result;
            if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &result, [this, x, y, button, buttonName]() {
                    const bool clicked = sendSyntheticClick(m_window, QPoint(x, y), button);
                    return QJsonObject{
                        {QStringLiteral("ok"), clicked},
                        {QStringLiteral("x"), x},
                        {QStringLiteral("y"), y},
                        {QStringLiteral("button"), buttonName.isEmpty() ? QStringLiteral("left") : buttonName}
                    };
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for click"));
                return;
            }
            writeJson(socket, result.value(QStringLiteral("ok")).toBool() ? 200 : 500, result);
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/menu")) {
            QJsonObject response;
            if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &response, []() {
                    return menuSnapshot(activePopupMenu());
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for menu"));
                return;
            }
            writeJson(socket, response.value(QStringLiteral("ok")).toBool() ? 200 : 404, response);
            return;
        }

        if (request.method == QStringLiteral("POST") && request.url.path() == QStringLiteral("/menu")) {
            QString error;
            const QJsonObject body = parseJsonObject(request.body, &error);
            if (!error.isEmpty()) {
                writeError(socket, 400, error);
                return;
            }
            const QString text = body.value(QStringLiteral("text")).toString();
            if (text.isEmpty()) {
                writeError(socket, 400, QStringLiteral("missing text"));
                return;
            }

            QJsonObject response;
            if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &response, [text]() {
                    QMenu* menu = activePopupMenu();
                    if (!menu) {
                        return QJsonObject{
                            {QStringLiteral("ok"), false},
                            {QStringLiteral("error"), QStringLiteral("no active popup menu")}
                        };
                    }

                    for (QAction* action : menu->actions()) {
                        if (!action || action->isSeparator()) {
                            continue;
                        }
                        if (action->text() == text) {
                            const bool enabled = action->isEnabled();
                            if (enabled) {
                                action->trigger();
                            }
                            return QJsonObject{
                                {QStringLiteral("ok"), enabled},
                                {QStringLiteral("text"), text},
                                {QStringLiteral("enabled"), enabled}
                            };
                        }
                    }

                    return QJsonObject{
                        {QStringLiteral("ok"), false},
                        {QStringLiteral("error"), QStringLiteral("menu action not found")},
                        {QStringLiteral("text"), text},
                        {QStringLiteral("menu"), menuSnapshot(menu)}
                    };
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for menu action"));
                return;
            }

            writeJson(socket, response.value(QStringLiteral("ok")).toBool() ? 200 : 404, response);
            return;
        }

        if (request.method == QStringLiteral("POST") && request.url.path() == QStringLiteral("/click-item")) {
            QString error;
            const QJsonObject body = parseJsonObject(request.body, &error);
            if (!error.isEmpty()) {
                writeError(socket, 400, error);
                return;
            }
            const QString id = body.value(QStringLiteral("id")).toString();
            if (id.isEmpty()) {
                writeError(socket, 400, QStringLiteral("missing id"));
                return;
            }

            QJsonObject response;
            if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &response, [this, id]() {
                    QWidget* widget = findWidgetByObjectName(m_window, id);
                    if (!widget) {
                        return QJsonObject{
                            {QStringLiteral("ok"), false},
                            {QStringLiteral("error"), QStringLiteral("widget not found")},
                            {QStringLiteral("id"), id}
                        };
                    }

                    const QJsonObject before = widgetSnapshot(widget);
                    const QJsonObject profileBefore = m_profilingCallback ? m_profilingCallback() : QJsonObject{};

                    bool clicked = false;
                    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
                        button->click();
                        clicked = true;
                    } else {
                        clicked = sendSyntheticClick(m_window, widget->mapTo(m_window, widget->rect().center()));
                    }

                    const QJsonObject after = widgetSnapshot(widget);
                    const QJsonObject profileAfter = m_profilingCallback ? m_profilingCallback() : QJsonObject{};
                    const bool confirmed = clicked && (before != after || profileBefore != profileAfter);

                    return QJsonObject{
                        {QStringLiteral("ok"), clicked},
                        {QStringLiteral("id"), id},
                        {QStringLiteral("confirmed"), confirmed},
                        {QStringLiteral("before"), before},
                        {QStringLiteral("after"), after},
                        {QStringLiteral("profile_before"), profileBefore},
                        {QStringLiteral("profile_after"), profileAfter}
                    };
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for click-item"));
                return;
            }

            writeJson(socket, response.value(QStringLiteral("ok")).toBool() ? 200 : 404, response);
            return;
        }

        if (request.url.path().isEmpty()) {
            writeError(socket, 400, QStringLiteral("invalid request"));
            return;
        }

        writeError(socket, request.method == QStringLiteral("GET") || request.method == QStringLiteral("POST") ? 404 : 405,
                   request.method == QStringLiteral("GET") || request.method == QStringLiteral("POST")
                       ? QStringLiteral("not found")
                       : QStringLiteral("method not allowed"));
    }

    QPointer<QWidget> m_window;
    std::function<QJsonObject()> m_fastSnapshotCallback;
    std::function<QJsonObject()> m_stateSnapshotCallback;
    std::function<QJsonObject()> m_projectSnapshotCallback;
    std::function<QJsonObject()> m_historySnapshotCallback;
    std::function<QJsonObject()> m_profilingCallback;
    std::function<void()> m_resetProfilingCallback;
    std::function<void(int64_t)> m_setPlayheadCallback;
    std::function<QJsonObject()> m_renderResultCallback;
    std::unique_ptr<QTcpServer> m_server;
    std::unique_ptr<QTimer> m_refreshTimer;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    quint16 m_listenPort = 0;
    qint64 m_startTimeMs = QDateTime::currentMSecsSinceEpoch();
    QJsonObject m_lastProfileSnapshot;
    qint64 m_lastProfileSnapshotMs = 0;
    QString m_lastProfileRefreshError;
    qint64 m_profileSuccessCount = 0;
    qint64 m_profileTimeoutCount = 0;
    qint64 m_profileServedCachedCount = 0;
    qint64 m_lastProfileRefreshAttemptMs = 0;
    qint64 m_lastProfileDemandMs = 0;
    QJsonObject m_lastStateSnapshot;
    qint64 m_lastStateSnapshotMs = 0;
    QString m_lastStateRefreshError;
    qint64 m_stateSnapshotSuccessCount = 0;
    qint64 m_stateSnapshotTimeoutCount = 0;
    qint64 m_stateSnapshotServedCachedCount = 0;
    qint64 m_lastStateRefreshAttemptMs = 0;
    qint64 m_lastStateDemandMs = 0;
    QJsonObject m_lastProjectSnapshot;
    qint64 m_lastProjectSnapshotMs = 0;
    QString m_lastProjectRefreshError;
    qint64 m_projectSnapshotSuccessCount = 0;
    qint64 m_projectSnapshotTimeoutCount = 0;
    qint64 m_lastProjectRefreshAttemptMs = 0;
    qint64 m_lastProjectDemandMs = 0;
    QJsonObject m_lastHistorySnapshot;
    qint64 m_lastHistorySnapshotMs = 0;
    QString m_lastHistoryRefreshError;
    qint64 m_historySnapshotSuccessCount = 0;
    qint64 m_historySnapshotTimeoutCount = 0;
    qint64 m_lastHistoryRefreshAttemptMs = 0;
    qint64 m_lastHistoryDemandMs = 0;
    QJsonObject m_lastUiTreeSnapshot;
    qint64 m_lastUiTreeSnapshotMs = 0;
    QString m_lastUiTreeRefreshError;
    qint64 m_uiTreeSnapshotSuccessCount = 0;
    qint64 m_uiTreeSnapshotTimeoutCount = 0;
    qint64 m_lastUiTreeRefreshAttemptMs = 0;
    qint64 m_lastUiTreeDemandMs = 0;
    int m_refreshCursor = 0;
    qint64 m_lastScreenshotRequestMs = 0;
    qint64 m_screenshotRateLimitedCount = 0;
    qint64 m_lastUiRefreshTimeoutMs = 0;
    bool m_cacheRefreshPausedForPlayback = false;
    QJsonArray m_frameTraceSamples;
    QJsonArray m_freezeEvents;
    bool m_stallActive = false;
    qint64 m_stallStartedMs = 0;
    int m_stallConsecutiveOverThreshold = 0;
    DecodeJobState m_decodeRatesJob;
    DecodeJobState m_decodeSeeksJob;
    std::unique_ptr<QFutureWatcher<QJsonObject>> m_decodeRatesWatcher;
    std::unique_ptr<QFutureWatcher<QJsonObject>> m_decodeSeeksWatcher;
};

} // namespace

ControlServer::ControlServer(QWidget* window,
                             std::function<QJsonObject()> fastSnapshotCallback,
                             std::function<QJsonObject()> stateSnapshotCallback,
                             std::function<QJsonObject()> projectSnapshotCallback,
                             std::function<QJsonObject()> historySnapshotCallback,
                             std::function<QJsonObject()> profilingCallback,
                             std::function<void()> resetProfilingCallback,
                             std::function<void(int64_t)> setPlayheadCallback,
                             std::function<QJsonObject()> renderResultCallback,
                             QObject* parent)
    : QObject(parent)
    , m_window(window)
    , m_fastSnapshotCallback(std::move(fastSnapshotCallback))
    , m_stateSnapshotCallback(std::move(stateSnapshotCallback))
    , m_projectSnapshotCallback(std::move(projectSnapshotCallback))
    , m_historySnapshotCallback(std::move(historySnapshotCallback))
    , m_profilingCallback(std::move(profilingCallback))
    , m_resetProfilingCallback(std::move(resetProfilingCallback))
    , m_setPlayheadCallback(std::move(setPlayheadCallback))
    , m_renderResultCallback(std::move(renderResultCallback)) {}

ControlServer::~ControlServer() {
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, [worker = m_worker]() {
            static_cast<ControlServerWorker*>(worker)->stopListening();
        }, Qt::BlockingQueuedConnection);
        m_worker->deleteLater();
        m_worker = nullptr;
    }
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

bool ControlServer::start(quint16 port) {
    if (m_thread) {
        qDebug() << "[ControlServer] start() ignored because the server thread already exists";
        return false;
    }

    m_thread = std::make_unique<QThread>();
    auto* worker = new ControlServerWorker(m_window,
                                           m_fastSnapshotCallback,
                                           m_stateSnapshotCallback,
                                           m_projectSnapshotCallback,
                                           m_historySnapshotCallback,
                                           m_profilingCallback,
                                           m_resetProfilingCallback,
                                           m_setPlayheadCallback,
                                           m_renderResultCallback);
    m_worker = worker;
    worker->moveToThread(m_thread.get());
    connect(m_thread.get(), &QThread::finished, worker, &QObject::deleteLater);
    m_thread->start();

    bool started = false;
    if (!QMetaObject::invokeMethod(
            worker,
            [&started, worker, port]() {
                started = worker->startListening(port);
            },
            Qt::BlockingQueuedConnection)) {
        qDebug() << "[ControlServer] Failed to invoke worker startup on port" << port;
        return false;
    }

    if (!started) {
        qDebug() << "[ControlServer] Failed to initialize on port" << port;
        worker->deleteLater();
        m_worker = nullptr;
        m_thread->quit();
        m_thread->wait();
        m_thread.reset();
        return false;
    }

    qDebug() << "[ControlServer] Initialized on port" << port;
    return true;
}

void ControlServer::onNewConnection() {}

#include "control_server.moc"
