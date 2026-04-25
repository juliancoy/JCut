#include "control_server.h"

#include "control_server_worker.h"

#include <QDebug>
#include <QMetaObject>
#include <QThread>

ControlServer::ControlServer(QWidget* window,
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
    , m_getThrottlesCallback(std::move(getThrottlesCallback))
    , m_setThrottlesCallback(std::move(setThrottlesCallback))
    , m_getPlaybackConfigCallback(std::move(getPlaybackConfigCallback))
    , m_setPlaybackConfigCallback(std::move(setPlaybackConfigCallback))
    , m_renderResultCallback(std::move(renderResultCallback)) {}

ControlServer::~ControlServer() {
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, [worker = m_worker]() {
            static_cast<control_server::ControlServerWorker*>(worker)->stopListening();
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
    auto* worker = new control_server::ControlServerWorker(m_window,
                                                           m_fastSnapshotCallback,
                                                           m_stateSnapshotCallback,
                                                           m_projectSnapshotCallback,
                                                           m_historySnapshotCallback,
                                                           m_profilingCallback,
                                                           m_resetProfilingCallback,
                                                           m_setPlayheadCallback,
                                                           m_getThrottlesCallback,
                                                           m_setThrottlesCallback,
                                                           m_getPlaybackConfigCallback,
                                                           m_setPlaybackConfigCallback,
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
