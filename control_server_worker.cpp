#include "control_server_worker.h"

#include "build_info.h"
#include "clip_serialization.h"
#include "debug_controls.h"
#include "editor.h"
#include "editor_shared.h"
#include "control_server_media_diag.h"
#include "control_server_ui_utils.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMetaObject>
#include <QProcess>
#include <QSlider>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>

namespace control_server {

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

} // namespace

ControlServerWorker::ControlServerWorker(QWidget* window,
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

ControlServerWorker::~ControlServerWorker() = default;

bool ControlServerWorker::startListening(quint16 port) {
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

void ControlServerWorker::stopListening() {
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

void ControlServerWorker::onNewConnection() {
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

void ControlServerWorker::refreshBackgroundCaches() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QJsonObject snapshot = fastSnapshot();
    recordFrameTraceSample(snapshot, now);
    const bool uiResponsive = uiThreadResponsive(snapshot);
    const bool playbackActive = snapshot.value(QStringLiteral("playback_active")).toBool();

    if (playbackActive) {
        m_cacheRefreshPausedForPlayback = true;
        return;
    }
    m_cacheRefreshPausedForPlayback = false;

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

void ControlServerWorker::onDecodeRatesJobFinished() {
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

void ControlServerWorker::onDecodeSeeksJobFinished() {
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

void ControlServerWorker::markStateDemand() {
    m_lastStateDemandMs = QDateTime::currentMSecsSinceEpoch();
}

void ControlServerWorker::markProjectDemand() {
    m_lastProjectDemandMs = QDateTime::currentMSecsSinceEpoch();
}

void ControlServerWorker::markHistoryDemand() {
    m_lastHistoryDemandMs = QDateTime::currentMSecsSinceEpoch();
}

void ControlServerWorker::markUiTreeDemand() {
    m_lastUiTreeDemandMs = QDateTime::currentMSecsSinceEpoch();
}

QJsonObject ControlServerWorker::decodeSnapshotMeta(const QJsonObject& stateSnapshot) {
    return QJsonObject{
        {QStringLiteral("currentFrame"), stateSnapshot.value(QStringLiteral("currentFrame")).toInteger()},
        {QStringLiteral("timeline_count"), stateSnapshot.value(QStringLiteral("timeline")).toArray().size()},
        {QStringLiteral("tracks_count"), stateSnapshot.value(QStringLiteral("tracks")).toArray().size()}
    };
}

QJsonObject ControlServerWorker::decodeJobMeta(const DecodeJobState& job, const QString& name) const {
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

QJsonObject ControlServerWorker::decodeJobResponse(const DecodeJobState& job, const QString& name) const {
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

bool ControlServerWorker::startDecodeRatesJob(QString* errorOut) {
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

bool ControlServerWorker::startDecodeSeeksJob(QString* errorOut) {
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

void ControlServerWorker::appendFreezeEvent(QJsonObject event) {
    event[QStringLiteral("event_index")] = static_cast<qint64>(m_freezeEvents.size());
    m_freezeEvents.append(std::move(event));
    while (m_freezeEvents.size() > kFreezeEventCap) {
        m_freezeEvents.removeAt(0);
    }
}

void ControlServerWorker::recordFrameTraceSample(const QJsonObject& snapshot, qint64 nowMs) {
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

QJsonObject ControlServerWorker::frameTraceSnapshot(const QUrlQuery& query) const {
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

bool ControlServerWorker::refreshProfileCacheFromUi(int timeoutMs, QString* errorOut) {
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

bool ControlServerWorker::refreshStateCacheFromUi(int timeoutMs, QString* errorOut) {
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

bool ControlServerWorker::refreshProjectCacheFromUi(int timeoutMs, QString* errorOut) {
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

bool ControlServerWorker::refreshHistoryCacheFromUi(int timeoutMs, QString* errorOut) {
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

bool ControlServerWorker::refreshUiTreeCacheFromUi(int timeoutMs, QString* errorOut) {
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

QJsonObject ControlServerWorker::profileCacheMeta() const {
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

QJsonObject ControlServerWorker::stateCacheMeta() const {
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

QJsonObject ControlServerWorker::projectCacheMeta() const {
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

QJsonObject ControlServerWorker::historyCacheMeta() const {
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

QJsonObject ControlServerWorker::uiTreeCacheMeta() const {
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

void ControlServerWorker::onReadyRead(QTcpSocket* socket) {
    m_buffers[socket].append(socket->readAll());
    std::optional<Request> request = tryParseRequest(m_buffers[socket]);
    if (!request.has_value()) {
        return;
    }
    handleRequest(socket, *request);
    m_buffers.remove(socket);
}

QJsonObject ControlServerWorker::fastSnapshot() const {
    return m_fastSnapshotCallback ? m_fastSnapshotCallback() : QJsonObject{};
}

bool ControlServerWorker::uiThreadResponsive(const QJsonObject& snapshot) {
    return snapshot.value(QStringLiteral("main_thread_heartbeat_age_ms")).toInteger(-1) <= kUiHeartbeatStaleMs;
}

void ControlServerWorker::writeResponse(QTcpSocket* socket, int statusCode, const QByteArray& body, const QByteArray& contentType) {
    const QByteArray header =
        "HTTP/1.1 " + QByteArray::number(statusCode) + ' ' + reasonPhrase(statusCode).toUtf8() + "\r\n"
        "Content-Type: " + contentType + "\r\n"
        "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
        "Connection: close\r\n\r\n";
    socket->write(header);
    socket->write(body);
    socket->disconnectFromHost();
}

void ControlServerWorker::writeJson(QTcpSocket* socket, int statusCode, const QJsonObject& object) {
    writeResponse(socket, statusCode, jsonBytes(object), "application/json");
}

void ControlServerWorker::writeError(QTcpSocket* socket, int statusCode, const QString& error) {
    writeJson(socket, statusCode, QJsonObject{
        {QStringLiteral("ok"), false},
        {QStringLiteral("error"), error}
    });
}

void ControlServerWorker::noteDemand(const Request& request) {
    const QString path = request.url.path();
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
}

bool ControlServerWorker::handleUiBoundRouteGuard(QTcpSocket* socket, const Request& request) {
    const QString path = request.url.path();
    const bool uiBoundRoute =
        (request.method == QStringLiteral("POST") && path == QStringLiteral("/playhead")) ||
        (request.method == QStringLiteral("GET") &&
         (path == QStringLiteral("/windows") ||
          path == QStringLiteral("/screenshot") || path == QStringLiteral("/menu"))) ||
        ((request.method == QStringLiteral("POST")) &&
         (path == QStringLiteral("/menu") || path == QStringLiteral("/click-item") ||
          path == QStringLiteral("/click") || path == QStringLiteral("/profile/reset") ||
          path == QStringLiteral("/clips"))) ||
        ((request.method == QStringLiteral("GET") || request.method == QStringLiteral("POST")) &&
         path == QStringLiteral("/click"));

    if (uiBoundRoute && !uiThreadResponsive(fastSnapshot())) {
        writeError(socket, 503, QStringLiteral("ui thread is unresponsive"));
        return true;
    }
    return false;
}

void ControlServerWorker::handleRequest(QTcpSocket* socket, const Request& request) {
    noteDemand(request);
    if (handleRoot(socket, request)) return;
    if (handleHealth(socket, request)) return;
    if (handleVersion(socket, request)) return;
    if (handleUiBoundRouteGuard(socket, request)) return;
    if (handlePlayhead(socket, request)) return;
    if (handleStateRoutes(socket, request)) return;
    if (handleProjectRoutes(socket, request)) return;
    if (handleDecodeRoutes(socket, request)) return;
    if (handleHistoryRoutes(socket, request)) return;
    if (handleProfileRoutes(socket, request)) return;
    if (handleDebugRoutes(socket, request)) return;
    if (handleRenderRoutes(socket, request)) return;
    if (handleHardwareRoutes(socket, request)) return;
    if (handleUiRoutes(socket, request)) return;
    handleFallback(socket, request);
}

bool ControlServerWorker::handleRoot(QTcpSocket* socket, const Request& request) {
    if (request.method != QStringLiteral("GET") ||
        (request.url.path() != QStringLiteral("/") && request.url.path() != QStringLiteral("/index.html"))) {
        return false;
    }

    QFile htmlFile("control_server_webpage.html");
    if (htmlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QByteArray htmlContent = htmlFile.readAll();
        htmlFile.close();
        writeResponse(socket, 200, htmlContent, "text/html; charset=utf-8");
    } else {
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
    return true;
}

bool ControlServerWorker::handleHealth(QTcpSocket* socket, const Request& request) {
    if (request.method != QStringLiteral("GET") || request.url.path() != QStringLiteral("/health")) {
        return false;
    }

    QJsonObject snapshot = fastSnapshot();
    snapshot[QStringLiteral("ok")] = true;
    snapshot[QStringLiteral("port")] = static_cast<qint64>(m_listenPort);
    snapshot[QStringLiteral("pid")] = snapshot.value(QStringLiteral("pid")).toInteger(static_cast<qint64>(QCoreApplication::applicationPid()));
    writeJson(socket, 200, snapshot);
    return true;
}

bool ControlServerWorker::handleVersion(QTcpSocket* socket, const Request& request) {
    if (request.method != QStringLiteral("GET") || request.url.path() != QStringLiteral("/version")) {
        return false;
    }

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
    return true;
}

bool ControlServerWorker::handlePlayhead(QTcpSocket* socket, const Request& request) {
    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/playhead")) {
        const QJsonObject snapshot = fastSnapshot();
        writeJson(socket, 200, QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("pid"), snapshot.value(QStringLiteral("pid")).toInteger(static_cast<qint64>(QCoreApplication::applicationPid()))},
            {QStringLiteral("current_frame"), snapshot.value(QStringLiteral("current_frame")).toInteger()},
            {QStringLiteral("playback_active"), snapshot.value(QStringLiteral("playback_active")).toBool()},
            {QStringLiteral("main_thread_heartbeat_age_ms"), snapshot.value(QStringLiteral("main_thread_heartbeat_age_ms")).toInteger(-1)}
        });
        return true;
    }

    if (request.method == QStringLiteral("POST") && request.url.path() == QStringLiteral("/playhead")) {
        const QJsonObject body = QJsonDocument::fromJson(request.body).object();
        const qint64 frame = body.value(QStringLiteral("frame")).toInteger(-1);
        if (frame < 0) {
            writeError(socket, 400, QStringLiteral("invalid frame number"));
            return true;
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
            return true;
        }
        writeJson(socket, 200, QJsonObject{
            {QStringLiteral("ok"), success},
            {QStringLiteral("frame"), frame}
        });
        return true;
    }

    return false;
}

bool ControlServerWorker::handleStateRoutes(QTcpSocket* socket, const Request& request) {
    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/state")) {
        if (m_lastStateSnapshot.isEmpty()) {
            const QString error = m_lastStateRefreshError.isEmpty()
                ? QStringLiteral("state snapshot unavailable; cache warming")
                : m_lastStateRefreshError;
            writeError(socket, 503, error);
            return true;
        }
        ++m_stateSnapshotServedCachedCount;
        writeJson(socket, 200, QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("state"), m_lastStateSnapshot}
        });
        return true;
    }

    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/timeline")) {
        if (m_lastStateSnapshot.isEmpty()) {
            const QString error = m_lastStateRefreshError.isEmpty()
                ? QStringLiteral("state snapshot unavailable; cache warming")
                : m_lastStateRefreshError;
            writeError(socket, 503, error);
            return true;
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
        return true;
    }

    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/tracks")) {
        if (m_lastStateSnapshot.isEmpty()) {
            const QString error = m_lastStateRefreshError.isEmpty()
                ? QStringLiteral("state snapshot unavailable; cache warming")
                : m_lastStateRefreshError;
            writeError(socket, 503, error);
            return true;
        }
        ++m_stateSnapshotServedCachedCount;
        const QJsonObject& state = m_lastStateSnapshot;
        const QJsonArray tracks = state.value(QStringLiteral("tracks")).toArray();
        writeJson(socket, 200, QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("count"), tracks.size()},
            {QStringLiteral("tracks"), tracks}
        });
        return true;
    }

    if (request.method == QStringLiteral("POST") && request.url.path() == QStringLiteral("/clips")) {
        const QString parseErrorMessage;
        QString error;
        const QJsonObject body = control_server::parseJsonObject(request.body, &error);
        if (!error.isEmpty()) {
            writeError(socket, 400, error);
            return true;
        }

        const QString filePath = body.value(QStringLiteral("filePath")).toString().trimmed();
        if (filePath.isEmpty()) {
            writeError(socket, 400, QStringLiteral("missing filePath"));
            return true;
        }

        const qint64 startFrame = body.contains(QStringLiteral("startFrame"))
            ? body.value(QStringLiteral("startFrame")).toInteger(-1)
            : -1;

        bool success = false;
        if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &success, [this, filePath, startFrame]() {
                const auto editor = qobject_cast<EditorWindow*>(m_window);
                if (!editor) {
                    return false;
                }
                editor->addFileToTimeline(filePath, startFrame);
                return true;
            })) {
            writeError(socket, 503, QStringLiteral("timed out waiting for UI thread"));
            return true;
        }

        writeJson(socket, 200, QJsonObject{
            {QStringLiteral("ok"), success},
            {QStringLiteral("filePath"), filePath},
            {QStringLiteral("startFrame"), startFrame}
        });
        return true;
    }

    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/clips")) {
        if (m_lastStateSnapshot.isEmpty()) {
            const QString error = m_lastStateRefreshError.isEmpty()
                ? QStringLiteral("state snapshot unavailable; cache warming")
                : m_lastStateRefreshError;
            writeError(socket, 503, error);
            return true;
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
        return true;
    }

    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/clip")) {
        if (m_lastStateSnapshot.isEmpty()) {
            const QString error = m_lastStateRefreshError.isEmpty()
                ? QStringLiteral("state snapshot unavailable; cache warming")
                : m_lastStateRefreshError;
            writeError(socket, 503, error);
            return true;
        }
        ++m_stateSnapshotServedCachedCount;
        const QJsonObject& state = m_lastStateSnapshot;
        const QUrlQuery query(request.url);
        const QString clipId = query.queryItemValue(QStringLiteral("id")).trimmed();
        if (clipId.isEmpty()) {
            writeError(socket, 400, QStringLiteral("missing id"));
            return true;
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
                return true;
            }
        }
        writeError(socket, 404, QStringLiteral("clip not found"));
        return true;
    }

    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/keyframes")) {
        if (m_lastStateSnapshot.isEmpty()) {
            const QString error = m_lastStateRefreshError.isEmpty()
                ? QStringLiteral("state snapshot unavailable; cache warming")
                : m_lastStateRefreshError;
            writeError(socket, 503, error);
            return true;
        }
        ++m_stateSnapshotServedCachedCount;
        const QJsonObject& state = m_lastStateSnapshot;
        const QUrlQuery query(request.url);
        const QString clipId = query.queryItemValue(QStringLiteral("id")).trimmed();
        if (clipId.isEmpty()) {
            writeError(socket, 400, QStringLiteral("missing id"));
            return true;
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
            return true;
        }

        writeError(socket, 404, QStringLiteral("clip not found"));
        return true;
    }

    return false;
}

bool ControlServerWorker::handleProjectRoutes(QTcpSocket* socket, const Request& request) {
    if (request.method != QStringLiteral("GET") || request.url.path() != QStringLiteral("/project")) {
        return false;
    }

    if (m_lastProjectSnapshot.isEmpty()) {
        const QString error = m_lastProjectRefreshError.isEmpty()
            ? QStringLiteral("project snapshot unavailable; cache warming")
            : m_lastProjectRefreshError;
        writeError(socket, 503, error);
        return true;
    }
    writeJson(socket, 200, QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("project"), m_lastProjectSnapshot}
    });
    return true;
}

bool ControlServerWorker::handleDecodeRoutes(QTcpSocket* socket, const Request& request) {
    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/decode/rates")) {
        markStateDemand();
        const QUrlQuery query(request.url);
        const bool refresh = queryBool(query, QStringLiteral("refresh"));
        QString error;
        if ((refresh || m_decodeRatesJob.lastResult.isEmpty()) && !m_decodeRatesJob.running &&
            !startDecodeRatesJob(&error)) {
            writeError(socket, 503, error.isEmpty() ? QStringLiteral("failed to start decode rates job") : error);
            return true;
        }
        writeJson(socket, 200, decodeJobResponse(m_decodeRatesJob, QStringLiteral("decode/rates")));
        return true;
    }

    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/decode/seeks")) {
        markStateDemand();
        const QUrlQuery query(request.url);
        const bool refresh = queryBool(query, QStringLiteral("refresh"));
        QString error;
        if ((refresh || m_decodeSeeksJob.lastResult.isEmpty()) && !m_decodeSeeksJob.running &&
            !startDecodeSeeksJob(&error)) {
            writeError(socket, 503, error.isEmpty() ? QStringLiteral("failed to start decode seeks job") : error);
            return true;
        }
        writeJson(socket, 200, decodeJobResponse(m_decodeSeeksJob, QStringLiteral("decode/seeks")));
        return true;
    }

    return false;
}

bool ControlServerWorker::handleHistoryRoutes(QTcpSocket* socket, const Request& request) {
    if (request.method != QStringLiteral("GET") || request.url.path() != QStringLiteral("/history")) {
        return false;
    }

    if (m_lastHistorySnapshot.isEmpty()) {
        const QString error = m_lastHistoryRefreshError.isEmpty()
            ? QStringLiteral("history snapshot unavailable; cache warming")
            : m_lastHistoryRefreshError;
        writeError(socket, 503, error);
        return true;
    }
    writeJson(socket, 200, QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("history"), m_lastHistorySnapshot}
    });
    return true;
}

bool ControlServerWorker::handleProfileRoutes(QTcpSocket* socket, const Request& request) {
    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/profile")) {
        m_lastProfileDemandMs = QDateTime::currentMSecsSinceEpoch();
        if (m_lastProfileSnapshot.isEmpty()) {
            const QString error = m_lastProfileRefreshError.isEmpty()
                ? QStringLiteral("profile snapshot unavailable; cache warming")
                : m_lastProfileRefreshError;
            writeError(socket, 503, error);
            return true;
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
        return true;
    }

    if (request.method == QStringLiteral("GET") &&
        request.url.path() == QStringLiteral("/profile/cached")) {
        if (m_lastProfileSnapshot.isEmpty()) {
            writeError(socket, 404, QStringLiteral("no cached profile snapshot"));
            return true;
        }
        writeJson(socket, 200, QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("profile"), m_lastProfileSnapshot},
            {QStringLiteral("cache"), profileCacheMeta()}
        });
        return true;
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
        return true;
    }

    if (request.method == QStringLiteral("GET") &&
        request.url.path() == QStringLiteral("/diag/frame-trace")) {
        writeJson(socket, 200, frameTraceSnapshot(QUrlQuery(request.url)));
        return true;
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
            return true;
        }
        if (reset) {
            m_lastProfileSnapshot = QJsonObject{};
            m_lastProfileSnapshotMs = 0;
        }
        writeJson(socket, 200, QJsonObject{
            {QStringLiteral("ok"), reset},
            {QStringLiteral("message"), reset ? QStringLiteral("profiling stats reset") : QStringLiteral("no reset callback configured")}
        });
        return true;
    }

    return false;
}

bool ControlServerWorker::handleDebugRoutes(QTcpSocket* socket, const Request& request) {
    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/debug")) {
        writeJson(socket, 200, QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("debug"), editor::debugControlsSnapshot()}
        });
        return true;
    }

    if (request.method == QStringLiteral("POST") && request.url.path() == QStringLiteral("/debug")) {
        QString error;
        const QJsonObject body = parseJsonObject(request.body, &error);
        if (!error.isEmpty()) {
            writeError(socket, 400, error);
            return true;
        }
        bool updatedAny = false;
        for (auto it = body.begin(); it != body.end(); ++it) {
            if (it.key() == QStringLiteral("ok")) {
                continue;
            }
            if (it.value().isBool()) {
                if (!editor::setDebugControl(it.key(), it.value().toBool())) {
                    writeError(socket, 400, QStringLiteral("invalid debug field: %1").arg(it.key()));
                    return true;
                }
            } else if (it.value().isString()) {
                editor::DebugLogLevel level = editor::DebugLogLevel::Off;
                if (editor::parseDebugLogLevel(it.value().toString(), &level)) {
                    if (!editor::setDebugControlLevel(it.key(), level)) {
                        writeError(socket, 400, QStringLiteral("invalid debug field: %1").arg(it.key()));
                        return true;
                    }
                } else {
                    if (!editor::setDebugOption(it.key(), it.value())) {
                        writeError(socket, 400, QStringLiteral("invalid debug field: %1").arg(it.key()));
                        return true;
                    }
                }
            } else if (editor::setDebugOption(it.key(), it.value())) {
            } else {
                writeError(socket, 400, QStringLiteral("invalid debug field: %1").arg(it.key()));
                return true;
            }
            updatedAny = true;
        }
        if (!updatedAny) {
            writeError(socket, 400, QStringLiteral("no debug fields supplied"));
            return true;
        }
        writeJson(socket, 200, QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("debug"), editor::debugControlsSnapshot()}
        });
        return true;
    }

    return false;
}

bool ControlServerWorker::handleRenderRoutes(QTcpSocket* socket, const Request& request) {
    if (request.method != QStringLiteral("GET") || request.url.path() != QStringLiteral("/render/status")) {
        return false;
    }

    QJsonObject renderResult;
    if (m_renderResultCallback) {
        renderResult = m_renderResultCallback();
    }
    const bool usedGpu = renderResult.value(QStringLiteral("usedGpu")).toBool(false);
    writeJson(socket, 200, QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("usingGpu"), usedGpu},
        {QStringLiteral("path"), usedGpu ? QStringLiteral("gpu") : QStringLiteral("cpu_fallback")},
        {QStringLiteral("encoder"), renderResult.value(QStringLiteral("encoderLabel")).toString()},
        {QStringLiteral("usedHardwareEncode"), renderResult.value(QStringLiteral("usedHardwareEncode")).toBool(false)},
        {QStringLiteral("lastRenderResult"), renderResult}
    });
    return true;
}

bool ControlServerWorker::handleHardwareRoutes(QTcpSocket* socket, const Request& request) {
    if (request.method != QStringLiteral("GET") || request.url.path() != QStringLiteral("/hardware")) {
        return false;
    }

    QJsonObject hardware;

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

    QProcess amdProc;
    amdProc.start("lspci", QStringList() << "-v" << "-mm");
    amdProc.waitForFinished(2000);
    QString pciInfo = QString::fromUtf8(amdProc.readAllStandardOutput());
    if (pciInfo.contains("AMD") || pciInfo.contains("Radeon")) {
        hardware[QStringLiteral("amd_gpu_detected")] = true;
    }

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
    return true;
}

bool ControlServerWorker::handleUiRoutes(QTcpSocket* socket, const Request& request) {
    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/ui")) {
        if (m_lastUiTreeSnapshot.isEmpty()) {
            const QString error = m_lastUiTreeRefreshError.isEmpty()
                ? QStringLiteral("ui hierarchy unavailable; cache warming")
                : m_lastUiTreeRefreshError;
            writeError(socket, 503, error);
            return true;
        }
        writeJson(socket, 200, QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("ui"), m_lastUiTreeSnapshot},
            {QStringLiteral("window"), m_lastUiTreeSnapshot}
        });
        return true;
    }

    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/windows")) {
        QJsonArray windows;
        if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &windows, []() {
                return topLevelWindowsSnapshot();
            })) {
            writeError(socket, 503, QStringLiteral("timed out waiting for window sizes"));
            return true;
        }
        writeJson(socket, 200, QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("count"), windows.size()},
            {QStringLiteral("windows"), windows}
        });
        return true;
    }

    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/screenshot")) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_lastScreenshotRequestMs > 0 && (now - m_lastScreenshotRequestMs) < kScreenshotMinIntervalMs) {
            ++m_screenshotRateLimitedCount;
            writeError(socket, 429, QStringLiteral("screenshot requests are rate-limited"));
            return true;
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
            return true;
        }
        writeResponse(socket, 200, pngBytes, "image/png");
        return true;
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
                return true;
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
            return true;
        }
        writeJson(socket, result.value(QStringLiteral("ok")).toBool() ? 200 : 500, result);
        return true;
    }

    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/menu")) {
        QJsonObject response;
        if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &response, []() {
                return menuSnapshot(activePopupMenu());
            })) {
            writeError(socket, 503, QStringLiteral("timed out waiting for menu"));
            return true;
        }
        writeJson(socket, response.value(QStringLiteral("ok")).toBool() ? 200 : 404, response);
        return true;
    }

    if (request.method == QStringLiteral("POST") && request.url.path() == QStringLiteral("/menu")) {
        QString error;
        const QJsonObject body = parseJsonObject(request.body, &error);
        if (!error.isEmpty()) {
            writeError(socket, 400, error);
            return true;
        }
        const QString text = body.value(QStringLiteral("text")).toString();
        if (text.isEmpty()) {
            writeError(socket, 400, QStringLiteral("missing text"));
            return true;
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
            return true;
        }

        writeJson(socket, response.value(QStringLiteral("ok")).toBool() ? 200 : 404, response);
        return true;
    }

    if (request.method == QStringLiteral("POST") && request.url.path() == QStringLiteral("/click-item")) {
        QString error;
        const QJsonObject body = parseJsonObject(request.body, &error);
        if (!error.isEmpty()) {
            writeError(socket, 400, error);
            return true;
        }
        const QString id = body.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            writeError(socket, 400, QStringLiteral("missing id"));
            return true;
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
            return true;
        }

        writeJson(socket, response.value(QStringLiteral("ok")).toBool() ? 200 : 404, response);
        return true;
    }

    return false;
}

bool ControlServerWorker::handleFallback(QTcpSocket* socket, const Request& request) {
    if (request.url.path().isEmpty()) {
        writeError(socket, 400, QStringLiteral("invalid request"));
        return true;
    }

    writeError(socket,
               request.method == QStringLiteral("GET") || request.method == QStringLiteral("POST") ? 404 : 405,
               request.method == QStringLiteral("GET") || request.method == QStringLiteral("POST")
                   ? QStringLiteral("not found")
                   : QStringLiteral("method not allowed"));
    return true;
}

} // namespace control_server
