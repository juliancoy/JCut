#include "speakers_tab.h"
#include "speakers_tab_internal.h"
#include "speaker_document_edit_ops.h"

#include "json_io_utils.h"
#include "transcript_mining_core.h"
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
#include <QStringList>
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

QHash<QString, QStringList> transcriptWordsBySpeaker(const QJsonArray& segments)
{
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
    return wordsBySpeaker;
}

bool looksLikeOrganizationName(const QString& value)
{
    const QString candidate = value.trimmed();
    if (candidate.isEmpty()) {
        return false;
    }

    static const QRegularExpression orgWordRe(
        QStringLiteral("\\b(agency|association|bank|campaign|center|centre|city|college|committee|"
                       "company|corporation|council|county|department|foundation|government|group|"
                       "hospital|inc|institute|llc|ltd|ministry|network|office|organization|party|"
                       "school|studio|team|union|university)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression legalSuffixRe(
        QStringLiteral("\\b(inc\\.?|llc|ltd\\.?|corp\\.?|co\\.?)$"),
        QRegularExpression::CaseInsensitiveOption);

    return candidate.contains(QLatin1Char('&')) ||
           orgWordRe.match(candidate).hasMatch() ||
           legalSuffixRe.match(candidate).hasMatch();
}

bool looksLikeSpeakerPersonName(const QString& value)
{
    const QString candidate = value.trimmed();
    if (candidate.isEmpty() || looksLikeOrganizationName(candidate)) {
        return false;
    }

    const QStringList parts =
        candidate.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.size() < 2 || parts.size() > 4) {
        return false;
    }

    static const QSet<QString> disallowedLowerWords{
        QStringLiteral("and"), QStringLiteral("for"), QStringLiteral("from"), QStringLiteral("of"),
        QStringLiteral("the"), QStringLiteral("to"), QStringLiteral("with")
    };
    static const QRegularExpression namePartRe(
        QStringLiteral("^[A-Z][A-Za-z'\\-]{1,30}$"));
    for (const QString& part : parts) {
        if (disallowedLowerWords.contains(part.toLower())) {
            return false;
        }
        if (!namePartRe.match(part).hasMatch()) {
            return false;
        }
    }

    return true;
}

QString replacementSpeakerForCleanup(const QString& segmentSpeaker,
                                     const QSet<QString>& spuriousSpeakers,
                                     const QString& dominantSpeaker,
                                     const QString& currentSpeaker) {
    const QString replacement =
        !segmentSpeaker.isEmpty() && !spuriousSpeakers.contains(segmentSpeaker)
            ? segmentSpeaker
            : dominantSpeaker;
    if (replacement.isEmpty() || replacement == currentSpeaker) {
        return QString();
    }
    return replacement;
}
} // namespace

bool SpeakersTab::runAiFindSpeakerNames()
{
    if (!ensureAiActionReady(QStringLiteral("Mine Transcript (AI)"))) {
        return false;
    }
    if (!m_transcriptSession.hasObjectDocument() || m_transcriptSession.transcriptPath().isEmpty()) {
        return false;
    }
    QJsonObject root = m_transcriptSession.rootObject();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QVector<AiProposalRow> proposals;
    for (const jcut::TranscriptMiningProposal& proposal :
         jcut::mineTranscriptSpeakerNames(jcut::jsonio::toJson(root))) {
        proposals.push_back(AiProposalRow{
            QString::fromStdString(proposal.targetId),
            QStringLiteral("Name"),
            QString::fromStdString(proposal.currentValue),
            QString::fromStdString(proposal.proposedValue),
            proposal.confidence,
            QString::fromStdString(proposal.rationale)});
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

    QVector<SpeakerFieldValueUpdate> updates;
    updates.reserve(appliedProposals.size());
    for (const AiProposalRow& proposal : std::as_const(appliedProposals)) {
        updates.push_back(SpeakerFieldValueUpdate{proposal.targetId, proposal.proposedValue});
    }
    const SpeakerDocumentEditResult result =
        speaker_document_edit_ops::applyProfileStringFieldUpdates(
            m_transcriptSession, QString(kTranscriptSpeakerNameKey), updates);
    if (!result.ok || !result.changed || !saveLoadedTranscriptDocument()) {
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
    m_speakersTableRefreshSignature.clear();
    m_speakerSectionsTableRefreshSignature.clear();
    refreshTranscriptSpeakerViews(selectedSpeakerId(), false);
    return true;
}

bool SpeakersTab::runAiFindOrganizations()
{
    if (!ensureAiActionReady(QStringLiteral("Find Organizations (AI)"))) {
        return false;
    }
    if (!m_transcriptSession.hasObjectDocument() || m_transcriptSession.transcriptPath().isEmpty()) {
        return false;
    }
    QJsonObject root = m_transcriptSession.rootObject();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QVector<AiProposalRow> proposals;
    for (const jcut::TranscriptMiningProposal& proposal :
         jcut::mineTranscriptOrganizations(jcut::jsonio::toJson(root))) {
        proposals.push_back(AiProposalRow{
            QString::fromStdString(proposal.targetId),
            QStringLiteral("Organization"),
            QString::fromStdString(proposal.currentValue),
            QString::fromStdString(proposal.proposedValue),
            proposal.confidence,
            QString::fromStdString(proposal.rationale)});
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

    QVector<SpeakerFieldValueUpdate> updates;
    updates.reserve(proposals.size());
    for (const AiProposalRow& proposal : std::as_const(proposals)) {
        updates.push_back(SpeakerFieldValueUpdate{proposal.targetId, proposal.proposedValue});
    }
    const SpeakerDocumentEditResult result =
        speaker_document_edit_ops::applyProfileStringFieldUpdates(
            m_transcriptSession, QString(kTranscriptSpeakerOrganizationKey), updates);
    if (!result.ok || !result.changed || !saveLoadedTranscriptDocument()) {
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
    m_speakersTableRefreshSignature.clear();
    m_speakerSectionsTableRefreshSignature.clear();
    refreshTranscriptSpeakerViews(selectedSpeakerId(), false);
    return true;
}

bool SpeakersTab::runAiCleanSpuriousAssignments()
{
    if (!ensureAiActionReady(QStringLiteral("Clean Assignments (AI)"))) {
        return false;
    }
    if (!m_transcriptSession.hasObjectDocument() || m_transcriptSession.transcriptPath().isEmpty()) {
        return false;
    }
    QJsonObject root = m_transcriptSession.rootObject();
    const std::vector<jcut::TranscriptMiningProposal> coreProposals =
        jcut::mineSpuriousSpeakerAssignments(jcut::jsonio::toJson(root));
    if (coreProposals.empty()) {
        QMessageBox::information(
            nullptr,
            QStringLiteral("Clean Assignments"),
            QStringLiteral("No spurious one-off speaker assignments found."));
        return false;
    }
    QVector<AiProposalRow> reviewRows;
    reviewRows.reserve(static_cast<qsizetype>(coreProposals.size()));
    for (const jcut::TranscriptMiningProposal& proposal : coreProposals) {
        reviewRows.push_back({
            QString::fromStdString(proposal.targetId),
            QStringLiteral("Speaker"),
            QString::fromStdString(proposal.currentValue),
            QString::fromStdString(proposal.proposedValue),
            proposal.confidence,
            QString::fromStdString(proposal.rationale)});
    }
    if (!confirmAiProposals(
            nullptr,
            QStringLiteral("Clean Spurious Assignments (AI)"),
            QStringLiteral("Review proposed speaker reassignments before applying."),
            reviewRows)) {
        return false;
    }
    nlohmann::json neutralRoot = jcut::jsonio::toJson(root);
    std::string applyError;
    if (!jcut::applyTranscriptMiningProposals(
            &neutralRoot, coreProposals, &applyError)) {
        QMessageBox::warning(
            nullptr,
            QStringLiteral("Clean Assignments"),
            QString::fromStdString(applyError));
        return false;
    }
    root = jcut::jsonio::fromJson(neutralRoot).toObject();
    const bool neutralUpdated = updateLoadedTranscriptDocument(
        [&](QJsonObject& loadedRoot) {
            loadedRoot = root;
            return true;
        });
    if (!neutralUpdated || !saveLoadedTranscriptDocument()) {
        QMessageBox::warning(
            nullptr,
            QStringLiteral("Clean Assignments"),
            QStringLiteral(
                "Failed to save transcript after assignment cleanup."));
        return false;
    }
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
    m_speakersTableRefreshSignature.clear();
    m_speakerSectionsTableRefreshSignature.clear();
    refreshTranscriptSpeakerViews(selectedSpeakerId(), false);
    QMessageBox::information(
        nullptr,
        QStringLiteral("Clean Assignments"),
        QStringLiteral("Reassigned %1 one-off speaker word labels.")
            .arg(coreProposals.size()));
    return true;

#if 0 // Replaced by transcript_mining_core above.
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
            const QString replacement = replacementSpeakerForCleanup(
                segmentSpeaker, spuriousSpeakers, dominantSpeaker, currentSpeaker);
            if (replacement.isEmpty()) {
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
            const QString replacement = replacementSpeakerForCleanup(
                segmentSpeaker, spuriousSpeakers, dominantSpeaker, currentSpeaker);
            if (replacement.isEmpty()) {
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
    const bool updated = updateLoadedTranscriptDocument([&](QJsonObject& loadedRoot) {
            loadedRoot = root;
            return true;
        });
    if (!updated || !saveLoadedTranscriptDocument()) {
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
    m_speakersTableRefreshSignature.clear();
    m_speakerSectionsTableRefreshSignature.clear();
    refreshTranscriptSpeakerViews(selectedSpeakerId(), false);
    QMessageBox::information(nullptr,
                             QStringLiteral("Clean Assignments"),
                             QStringLiteral("Reassigned %1 one-off speaker word labels.")
                                 .arg(reassignedCount));
    return true;
#endif
}
#include "speaker_document_edit_ops.h"
