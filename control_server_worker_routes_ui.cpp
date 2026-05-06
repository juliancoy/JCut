#include "control_server_worker.h"

#include "control_server_ui_utils.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTcpSocket>
#include <QUrlQuery>

#include <algorithm>
#include <limits>

namespace control_server {
namespace {
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
                auto disabledButtonReason = [this](QAbstractButton* button) -> QString {
                    if (!button) {
                        return QStringLiteral("target button is disabled");
                    }
                    const QString text = button->text().trimmed();
                    if (text.startsWith(QStringLiteral("Face Stabilize"), Qt::CaseInsensitive)) {
                        QJsonObject stateObj;
                        if (m_stateSnapshotCallback) {
                            stateObj = m_stateSnapshotCallback().value(QStringLiteral("state")).toObject();
                        }
                        const QJsonObject selectedClip =
                            resolveSelectedClipState(stateObj).value(QStringLiteral("selectedClip")).toObject();
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
                                "Face Stabilize is disabled: selected clip has no FaceStream runtime binding. "
                                "Generate FaceStream for this clip first.");
                        }
                        return QStringLiteral(
                            "Face Stabilize is disabled by current UI state.");
                    }
                    if (text.startsWith(QStringLiteral("Tracking"), Qt::CaseInsensitive)) {
                        return QStringLiteral(
                            "Tracking is disabled: select a speaker with an Auto-Track FaceStream first.");
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
                            const QJsonObject selectedClip =
                                resolveSelectedClipState(stateObj).value(QStringLiteral("selectedClip")).toObject();
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
                                        "Face Stabilize is disabled: selected clip has no FaceStream runtime binding. "
                                        "Generate FaceStream for this clip first.");
                                } else {
                                    error = QStringLiteral("Face Stabilize is disabled by current UI state.");
                                }
                            }
                        } else if (text.startsWith(QStringLiteral("Tracking"), Qt::CaseInsensitive)) {
                            error = QStringLiteral(
                                "Tracking is disabled: select a speaker with an Auto-Track FaceStream first.");
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


} // namespace control_server
