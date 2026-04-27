#include "speakers_tab.h"
#include "speakers_tab_internal.h"

#include "transcript_engine.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
struct AiProposalRow {
    QString targetId;
    QString field;
    QString currentValue;
    QString proposedValue;
    qreal confidence = 0.0;
    QString rationale;
};

bool confirmAiProposals(QWidget* parent,
                        const QString& title,
                        const QString& summary,
                        const QVector<AiProposalRow>& proposals)
{
    if (proposals.isEmpty()) {
        return false;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.setModal(true);
    dialog.resize(860, 460);
    auto* rootLayout = new QVBoxLayout(&dialog);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(8);
    auto* intro = new QLabel(summary, &dialog);
    intro->setWordWrap(true);
    rootLayout->addWidget(intro);

    auto* table = new QTableWidget(&dialog);
    table->setColumnCount(6);
    table->setHorizontalHeaderLabels(
        QStringList{QStringLiteral("Target"),
                    QStringLiteral("Field"),
                    QStringLiteral("Current"),
                    QStringLiteral("Proposed"),
                    QStringLiteral("Confidence"),
                    QStringLiteral("Rationale")});
    table->setRowCount(proposals.size());
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    for (int i = 0; i < proposals.size(); ++i) {
        const AiProposalRow& p = proposals[i];
        table->setItem(i, 0, new QTableWidgetItem(p.targetId));
        table->setItem(i, 1, new QTableWidgetItem(p.field));
        table->setItem(i, 2, new QTableWidgetItem(p.currentValue));
        table->setItem(i, 3, new QTableWidgetItem(p.proposedValue));
        table->setItem(i, 4, new QTableWidgetItem(QStringLiteral("%1%").arg(qRound(p.confidence * 100.0))));
        table->setItem(i, 5, new QTableWidgetItem(p.rationale));
    }
    rootLayout->addWidget(table, 1);

    auto* actions = new QHBoxLayout;
    actions->addStretch(1);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
    auto* applyButton = new QPushButton(QStringLiteral("Apply"), &dialog);
    actions->addWidget(cancelButton);
    actions->addWidget(applyButton);
    rootLayout->addLayout(actions);
    QObject::connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(applyButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    return dialog.exec() == QDialog::Accepted;
}
} // namespace

bool SpeakersTab::runAiFindSpeakerNames()
{
    if (!ensureAiActionReady(QStringLiteral("Mine Transcript (AI)"))) {
        return false;
    }
    if (!m_loadedTranscriptDoc.isObject() || m_loadedTranscriptPath.isEmpty()) {
        return false;
    }
    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    QHash<QString, QStringList> wordsBySpeaker;
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segObj = segValue.toObject();
        const QString segmentSpeaker = segObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            QString speaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (speaker.isEmpty()) {
                speaker = segmentSpeaker;
            }
            const QString token = wordObj.value(QStringLiteral("word")).toString().trimmed();
            if (!speaker.isEmpty() && !token.isEmpty()) {
                wordsBySpeaker[speaker].push_back(token);
            }
        }
    }
    const QRegularExpression nameRe(QStringLiteral("\\b([A-Z][a-z]{1,20}\\s+[A-Z][a-z]{1,20})\\b"));
    QVector<AiProposalRow> proposals;
    for (auto it = wordsBySpeaker.constBegin(); it != wordsBySpeaker.constEnd(); ++it) {
        const QString speakerId = it.key();
        const QString text = it.value().join(QLatin1Char(' '));
        QHash<QString, int> counts;
        QRegularExpressionMatchIterator matchIt = nameRe.globalMatch(text);
        while (matchIt.hasNext()) {
            const QString name = matchIt.next().captured(1).trimmed();
            if (!name.isEmpty()) {
                counts[name] += 1;
            }
        }
        QString bestName = profiles.value(speakerId).toObject()
                               .value(QString(kTranscriptSpeakerNameKey)).toString().trimmed();
        int bestCount = 0;
        for (auto cIt = counts.constBegin(); cIt != counts.constEnd(); ++cIt) {
            if (cIt.value() > bestCount) {
                bestCount = cIt.value();
                bestName = cIt.key();
            }
        }
        if (bestName.isEmpty()) {
            continue;
        }
        const QJsonObject profile = profiles.value(speakerId).toObject();
        const QString currentName = profile.value(QString(kTranscriptSpeakerNameKey)).toString().trimmed();
        if (currentName == bestName) {
            continue;
        }
        const int totalCandidates = qMax(1, it.value().size());
        const qreal confidence = qBound<qreal>(0.30, static_cast<qreal>(bestCount) / totalCandidates, 0.98);
        proposals.push_back(AiProposalRow{
            speakerId,
            QStringLiteral("Name"),
            currentName,
            bestName,
            confidence,
            QStringLiteral("Most frequent full-name token pattern in speaker transcript words.")});
    }
    if (proposals.isEmpty()) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Find Speaker Names"),
                                 QStringLiteral("No stronger speaker-name candidates were found."));
        return false;
    }

    QVector<AiProposalRow> overwriteExistingNameProposals;
    QVector<AiProposalRow> autoApplyUnnamedProposals;
    overwriteExistingNameProposals.reserve(proposals.size());
    autoApplyUnnamedProposals.reserve(proposals.size());
    for (const AiProposalRow& proposal : std::as_const(proposals)) {
        const bool hasExistingName = !proposal.currentValue.trimmed().isEmpty();
        if (hasExistingName) {
            overwriteExistingNameProposals.push_back(proposal);
        } else {
            autoApplyUnnamedProposals.push_back(proposal);
        }
    }

    if (!overwriteExistingNameProposals.isEmpty()) {
        if (!confirmAiProposals(
                nullptr,
                QStringLiteral("Approve AI Name Overwrites"),
                QStringLiteral("Approve AI updates for speakers that already have names."),
                overwriteExistingNameProposals)) {
            return false;
        }
    }

    QVector<AiProposalRow> appliedProposals = autoApplyUnnamedProposals;
    appliedProposals += overwriteExistingNameProposals;

    for (const AiProposalRow& proposal : std::as_const(appliedProposals)) {
        QJsonObject profile = profiles.value(proposal.targetId).toObject();
        profile[QString(kTranscriptSpeakerNameKey)] = proposal.proposedValue;
        profiles[proposal.targetId] = profile;
    }
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);
    editor::TranscriptEngine engine;
    if (!engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Find Speaker Names"),
                             QStringLiteral("Failed to save transcript after AI speaker-name pass."));
        return false;
    }
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    QMessageBox::information(
        nullptr,
        QStringLiteral("Find Speaker Names"),
        QStringLiteral("AI updated %1 speaker name(s): %2 auto-filled unnamed speaker(s), %3 approved overwrite(s).")
            .arg(appliedProposals.size())
            .arg(autoApplyUnnamedProposals.size())
            .arg(overwriteExistingNameProposals.size()));
    refresh();
    return true;
}

bool SpeakersTab::runAiFindOrganizations()
{
    if (!ensureAiActionReady(QStringLiteral("Find Organizations (AI)"))) {
        return false;
    }
    if (!m_loadedTranscriptDoc.isObject() || m_loadedTranscriptPath.isEmpty()) {
        return false;
    }
    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    QHash<QString, QStringList> wordsBySpeaker;
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segObj = segValue.toObject();
        const QString segmentSpeaker = segObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            QString speaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (speaker.isEmpty()) {
                speaker = segmentSpeaker;
            }
            const QString token = wordObj.value(QStringLiteral("word")).toString().trimmed();
            if (!speaker.isEmpty() && !token.isEmpty()) {
                wordsBySpeaker[speaker].push_back(token);
            }
        }
    }
    const QRegularExpression orgRe(QStringLiteral(
        "\\b([A-Z][A-Za-z&]{2,}(?:\\s+[A-Z][A-Za-z&]{2,}){0,4}\\s+"
        "(Council|Committee|Party|University|College|County|City|Campaign|Association))\\b"));
    QVector<AiProposalRow> proposals;
    for (auto it = wordsBySpeaker.constBegin(); it != wordsBySpeaker.constEnd(); ++it) {
        const QString speakerId = it.key();
        const QString text = it.value().join(QLatin1Char(' '));
        QHash<QString, int> counts;
        QRegularExpressionMatchIterator matchIt = orgRe.globalMatch(text);
        while (matchIt.hasNext()) {
            const QString org = matchIt.next().captured(1).trimmed();
            if (!org.isEmpty()) {
                counts[org] += 1;
            }
        }
        QString bestOrg;
        int bestCount = 0;
        for (auto cIt = counts.constBegin(); cIt != counts.constEnd(); ++cIt) {
            if (cIt.value() > bestCount) {
                bestCount = cIt.value();
                bestOrg = cIt.key();
            }
        }
        if (bestOrg.isEmpty()) {
            continue;
        }
        const QJsonObject profile = profiles.value(speakerId).toObject();
        const QString key = QStringLiteral("organization");
        const QString currentOrg = profile.value(key).toString().trimmed();
        if (currentOrg == bestOrg) {
            continue;
        }
        const int totalMentions = qMax(1, it.value().size());
        const qreal confidence = qBound<qreal>(0.30, static_cast<qreal>(bestCount) / totalMentions, 0.98);
        proposals.push_back(AiProposalRow{
            speakerId,
            QStringLiteral("Organization"),
            currentOrg,
            bestOrg,
            confidence,
            QStringLiteral("Organization suffix match (Council/Committee/University/etc.) with highest frequency.")});
    }
    if (proposals.isEmpty()) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Find Organizations"),
                                 QStringLiteral("No organization candidates were found."));
        return false;
    }

    if (!confirmAiProposals(
            nullptr,
            QStringLiteral("Find Organizations (AI)"),
            QStringLiteral("Review proposed organization assignments before applying."),
            proposals)) {
        return false;
    }

    for (const AiProposalRow& proposal : std::as_const(proposals)) {
        QJsonObject profile = profiles.value(proposal.targetId).toObject();
        profile[QStringLiteral("organization")] = proposal.proposedValue;
        profiles[proposal.targetId] = profile;
    }
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);
    editor::TranscriptEngine engine;
    if (!engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Find Organizations"),
                             QStringLiteral("Failed to save transcript after organization pass."));
        return false;
    }
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    refresh();
    return true;
}

bool SpeakersTab::runAiCleanSpuriousAssignments()
{
    if (!ensureAiActionReady(QStringLiteral("Clean Assignments (AI)"))) {
        return false;
    }
    if (!m_loadedTranscriptDoc.isObject() || m_loadedTranscriptPath.isEmpty()) {
        return false;
    }
    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    QHash<QString, int> wordCountBySpeaker;
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segObj = segValue.toObject();
        const QString segmentSpeaker = segObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            QString speaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (speaker.isEmpty()) {
                speaker = segmentSpeaker;
            }
            if (!speaker.isEmpty()) {
                wordCountBySpeaker[speaker] += 1;
            }
        }
    }
    if (wordCountBySpeaker.isEmpty()) {
        return false;
    }
    QString dominantSpeaker;
    int dominantCount = 0;
    int totalWords = 0;
    for (auto it = wordCountBySpeaker.constBegin(); it != wordCountBySpeaker.constEnd(); ++it) {
        totalWords += it.value();
        if (it.value() > dominantCount) {
            dominantCount = it.value();
            dominantSpeaker = it.key();
        }
    }
    QSet<QString> spuriousSpeakers;
    for (auto it = wordCountBySpeaker.constBegin(); it != wordCountBySpeaker.constEnd(); ++it) {
        const double ratio = static_cast<double>(it.value()) / qMax(1, totalWords);
        if (it.value() <= 1 || ratio < 0.0025) {
            spuriousSpeakers.insert(it.key());
        }
    }
    if (spuriousSpeakers.isEmpty()) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Clean Assignments"),
                                 QStringLiteral("No spurious one-off speaker assignments found."));
        return false;
    }

    QVector<AiProposalRow> proposals;
    for (int i = 0; i < segments.size(); ++i) {
        QJsonObject segObj = segments[i].toObject();
        const QString segmentSpeaker = segObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        QJsonArray words = segObj.value(QStringLiteral("words")).toArray();
        for (int w = 0; w < words.size(); ++w) {
            QJsonObject wordObj = words[w].toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            const QString currentSpeaker =
                wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (!spuriousSpeakers.contains(currentSpeaker)) {
                continue;
            }
            const QString replacement =
                !segmentSpeaker.isEmpty() && !spuriousSpeakers.contains(segmentSpeaker)
                    ? segmentSpeaker
                    : dominantSpeaker;
            if (replacement.isEmpty() || replacement == currentSpeaker) {
                continue;
            }
            const qreal confidence = qBound<qreal>(
                0.50,
                1.0 - static_cast<qreal>(wordCountBySpeaker.value(currentSpeaker, 0)) /
                          qMax<qreal>(1.0, static_cast<qreal>(totalWords)),
                0.99);
            proposals.push_back(AiProposalRow{
                QStringLiteral("segment %1 word %2").arg(i + 1).arg(w + 1),
                QStringLiteral("Speaker"),
                currentSpeaker,
                replacement,
                confidence,
                QStringLiteral("One-off or very-low-ratio speaker label replaced with segment/dominant speaker.")});
        }
    }

    if (proposals.isEmpty()) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Clean Assignments"),
                                 QStringLiteral("No speaker assignments needed cleanup."));
        return false;
    }

    if (!confirmAiProposals(
            nullptr,
            QStringLiteral("Clean Spurious Assignments (AI)"),
            QStringLiteral("Review proposed speaker reassignments before applying."),
            proposals)) {
        return false;
    }

    int reassignedCount = 0;
    for (int i = 0; i < segments.size(); ++i) {
        QJsonObject segObj = segments[i].toObject();
        const QString segmentSpeaker = segObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        QJsonArray words = segObj.value(QStringLiteral("words")).toArray();
        for (int w = 0; w < words.size(); ++w) {
            QJsonObject wordObj = words[w].toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            const QString currentSpeaker =
                wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (!spuriousSpeakers.contains(currentSpeaker)) {
                continue;
            }
            const QString replacement =
                !segmentSpeaker.isEmpty() && !spuriousSpeakers.contains(segmentSpeaker)
                    ? segmentSpeaker
                    : dominantSpeaker;
            if (replacement.isEmpty() || replacement == currentSpeaker) {
                continue;
            }
            wordObj[QString(kTranscriptWordSpeakerKey)] = replacement;
            words[w] = wordObj;
            ++reassignedCount;
        }
        segObj[QStringLiteral("words")] = words;
        segments[i] = segObj;
    }

    root[QStringLiteral("segments")] = segments;
    m_loadedTranscriptDoc.setObject(root);
    editor::TranscriptEngine engine;
    if (!engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Clean Assignments"),
                             QStringLiteral("Failed to save transcript after assignment cleanup."));
        return false;
    }
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    refresh();
    QMessageBox::information(nullptr,
                             QStringLiteral("Clean Assignments"),
                             QStringLiteral("Reassigned %1 one-off speaker word labels.")
                                 .arg(reassignedCount));
    return true;
}
