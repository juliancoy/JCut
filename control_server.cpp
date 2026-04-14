#include "control_server.h"
#include "build_info.h"
#include "debug_controls.h"

#include <QAbstractButton>
#include <QApplication>
#include <QBuffer>
#include <QCoreApplication>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QFile>
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
#include <QSemaphore>
#include <QSlider>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWidget>

#include <memory>
#include <optional>

namespace {

constexpr int kUiInvokeTimeoutMs = 500;
constexpr int kUiBackgroundInvokeTimeoutMs = 200;
constexpr qint64 kUiHeartbeatStaleMs = 1000;
constexpr qint64 kProfileCacheFreshMs = 250;
constexpr qint64 kBackgroundRefreshTickMs = 50;
constexpr qint64 kProfileRefreshIntervalMs = 100;
constexpr qint64 kProfileDemandWindowMs = 15000;
constexpr qint64 kStateRefreshIntervalMs = 100;
constexpr qint64 kProjectRefreshIntervalMs = 750;
constexpr qint64 kHistoryRefreshIntervalMs = 400;
constexpr qint64 kUiTreeRefreshIntervalMs = 250;
constexpr qint64 kScreenshotMinIntervalMs = 250;

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

template <typename Result, typename Fn>
bool invokeOnUiThread(QWidget* window, int timeoutMs, Result* out, Fn&& fn) {
    if (!window || !out) {
        return false;
    }

    struct SharedState {
        QSemaphore semaphore;
        Result result{};
        bool invoked = false;
    };

    auto state = std::make_shared<SharedState>();
    const bool scheduled = QMetaObject::invokeMethod(
        window,
        [state, fn = std::forward<Fn>(fn)]() mutable {
            state->result = fn();
            state->invoked = true;
            state->semaphore.release();
        },
        Qt::QueuedConnection);

    if (!scheduled) {
        return false;
    }
    if (!state->semaphore.tryAcquire(1, timeoutMs)) {
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
        const bool uiResponsive = uiThreadResponsive();
        const bool profileInDemand = (now - m_lastProfileDemandMs) <= kProfileDemandWindowMs;
        const qint64 profileIntervalMs = uiResponsive ? kProfileRefreshIntervalMs : 1000;
        const qint64 stateIntervalMs = uiResponsive ? kStateRefreshIntervalMs : 1000;
        const qint64 projectIntervalMs = uiResponsive ? kProjectRefreshIntervalMs : 2000;
        const qint64 historyIntervalMs = uiResponsive ? kHistoryRefreshIntervalMs : 1500;
        const qint64 uiTreeIntervalMs = uiResponsive ? kUiTreeRefreshIntervalMs : 2000;

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
                }
                m_refreshCursor = (index + 1) % 5;
                return;
            }
            if (index == 1 && (now - m_lastStateRefreshAttemptMs) >= stateIntervalMs) {
                m_lastStateRefreshAttemptMs = now;
                QString error;
                if (refreshStateCacheFromUi(kUiBackgroundInvokeTimeoutMs, &error)) {
                    m_lastStateRefreshError.clear();
                } else {
                    m_lastStateRefreshError = uiResponsive ? error : QStringLiteral("ui thread is unresponsive");
                }
                m_refreshCursor = (index + 1) % 5;
                return;
            }
            if (index == 2 && (now - m_lastProjectRefreshAttemptMs) >= projectIntervalMs) {
                m_lastProjectRefreshAttemptMs = now;
                QString error;
                if (refreshProjectCacheFromUi(kUiBackgroundInvokeTimeoutMs, &error)) {
                    m_lastProjectRefreshError.clear();
                } else {
                    m_lastProjectRefreshError = uiResponsive ? error : QStringLiteral("ui thread is unresponsive");
                }
                m_refreshCursor = (index + 1) % 5;
                return;
            }
            if (index == 3 && (now - m_lastHistoryRefreshAttemptMs) >= historyIntervalMs) {
                m_lastHistoryRefreshAttemptMs = now;
                QString error;
                if (refreshHistoryCacheFromUi(kUiBackgroundInvokeTimeoutMs, &error)) {
                    m_lastHistoryRefreshError.clear();
                } else {
                    m_lastHistoryRefreshError = uiResponsive ? error : QStringLiteral("ui thread is unresponsive");
                }
                m_refreshCursor = (index + 1) % 5;
                return;
            }
            if (index == 4 && (now - m_lastUiTreeRefreshAttemptMs) >= uiTreeIntervalMs) {
                m_lastUiTreeRefreshAttemptMs = now;
                QString error;
                if (refreshUiTreeCacheFromUi(kUiBackgroundInvokeTimeoutMs, &error)) {
                    m_lastUiTreeRefreshError.clear();
                } else {
                    m_lastUiTreeRefreshError = uiResponsive ? error : QStringLiteral("ui thread is unresponsive");
                }
                m_refreshCursor = (index + 1) % 5;
                return;
            }
        }
    }

private:
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

    bool uiThreadResponsive() const {
        const QJsonObject snapshot = fastSnapshot();
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
        <div class="endpoint"><strong>GET /diag/perf</strong> - Performance diagnostics</div>
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

        if (uiBoundRoute && !uiThreadResponsive()) {
            writeError(socket, 503, QStringLiteral("ui thread is unresponsive"));
            return;
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
                
                // Add endFrame for easier overlap detection
                const qint64 startFrame = clip.value(QStringLiteral("startFrame")).toInteger();
                const qint64 durationFrames = clip.value(QStringLiteral("durationFrames")).toInteger();
                clip[QStringLiteral("endFrame")] = startFrame + durationFrames;
                
                filtered.push_back(clip);
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
                        {QStringLiteral("clip"), clip}
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
                {QStringLiteral("debug"), editor::debugControlsSnapshot()}
            });
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
    QJsonObject m_lastProjectSnapshot;
    qint64 m_lastProjectSnapshotMs = 0;
    QString m_lastProjectRefreshError;
    qint64 m_projectSnapshotSuccessCount = 0;
    qint64 m_projectSnapshotTimeoutCount = 0;
    qint64 m_lastProjectRefreshAttemptMs = 0;
    QJsonObject m_lastHistorySnapshot;
    qint64 m_lastHistorySnapshotMs = 0;
    QString m_lastHistoryRefreshError;
    qint64 m_historySnapshotSuccessCount = 0;
    qint64 m_historySnapshotTimeoutCount = 0;
    qint64 m_lastHistoryRefreshAttemptMs = 0;
    QJsonObject m_lastUiTreeSnapshot;
    qint64 m_lastUiTreeSnapshotMs = 0;
    QString m_lastUiTreeRefreshError;
    qint64 m_uiTreeSnapshotSuccessCount = 0;
    qint64 m_uiTreeSnapshotTimeoutCount = 0;
    qint64 m_lastUiTreeRefreshAttemptMs = 0;
    int m_refreshCursor = 0;
    qint64 m_lastScreenshotRequestMs = 0;
    qint64 m_screenshotRateLimitedCount = 0;
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
