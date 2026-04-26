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

#include <limits>

namespace control_server {

ControlServerWorker::ControlServerWorker(QWidget* window,
                                         std::function<QJsonObject()> fastSnapshotCallback,
                                         std::function<QJsonObject()> stateSnapshotCallback,
                                         std::function<QJsonObject()> projectSnapshotCallback,
                                         std::function<QJsonObject()> historySnapshotCallback,
                                         std::function<QJsonObject()> profilingCallback,
                                         std::function<void()> resetProfilingCallback,
                                         std::function<void(int64_t)> setPlayheadCallback,
                                         std::function<QJsonObject()> getThrottlesCallback,
                                         std::function<QJsonObject(const QJsonObject&)> setThrottlesCallback,
                                         std::function<QJsonObject()> getPlaybackConfigCallback,
                                         std::function<QJsonObject(const QJsonObject&)> setPlaybackConfigCallback,
                                         std::function<QJsonObject()> renderResultCallback)
    : m_window(window)
    , m_fastSnapshotCallback(std::move(fastSnapshotCallback))
    , m_stateSnapshotCallback(std::move(stateSnapshotCallback))
    , m_projectSnapshotCallback(std::move(projectSnapshotCallback))
    , m_historySnapshotCallback(std::move(historySnapshotCallback))
    , m_profilingCallback(std::move(profilingCallback))
    , m_resetProfilingCallback(std::move(resetProfilingCallback))
    , m_setPlayheadCallback(std::move(setPlayheadCallback))
    , m_getThrottlesCallback(std::move(getThrottlesCallback))
    , m_setThrottlesCallback(std::move(setThrottlesCallback))
    , m_getPlaybackConfigCallback(std::move(getPlaybackConfigCallback))
    , m_setPlaybackConfigCallback(std::move(setPlaybackConfigCallback))
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
    m_refreshTimer->setInterval(static_cast<int>(m_backgroundRefreshTickMs));
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
        (now - m_lastUiRefreshTimeoutMs) < m_uiRefreshCooldownAfterTimeoutMs) {
        return;
    }

    const bool profileInDemand = (now - m_lastProfileDemandMs) <= m_profileDemandWindowMs;
    const bool stateInDemand =
        m_lastStateSnapshot.isEmpty() || (now - m_lastStateDemandMs) <= m_snapshotDemandWindowMs;
    const bool projectInDemand =
        m_lastProjectSnapshot.isEmpty() || (now - m_lastProjectDemandMs) <= m_snapshotDemandWindowMs;
    const bool historyInDemand =
        m_lastHistorySnapshot.isEmpty() || (now - m_lastHistoryDemandMs) <= m_snapshotDemandWindowMs;
    const bool uiTreeInDemand =
        m_lastUiTreeSnapshot.isEmpty() || (now - m_lastUiTreeDemandMs) <= m_snapshotDemandWindowMs;
    const qint64 profileIntervalMs = uiResponsive ? m_profileRefreshIntervalMs : 1000;
    const qint64 stateIntervalMs = uiResponsive
        ? (stateInDemand ? m_stateRefreshIntervalMs : m_idleStateRefreshIntervalMs)
        : 1000;
    const qint64 projectIntervalMs = uiResponsive
        ? (projectInDemand ? m_projectRefreshIntervalMs : m_idleProjectRefreshIntervalMs)
        : 2000;
    const qint64 historyIntervalMs = uiResponsive
        ? (historyInDemand ? m_historyRefreshIntervalMs : m_idleHistoryRefreshIntervalMs)
        : 1500;
    const qint64 uiTreeIntervalMs = uiResponsive
        ? (uiTreeInDemand ? m_uiTreeRefreshIntervalMs : m_idleUiTreeRefreshIntervalMs)
        : 2000;

    for (int step = 0; step < 5; ++step) {
        const int index = (m_refreshCursor + step) % 5;
        if (index == 0 && profileInDemand && (now - m_lastProfileRefreshAttemptMs) >= profileIntervalMs) {
            m_lastProfileRefreshAttemptMs = now;
            QString error;
            if (refreshProfileCacheFromUi(m_uiBackgroundInvokeTimeoutMs, &error)) {
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
            if (refreshStateCacheFromUi(m_uiBackgroundInvokeTimeoutMs, &error)) {
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
            if (refreshProjectCacheFromUi(m_uiBackgroundInvokeTimeoutMs, &error)) {
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
            if (refreshHistoryCacheFromUi(m_uiBackgroundInvokeTimeoutMs, &error)) {
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
            if (refreshUiTreeCacheFromUi(m_uiBackgroundInvokeTimeoutMs, &error)) {
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
        if (!refreshStateCacheFromUi(m_uiInvokeTimeoutMs, &error)) {
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
        if (!refreshStateCacheFromUi(m_uiInvokeTimeoutMs, &error)) {
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
    while (m_freezeEvents.size() > m_freezeEventCap) {
        m_freezeEvents.removeAt(0);
    }
}

void ControlServerWorker::recordFrameTraceSample(const QJsonObject& snapshot, qint64 nowMs) {
    const qint64 heartbeatAgeMs =
        snapshot.value(QStringLiteral("main_thread_heartbeat_age_ms")).toInteger(-1);
    const qint64 playheadAdvanceAgeMs =
        snapshot.value(QStringLiteral("last_playhead_advance_age_ms")).toInteger(-1);
    const bool uiResponsive = heartbeatAgeMs >= 0 && heartbeatAgeMs <= m_uiHeartbeatStaleMs;
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
    while (m_frameTraceSamples.size() > m_frameTraceSampleCap) {
        m_frameTraceSamples.removeAt(0);
    }

    if (heartbeatAgeMs >= m_stallDetectHeartbeatMs) {
        ++m_stallConsecutiveOverThreshold;
        if (!m_stallActive && m_stallConsecutiveOverThreshold >= m_stallConsecutiveThreshold) {
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
    limit = qMin(limit, static_cast<int>(m_frameTraceSampleCap));
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
        {QStringLiteral("stall_threshold_ms"), m_stallDetectHeartbeatMs},
        {QStringLiteral("stall_threshold_consecutive"), m_stallConsecutiveThreshold},
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


} // namespace control_server
