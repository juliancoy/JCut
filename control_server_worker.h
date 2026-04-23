#pragma once

#include "control_server_http_utils.h"

#include <QDateTime>
#include <QFutureWatcher>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QSemaphore>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace control_server {

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
                        std::function<QJsonObject()> renderResultCallback);
    ~ControlServerWorker() override;

    bool startListening(quint16 port);
    void stopListening();

private slots:
    void onNewConnection();
    void refreshBackgroundCaches();
    void onDecodeRatesJobFinished();
    void onDecodeSeeksJobFinished();

private:
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

    void markStateDemand();
    void markProjectDemand();
    void markHistoryDemand();
    void markUiTreeDemand();

    static QJsonObject decodeSnapshotMeta(const QJsonObject& stateSnapshot);
    QJsonObject decodeJobMeta(const DecodeJobState& job, const QString& name) const;
    QJsonObject decodeJobResponse(const DecodeJobState& job, const QString& name) const;
    bool startDecodeRatesJob(QString* errorOut);
    bool startDecodeSeeksJob(QString* errorOut);
    void appendFreezeEvent(QJsonObject event);
    void recordFrameTraceSample(const QJsonObject& snapshot, qint64 nowMs);
    QJsonObject frameTraceSnapshot(const QUrlQuery& query) const;

    bool refreshProfileCacheFromUi(int timeoutMs, QString* errorOut);
    bool refreshStateCacheFromUi(int timeoutMs, QString* errorOut);
    bool refreshProjectCacheFromUi(int timeoutMs, QString* errorOut);
    bool refreshHistoryCacheFromUi(int timeoutMs, QString* errorOut);
    bool refreshUiTreeCacheFromUi(int timeoutMs, QString* errorOut);

    QJsonObject profileCacheMeta() const;
    QJsonObject stateCacheMeta() const;
    QJsonObject projectCacheMeta() const;
    QJsonObject historyCacheMeta() const;
    QJsonObject uiTreeCacheMeta() const;

    void onReadyRead(QTcpSocket* socket);
    QJsonObject fastSnapshot() const;
    static bool uiThreadResponsive(const QJsonObject& snapshot);
    void writeResponse(QTcpSocket* socket, int statusCode, const QByteArray& body, const QByteArray& contentType);
    void writeJson(QTcpSocket* socket, int statusCode, const QJsonObject& object);
    void writeError(QTcpSocket* socket, int statusCode, const QString& error);
    void handleRequest(QTcpSocket* socket, const Request& request);

    bool handleRoot(QTcpSocket* socket, const Request& request);
    bool handleHealth(QTcpSocket* socket, const Request& request);
    bool handleVersion(QTcpSocket* socket, const Request& request);
    bool handleUiBoundRouteGuard(QTcpSocket* socket, const Request& request);
    void noteDemand(const Request& request);

    bool handlePlayhead(QTcpSocket* socket, const Request& request);
    bool handleStateRoutes(QTcpSocket* socket, const Request& request);
    bool handleProjectRoutes(QTcpSocket* socket, const Request& request);
    bool handleDecodeRoutes(QTcpSocket* socket, const Request& request);
    bool handleHistoryRoutes(QTcpSocket* socket, const Request& request);
    bool handleProfileRoutes(QTcpSocket* socket, const Request& request);
    bool handleDebugRoutes(QTcpSocket* socket, const Request& request);
    bool handleRenderRoutes(QTcpSocket* socket, const Request& request);
    bool handleHardwareRoutes(QTcpSocket* socket, const Request& request);
    bool handleUiRoutes(QTcpSocket* socket, const Request& request);
    bool handleFallback(QTcpSocket* socket, const Request& request);

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

} // namespace control_server
