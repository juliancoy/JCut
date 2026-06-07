#pragma once

#include <QJsonObject>
#include <QString>

namespace editor {

struct ActionResult {
    bool ok = false;
    QString code;
    QString message;
    QJsonObject details;

    static ActionResult success() {
        return ActionResult{true, QString(), QString(), QJsonObject{}};
    }

    static ActionResult success(const QJsonObject& details) {
        return ActionResult{true, QString(), QString(), details};
    }

    static ActionResult failure(const QString& code,
                                const QString& message,
                                const QJsonObject& details = QJsonObject{}) {
        return ActionResult{false, code, message, details};
    }
};

} // namespace editor
