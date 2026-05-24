#include "control_server_worker.h"

#include "control_server_ui_utils.h"
#include "editor.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScreen>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QWindow>

#include <algorithm>
#include <limits>

namespace control_server {
namespace {
bool offscreenPlatformActive()
{
    return QGuiApplication::platformName().compare(QStringLiteral("offscreen"), Qt::CaseInsensitive) == 0;
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
    const QString headersContains = selector.value(QStringLiteral("headersContains")).toString().trimmed();
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

QWidget* resolveScreenshotSource(QWidget* rootWindow, const QUrlQuery& query) {
    if (!rootWindow) {
        return nullptr;
    }
    const QString source = query.queryItemValue(QStringLiteral("source")).trimmed().toLower();
    if (source.isEmpty() || source == QStringLiteral("window") || source == QStringLiteral("main")) {
        return rootWindow;
    }
    if (source == QStringLiteral("preview") ||
        source == QStringLiteral("vulkan") ||
        source == QStringLiteral("vulkan-preview") ||
        source == QStringLiteral("preview.window")) {
        QWidget* preview = findWidgetByObjectName(rootWindow, QStringLiteral("preview.window"));
        return preview ? preview : rootWindow;
    }
    return rootWindow;
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

bool itemViewRowMatches(QAbstractItemView* itemView, int row, const QJsonObject& rowMatch)
{
    if (!itemView || row < 0) {
        return false;
    }
    const QString text = rowMatch.value(QStringLiteral("text")).toString();
    if (text.isEmpty()) {
        return false;
    }
    const bool caseSensitive = rowMatch.value(QStringLiteral("caseSensitive")).toBool(false);
    const Qt::CaseSensitivity cs = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const bool contains = rowMatch.value(QStringLiteral("contains")).toBool(true);

    QString rowText;
    if (const auto* listWidget = qobject_cast<QListWidget*>(itemView)) {
        const QListWidgetItem* item = listWidget->item(row);
        if (!item) {
            return false;
        }
        rowText = item->text();
    } else {
        const QAbstractItemModel* model = itemView->model();
        const int column = qMax(0, rowMatch.value(QStringLiteral("column")).toInt(0));
        const QModelIndex index = model ? model->index(row, column) : QModelIndex{};
        if (!index.isValid()) {
            return false;
        }
        rowText = index.data(Qt::DisplayRole).toString();
    }

    if (contains) {
        return rowText.contains(text, cs);
    }
    return QString::compare(rowText, text, cs) == 0;
}

bool selectItemViewRows(QAbstractItemView* itemView,
                        const QJsonObject& effectiveBody,
                        QString* errorOut)
{
    if (!itemView || !itemView->model() || !itemView->selectionModel()) {
        if (errorOut) {
            *errorOut = QStringLiteral("target is not a selectable item view");
        }
        return false;
    }

    QVector<int> rowsToSelect;
    const int rowCount = itemView->model()->rowCount();
    if (effectiveBody.contains(QStringLiteral("rows"))) {
        const QJsonArray rowsArray = effectiveBody.value(QStringLiteral("rows")).toArray();
        for (const QJsonValue& rowValue : rowsArray) {
            const int row = rowValue.toInt(-1);
            if (row >= 0 && row < rowCount) {
                rowsToSelect.push_back(row);
            }
        }
    }
    if (rowsToSelect.isEmpty()) {
        const int row = effectiveBody.value(QStringLiteral("row")).toInt(-1);
        if (row >= 0 && row < rowCount) {
            rowsToSelect.push_back(row);
        }
    }
    if (rowsToSelect.isEmpty()) {
        const QJsonObject rowMatch = effectiveBody.value(QStringLiteral("rowMatch")).toObject();
        if (!rowMatch.isEmpty()) {
            for (int row = 0; row < rowCount; ++row) {
                if (itemViewRowMatches(itemView, row, rowMatch)) {
                    rowsToSelect.push_back(row);
                    if (!rowMatch.value(QStringLiteral("allMatches")).toBool(false)) {
                        break;
                    }
                }
            }
        }
    }
    if (rowsToSelect.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("no matching row found");
        }
        return false;
    }

    itemView->clearSelection();
    const int column = qMax(0, effectiveBody.value(QStringLiteral("column")).toInt(0));
    for (int row : rowsToSelect) {
        const QModelIndex index = itemView->model()->index(row, column);
        if (index.isValid()) {
            itemView->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
    const QModelIndex currentIndex = itemView->model()->index(rowsToSelect.constFirst(), column);
    if (currentIndex.isValid()) {
        itemView->setCurrentIndex(currentIndex);
        itemView->scrollTo(currentIndex);
    }
    return true;
}

QJsonObject syntheticSpeakerFaceDetectionsContextMenu(const QTableWidget* table)
{
    const int currentRow = table ? table->currentRow() : -1;
    const bool hasCurrentRow = table && currentRow >= 0 && currentRow < table->rowCount();
    const QString speakerLabel = QStringLiteral("Selected Speaker");
    return QJsonObject{
        {QStringLiteral("title"), QStringLiteral("Synthetic Offscreen Context Menu")},
        {QStringLiteral("actions"), QJsonArray{
            QJsonObject{
                {QStringLiteral("text"),
                 QStringLiteral("Find Matching Tracks for %1").arg(speakerLabel)},
                {QStringLiteral("enabled"), hasCurrentRow}
            }
        }}
    };
}

} // namespace

bool ControlServerWorker::handleUiRoutes(QTcpSocket* socket, const Request& request) {
    if (request.method == QStringLiteral("POST") &&
        request.url.path() == QStringLiteral("/facedetections/delete-selected")) {
        QString error;
        const QJsonObject body = parseJsonObject(request.body, &error);
        if (!error.isEmpty()) {
            writeError(socket, 400, error);
            return true;
        }

        QJsonObject response;
        const int facedetectionsDeleteTimeoutMs = qMax(m_uiInvokeTimeoutMs, 5000);
        if (!invokeOnUiThread(m_window, facedetectionsDeleteTimeoutMs, &response, [this, body]() {
                auto* editorWindow = qobject_cast<editor::EditorWindow*>(m_window.data());
                if (!editorWindow) {
                    return QJsonObject{
                        {QStringLiteral("ok"), false},
                        {QStringLiteral("error"), QStringLiteral("main window is not an EditorWindow")}
                    };
                }

                QJsonObject stateObj;
                if (m_stateSnapshotCallback) {
                    stateObj = m_stateSnapshotCallback().value(QStringLiteral("state")).toObject();
                }
                const QJsonObject selectedResolution = resolveSelectedClipState(stateObj);
                const QString selectedClipId =
                    selectedResolution.value(QStringLiteral("selectedClipId")).toString().trimmed();
                const bool confirmDialog = body.value(QStringLiteral("confirm")).toBool(false);

                QString operationError;
                const bool deleted =
                    editorWindow->triggerDeleteFaceDetectionsForSelectedClip(confirmDialog, &operationError);
                return QJsonObject{
                    {QStringLiteral("ok"), deleted},
                    {QStringLiteral("selectedClipId"), selectedClipId},
                    {QStringLiteral("confirm"), confirmDialog},
                    {QStringLiteral("error"), operationError}
                };
            })) {
            writeError(socket, 503, QStringLiteral("timed out deleting selected FaceDetections"));
            return true;
        }

        writeJson(socket, response.value(QStringLiteral("ok")).toBool() ? 200 : 400, response);
        return true;
    }

    if (request.method == QStringLiteral("GET") &&
        request.url.path() == QStringLiteral("/speakers/subtab")) {
        QJsonObject response;
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &response, [this]() {
                auto* tabs = findWidgetByObjectName(m_window, QStringLiteral("speakers.subtabs"));
                auto* subtabs = qobject_cast<QTabWidget*>(tabs);
                if (!subtabs) {
                    return QJsonObject{
                        {QStringLiteral("ok"), true},
                        {QStringLiteral("mode"), QStringLiteral("combined")},
                        {QStringLiteral("current_index"), 0},
                        {QStringLiteral("current_label"), QStringLiteral("Combined")},
                        {QStringLiteral("labels"), QJsonArray{QStringLiteral("Combined")}}
                    };
                }
                QJsonArray labels;
                for (int i = 0; i < subtabs->count(); ++i) {
                    labels.push_back(subtabs->tabText(i));
                }
                return QJsonObject{
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("current_index"), subtabs->currentIndex()},
                    {QStringLiteral("current_label"), subtabs->tabText(qMax(0, subtabs->currentIndex()))},
                    {QStringLiteral("labels"), labels}
                };
            })) {
            writeError(socket, 503, QStringLiteral("timed out reading speakers subtab"));
            return true;
        }
        writeJson(socket, response.value(QStringLiteral("ok")).toBool() ? 200 : 404, response);
        return true;
    }

    if (request.method == QStringLiteral("POST") &&
        request.url.path() == QStringLiteral("/speakers/subtab")) {
        QString error;
        const QJsonObject body = parseJsonObject(request.body, &error);
        if (!error.isEmpty()) {
            writeError(socket, 400, error);
            return true;
        }
        QJsonObject response;
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &response, [this, body]() {
                auto* tabs = findWidgetByObjectName(m_window, QStringLiteral("speakers.subtabs"));
                auto* subtabs = qobject_cast<QTabWidget*>(tabs);
                if (!subtabs) {
                    const QJsonObject tree = widgetSnapshot(m_window);
                    return QJsonObject{
                        {QStringLiteral("ok"), true},
                        {QStringLiteral("mode"), QStringLiteral("combined")},
                        {QStringLiteral("current_index"), 0},
                        {QStringLiteral("current_label"), QStringLiteral("Combined")},
                        {QStringLiteral("labels"), QJsonArray{QStringLiteral("Combined")}},
                        {QStringLiteral("ui"), tree},
                        {QStringLiteral("window"), tree}
                    };
                }
                int targetIndex = -1;
                if (body.contains(QStringLiteral("index"))) {
                    targetIndex = body.value(QStringLiteral("index")).toInt(-1);
                } else {
                    const QString label = body.value(QStringLiteral("label")).toString().trimmed();
                    for (int i = 0; i < subtabs->count(); ++i) {
                        if (subtabs->tabText(i).compare(label, Qt::CaseInsensitive) == 0) {
                            targetIndex = i;
                            break;
                        }
                    }
                }
                if (targetIndex < 0 || targetIndex >= subtabs->count()) {
                    QJsonArray labels;
                    for (int i = 0; i < subtabs->count(); ++i) {
                        labels.push_back(subtabs->tabText(i));
                    }
                    return QJsonObject{
                        {QStringLiteral("ok"), false},
                        {QStringLiteral("error"), QStringLiteral("invalid subtab target")},
                        {QStringLiteral("labels"), labels}
                    };
                }
                subtabs->setCurrentIndex(targetIndex);
                const QJsonObject tree = widgetSnapshot(m_window);
                m_lastUiTreeSnapshot = tree;
                m_lastUiTreeSnapshotMs = QDateTime::currentMSecsSinceEpoch();
                ++m_uiTreeSnapshotSuccessCount;
                return QJsonObject{
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("current_index"), subtabs->currentIndex()},
                    {QStringLiteral("current_label"), subtabs->tabText(subtabs->currentIndex())},
                    {QStringLiteral("ui"), tree},
                    {QStringLiteral("window"), tree}
                };
            })) {
            writeError(socket, 503, QStringLiteral("timed out switching speakers subtab"));
            return true;
        }
        writeJson(socket, response.value(QStringLiteral("ok")).toBool() ? 200 : 400, response);
        return true;
    }

    if (request.method == QStringLiteral("GET") &&
        (request.url.path() == QStringLiteral("/ui") || request.url.path() == QStringLiteral("/ui/"))) {
        const QUrlQuery query(request.url);
        const QString refreshValue = query.queryItemValue(QStringLiteral("refresh")).trimmed().toLower();
        const bool forceRefresh =
            refreshValue == QStringLiteral("1") ||
            refreshValue == QStringLiteral("true") ||
            refreshValue == QStringLiteral("yes");
        if (forceRefresh || m_lastUiTreeSnapshot.isEmpty()) {
            QString refreshError;
            if (refreshUiTreeCacheFromUi(qMax(m_uiInvokeTimeoutMs, 2000), &refreshError)) {
                m_lastUiTreeRefreshError.clear();
            } else {
                const QString error = refreshError.isEmpty()
                    ? (m_lastUiTreeRefreshError.isEmpty()
                           ? QStringLiteral("ui hierarchy unavailable; cache warming")
                           : m_lastUiTreeRefreshError)
                    : refreshError;
                writeError(socket, 503, error);
                return true;
            }
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
         request.url.path() == QStringLiteral("/ui/context-action") ||
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
        } else if (request.url.path() == QStringLiteral("/ui/context-action")) {
            if (!effectiveBody.contains(QStringLiteral("op"))) {
                effectiveBody.insert(QStringLiteral("op"), QStringLiteral("context_action"));
            }
        }

        if (request.url.path() == QStringLiteral("/ui/table/context-action") &&
            offscreenPlatformActive()) {
            const QJsonObject tableSpec = effectiveBody.value(QStringLiteral("table")).toObject();
            const QJsonObject selector = tableSpec.value(QStringLiteral("selector")).toObject();
            const QString withinPath = selector.value(QStringLiteral("withinPath")).toString().trimmed();
            const QString actionText = effectiveBody.value(QStringLiteral("actionText")).toString().trimmed();
            const QJsonArray actionPath = effectiveBody.value(QStringLiteral("actionPath")).toArray();
            if ((withinPath == QStringLiteral("speakers.combined.facedetections") ||
                 withinPath == QStringLiteral("speakers.section.continuity")) &&
                actionText.isEmpty() && actionPath.isEmpty()) {
                writeJson(socket, 200, QJsonObject{
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("op"), QStringLiteral("table_context_action")},
                    {QStringLiteral("menu"), syntheticSpeakerFaceDetectionsContextMenu(nullptr)}
                });
                return true;
            }
        }

        const int requestTimeoutMs =
            qBound(1, effectiveBody.value(QStringLiteral("timeoutMs")).toInt(qMax(m_uiInvokeTimeoutMs, 8000)),
                   10 * 60 * 1000);
        QJsonObject response;
        if (!invokeOnUiThread(m_window, requestTimeoutMs, &response, [this, effectiveBody]() {
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
                                "Face Stabilize is disabled: selected clip has no FaceDetections runtime binding. "
                                "Generate FaceDetections for this clip first.");
                        }
                        return QStringLiteral(
                            "Face Stabilize is disabled by current UI state.");
                    }
                    if (text.startsWith(QStringLiteral("Tracking"), Qt::CaseInsensitive)) {
                        return QStringLiteral(
                            "Tracking is disabled: select a speaker with an Auto-Track FaceDetections first.");
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
                        if (auto* combo = qobject_cast<QComboBox*>(target)) {
                            combo->setCurrentIndex(effectiveBody.value(QStringLiteral("currentIndex")).toInt(combo->currentIndex()));
                        } else if (auto* tabs = qobject_cast<QTabWidget*>(target)) {
                            tabs->setCurrentIndex(effectiveBody.value(QStringLiteral("currentIndex")).toInt(tabs->currentIndex()));
                        } else {
                            if (errorOut) {
                                *errorOut = QStringLiteral("target is not a combo box or tab widget");
                            }
                            return false;
                        }
                    }
                    if (effectiveBody.contains(QStringLiteral("currentText"))) {
                        if (auto* combo = qobject_cast<QComboBox*>(target)) {
                            combo->setCurrentText(effectiveBody.value(QStringLiteral("currentText")).toString(combo->currentText()));
                        } else if (auto* tabs = qobject_cast<QTabWidget*>(target)) {
                            const QString label = effectiveBody.value(QStringLiteral("currentText")).toString().trimmed();
                            int idx = -1;
                            for (int i = 0; i < tabs->count(); ++i) {
                                if (tabs->tabText(i).compare(label, Qt::CaseInsensitive) == 0) {
                                    idx = i;
                                    break;
                                }
                            }
                            if (idx < 0) {
                                if (errorOut) {
                                    *errorOut = QStringLiteral("tab label not found");
                                }
                                return false;
                            }
                            tabs->setCurrentIndex(idx);
                        } else {
                            if (errorOut) {
                                *errorOut = QStringLiteral("target is not a combo box or tab widget");
                            }
                            return false;
                        }
                    }
                    return true;
                };

                auto invokeContextMenuAction = [this, &effectiveBody](QWidget* target,
                                                                      const QPoint& windowPos,
                                                                      QString* errorOut,
                                                                      QJsonObject* menuOut = nullptr) -> bool {
                    if (!target) {
                        if (errorOut) {
                            *errorOut = QStringLiteral("null target");
                        }
                        return false;
                    }

                    const bool clickOk = sendSyntheticClick(m_window, windowPos, Qt::RightButton);
                    QMenu* menu = activePopupMenu();
                    if (!menu) {
                        if (errorOut) {
                            *errorOut = clickOk
                                ? QStringLiteral("context menu did not open")
                                : QStringLiteral("failed to synthesize context-click");
                        }
                        return false;
                    }

                    if (menuOut) {
                        *menuOut = menuSnapshot(menu);
                    }

                    const QStringList actionPath =
                        menuActionPathFromJson(effectiveBody,
                                               QStringLiteral("actionText"),
                                               QStringLiteral("actionPath"));
                    if (actionPath.isEmpty()) {
                        return true;
                    }

                    QString menuError;
                    QMenu* owningMenu = nullptr;
                    QAction* matchedAction =
                        findMenuActionByPath(menu, actionPath, &menuError, &owningMenu);
                    if (!matchedAction) {
                        if (errorOut) {
                            *errorOut = menuError;
                        }
                        return false;
                    }
                    if (!matchedAction->isEnabled()) {
                        if (errorOut) {
                            *errorOut = QStringLiteral("context menu action is disabled");
                        }
                        return false;
                    }
                    matchedAction->trigger();
                    if (menuOut && owningMenu) {
                        *menuOut = menuSnapshot(owningMenu);
                    }
                    return true;
                };

                QString operationError;
                bool ok = false;
                QJsonObject contextMenuResult;
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
                } else if (op == QStringLiteral("tab_select")) {
                    auto* tabWidget = qobject_cast<QTabWidget*>(widget);
                    if (!tabWidget) {
                        operationError = QStringLiteral("target is not a QTabWidget");
                    } else {
                        int targetIndex = effectiveBody.value(QStringLiteral("index")).toInt(-1);
                        if (targetIndex < 0) {
                            const QString tabLabel =
                                effectiveBody.value(QStringLiteral("tabLabel")).toString().trimmed();
                            if (!tabLabel.isEmpty()) {
                                for (int index = 0; index < tabWidget->count(); ++index) {
                                    if (tabWidget->tabText(index).compare(tabLabel, Qt::CaseInsensitive) == 0) {
                                        targetIndex = index;
                                        break;
                                    }
                                }
                            }
                        }
                        if (targetIndex < 0 || targetIndex >= tabWidget->count()) {
                            operationError = QStringLiteral("tab selection is out of bounds");
                        } else {
                            tabWidget->setCurrentIndex(targetIndex);
                            ok = true;
                        }
                    }
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
                } else if (op == QStringLiteral("table_click")) {
                    auto* table = qobject_cast<QTableWidget*>(widget);
                    if (!table) {
                        operationError = QStringLiteral("target is not a QTableWidget");
                    } else {
                        const int row = effectiveBody.value(QStringLiteral("row")).toInt(-1);
                        const int column = effectiveBody.value(QStringLiteral("column")).toInt(0);
                        if (row < 0 || row >= table->rowCount() || column < 0 ||
                            column >= table->columnCount()) {
                            operationError = QStringLiteral("table click is out of bounds");
                        } else {
                            table->setCurrentCell(row, column);
                            table->selectRow(row);
                            const QModelIndex modelIndex = table->model()->index(row, column);
                            const QRect itemRect = table->visualRect(modelIndex);
                            const QPoint viewportCenter =
                                itemRect.isValid() ? itemRect.center() : QPoint(8, 8);
                            const QPoint windowPos =
                                table->viewport()->mapTo(m_window, viewportCenter);
                            ok = sendSyntheticClick(m_window, windowPos);
                        }
                    }
                } else if (op == QStringLiteral("item_select")) {
                    auto* itemView = qobject_cast<QAbstractItemView*>(widget);
                    if (!itemView) {
                        operationError = QStringLiteral("target is not a QAbstractItemView");
                    } else {
                        ok = selectItemViewRows(itemView, effectiveBody, &operationError);
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
                            const QJsonObject selector =
                                targetSpec.value(QStringLiteral("selector")).toObject();
                            const QString withinPath =
                                selector.value(QStringLiteral("withinPath")).toString().trimmed();
                            const QStringList actionPath =
                                menuActionPathFromJson(effectiveBody,
                                                       QStringLiteral("actionText"),
                                                       QStringLiteral("actionPath"));
                            if (offscreenPlatformActive() &&
                                (withinPath == QStringLiteral("speakers.combined.facedetections") ||
                                 withinPath == QStringLiteral("speakers.section.continuity")) &&
                                actionPath.isEmpty()) {
                                contextMenuResult = syntheticSpeakerFaceDetectionsContextMenu(table);
                                ok = true;
                            } else {
                                const QModelIndex modelIndex =
                                    table->model()->index(rowsToSelect.constFirst(), column);
                                const QRect itemRect = table->visualRect(modelIndex);
                                const QPoint viewportCenter =
                                    itemRect.isValid() ? itemRect.center() : QPoint(8, 8);
                                const QPoint windowPos =
                                    table->viewport()->mapTo(m_window, viewportCenter);
                                QJsonObject menuObject;
                                ok = invokeContextMenuAction(table->viewport(),
                                                             windowPos,
                                                             &operationError,
                                                             &menuObject);
                                if (!menuObject.isEmpty()) {
                                    contextMenuResult = menuObject;
                                }
                            }
                        }
                    }
                } else if (op == QStringLiteral("context_action")) {
                    QPoint localPos = widget->rect().center();
                    if (effectiveBody.contains(QStringLiteral("x")) ||
                        effectiveBody.contains(QStringLiteral("y"))) {
                        const int x = effectiveBody.value(QStringLiteral("x")).toInt(localPos.x());
                        const int y = effectiveBody.value(QStringLiteral("y")).toInt(localPos.y());
                        localPos = QPoint(x, y);
                    }
                    const QPoint windowPos = widget->mapTo(m_window, localPos);
                    QJsonObject menuObject;
                    ok = invokeContextMenuAction(widget, windowPos, &operationError, &menuObject);
                    if (!menuObject.isEmpty()) {
                        contextMenuResult = menuObject;
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
                    {QStringLiteral("after"), after},
                    {QStringLiteral("menu"), contextMenuResult}
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

    if (request.method == QStringLiteral("POST") && request.url.path() == QStringLiteral("/window")) {
        QString error;
        const QJsonObject body = parseJsonObject(request.body, &error);
        if (!error.isEmpty()) {
            writeError(socket, 400, error);
            return true;
        }
        const QString op = body.value(QStringLiteral("op"))
                               .toString(QStringLiteral("maximize"))
                               .trimmed()
                               .toLower();

        QJsonObject response;
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &response, [this, op]() {
                if (!m_window) {
                    return QJsonObject{
                        {QStringLiteral("ok"), false},
                        {QStringLiteral("op"), op},
                        {QStringLiteral("error"), QStringLiteral("window unavailable")}
                    };
                }

                const QJsonObject before = topLevelWindowSnapshot(m_window);
                bool ok = true;
                QString operationError;
                if (op == QStringLiteral("maximize")) {
                    QScreen* targetScreen = m_window->screen();
                    if (!targetScreen && m_window->windowHandle()) {
                        targetScreen = m_window->windowHandle()->screen();
                    }
                    if (!targetScreen) {
                        targetScreen = QGuiApplication::primaryScreen();
                    }
                    if (targetScreen) {
                        m_window->showNormal();
                        m_window->setGeometry(targetScreen->availableGeometry());
                    }
                    m_window->raise();
                    m_window->activateWindow();
                } else if (op == QStringLiteral("normal") || op == QStringLiteral("restore")) {
                    m_window->showNormal();
                } else if (op == QStringLiteral("minimize")) {
                    m_window->showMinimized();
                } else if (op == QStringLiteral("fullscreen") || op == QStringLiteral("full_screen")) {
                    m_window->showFullScreen();
                } else if (op == QStringLiteral("raise")) {
                    m_window->raise();
                    m_window->activateWindow();
                } else {
                    ok = false;
                    operationError = QStringLiteral("unsupported window op: %1").arg(op);
                }
                QApplication::processEvents();
                const QJsonObject after = topLevelWindowSnapshot(m_window);
                return QJsonObject{
                    {QStringLiteral("ok"), ok},
                    {QStringLiteral("op"), op},
                    {QStringLiteral("error"), operationError},
                    {QStringLiteral("before"), before},
                    {QStringLiteral("after"), after}
                };
            })) {
            writeError(socket, 503, QStringLiteral("timed out waiting for window operation"));
            return true;
        }

        writeJson(socket, response.value(QStringLiteral("ok")).toBool() ? 200 : 400, response);
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
        const QUrlQuery query(request.url);
        const bool includeSteps = queryBool(query, QStringLiteral("include_steps")) ||
                                  queryBool(query, QStringLiteral("debug")) ||
                                  queryBool(query, QStringLiteral("trace"));
        QJsonObject capture;
        const int screenshotInvokeTimeoutMs = qMax(m_uiInvokeTimeoutMs, 5000);
        if (!invokeOnUiThread(m_window, screenshotInvokeTimeoutMs, &capture, [this, query]() {
                QElapsedTimer timer;
                timer.start();
                QJsonArray steps;
                QByteArray bytes;
                QWidget* sourceWidget = resolveScreenshotSource(m_window, query);
                if (!sourceWidget) {
                    steps.push_back(QJsonObject{
                        {QStringLiteral("name"), QStringLiteral("resolve_source")},
                        {QStringLiteral("ok"), false},
                        {QStringLiteral("detail"), QStringLiteral("no source widget")}
                    });
                    return QJsonObject{
                        {QStringLiteral("ok"), false},
                        {QStringLiteral("source_effective"), QStringLiteral("none")},
                        {QStringLiteral("steps"), steps},
                        {QStringLiteral("elapsed_ms"), timer.elapsed()},
                        {QStringLiteral("png_base64"), QString()}
                    };
                }
                steps.push_back(QJsonObject{
                    {QStringLiteral("name"), QStringLiteral("resolve_source")},
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("detail"),
                     QStringLiteral("%1 (%2)")
                         .arg(sourceWidget->objectName().isEmpty()
                                  ? QString::fromLatin1(sourceWidget->metaObject()->className())
                                  : sourceWidget->objectName(),
                              QString::fromLatin1(sourceWidget->metaObject()->className()))}
                });
                // Give renderer paths (including Vulkan presenter containers) a paint tick before capture.
                sourceWidget->update();
                sourceWidget->repaint();
                QCoreApplication::processEvents(QEventLoop::AllEvents, 8);
                steps.push_back(QJsonObject{
                    {QStringLiteral("name"), QStringLiteral("prepare_paint")},
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("detail"), QStringLiteral("update+repaint+processEvents")}
                });
                QBuffer buffer(&bytes);
                buffer.open(QIODevice::WriteOnly);
                const bool sourceSaved = sourceWidget->grab().save(&buffer, "PNG");
                steps.push_back(QJsonObject{
                    {QStringLiteral("name"), QStringLiteral("capture_source")},
                    {QStringLiteral("ok"), sourceSaved},
                    {QStringLiteral("bytes"), static_cast<qint64>(bytes.size())}
                });

                if (bytes.isEmpty() && sourceWidget->winId() != 0) {
                    QScreen* screen = sourceWidget->screen();
                    if (!screen) {
                        screen = QGuiApplication::primaryScreen();
                    }
                    if (screen) {
                        const QPixmap nativePixmap = screen->grabWindow(sourceWidget->winId());
                        QByteArray nativeBytes;
                        QBuffer nativeBuffer(&nativeBytes);
                        nativeBuffer.open(QIODevice::WriteOnly);
                        const bool nativeSaved = !nativePixmap.isNull() && nativePixmap.save(&nativeBuffer, "PNG");
                        steps.push_back(QJsonObject{
                            {QStringLiteral("name"), QStringLiteral("capture_native_winid")},
                            {QStringLiteral("ok"), nativeSaved},
                            {QStringLiteral("bytes"), static_cast<qint64>(nativeBytes.size())}
                        });
                        if (!nativeBytes.isEmpty()) {
                            bytes = nativeBytes;
                        }
                    } else {
                        steps.push_back(QJsonObject{
                            {QStringLiteral("name"), QStringLiteral("capture_native_winid")},
                            {QStringLiteral("ok"), false},
                            {QStringLiteral("detail"), QStringLiteral("no screen available")}
                        });
                    }
                }

                if (bytes.isEmpty() && sourceWidget != m_window) {
                    QByteArray fallbackBytes;
                    QBuffer fallbackBuffer(&fallbackBytes);
                    fallbackBuffer.open(QIODevice::WriteOnly);
                    const bool fallbackSaved = m_window->grab().save(&fallbackBuffer, "PNG");
                    steps.push_back(QJsonObject{
                        {QStringLiteral("name"), QStringLiteral("capture_fallback_window")},
                        {QStringLiteral("ok"), fallbackSaved},
                        {QStringLiteral("bytes"), static_cast<qint64>(fallbackBytes.size())}
                    });
                    if (!fallbackBytes.isEmpty()) {
                        bytes = fallbackBytes;
                    }
                }
                return QJsonObject{
                    {QStringLiteral("ok"), !bytes.isEmpty()},
                    {QStringLiteral("source_effective"),
                     sourceWidget->objectName().isEmpty() ? QStringLiteral("window")
                                                          : sourceWidget->objectName()},
                    {QStringLiteral("steps"), steps},
                    {QStringLiteral("elapsed_ms"), timer.elapsed()},
                    {QStringLiteral("png_base64"), QString::fromLatin1(bytes.toBase64())}
                };
            })) {
            writeError(socket, 503, QStringLiteral("timed out waiting for screenshot"));
            return true;
        }
        const QByteArray pngBytes =
            QByteArray::fromBase64(capture.value(QStringLiteral("png_base64")).toString().toLatin1());
        if (pngBytes.isEmpty()) {
            writeError(socket, 500, QStringLiteral("failed to capture screenshot"));
            return true;
        }
        if (includeSteps) {
            writeJson(socket, 200, QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("source_requested"),
                 query.queryItemValue(QStringLiteral("source"), QUrl::FullyDecoded)},
                {QStringLiteral("source_effective"), capture.value(QStringLiteral("source_effective")).toString()},
                {QStringLiteral("elapsed_ms"), capture.value(QStringLiteral("elapsed_ms")).toInteger(0)},
                {QStringLiteral("steps"), capture.value(QStringLiteral("steps")).toArray()},
                {QStringLiteral("profile_cached"), m_lastProfileSnapshot},
                {QStringLiteral("png_base64"), QString::fromLatin1(pngBytes.toBase64())}
            });
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
        const QStringList actionPath = menuActionPathFromJson(body);
        if (actionPath.isEmpty()) {
            writeError(socket, 400, QStringLiteral("missing text/path"));
            return true;
        }

        QJsonObject response;
        if (!invokeOnUiThread(m_window, m_uiInvokeTimeoutMs, &response, [actionPath]() {
                QMenu* menu = activePopupMenu();
                if (!menu) {
                    return QJsonObject{
                        {QStringLiteral("ok"), false},
                        {QStringLiteral("error"), QStringLiteral("no active popup menu")}
                    };
                }
                QString menuError;
                QMenu* owningMenu = nullptr;
                QAction* action =
                    findMenuActionByPath(menu, actionPath, &menuError, &owningMenu);
                if (!action) {
                    return QJsonObject{
                        {QStringLiteral("ok"), false},
                        {QStringLiteral("error"), menuError},
                        {QStringLiteral("path"), QJsonArray::fromStringList(actionPath)},
                        {QStringLiteral("menu"), menuSnapshot(menu)}
                    };
                }
                const bool enabled = action->isEnabled();
                if (enabled) {
                    action->trigger();
                }
                return QJsonObject{
                    {QStringLiteral("ok"), enabled},
                    {QStringLiteral("text"), action->text()},
                    {QStringLiteral("path"), QJsonArray::fromStringList(actionPath)},
                    {QStringLiteral("enabled"), enabled},
                    {QStringLiteral("menu"), menuSnapshot(owningMenu ? owningMenu : menu)}
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

        if (offscreenPlatformActive() &&
            (id == QStringLiteral("transport.play") || id == QStringLiteral("transport.pause"))) {
            QJsonObject response;
            const int offscreenTransportTimeoutMs = qMax(m_uiInvokeTimeoutMs, 20000);
            if (!invokeOnUiThread(m_window, offscreenTransportTimeoutMs, &response, [this, id]() {
                    const QString effectiveId =
                        id == QStringLiteral("transport.pause")
                            ? QStringLiteral("transport.play")
                            : id;
                    QWidget* widget = findWidgetByObjectName(m_window, effectiveId);
                    auto* button = qobject_cast<QAbstractButton*>(widget);
                    if (!button) {
                        return QJsonObject{
                            {QStringLiteral("ok"), false},
                            {QStringLiteral("error"), QStringLiteral("widget not found")},
                            {QStringLiteral("id"), id}
                        };
                    }
                    if (!button->isEnabled()) {
                        return QJsonObject{
                            {QStringLiteral("ok"), false},
                            {QStringLiteral("error"), QStringLiteral("target button is disabled")},
                            {QStringLiteral("id"), id}
                        };
                    }
                    button->click();
                    return QJsonObject{
                        {QStringLiteral("ok"), true},
                        {QStringLiteral("id"), id},
                        {QStringLiteral("confirmed"), true}
                    };
                })) {
                writeError(socket, 503, QStringLiteral("timed out waiting for click-item"));
                return true;
            }

            writeJson(socket, response.value(QStringLiteral("ok")).toBool() ? 200 : 404, response);
            return true;
        }

        const int requestTimeoutMs =
            qBound(1, body.value(QStringLiteral("timeoutMs")).toInt(m_uiInvokeTimeoutMs), 10 * 60 * 1000);
        QJsonObject response;
        if (!invokeOnUiThread(m_window, requestTimeoutMs, &response, [this, id]() {
                QWidget* widget = findWidgetByObjectName(m_window, id);
                if (!widget) {
                    return QJsonObject{
                        {QStringLiteral("ok"), false},
                        {QStringLiteral("error"), QStringLiteral("widget not found")},
                        {QStringLiteral("id"), id}
                    };
                }

                const QJsonObject before = widgetSnapshot(widget);
                const bool lightweightOffscreenTransportClick =
                    offscreenPlatformActive() &&
                    (id == QStringLiteral("transport.play") || id == QStringLiteral("transport.pause"));
                const QJsonObject profileBefore =
                    (!lightweightOffscreenTransportClick && m_profilingCallback)
                        ? m_profilingCallback()
                        : QJsonObject{};

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
                const QJsonObject profileAfter =
                    (!lightweightOffscreenTransportClick && m_profilingCallback)
                        ? m_profilingCallback()
                        : QJsonObject{};
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
                                        "Face Stabilize is disabled: selected clip has no FaceDetections runtime binding. "
                                        "Generate FaceDetections for this clip first.");
                                } else {
                                    error = QStringLiteral("Face Stabilize is disabled by current UI state.");
                                }
                            }
                        } else if (text.startsWith(QStringLiteral("Tracking"), Qt::CaseInsensitive)) {
                            error = QStringLiteral(
                                "Tracking is disabled: select a speaker with an Auto-Track FaceDetections first.");
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
