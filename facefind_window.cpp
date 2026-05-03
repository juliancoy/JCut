#include "facefind_window.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSize>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace facefind {

AssignmentDialogResult showFaceFindWindow(
    QWidget* parent,
    const QVector<Candidate>& candidates,
    const QStringList& speakerIds,
    const QHash<QString, QString>& speakerLabels,
    const QStringList& suggestedSpeakerIds,
    const QStringList& autoSuggestedSpeakerIds,
    const QStringList& defaultSourceLabels)
{
    AssignmentDialogResult result;
    if (candidates.isEmpty() || speakerIds.isEmpty()) {
        return result;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("FaceFind: Assign Pre-cropped Face Candidates"));
    dialog.resize(920, 560);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    auto* intro = new QLabel(
        QStringLiteral("Review sampled face crops, then assign each crop to a transcript speaker. "
                       "Auto suggestions are preselected from transcript timing; manual speaker ID override is optional. "
                       "On apply, assignments fill only empty Ref1/Ref2 slots."),
        &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* table = new QTableWidget(&dialog);
    table->setColumnCount(9);
    table->setHorizontalHeaderLabels(
        QStringList{
            QStringLiteral("Crop"),
            QStringLiteral("Frame"),
            QStringLiteral("Track"),
            QStringLiteral("X"),
            QStringLiteral("Y"),
            QStringLiteral("Box"),
            QStringLiteral("Default Source"),
            QStringLiteral("Assign To (Auto)"),
            QStringLiteral("Manual Speaker ID (Override)")});
    table->setRowCount(candidates.size());
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Stretch);

    auto* allowNewSpeakerIdsCheck = new QCheckBox(
        QStringLiteral("Allow creating new speaker IDs from manual override"),
        &dialog);
    allowNewSpeakerIdsCheck->setChecked(false);
    allowNewSpeakerIdsCheck->setToolTip(
        QStringLiteral("Disabled by default to prevent typo-based speaker ID creation."));

    for (int row = 0; row < candidates.size(); ++row) {
        const Candidate& candidate = candidates.at(row);
        auto* cropItem = new QTableWidgetItem;
        cropItem->setFlags(cropItem->flags() & ~Qt::ItemIsEditable);
        cropItem->setTextAlignment(Qt::AlignCenter);
        if (!candidate.cropPath.isEmpty()) {
            QPixmap crop(candidate.cropPath);
            if (!crop.isNull()) {
                cropItem->setIcon(QIcon(crop.scaled(84, 84, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            }
        }
        cropItem->setToolTip(QStringLiteral("score=%1").arg(QString::number(candidate.score, 'f', 3)));
        cropItem->setSizeHint(QSize(88, 88));
        table->setItem(row, 0, cropItem);
        table->setItem(row, 1, new QTableWidgetItem(QString::number(candidate.frame)));
        table->setItem(row, 2, new QTableWidgetItem(
            candidate.trackId >= 0 ? QStringLiteral("T%1").arg(candidate.trackId) : QStringLiteral("-")));
        table->setItem(row, 3, new QTableWidgetItem(QString::number(candidate.x, 'f', 3)));
        table->setItem(row, 4, new QTableWidgetItem(QString::number(candidate.y, 'f', 3)));
        table->setItem(row, 5, new QTableWidgetItem(QString::number(candidate.box, 'f', 3)));

        const QString defaultSource =
            (row < defaultSourceLabels.size())
                ? defaultSourceLabels.at(row)
                : QStringLiteral("Auto (Timing)");
        auto* sourceItem = new QTableWidgetItem(defaultSource);
        sourceItem->setFlags(sourceItem->flags() & ~Qt::ItemIsEditable);
        sourceItem->setToolTip(
            QStringLiteral("Shows how the default assignment was selected."));
        table->setItem(row, 6, sourceItem);

        auto* assignCombo = new QComboBox(table);
        assignCombo->addItem(QStringLiteral("Skip"), QString());
        for (const QString& speakerId : speakerIds) {
            assignCombo->addItem(speakerLabels.value(speakerId, speakerId), speakerId);
        }
        table->setCellWidget(row, 7, assignCombo);
        if (row < suggestedSpeakerIds.size()) {
            const int suggestedIndex = assignCombo->findData(suggestedSpeakerIds.at(row));
            if (suggestedIndex >= 0) {
                assignCombo->setCurrentIndex(suggestedIndex);
            }
        }

        auto* manualIdEdit = new QLineEdit(table);
        manualIdEdit->setPlaceholderText(QStringLiteral("Optional: type speaker ID"));
        table->setCellWidget(row, 8, manualIdEdit);
        table->setRowHeight(row, 92);
    }
    layout->addWidget(table, 1);
    layout->addWidget(allowNewSpeakerIdsCheck);

    auto* defaultsRow = new QHBoxLayout;
    defaultsRow->setContentsMargins(0, 0, 0, 0);
    defaultsRow->setSpacing(6);
    auto* useAutoDefaultsButton = new QPushButton(QStringLiteral("Use Auto Suggestions"), &dialog);
    auto* usePersistedDefaultsButton = new QPushButton(QStringLiteral("Use Persisted Mapping"), &dialog);
    defaultsRow->addWidget(useAutoDefaultsButton);
    defaultsRow->addWidget(usePersistedDefaultsButton);
    defaultsRow->addStretch(1);
    layout->addLayout(defaultsRow);

    auto applyDefaults = [&](const QStringList& suggestedIds, const QString& sourceLabel) {
        for (int row = 0; row < table->rowCount(); ++row) {
            auto* combo = qobject_cast<QComboBox*>(table->cellWidget(row, 7));
            auto* sourceItem = table->item(row, 6);
            if (!combo) {
                continue;
            }
            const QString targetId = row < suggestedIds.size() ? suggestedIds.at(row) : QString();
            const int idx = combo->findData(targetId);
            combo->setCurrentIndex(idx >= 0 ? idx : 0);
            if (sourceItem) {
                sourceItem->setText(sourceLabel);
            }
        }
    };
    QObject::connect(useAutoDefaultsButton, &QPushButton::clicked, &dialog, [&]() {
        applyDefaults(autoSuggestedSpeakerIds, QStringLiteral("Auto (Timing)"));
    });
    QObject::connect(usePersistedDefaultsButton, &QPushButton::clicked, &dialog, [&]() {
        applyDefaults(suggestedSpeakerIds, QStringLiteral("Persisted (Human)"));
    });

    auto* actions = new QHBoxLayout;
    actions->addStretch(1);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
    auto* applyButton = new QPushButton(QStringLiteral("Apply Assignments"), &dialog);
    actions->addWidget(cancelButton);
    actions->addWidget(applyButton);
    layout->addLayout(actions);
    QObject::connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(applyButton, &QPushButton::clicked, &dialog, &QDialog::accept);

    QHash<QString, QString> speakerIdByLower;
    speakerIdByLower.reserve(speakerIds.size());
    for (const QString& speakerId : speakerIds) {
        speakerIdByLower.insert(speakerId.toLower(), speakerId);
    }
    const QRegularExpression speakerIdPattern(QStringLiteral("^[A-Za-z0-9_.-]{1,64}$"));

    while (true) {
        if (dialog.exec() != QDialog::Accepted) {
            return result;
        }

        result.assignmentsBySpeaker.clear();
        result.assignmentTableRows = QJsonArray();
        QStringList invalidIds;
        QStringList unknownIds;

        for (int row = 0; row < table->rowCount(); ++row) {
            auto* combo = qobject_cast<QComboBox*>(table->cellWidget(row, 7));
            auto* manualIdEdit = qobject_cast<QLineEdit*>(table->cellWidget(row, 8));
            if (!combo || !manualIdEdit) {
                continue;
            }
            const QString manualSpeakerId = manualIdEdit->text().trimmed();
            QString speakerId =
                manualSpeakerId.isEmpty() ? combo->currentData().toString().trimmed() : manualSpeakerId;
            const Candidate& c = candidates.at(row);

            QJsonObject assignmentRow{
                {QStringLiteral("row"), row},
                {QStringLiteral("frame"), static_cast<qint64>(c.frame)},
                {QStringLiteral("x"), c.x},
                {QStringLiteral("y"), c.y},
                {QStringLiteral("box"), c.box},
                {QStringLiteral("score"), c.score},
                {QStringLiteral("track_id"), c.trackId},
                {QStringLiteral("auto_selected"), combo->currentData().toString().trimmed()},
                {QStringLiteral("manual_override"), manualSpeakerId},
                {QStringLiteral("resolved_speaker_id"), speakerId}
            };
            if (speakerId.isEmpty()) {
                assignmentRow[QStringLiteral("decision")] = QStringLiteral("skipped");
                result.assignmentTableRows.push_back(assignmentRow);
                continue;
            }
            if (!speakerIdPattern.match(speakerId).hasMatch()) {
                if (!invalidIds.contains(speakerId)) {
                    invalidIds.push_back(speakerId);
                }
                assignmentRow[QStringLiteral("decision")] = QStringLiteral("invalid_id");
                result.assignmentTableRows.push_back(assignmentRow);
                continue;
            }
            const QString normalizedExistingId = speakerIdByLower.value(speakerId.toLower(), QString());
            if (!normalizedExistingId.isEmpty()) {
                speakerId = normalizedExistingId;
            } else if (!allowNewSpeakerIdsCheck->isChecked()) {
                if (!unknownIds.contains(speakerId)) {
                    unknownIds.push_back(speakerId);
                }
                assignmentRow[QStringLiteral("decision")] = QStringLiteral("unknown_id_blocked");
                result.assignmentTableRows.push_back(assignmentRow);
                continue;
            }
            assignmentRow[QStringLiteral("resolved_speaker_id")] = speakerId;
            assignmentRow[QStringLiteral("decision")] = QStringLiteral("accepted");
            result.assignmentTableRows.push_back(assignmentRow);
            result.assignmentsBySpeaker[speakerId].push_back(candidates.at(row));
        }

        if (!invalidIds.isEmpty()) {
            QMessageBox::warning(
                &dialog,
                QStringLiteral("Invalid Manual Speaker ID"),
                QStringLiteral("Manual speaker IDs must match `[A-Za-z0-9_.-]` and be 1-64 chars.\n\nInvalid: %1")
                    .arg(invalidIds.join(QStringLiteral(", "))));
            continue;
        }
        if (!unknownIds.isEmpty()) {
            QMessageBox::warning(
                &dialog,
                QStringLiteral("Unknown Speaker ID"),
                QStringLiteral("Manual speaker IDs must match existing transcript speakers unless "
                               "\"Allow creating new speaker IDs\" is enabled.\n\nUnknown: %1")
                    .arg(unknownIds.join(QStringLiteral(", "))));
            continue;
        }
        result.accepted = true;
        return result;
    }
}

} // namespace facefind
