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
#include <QUrl>
#include <QUrlQuery>
#include <QWidget>

#include <memory>
#include <optional>

namespace {

constexpr int kUiInvokeTimeoutMs = 250;
constexpr qint64 kUiHeartbeatStaleMs = 1000;

QString reasonPhrase(int statusCode) {
    switch (statusCode) {
    case 200: return QStringLiteral("OK");
    case 400: return QStringLiteral("Bad Request");
    case 404: return QStringLiteral("Not Found");
    case 405: return QStringLiteral("Method Not Allowed");
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
                        std::function<void()> resetProfilingCallback)
        : m_window(window)
        , m_fastSnapshotCallback(std::move(fastSnapshotCallback))
        , m_stateSnapshotCallback(std::move(stateSnapshotCallback))
        , m_projectSnapshotCallback(std::move(projectSnapshotCallback))
        , m_historySnapshotCallback(std::move(historySnapshotCallback))
        , m_profilingCallback(std::move(profilingCallback))
        , m_resetProfilingCallback(std::move(resetProfilingCallback)) {}

    ~ControlServerWorker() override = default;

    bool startListening(quint16 port) {
        m_server = std::make_unique<QTcpServer>();
        connect(m_server.get(), &QTcpServer::newConnection, this, &ControlServerWorker::onNewConnection);
        if (!m_server->listen(QHostAddress::LocalHost, port)) {
            return false;
        }
        m_listenPort = m_server->serverPort();
        fprintf(stderr, "ControlServer listening on http://127.0.0.1: %u\n", static_cast<unsigned>(m_listenPort));
        fflush(stderr);
        return true;
    }

    void stopListening() {
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

    QJsonObject profileCacheMeta() const {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        return QJsonObject{
            {QStringLiteral("has_snapshot"), !m_lastProfileSnapshot.isEmpty()},
            {QStringLiteral("last_snapshot_ms"), m_lastProfileSnapshotMs},
            {QStringLiteral("last_snapshot_age_ms"),
             m_lastProfileSnapshotMs > 0 ? now - m_lastProfileSnapshotMs : -1},
            {QStringLiteral("success_count"), m_profileSuccessCount},
            {QStringLiteral("timeout_count"), m_profileTimeoutCount}};
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

    bool fetchStateSnapshot(int timeoutMs, QJsonObject* out, QString* errorOut) {
        if (!out) {
            return false;
        }
        if (!m_stateSnapshotCallback) {
            if (errorOut) {
                *errorOut = QStringLiteral("state snapshot callback unavailable");
            }
            return false;
        }
        if (!invokeOnUiThread(m_window, timeoutMs, out, [this]() { return m_stateSnapshotCallback(); })) {
            if (errorOut) {
                *errorOut = QStringLiteral("timed out waiting for state snapshot");
            }
            return false;
        }
        return true;
    }

    bool fetchProjectSnapshot(int timeoutMs, QJsonObject* out, QString* errorOut) {
        if (!out) {
            return false;
        }
        if (!m_projectSnapshotCallback) {
            if (errorOut) {
                *errorOut = QStringLiteral("project snapshot callback unavailable");
            }
            return false;
        }
        if (!invokeOnUiThread(m_window, timeoutMs, out, [this]() { return m_projectSnapshotCallback(); })) {
            if (errorOut) {
                *errorOut = QStringLiteral("timed out waiting for project snapshot");
            }
            return false;
        }
        return true;
    }

    bool fetchHistorySnapshot(int timeoutMs, QJsonObject* out, QString* errorOut) {
        if (!out) {
            return false;
        }
        if (!m_historySnapshotCallback) {
            if (errorOut) {
                *errorOut = QStringLiteral("history snapshot callback unavailable");
            }
            return false;
        }
        if (!invokeOnUiThread(m_window, timeoutMs, out, [this]() { return m_historySnapshotCallback(); })) {
            if (errorOut) {
                *errorOut = QStringLiteral("timed out waiting for history snapshot");
            }
            return false;
        }
        return true;
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

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/state")) {
            QJsonObject state;
            QString error;
            if (!fetchStateSnapshot(kUiInvokeTimeoutMs, &state, &error)) {
                writeError(socket, 503, error);
                return;
            }
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("state"), state}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/timeline")) {
            QJsonObject state;
            QString error;
            if (!fetchStateSnapshot(kUiInvokeTimeoutMs, &state, &error)) {
                writeError(socket, 503, error);
                return;
            }
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
            QJsonObject state;
            QString error;
            if (!fetchStateSnapshot(kUiInvokeTimeoutMs, &state, &error)) {
                writeError(socket, 503, error);
                return;
            }
            const QJsonArray tracks = state.value(QStringLiteral("tracks")).toArray();
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("count"), tracks.size()},
                {QStringLiteral("tracks"), tracks}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/clips")) {
            QJsonObject state;
            QString error;
            if (!fetchStateSnapshot(kUiInvokeTimeoutMs, &state, &error)) {
                writeError(socket, 503, error);
                return;
            }

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
                const QJsonObject clip = value.toObject();
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
            QJsonObject state;
            QString error;
            if (!fetchStateSnapshot(kUiInvokeTimeoutMs, &state, &error)) {
                writeError(socket, 503, error);
                return;
            }
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
            QJsonObject state;
            QString error;
            if (!fetchStateSnapshot(kUiInvokeTimeoutMs, &state, &error)) {
                writeError(socket, 503, error);
                return;
            }
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
            QJsonObject project;
            QString error;
            if (!fetchProjectSnapshot(kUiInvokeTimeoutMs, &project, &error)) {
                writeError(socket, 503, error);
                return;
            }
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("project"), project}
            });
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/history")) {
            QJsonObject history;
            QString error;
            if (!fetchHistorySnapshot(kUiInvokeTimeoutMs, &history, &error)) {
                writeError(socket, 503, error);
                return;
            }
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("history"), history}
            });
            return;
        }

        if (!uiThreadResponsive()) {
            writeError(socket, 503, QStringLiteral("ui thread is unresponsive"));
            return;
        }

        if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/profile")) {
            QString uiError;
            const bool live = refreshProfileCacheFromUi(kUiInvokeTimeoutMs, &uiError);
            if (!live && m_lastProfileSnapshot.isEmpty()) {
                writeError(socket, 503, uiError);
                return;
            }

            const QJsonObject snapshot = fastSnapshot();
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("live"), live},
                {QStringLiteral("ui_thread_responsive"),
                 snapshot.value(QStringLiteral("main_thread_heartbeat_age_ms")).toInteger(-1) <=
                     kUiHeartbeatStaleMs},
                {QStringLiteral("ui_error"), live ? QString() : uiError},
                {QStringLiteral("profile"), m_lastProfileSnapshot},
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
                    if (!editor::parseDebugLogLevel(it.value().toString(), &level) ||
                        !editor::setDebugControlLevel(it.key(), level)) {
                        writeError(socket, 400, QStringLiteral("invalid debug field: %1").arg(it.key()));
                        return;
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
            QJsonObject tree;
            if (!invokeOnUiThread(m_window, kUiInvokeTimeoutMs, &tree, [this]() {
                    return widgetSnapshot(m_window);
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for ui hierarchy"));
                return;
            }
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("ui"), tree},
                {QStringLiteral("window"), tree}
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
    std::unique_ptr<QTcpServer> m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    quint16 m_listenPort = 0;
    qint64 m_startTimeMs = QDateTime::currentMSecsSinceEpoch();
    QJsonObject m_lastProfileSnapshot;
    qint64 m_lastProfileSnapshotMs = 0;
    qint64 m_profileSuccessCount = 0;
    qint64 m_profileTimeoutCount = 0;
};

} // namespace

ControlServer::ControlServer(QWidget* window,
                             std::function<QJsonObject()> fastSnapshotCallback,
                             std::function<QJsonObject()> stateSnapshotCallback,
                             std::function<QJsonObject()> projectSnapshotCallback,
                             std::function<QJsonObject()> historySnapshotCallback,
                             std::function<QJsonObject()> profilingCallback,
                             std::function<void()> resetProfilingCallback,
                             QObject* parent)
    : QObject(parent)
    , m_window(window)
    , m_fastSnapshotCallback(std::move(fastSnapshotCallback))
    , m_stateSnapshotCallback(std::move(stateSnapshotCallback))
    , m_projectSnapshotCallback(std::move(projectSnapshotCallback))
    , m_historySnapshotCallback(std::move(historySnapshotCallback))
    , m_profilingCallback(std::move(profilingCallback))
    , m_resetProfilingCallback(std::move(resetProfilingCallback)) {}

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
                                           m_resetProfilingCallback);
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
