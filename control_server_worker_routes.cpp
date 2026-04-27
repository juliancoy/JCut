#include "control_server_worker.h"

#include "build_info.h"
#include "clip_serialization.h"
#include "control_server_media_diag.h"
#include "control_server_ui_utils.h"
#include "debug_controls.h"
#include "editor.h"
#include "editor_shared.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSpinBox>
#include <QTableWidget>
#include <QTcpSocket>
#include <QUrlQuery>

#include <algorithm>
#include <limits>

namespace control_server {
namespace {

void mergePlaybackConfigIntoState(const QJsonObject& playbackConfig, QJsonObject* state) {
    if (!state) {
        return;
    }
    if (!playbackConfig.value(QStringLiteral("ok")).toBool(true)) {
        return;
    }

    if (playbackConfig.contains(QStringLiteral("playback_speed"))) {
        (*state)[QStringLiteral("playbackSpeed")] =
            playbackConfig.value(QStringLiteral("playback_speed")).toDouble(
                state->value(QStringLiteral("playbackSpeed")).toDouble(1.0));
    }
    if (playbackConfig.contains(QStringLiteral("clock_source"))) {
        (*state)[QStringLiteral("playbackClockSource")] =
            playbackConfig.value(QStringLiteral("clock_source")).toString(
                state->value(QStringLiteral("playbackClockSource")).toString(QStringLiteral("auto")));
    }
    if (playbackConfig.contains(QStringLiteral("audio_warp_mode"))) {
        (*state)[QStringLiteral("playbackAudioWarpMode")] =
            playbackConfig.value(QStringLiteral("audio_warp_mode")).toString(
                state->value(QStringLiteral("playbackAudioWarpMode"))
                    .toString(QStringLiteral("disabled")));
    }
}

QString widgetTextForSelector(QWidget* widget) {
    if (!widget) {
        return QString();
    }
    if (const auto* button = qobject_cast<QAbstractButton*>(widget)) {
        return button->text();
    }
    if (const auto* label = qobject_cast<QLabel*>(widget)) {
        return label->text();
    }
    if (const auto* lineEdit = qobject_cast<QLineEdit*>(widget)) {
        return lineEdit->text();
    }
    if (const auto* plainText = qobject_cast<QPlainTextEdit*>(widget)) {
        return plainText->toPlainText();
    }
    if (const auto* combo = qobject_cast<QComboBox*>(widget)) {
        return combo->currentText();
    }
    return QString();
}

QWidget* resolveWidgetTarget(QWidget* root, const QJsonObject& target) {
    if (!root) {
        return nullptr;
    }

    QString id = target.value(QStringLiteral("id")).toString().trimmed();
    QString path = target.value(QStringLiteral("path")).toString().trimmed();
    QJsonObject selector = target.value(QStringLiteral("selector")).toObject();
    if (!selector.isEmpty()) {
        if (id.isEmpty()) {
            id = selector.value(QStringLiteral("id")).toString().trimmed();
        }
        if (path.isEmpty()) {
            path = selector.value(QStringLiteral("path")).toString().trimmed();
        }
    }

    if (!id.isEmpty()) {
        return findWidgetByObjectName(root, id);
    }
    if (!path.isEmpty()) {
        return findWidgetByHierarchyPath(root, path);
    }

    const QString className = selector.value(QStringLiteral("class")).toString().trimmed();
    const QString text = selector.value(QStringLiteral("text")).toString().trimmed();
    const QString headersContains =
        selector.value(QStringLiteral("headersContains")).toString().trimmed();
    const bool visibleOnly = selector.value(QStringLiteral("visibleOnly")).toBool(false);
    const int index = qMax(0, selector.value(QStringLiteral("index")).toInt(0));

    QWidget* searchRoot = root;
    const QString withinPath = selector.value(QStringLiteral("withinPath")).toString().trimmed();
    if (!withinPath.isEmpty()) {
        QWidget* scopedRoot = findWidgetByHierarchyPath(root, withinPath);
        if (scopedRoot) {
            searchRoot = scopedRoot;
        }
    }

    QList<QWidget*> matches;
    const auto widgets = searchRoot->findChildren<QWidget*>(QString(), Qt::FindChildrenRecursively);
    for (QWidget* widget : widgets) {
        if (!widget) {
            continue;
        }
        if (!className.isEmpty() &&
            QString::fromLatin1(widget->metaObject()->className()) != className) {
            continue;
        }
        if (visibleOnly && !widget->isVisible()) {
            continue;
        }
        if (!text.isEmpty()) {
            const QString widgetText = widgetTextForSelector(widget);
            if (!widgetText.contains(text, Qt::CaseInsensitive)) {
                continue;
            }
        }
        if (!headersContains.isEmpty()) {
            const auto* table = qobject_cast<QTableWidget*>(widget);
            if (!table) {
                continue;
            }
            bool found = false;
            for (int column = 0; column < table->columnCount(); ++column) {
                const QTableWidgetItem* headerItem = table->horizontalHeaderItem(column);
                const QString headerText = headerItem ? headerItem->text() : QString();
                if (headerText.contains(headersContains, Qt::CaseInsensitive)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                continue;
            }
        }
        matches.push_back(widget);
    }

    if (index >= 0 && index < matches.size()) {
        return matches.at(index);
    }
    return nullptr;
}

int resolveTableColumn(const QTableWidget* table, const QJsonObject& rowMatch) {
    if (!table) {
        return -1;
    }
    if (rowMatch.contains(QStringLiteral("column"))) {
        const int column = rowMatch.value(QStringLiteral("column")).toInt(-1);
        if (column >= 0 && column < table->columnCount()) {
            return column;
        }
    }
    const QString header = rowMatch.value(QStringLiteral("header")).toString().trimmed();
    if (!header.isEmpty()) {
        for (int column = 0; column < table->columnCount(); ++column) {
            const QTableWidgetItem* headerItem = table->horizontalHeaderItem(column);
            const QString headerText = headerItem ? headerItem->text() : QString();
            if (headerText.compare(header, Qt::CaseInsensitive) == 0) {
                return column;
            }
        }
    }
    return -1;
}

bool tableRowMatches(const QTableWidget* table, int row, int column, const QJsonObject& rowMatch) {
    if (!table || row < 0 || row >= table->rowCount() || column < 0 || column >= table->columnCount()) {
        return false;
    }
    const QTableWidgetItem* item = table->item(row, column);
    const QString cellText = item ? item->text() : QString();
    const QString text = rowMatch.value(QStringLiteral("text")).toString();
    if (text.isEmpty()) {
        return false;
    }
    const bool caseSensitive = rowMatch.value(QStringLiteral("caseSensitive")).toBool(false);
    const Qt::CaseSensitivity cs = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const bool contains = rowMatch.value(QStringLiteral("contains")).toBool(true);
    if (contains) {
        return cellText.contains(text, cs);
    }
    return QString::compare(cellText, text, cs) == 0;
}

QString normalizedActionText(const QString& text) {
    QString normalized = text;
    normalized.remove('&');
    return normalized.trimmed();
}

} // namespace

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

bool ControlServerWorker::uiThreadResponsive(const QJsonObject& snapshot) const {
    return snapshot.value(QStringLiteral("main_thread_heartbeat_age_ms")).toInteger(-1) <= m_uiHeartbeatStaleMs;
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
    if (request.method == QStringLiteral("GET") &&
        (path == QStringLiteral("/ui") || path == QStringLiteral("/ui/"))) {
        markUiTreeDemand();
    }
}

bool ControlServerWorker::handleUiBoundRouteGuard(QTcpSocket* socket, const Request& request) {
    const QString path = request.url.path();
    const bool uiBoundRoute =
        (request.method == QStringLiteral("POST") && path == QStringLiteral("/playhead")) ||
        (request.method == QStringLiteral("POST") && path == QStringLiteral("/playback")) ||
        (request.method == QStringLiteral("POST") &&
         (path == QStringLiteral("/ui") ||
          path == QStringLiteral("/ui/") ||
          path == QStringLiteral("/ui/table/context-action"))) ||
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
    if (handleParadigms(socket, request)) return;
    if (handleHealth(socket, request)) return;
    if (handleVersion(socket, request)) return;
    if (handleUiBoundRouteGuard(socket, request)) return;
    if (handlePlayhead(socket, request)) return;
    if (handleStateRoutes(socket, request)) return;
    if (handleProjectRoutes(socket, request)) return;
    if (handleDecodeRoutes(socket, request)) return;
    if (handleHistoryRoutes(socket, request)) return;
    if (handleProfileRoutes(socket, request)) return;
    if (handleThrottleRoutes(socket, request)) return;
    if (handlePlaybackRoutes(socket, request)) return;
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
        <div class="endpoint"><strong>GET /profile/startup</strong> - Startup timing breakdown</div>
        <div class="endpoint"><strong>GET /decode/rates</strong> - Decode benchmark for current project media</div>
        <div class="endpoint"><strong>GET /decode/seeks</strong> - Seek benchmark for current project media</div>
        <div class="endpoint"><strong>GET /diag/perf</strong> - Performance diagnostics</div>
        <div class="endpoint"><strong>GET /diag/frame-trace</strong> - Recent frame/heartbeat trace and freeze events</div>
        <div class="endpoint"><strong>GET /timeline</strong> - Timeline information</div>
        <div class="endpoint"><strong>GET /project</strong> - Project information</div>
        <div class="endpoint"><strong>GET /history</strong> - History snapshot</div>
        <div class="endpoint"><strong>GET /ui</strong> - UI hierarchy</div>
        <div class="endpoint"><strong>GET /throttles</strong> - Current throttle configuration</div>
        <div class="endpoint"><strong>GET /playback</strong> - Current playback policy configuration</div>
        <div class="endpoint"><strong>GET /paradigms</strong> - Architecture organizational paradigms and file positioning</div>

        <h2>Controls:</h2>
        <div class="endpoint"><strong>POST /playhead</strong> - Set playhead position</div>
        <div class="endpoint"><strong>POST /ui</strong> - Mutate UI widgets/tables by id or selector</div>
        <div class="endpoint"><strong>POST /profile/reset</strong> - Reset profiling stats</div>
        <div class="endpoint"><strong>POST /throttles</strong> - Patch throttle configuration</div>
        <div class="endpoint"><strong>POST /playback</strong> - Patch playback policy configuration</div>

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

bool ControlServerWorker::handleParadigms(QTcpSocket* socket, const Request& request) {
    if (request.method != QStringLiteral("GET") || request.url.path() != QStringLiteral("/paradigms")) {
        return false;
    }

    static const QByteArray paradigmsHtml = R"(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8" />
    <title>JCut Organizational Paradigms</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 32px; background: #f6f8fa; color: #111827; }
        .container { max-width: 1280px; margin: 0 auto; background: #ffffff; padding: 24px; border-radius: 10px; box-shadow: 0 2px 14px rgba(0,0,0,0.08); }
        h1 { margin-top: 0; }
        .meta { color: #4b5563; margin-bottom: 16px; }
        .chip { display: inline-block; margin: 2px 8px 2px 0; padding: 2px 8px; border-radius: 999px; background: #f3f4f6; }
        table { width: 100%; border-collapse: collapse; margin-top: 16px; font-size: 14px; }
        th, td { border: 1px solid #e5e7eb; padding: 8px 10px; text-align: left; vertical-align: top; }
        th { background: #f9fafb; }
        code { background: #f3f4f6; padding: 1px 4px; border-radius: 4px; }
        .footer { margin-top: 16px; font-size: 13px; color: #4b5563; }
    </style>
</head>
<body>
    <div class="container">
        <h1>Organizational Paradigms</h1>
        <div class="meta">Source of truth: <code>ARCHITECTURE.md</code> / “Organizational Paradigms” + “File Positioning By Paradigm”.</div>

        <div>
            <span class="chip">🟦 Facade/core owner</span>
            <span class="chip">🟩 Satellite/extracted feature owner</span>
            <span class="chip">🟪 Shared helper module/hub</span>
            <span class="chip">🟧 Orchestration/lifecycle responsibility</span>
            <span class="chip">🟥 Runtime engine responsibility</span>
            <span class="chip">🟫 Storage/data-structure responsibility</span>
            <span class="chip">🟨 UI/interaction responsibility</span>
        </div>

        <table>
            <thead>
                <tr>
                    <th>File</th>
                    <th>Vertical Slice</th>
                    <th>Facade/Satellite</th>
                    <th>Primary Role</th>
                    <th>Line-Cap Role</th>
                </tr>
            </thead>
            <tbody>
                <tr><td><code>control_server_worker.cpp</code></td><td>🟦 Core worker</td><td>🟦 Facade</td><td>🟧 Lifecycle/cache/decode state</td><td>🟩 Below cap</td></tr>
                <tr><td><code>control_server_worker_routes.cpp</code></td><td>🟩 HTTP routes</td><td>🟩 Satellite</td><td>🟨 Endpoint handlers</td><td>🟩 Extracted to cap</td></tr>
                <tr><td><code>editor.cpp</code></td><td>🟦 Editor orchestration</td><td>🟦 Facade</td><td>🟧 Startup/wiring</td><td>🟩 Below cap</td></tr>
                <tr><td><code>editor_playback.cpp</code></td><td>🟩 Playback runtime</td><td>🟩 Satellite</td><td>🟥 Playback transport/clocking</td><td>🟩 Extracted to cap</td></tr>
                <tr><td><code>inspector_pane.cpp</code></td><td>🟦 Inspector shell</td><td>🟦 Facade</td><td>🟧 Tab composition</td><td>🟩 Below cap</td></tr>
                <tr><td><code>inspector_pane_secondary_tabs.cpp</code></td><td>🟩 Secondary tabs</td><td>🟩 Satellite</td><td>🟨 Tab builders</td><td>🟩 Extracted to cap</td></tr>
                <tr><td><code>timeline_widget_input.cpp</code></td><td>🟦 Timeline input core</td><td>🟦 Facade</td><td>🟨 Input/drag/wheel</td><td>🟩 Below cap</td></tr>
                <tr><td><code>timeline_widget_context_menu.cpp</code></td><td>🟩 Timeline menu</td><td>🟩 Satellite</td><td>🟨 Context commands</td><td>🟩 Extracted to cap</td></tr>
                <tr><td><code>timeline_cache.cpp</code></td><td>🟦 Cache orchestrator</td><td>🟦 Facade</td><td>🟧 Prefetch/scheduling</td><td>🟩 Below cap</td></tr>
                <tr><td><code>timeline_cache_storage.cpp</code></td><td>🟩 Cache storage</td><td>🟩 Satellite</td><td>🟫 Buffer/cache storage+eviction</td><td>🟩 Extracted to cap</td></tr>
                <tr><td><code>transcript_tab.cpp</code></td><td>🟦 Transcript interaction core</td><td>🟦 Facade</td><td>🟨 Table/edit interactions</td><td>🟩 Below cap</td></tr>
                <tr><td><code>transcript_tab_document.cpp</code></td><td>🟩 Transcript document</td><td>🟩 Satellite</td><td>🟫 Load/parse/version/persist</td><td>🟩 Extracted to cap</td></tr>
                <tr><td><code>speakers_tab.cpp</code></td><td>🟦 Speakers core</td><td>🟦 Facade</td><td>🟧 Wiring/refresh/model summary</td><td>🟩 Below cap</td></tr>
                <tr><td><code>speakers_tab_interactions.cpp</code></td><td>🟩 Speakers interactions</td><td>🟩 Satellite</td><td>🟨 Selection/reference/context actions</td><td>🟩 Extracted to cap</td></tr>
                <tr><td><code>speakers_tab_boxstream_engines.cpp</code></td><td>🟩 BoxStream engines</td><td>🟩 Satellite</td><td>🟥 Native/docker engine execution</td><td>🟩 Extracted to cap</td></tr>
                <tr><td><code>speakers_tab_boxstream_actions.cpp</code></td><td>🟩 BoxStream actions</td><td>🟩 Satellite</td><td>🟧 Workflow orchestration + preview framing</td><td>🟩 Extracted to cap</td></tr>
                <tr><td><code>speakers_tab_internal.h</code></td><td>🟪 Shared helper slice</td><td>🟪 Helper module</td><td>🟪 Internal constants/utilities</td><td>🟩 Prevents duplication</td></tr>
            </tbody>
        </table>

        <div class="footer">
            <a href="/">Back to dashboard</a>
        </div>
    </div>
</body>
</html>
)";

    writeResponse(socket, 200, paradigmsHtml, "text/html; charset=utf-8");
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
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &success, [this, frame]() {
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

        QJsonObject state = m_lastStateSnapshot;
        QJsonObject playbackConfig;
        if (invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &playbackConfig, [this]() {
                return m_getPlaybackConfigCallback ? m_getPlaybackConfigCallback() : QJsonObject{};
            })) {
            mergePlaybackConfigIntoState(playbackConfig, &state);
            m_lastStateSnapshot = state;
            m_lastStateSnapshotMs = QDateTime::currentMSecsSinceEpoch();
        }

        ++m_stateSnapshotServedCachedCount;
        writeJson(socket, 200, QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("state"), state}
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
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &success, [this, filePath, startFrame]() {
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
        const bool live = ageMs >= 0 && ageMs <= m_profileCacheFreshMs;
        const QJsonObject snapshot = fastSnapshot();
        writeJson(socket, 200, QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("live"), live},
            {QStringLiteral("ui_thread_responsive"),
             snapshot.value(QStringLiteral("main_thread_heartbeat_age_ms")).toInteger(-1) <=
                 m_uiHeartbeatStaleMs},
            {QStringLiteral("ui_error"), m_lastProfileRefreshError},
            {QStringLiteral("profile"), m_lastProfileSnapshot},
            {QStringLiteral("served_cached"), true},
            {QStringLiteral("cache"), profileCacheMeta()}
        });
        return true;
    }

    if (request.method == QStringLiteral("GET") &&
        request.url.path() == QStringLiteral("/profile/startup")) {
        m_lastProfileDemandMs = QDateTime::currentMSecsSinceEpoch();
        if (m_lastProfileSnapshot.isEmpty()) {
            const QString error = m_lastProfileRefreshError.isEmpty()
                ? QStringLiteral("profile snapshot unavailable; cache warming")
                : m_lastProfileRefreshError;
            writeError(socket, 503, error);
            return true;
        }
        ++m_profileServedCachedCount;
        const QJsonObject startup = m_lastProfileSnapshot.value(QStringLiteral("startup")).toObject();
        const QUrlQuery query(request.url);
        const QString flatParam = query.queryItemValue(QStringLiteral("flat")).trimmed().toLower();
        const bool flat = (flatParam == QStringLiteral("1") ||
                           flatParam == QStringLiteral("true") ||
                           flatParam == QStringLiteral("yes"));
        if (flat) {
            struct FlatRow {
                QString phase;
                qint64 deltaMs = 0;
                qint64 cumulativeMs = 0;
            };
            QVector<FlatRow> rows;
            const QJsonArray marks = startup.value(QStringLiteral("marks")).toArray();
            rows.reserve(marks.size());
            for (const QJsonValue& markValue : marks) {
                const QJsonObject mark = markValue.toObject();
                FlatRow row;
                row.phase = mark.value(QStringLiteral("phase")).toString();
                row.deltaMs = mark.value(QStringLiteral("delta_ms")).toInteger(0);
                row.cumulativeMs = mark.value(QStringLiteral("t_ms")).toInteger(0);
                if (!row.phase.isEmpty()) {
                    rows.push_back(row);
                }
            }
            std::stable_sort(rows.begin(), rows.end(),
                             [](const FlatRow& a, const FlatRow& b) {
                                 if (a.deltaMs != b.deltaMs) {
                                     return a.deltaMs > b.deltaMs;
                                 }
                                 return a.cumulativeMs < b.cumulativeMs;
                             });
            QJsonArray ranked;
            int rank = 1;
            for (const FlatRow& row : rows) {
                ranked.push_back(QJsonObject{
                    {QStringLiteral("rank"), rank++},
                    {QStringLiteral("phase"), row.phase},
                    {QStringLiteral("delta_ms"), row.deltaMs},
                    {QStringLiteral("cumulative_ms"), row.cumulativeMs}
                });
            }
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("flat"), true},
                {QStringLiteral("total_ms"), startup.value(QStringLiteral("total_ms")).toInteger(0)},
                {QStringLiteral("completed"), startup.value(QStringLiteral("completed")).toBool(false)},
                {QStringLiteral("mark_count"), startup.value(QStringLiteral("mark_count")).toInteger(0)},
                {QStringLiteral("ranked_phases"), ranked},
                {QStringLiteral("served_cached"), true},
                {QStringLiteral("cache"), profileCacheMeta()}
            });
            return true;
        }
        writeJson(socket, 200, QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("startup"), startup},
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
                 m_uiHeartbeatStaleMs},
            {QStringLiteral("fast_snapshot"), snapshot},
            {QStringLiteral("profile_cache"), profileCacheMeta()},
            {QStringLiteral("state_cache"), stateCacheMeta()},
            {QStringLiteral("project_cache"), projectCacheMeta()},
            {QStringLiteral("history_cache"), historyCacheMeta()},
            {QStringLiteral("ui_tree_cache"), uiTreeCacheMeta()},
            {QStringLiteral("rate_limit"), QJsonObject{
                {QStringLiteral("screenshot_count"), m_screenshotRateLimitedCount},
                {QStringLiteral("screenshot_min_interval_ms"), m_screenshotMinIntervalMs}
            }},
            {QStringLiteral("freeze_monitor"), QJsonObject{
                {QStringLiteral("stall_threshold_ms"), m_stallDetectHeartbeatMs},
                {QStringLiteral("stall_threshold_consecutive"), m_stallConsecutiveThreshold},
                {QStringLiteral("stall_active"), m_stallActive},
                {QStringLiteral("stall_started_ms"), m_stallStartedMs},
                {QStringLiteral("stall_consecutive_over_threshold"), m_stallConsecutiveOverThreshold},
                {QStringLiteral("frame_trace_sample_count"), m_frameTraceSamples.size()},
                {QStringLiteral("freeze_event_count"), m_freezeEvents.size()},
                {QStringLiteral("cache_refresh_paused_for_playback"), m_cacheRefreshPausedForPlayback},
                {QStringLiteral("last_ui_refresh_timeout_ms"), m_lastUiRefreshTimeoutMs},
                {QStringLiteral("ui_refresh_cooldown_remaining_ms"),
                 qMax<qint64>(0, m_uiRefreshCooldownAfterTimeoutMs -
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
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &reset, [this]() {
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

bool ControlServerWorker::handleThrottleRoutes(QTcpSocket* socket, const Request& request) {
    const auto buildThrottleResponse = [this](const QJsonObject& editorThrottles) {
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("editor"), editorThrottles},
            {QStringLiteral("control_server"), QJsonObject{
                {QStringLiteral("ui_invoke_timeout_ms"), m_uiInvokeTimeoutMs},
                {QStringLiteral("ui_background_invoke_timeout_ms"), m_uiBackgroundInvokeTimeoutMs},
                {QStringLiteral("ui_heartbeat_stale_ms"), m_uiHeartbeatStaleMs},
                {QStringLiteral("profile_cache_fresh_ms"), m_profileCacheFreshMs},
                {QStringLiteral("background_refresh_tick_ms"), m_backgroundRefreshTickMs},
                {QStringLiteral("profile_refresh_interval_ms"), m_profileRefreshIntervalMs},
                {QStringLiteral("profile_demand_window_ms"), m_profileDemandWindowMs},
                {QStringLiteral("snapshot_demand_window_ms"), m_snapshotDemandWindowMs},
                {QStringLiteral("state_refresh_interval_ms"), m_stateRefreshIntervalMs},
                {QStringLiteral("project_refresh_interval_ms"), m_projectRefreshIntervalMs},
                {QStringLiteral("history_refresh_interval_ms"), m_historyRefreshIntervalMs},
                {QStringLiteral("ui_tree_refresh_interval_ms"), m_uiTreeRefreshIntervalMs},
                {QStringLiteral("idle_state_refresh_interval_ms"), m_idleStateRefreshIntervalMs},
                {QStringLiteral("idle_project_refresh_interval_ms"), m_idleProjectRefreshIntervalMs},
                {QStringLiteral("idle_history_refresh_interval_ms"), m_idleHistoryRefreshIntervalMs},
                {QStringLiteral("idle_ui_tree_refresh_interval_ms"), m_idleUiTreeRefreshIntervalMs},
                {QStringLiteral("screenshot_min_interval_ms"), m_screenshotMinIntervalMs},
                {QStringLiteral("ui_refresh_cooldown_after_timeout_ms"), m_uiRefreshCooldownAfterTimeoutMs},
                {QStringLiteral("frame_trace_sample_cap"), m_frameTraceSampleCap},
                {QStringLiteral("freeze_event_cap"), m_freezeEventCap},
                {QStringLiteral("stall_detect_heartbeat_ms"), m_stallDetectHeartbeatMs},
                {QStringLiteral("stall_consecutive_threshold"), m_stallConsecutiveThreshold}
            }}
        };
    };

    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/throttles")) {
        QJsonObject editorThrottles;
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &editorThrottles, [this]() {
                return m_getThrottlesCallback ? m_getThrottlesCallback() : QJsonObject{};
            })) {
            writeError(socket, 503, QStringLiteral("timed out waiting for throttle snapshot"));
            return true;
        }
        if (editorThrottles.isEmpty()) {
            editorThrottles = QJsonObject{{QStringLiteral("ok"), false},
                                          {QStringLiteral("error"), QStringLiteral("throttle snapshot callback not configured")}};
        }
        writeJson(socket, 200, buildThrottleResponse(editorThrottles));
        return true;
    }

    if (request.method == QStringLiteral("POST") && request.url.path() == QStringLiteral("/throttles")) {
        QString error;
        const QJsonObject body = parseJsonObject(request.body, &error);
        if (!error.isEmpty()) {
            writeError(socket, 400, error);
            return true;
        }

        auto applyPositiveInt = [&error](const QJsonObject& obj, const QString& key, qint64* target) -> bool {
            if (!obj.contains(key)) return true;
            bool ok = false;
            const qint64 value = obj.value(key).toVariant().toLongLong(&ok);
            if (!ok || value <= 0) {
                error = QStringLiteral("%1 must be a positive integer").arg(key);
                return false;
            }
            *target = value;
            return true;
        };
        auto applyPositiveIntToInt = [&error](const QJsonObject& obj, const QString& key, int* target) -> bool {
            if (!obj.contains(key)) return true;
            bool ok = false;
            const qint64 value = obj.value(key).toVariant().toLongLong(&ok);
            if (!ok || value <= 0 || value > std::numeric_limits<int>::max()) {
                error = QStringLiteral("%1 must be a positive integer").arg(key);
                return false;
            }
            *target = static_cast<int>(value);
            return true;
        };

        auto applyNonNegativeIntToInt = [&error](const QJsonObject& obj, const QString& key, int* target) -> bool {
            if (!obj.contains(key)) return true;
            bool ok = false;
            const qint64 value = obj.value(key).toVariant().toLongLong(&ok);
            if (!ok || value < 0 || value > std::numeric_limits<int>::max()) {
                error = QStringLiteral("%1 must be a non-negative integer").arg(key);
                return false;
            }
            *target = static_cast<int>(value);
            return true;
        };

        QJsonObject editorResult;
        if (body.contains(QStringLiteral("editor"))) {
            const QJsonObject editorPatch = body.value(QStringLiteral("editor")).toObject();
            if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &editorResult, [this, editorPatch]() {
                    return m_setThrottlesCallback ? m_setThrottlesCallback(editorPatch)
                                                  : QJsonObject{{QStringLiteral("ok"), false},
                                                                {QStringLiteral("error"), QStringLiteral("throttle patch callback not configured")}};
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for throttle update"));
                return true;
            }
            if (!editorResult.value(QStringLiteral("ok")).toBool()) {
                writeError(socket, 400, editorResult.value(QStringLiteral("error")).toString(
                    QStringLiteral("failed to update editor throttles")));
                return true;
            }
        }

        if (body.contains(QStringLiteral("control_server"))) {
            const QJsonObject patch = body.value(QStringLiteral("control_server")).toObject();
            if (!applyPositiveIntToInt(patch, QStringLiteral("ui_invoke_timeout_ms"), &m_uiInvokeTimeoutMs) ||
                !applyPositiveIntToInt(patch, QStringLiteral("ui_background_invoke_timeout_ms"), &m_uiBackgroundInvokeTimeoutMs) ||
                !applyPositiveInt(patch, QStringLiteral("ui_heartbeat_stale_ms"), &m_uiHeartbeatStaleMs) ||
                !applyPositiveInt(patch, QStringLiteral("profile_cache_fresh_ms"), &m_profileCacheFreshMs) ||
                !applyPositiveInt(patch, QStringLiteral("background_refresh_tick_ms"), &m_backgroundRefreshTickMs) ||
                !applyPositiveInt(patch, QStringLiteral("profile_refresh_interval_ms"), &m_profileRefreshIntervalMs) ||
                !applyPositiveInt(patch, QStringLiteral("profile_demand_window_ms"), &m_profileDemandWindowMs) ||
                !applyPositiveInt(patch, QStringLiteral("snapshot_demand_window_ms"), &m_snapshotDemandWindowMs) ||
                !applyPositiveInt(patch, QStringLiteral("state_refresh_interval_ms"), &m_stateRefreshIntervalMs) ||
                !applyPositiveInt(patch, QStringLiteral("project_refresh_interval_ms"), &m_projectRefreshIntervalMs) ||
                !applyPositiveInt(patch, QStringLiteral("history_refresh_interval_ms"), &m_historyRefreshIntervalMs) ||
                !applyPositiveInt(patch, QStringLiteral("ui_tree_refresh_interval_ms"), &m_uiTreeRefreshIntervalMs) ||
                !applyPositiveInt(patch, QStringLiteral("idle_state_refresh_interval_ms"), &m_idleStateRefreshIntervalMs) ||
                !applyPositiveInt(patch, QStringLiteral("idle_project_refresh_interval_ms"), &m_idleProjectRefreshIntervalMs) ||
                !applyPositiveInt(patch, QStringLiteral("idle_history_refresh_interval_ms"), &m_idleHistoryRefreshIntervalMs) ||
                !applyPositiveInt(patch, QStringLiteral("idle_ui_tree_refresh_interval_ms"), &m_idleUiTreeRefreshIntervalMs) ||
                !applyPositiveInt(patch, QStringLiteral("screenshot_min_interval_ms"), &m_screenshotMinIntervalMs) ||
                !applyPositiveInt(patch, QStringLiteral("ui_refresh_cooldown_after_timeout_ms"), &m_uiRefreshCooldownAfterTimeoutMs) ||
                !applyPositiveInt(patch, QStringLiteral("frame_trace_sample_cap"), &m_frameTraceSampleCap) ||
                !applyPositiveInt(patch, QStringLiteral("freeze_event_cap"), &m_freezeEventCap) ||
                !applyPositiveInt(patch, QStringLiteral("stall_detect_heartbeat_ms"), &m_stallDetectHeartbeatMs) ||
                !applyNonNegativeIntToInt(patch, QStringLiteral("stall_consecutive_threshold"), &m_stallConsecutiveThreshold)) {
                writeError(socket, 400, error);
                return true;
            }
            if (m_refreshTimer) {
                m_refreshTimer->setInterval(static_cast<int>(m_backgroundRefreshTickMs));
            }
        }

        QJsonObject editorThrottles;
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &editorThrottles, [this]() {
                return m_getThrottlesCallback ? m_getThrottlesCallback() : QJsonObject{};
            })) {
            writeError(socket, 503, QStringLiteral("timed out waiting for updated throttle snapshot"));
            return true;
        }
        if (editorThrottles.isEmpty()) {
            editorThrottles = QJsonObject{{QStringLiteral("ok"), false},
                                          {QStringLiteral("error"), QStringLiteral("throttle snapshot callback not configured")}};
        }
        writeJson(socket, 200, buildThrottleResponse(editorThrottles));
        return true;
    }

    return false;
}

bool ControlServerWorker::handlePlaybackRoutes(QTcpSocket* socket, const Request& request) {
    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/playback")) {
        QJsonObject playbackConfig;
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &playbackConfig, [this]() {
                return m_getPlaybackConfigCallback ? m_getPlaybackConfigCallback()
                                                   : QJsonObject{};
            })) {
            writeError(socket, 503, QStringLiteral("timed out waiting for playback config snapshot"));
            return true;
        }
        if (playbackConfig.isEmpty()) {
            playbackConfig = QJsonObject{{QStringLiteral("ok"), false},
                                         {QStringLiteral("error"), QStringLiteral("playback config callback not configured")}};
        }
        writeJson(socket, 200, QJsonObject{
            {QStringLiteral("ok"), playbackConfig.value(QStringLiteral("ok")).toBool(true)},
            {QStringLiteral("editor"), playbackConfig}
        });
        return true;
    }

    if (request.method == QStringLiteral("POST") && request.url.path() == QStringLiteral("/playback")) {
        QString error;
        const QJsonObject body = parseJsonObject(request.body, &error);
        if (!error.isEmpty()) {
            writeError(socket, 400, error);
            return true;
        }

        QJsonObject result;
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &result, [this, body]() {
                return m_setPlaybackConfigCallback
                    ? m_setPlaybackConfigCallback(body)
                    : QJsonObject{{QStringLiteral("ok"), false},
                                  {QStringLiteral("error"), QStringLiteral("playback config callback not configured")}};
            })) {
            writeError(socket, 503, QStringLiteral("timed out waiting for playback config update"));
            return true;
        }
        if (!result.value(QStringLiteral("ok")).toBool()) {
            writeError(socket, 400, result.value(QStringLiteral("error")).toString(
                QStringLiteral("failed to update playback config")));
            return true;
        }

        if (!m_lastStateSnapshot.isEmpty()) {
            mergePlaybackConfigIntoState(result, &m_lastStateSnapshot);
            m_lastStateSnapshotMs = QDateTime::currentMSecsSinceEpoch();
        }

        writeJson(socket, 200, QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("editor"), result}
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
    if (request.method == QStringLiteral("GET") &&
        (request.url.path() == QStringLiteral("/ui") || request.url.path() == QStringLiteral("/ui/"))) {
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

    if (request.method == QStringLiteral("POST") &&
        (request.url.path() == QStringLiteral("/ui") ||
         request.url.path() == QStringLiteral("/ui/") ||
         request.url.path() == QStringLiteral("/ui/table/context-action"))) {
        QString error;
        const QJsonObject body = parseJsonObject(request.body, &error);
        if (!error.isEmpty()) {
            writeError(socket, 400, error);
            return true;
        }
        QJsonObject effectiveBody = body;
        if (request.url.path() == QStringLiteral("/ui/table/context-action")) {
            if (!effectiveBody.contains(QStringLiteral("op"))) {
                effectiveBody.insert(QStringLiteral("op"), QStringLiteral("table_context_action"));
            }
        }

        QJsonObject response;
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &response, [this, effectiveBody]() {
                const QString op = effectiveBody.value(QStringLiteral("op"))
                                       .toString(QStringLiteral("set"))
                                       .trimmed();
                QJsonObject targetSpec = effectiveBody;
                if (op == QStringLiteral("table_context_action") &&
                    effectiveBody.value(QStringLiteral("table")).isObject()) {
                    targetSpec = effectiveBody.value(QStringLiteral("table")).toObject();
                }
                QWidget* widget = resolveWidgetTarget(m_window, targetSpec);
                if (!widget) {
                    return QJsonObject{
                        {QStringLiteral("ok"), false},
                        {QStringLiteral("error"), QStringLiteral("target widget not found (id/path/selector)")},
                        {QStringLiteral("request"), effectiveBody}
                    };
                }

                const QJsonObject before = widgetSnapshot(widget);
                auto selectedClipFromState = [](const QJsonObject& stateObj) -> QJsonObject {
                    const QJsonObject direct = stateObj.value(QStringLiteral("selectedClip")).toObject();
                    if (!direct.isEmpty()) {
                        return direct;
                    }
                    const QString selectedClipId =
                        stateObj.value(QStringLiteral("selectedClipId")).toString().trimmed();
                    if (selectedClipId.isEmpty()) {
                        return {};
                    }
                    const QJsonArray timeline = stateObj.value(QStringLiteral("timeline")).toArray();
                    for (const QJsonValue& value : timeline) {
                        const QJsonObject clipObj = value.toObject();
                        if (clipObj.value(QStringLiteral("id")).toString().trimmed() == selectedClipId) {
                            return clipObj;
                        }
                    }
                    return {};
                };
                auto disabledButtonReason = [this, selectedClipFromState](QAbstractButton* button) -> QString {
                    if (!button) {
                        return QStringLiteral("target button is disabled");
                    }
                    const QString text = button->text().trimmed();
                    if (text.startsWith(QStringLiteral("Face Stabilize"), Qt::CaseInsensitive)) {
                        QJsonObject stateObj;
                        if (m_stateSnapshotCallback) {
                            stateObj = m_stateSnapshotCallback().value(QStringLiteral("state")).toObject();
                        }
                        const QJsonObject selectedClip = selectedClipFromState(stateObj);
                        if (selectedClip.isEmpty()) {
                            return QStringLiteral(
                                "Face Stabilize is disabled: no clip is selected. Select a clip first.");
                        }
                        const int keyCount =
                            selectedClip.value(QStringLiteral("speakerFramingKeyframes")).toArray().size();
                        const QString runtimeSpeakerId =
                            selectedClip.value(QStringLiteral("speakerFramingSpeakerId"))
                                .toString()
                                .trimmed();
                        if (keyCount <= 0 && runtimeSpeakerId.isEmpty()) {
                            return QStringLiteral(
                                "Face Stabilize is disabled: selected clip has no BoxStream runtime binding. "
                                "Generate BoxStream for this clip first.");
                        }
                        return QStringLiteral(
                            "Face Stabilize is disabled by current UI state.");
                    }
                    if (text.startsWith(QStringLiteral("Tracking"), Qt::CaseInsensitive)) {
                        return QStringLiteral(
                            "Tracking is disabled: select a speaker with an Auto-Track BoxStream first.");
                    }
                    return QStringLiteral("target button is disabled");
                };

                auto applyGenericSet = [&effectiveBody](QWidget* target, QString* errorOut) -> bool {
                    if (!target) {
                        if (errorOut) {
                            *errorOut = QStringLiteral("null target");
                        }
                        return false;
                    }
                    if (effectiveBody.contains(QStringLiteral("enabled"))) {
                        target->setEnabled(effectiveBody.value(QStringLiteral("enabled")).toBool(target->isEnabled()));
                    }
                    if (effectiveBody.contains(QStringLiteral("visible"))) {
                        target->setVisible(effectiveBody.value(QStringLiteral("visible")).toBool(target->isVisible()));
                    }
                    if (effectiveBody.contains(QStringLiteral("checked"))) {
                        auto* button = qobject_cast<QAbstractButton*>(target);
                        if (!button || !button->isCheckable()) {
                            if (errorOut) {
                                *errorOut = QStringLiteral("target is not a checkable button");
                            }
                            return false;
                        }
                        button->setChecked(effectiveBody.value(QStringLiteral("checked")).toBool(button->isChecked()));
                    }
                    if (effectiveBody.contains(QStringLiteral("value"))) {
                        const QJsonValue value = effectiveBody.value(QStringLiteral("value"));
                        if (auto* slider = qobject_cast<QSlider*>(target)) {
                            slider->setValue(value.toInt(slider->value()));
                        } else if (auto* spin = qobject_cast<QSpinBox*>(target)) {
                            spin->setValue(value.toInt(spin->value()));
                        } else if (auto* dspin = qobject_cast<QDoubleSpinBox*>(target)) {
                            dspin->setValue(value.toDouble(dspin->value()));
                        } else {
                            if (errorOut) {
                                *errorOut = QStringLiteral("target does not accept numeric value");
                            }
                            return false;
                        }
                    }
                    if (effectiveBody.contains(QStringLiteral("text"))) {
                        const QString textValue = effectiveBody.value(QStringLiteral("text")).toString();
                        if (auto* lineEdit = qobject_cast<QLineEdit*>(target)) {
                            lineEdit->setText(textValue);
                        } else if (auto* plainText = qobject_cast<QPlainTextEdit*>(target)) {
                            plainText->setPlainText(textValue);
                        } else if (auto* button = qobject_cast<QAbstractButton*>(target)) {
                            button->setText(textValue);
                        } else {
                            if (errorOut) {
                                *errorOut = QStringLiteral("target does not accept text");
                            }
                            return false;
                        }
                    }
                    if (effectiveBody.contains(QStringLiteral("currentIndex"))) {
                        auto* combo = qobject_cast<QComboBox*>(target);
                        if (!combo) {
                            if (errorOut) {
                                *errorOut = QStringLiteral("target is not a combo box");
                            }
                            return false;
                        }
                        combo->setCurrentIndex(effectiveBody.value(QStringLiteral("currentIndex")).toInt(combo->currentIndex()));
                    }
                    if (effectiveBody.contains(QStringLiteral("currentText"))) {
                        auto* combo = qobject_cast<QComboBox*>(target);
                        if (!combo) {
                            if (errorOut) {
                                *errorOut = QStringLiteral("target is not a combo box");
                            }
                            return false;
                        }
                        combo->setCurrentText(effectiveBody.value(QStringLiteral("currentText")).toString(combo->currentText()));
                    }
                    return true;
                };

                QString operationError;
                bool ok = false;
                if (op == QStringLiteral("click")) {
                    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
                        if (!button->isEnabled()) {
                            operationError = disabledButtonReason(button);
                            ok = false;
                        } else {
                            button->click();
                            ok = true;
                        }
                    } else {
                        ok = sendSyntheticClick(m_window, widget->mapTo(m_window, widget->rect().center()));
                    }
                } else if (op == QStringLiteral("set")) {
                    ok = applyGenericSet(widget, &operationError);
                } else if (op == QStringLiteral("table_set")) {
                    auto* table = qobject_cast<QTableWidget*>(widget);
                    if (!table) {
                        operationError = QStringLiteral("target is not a QTableWidget");
                    } else {
                            const int row = effectiveBody.value(QStringLiteral("row")).toInt(-1);
                            const int column = effectiveBody.value(QStringLiteral("column")).toInt(-1);
                        if (row < 0 || column < 0) {
                            operationError = QStringLiteral("row and column are required for table_set");
                        } else if (row >= table->rowCount() || column >= table->columnCount()) {
                            operationError = QStringLiteral("table cell is out of bounds");
                        } else {
                            const QString cellText = effectiveBody.contains(QStringLiteral("text"))
                                ? effectiveBody.value(QStringLiteral("text")).toString()
                                : effectiveBody.value(QStringLiteral("value")).toVariant().toString();
                            QTableWidgetItem* item = table->item(row, column);
                            if (!item) {
                                item = new QTableWidgetItem;
                                table->setItem(row, column, item);
                            }
                            item->setText(cellText);
                            ok = true;
                        }
                    }
                } else if (op == QStringLiteral("table_select")) {
                    auto* table = qobject_cast<QTableWidget*>(widget);
                    if (!table) {
                        operationError = QStringLiteral("target is not a QTableWidget");
                    } else {
                        const int row = effectiveBody.value(QStringLiteral("row")).toInt(-1);
                        const int column = effectiveBody.value(QStringLiteral("column")).toInt(0);
                        if (row < 0 || row >= table->rowCount() || column < 0 ||
                            column >= table->columnCount()) {
                            operationError = QStringLiteral("table selection is out of bounds");
                        } else {
                            table->setCurrentCell(row, column);
                            ok = true;
                        }
                    }
                } else if (op == QStringLiteral("table_context_action")) {
                    auto* table = qobject_cast<QTableWidget*>(widget);
                    if (!table) {
                        operationError = QStringLiteral("target is not a QTableWidget");
                    } else {
                        QVector<int> rowsToSelect;
                        if (effectiveBody.contains(QStringLiteral("rows"))) {
                            const QJsonArray rowsArray = effectiveBody.value(QStringLiteral("rows")).toArray();
                            for (const QJsonValue& rowValue : rowsArray) {
                                const int row = rowValue.toInt(-1);
                                if (row >= 0 && row < table->rowCount()) {
                                    rowsToSelect.push_back(row);
                                }
                            }
                        }
                        if (rowsToSelect.isEmpty()) {
                            const int row = effectiveBody.value(QStringLiteral("row")).toInt(-1);
                            if (row >= 0 && row < table->rowCount()) {
                                rowsToSelect.push_back(row);
                            }
                        }
                        if (rowsToSelect.isEmpty()) {
                            const QJsonObject rowMatch = effectiveBody.value(QStringLiteral("rowMatch")).toObject();
                            if (!rowMatch.isEmpty()) {
                                const int matchColumn = resolveTableColumn(table, rowMatch);
                                if (matchColumn >= 0) {
                                    for (int row = 0; row < table->rowCount(); ++row) {
                                        if (tableRowMatches(table, row, matchColumn, rowMatch)) {
                                            rowsToSelect.push_back(row);
                                            const bool allMatches =
                                                rowMatch.value(QStringLiteral("allMatches")).toBool(false);
                                            if (!allMatches) {
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        if (rowsToSelect.isEmpty()) {
                            operationError = QStringLiteral("no matching row found");
                        } else {
                            const int column = qBound(0,
                                                      effectiveBody.value(QStringLiteral("column")).toInt(0),
                                                      qMax(0, table->columnCount() - 1));
                            table->clearSelection();
                            for (int row : rowsToSelect) {
                                table->selectRow(row);
                            }
                            table->setCurrentCell(rowsToSelect.constFirst(), column);

                            const QModelIndex modelIndex =
                                table->model()->index(rowsToSelect.constFirst(), column);
                            const QRect itemRect = table->visualRect(modelIndex);
                            const QPoint viewportCenter =
                                itemRect.isValid() ? itemRect.center() : QPoint(8, 8);
                            const QPoint windowPos =
                                table->viewport()->mapTo(m_window, viewportCenter);
                            const bool clickOk =
                                sendSyntheticClick(m_window, windowPos, Qt::RightButton);
                            QMenu* menu = activePopupMenu();
                            if (!menu) {
                                operationError =
                                    clickOk
                                        ? QStringLiteral("context menu did not open")
                                        : QStringLiteral("failed to synthesize context-click");
                            } else {
                                const QString actionText =
                                    effectiveBody.value(QStringLiteral("actionText")).toString().trimmed();
                                if (actionText.isEmpty()) {
                                    operationError = QStringLiteral("actionText is required");
                                } else {
                                    const bool actionContains =
                                        effectiveBody.value(QStringLiteral("actionContains")).toBool(true);
                                    const bool caseSensitive =
                                        effectiveBody.value(QStringLiteral("actionCaseSensitive")).toBool(false);
                                    const Qt::CaseSensitivity cs =
                                        caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
                                    QAction* matchedAction = nullptr;
                                    const QString wanted = normalizedActionText(actionText);
                                    for (QAction* action : menu->actions()) {
                                        if (!action || action->isSeparator()) {
                                            continue;
                                        }
                                        const QString candidate =
                                            normalizedActionText(action->text());
                                        const bool matched = actionContains
                                            ? candidate.contains(wanted, cs)
                                            : QString::compare(candidate, wanted, cs) == 0;
                                        if (matched) {
                                            matchedAction = action;
                                            break;
                                        }
                                    }
                                    if (!matchedAction) {
                                        operationError = QStringLiteral("context menu action not found");
                                    } else if (!matchedAction->isEnabled()) {
                                        operationError = QStringLiteral("context menu action is disabled");
                                    } else {
                                        matchedAction->trigger();
                                        ok = true;
                                    }
                                }
                            }
                        }
                    }
                } else {
                    operationError = QStringLiteral("unsupported op: %1").arg(op);
                }

                const QJsonObject after = widgetSnapshot(widget);
                return QJsonObject{
                    {QStringLiteral("ok"), ok},
                    {QStringLiteral("op"), op},
                    {QStringLiteral("error"), operationError},
                    {QStringLiteral("target_id"), widget->objectName()},
                    {QStringLiteral("target_class"), QString::fromLatin1(widget->metaObject()->className())},
                    {QStringLiteral("before"), before},
                    {QStringLiteral("after"), after}
                };
            })) {
            writeError(socket, 503, QStringLiteral("timed out waiting for UI mutation"));
            return true;
        }

        writeJson(socket, response.value(QStringLiteral("ok")).toBool() ? 200 : 400, response);
        return true;
    }

    if (request.method == QStringLiteral("GET") && request.url.path() == QStringLiteral("/windows")) {
        QJsonArray windows;
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &windows, []() {
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
        if (m_lastScreenshotRequestMs > 0 && (now - m_lastScreenshotRequestMs) < m_screenshotMinIntervalMs) {
            ++m_screenshotRateLimitedCount;
            writeError(socket, 429, QStringLiteral("screenshot requests are rate-limited"));
            return true;
        }
        m_lastScreenshotRequestMs = now;
        QByteArray pngBytes;
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &pngBytes, [this]() {
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
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &result, [this, x, y, button, buttonName]() {
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
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &response, []() {
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
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &response, [text]() {
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
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &response, [this, id]() {
                auto selectedClipFromState = [](const QJsonObject& stateObj) -> QJsonObject {
                    const QJsonObject direct = stateObj.value(QStringLiteral("selectedClip")).toObject();
                    if (!direct.isEmpty()) {
                        return direct;
                    }
                    const QString selectedClipId =
                        stateObj.value(QStringLiteral("selectedClipId")).toString().trimmed();
                    if (selectedClipId.isEmpty()) {
                        return {};
                    }
                    const QJsonArray timeline = stateObj.value(QStringLiteral("timeline")).toArray();
                    for (const QJsonValue& value : timeline) {
                        const QJsonObject clipObj = value.toObject();
                        if (clipObj.value(QStringLiteral("id")).toString().trimmed() == selectedClipId) {
                            return clipObj;
                        }
                    }
                    return {};
                };
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
                    if (button->isEnabled()) {
                        button->click();
                        clicked = true;
                    }
                } else {
                    clicked = sendSyntheticClick(m_window, widget->mapTo(m_window, widget->rect().center()));
                }

                const QJsonObject after = widgetSnapshot(widget);
                const QJsonObject profileAfter = m_profilingCallback ? m_profilingCallback() : QJsonObject{};
                const bool confirmed = clicked && (before != after || profileBefore != profileAfter);
                QString error;
                if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
                    if (!button->isEnabled()) {
                        const QString text = button->text().trimmed();
                        if (text.startsWith(QStringLiteral("Face Stabilize"), Qt::CaseInsensitive)) {
                            QJsonObject stateObj;
                            if (m_stateSnapshotCallback) {
                                stateObj = m_stateSnapshotCallback().value(QStringLiteral("state")).toObject();
                            }
                            const QJsonObject selectedClip = selectedClipFromState(stateObj);
                            if (selectedClip.isEmpty()) {
                                error = QStringLiteral(
                                    "Face Stabilize is disabled: no clip is selected. Select a clip first.");
                            } else {
                                const int keyCount = selectedClip.value(
                                    QStringLiteral("speakerFramingKeyframes")).toArray().size();
                                const QString runtimeSpeakerId =
                                    selectedClip.value(QStringLiteral("speakerFramingSpeakerId"))
                                        .toString()
                                        .trimmed();
                                if (keyCount <= 0 && runtimeSpeakerId.isEmpty()) {
                                    error = QStringLiteral(
                                        "Face Stabilize is disabled: selected clip has no BoxStream runtime binding. "
                                        "Generate BoxStream for this clip first.");
                                } else {
                                    error = QStringLiteral("Face Stabilize is disabled by current UI state.");
                                }
                            }
                        } else if (text.startsWith(QStringLiteral("Tracking"), Qt::CaseInsensitive)) {
                            error = QStringLiteral(
                                "Tracking is disabled: select a speaker with an Auto-Track BoxStream first.");
                        } else {
                            error = QStringLiteral("target button is disabled");
                        }
                    }
                }

                return QJsonObject{
                    {QStringLiteral("ok"), clicked},
                    {QStringLiteral("id"), id},
                    {QStringLiteral("confirmed"), confirmed},
                    {QStringLiteral("error"), error},
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
