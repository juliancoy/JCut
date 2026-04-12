#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <functional>
#include <memory>

class QWidget;
class QTcpServer;
class QTcpSocket;
class QThread;

class ControlServer final : public QObject {
    Q_OBJECT
public:
    explicit ControlServer(
        QWidget* window,
        std::function<QJsonObject()> fastSnapshotCallback = {},
        std::function<QJsonObject()> stateSnapshotCallback = {},
        std::function<QJsonObject()> projectSnapshotCallback = {},
        std::function<QJsonObject()> historySnapshotCallback = {},
        std::function<QJsonObject()> profilingCallback = {},
        std::function<void()> resetProfilingCallback = {},
        std::function<void(int64_t)> setPlayheadCallback = {},
        QObject* parent = nullptr);
    ~ControlServer() override;

    bool start(quint16 port);

private slots:
    void onNewConnection();

private:
    struct ParsedRequest;

    QWidget* m_window = nullptr;
    std::function<QJsonObject()> m_fastSnapshotCallback;
    std::function<QJsonObject()> m_stateSnapshotCallback;
    std::function<QJsonObject()> m_projectSnapshotCallback;
    std::function<QJsonObject()> m_historySnapshotCallback;
    std::function<QJsonObject()> m_profilingCallback;
    std::function<void()> m_resetProfilingCallback;
    std::function<void(int64_t)> m_setPlayheadCallback;
    std::unique_ptr<QThread> m_thread;
    QObject* m_worker = nullptr;
};
