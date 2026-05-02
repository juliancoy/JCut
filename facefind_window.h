#pragma once

#include <QHash>
#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <QVector>

class QWidget;

namespace facefind {

struct Candidate {
    int64_t frame = 0;
    qreal x = 0.5;
    qreal y = 0.5;
    qreal box = 0.20;
    qreal score = 0.0;
    int trackId = -1;
    QString cropPath;
};

struct AssignmentDialogResult {
    bool accepted = false;
    QHash<QString, QVector<Candidate>> assignmentsBySpeaker;
    QJsonArray assignmentTableRows;
};

AssignmentDialogResult showFaceFindWindow(
    QWidget* parent,
    const QVector<Candidate>& candidates,
    const QStringList& speakerIds,
    const QHash<QString, QString>& speakerLabels,
    const QStringList& suggestedSpeakerIds,
    const QStringList& autoSuggestedSpeakerIds,
    const QStringList& defaultSourceLabels);

} // namespace facefind
