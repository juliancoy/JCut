#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QWidget>

#include <functional>
#include <cstdint>
#include <memory>

class ControlServer : public QObject {
    Q_OBJECT
public:
    explicit ControlServer(QWidget* window,
                           std::function<QJsonObject()> fastSnapshotCallback,
                           std::function<QJsonObject()> stateSnapshotCallback,
                           std::function<QJsonObject()> projectSnapshotCallback,
                           std::function<QJsonObject()> historySnapshotCallback,
                           std::function<QJsonObject()> profilingCallback,
                           std::function<void()> resetProfilingCallback,
                           std::function<void(int64_t)> setPlayheadCallback,
                           std::function<QJsonObject()> renderResultCallback,
                           QObject* parent = nullptr);
    ~ControlServer() override;

    bool start(quint16 port);

private slots:
    void onNewConnection();

private:
    QPointer<QWidget> m_window;
    std::function<QJsonObject()> m_fastSnapshotCallback;
    std::function<QJsonObject()> m_stateSnapshotCallback;
    std::function<QJsonObject()> m_projectSnapshotCallback;
    std::function<QJsonObject()> m_historySnapshotCallback;
    std::function<QJsonObject()> m_profilingCallback;
    std::function<void()> m_resetProfilingCallback;
    std::function<void(int64_t)> m_setPlayheadCallback;
    std::function<QJsonObject()> m_renderResultCallback;
    QObject* m_worker = nullptr;
    std::unique_ptr<QThread> m_thread;
};
