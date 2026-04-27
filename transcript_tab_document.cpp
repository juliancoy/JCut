#include "transcript_tab.h"

#include "clip_serialization.h"
#include "editor_shared.h"

#include <QAbstractItemView>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMenu>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>

namespace {
const QLatin1String kTranscriptWordSkippedKey("skipped");
const QLatin1String kTranscriptWordSpeakerKey("speaker");
const QLatin1String kTranscriptSegmentSpeakerKey("speaker");
const QLatin1String kTranscriptWordEditsKey("transcript_edits");
const QLatin1String kTranscriptDeletedEditsCountKey("transcript_deleted_edits");
const QLatin1String kTranscriptEditTimingTag("timing");
const QLatin1String kTranscriptEditTextTag("text");
const QLatin1String kTranscriptEditSkipTag("skip");
const QLatin1String kTranscriptEditInsertedTag("inserted");
const QLatin1String kTranscriptWordRenderOrderKey("render_order");
const QLatin1String kTranscriptWordOriginalSegmentKey("original_segment_index");
const QLatin1String kTranscriptWordOriginalWordKey("original_word_index");
const QLatin1String kAllSpeakersFilterValue("__all__");

enum TranscriptTableColumn {
    kTranscriptColSourceStart = 0,
    kTranscriptColSourceEnd = 1,
    kTranscriptColSpeaker = 2,
    kTranscriptColText = 3,
    kTranscriptColEdits = 4
};

constexpr int kEditFlagNone = 0;
constexpr int kEditFlagTiming = 1 << 0;
constexpr int kEditFlagText = 1 << 1;
constexpr int kEditFlagSkip = 1 << 2;
constexpr int kEditFlagInserted = 1 << 3;

int transcriptEditFlagsFromWordObject(const QJsonObject& word)
{
    int flags = kEditFlagNone;
    const QJsonArray editArray = word.value(QString(kTranscriptWordEditsKey)).toArray();
    for (const QJsonValue& value : editArray) {
        const QString tag = value.toString();
        if (tag == QString(kTranscriptEditTimingTag)) {
            flags |= kEditFlagTiming;
        } else if (tag == QString(kTranscriptEditTextTag)) {
            flags |= kEditFlagText;
        } else if (tag == QString(kTranscriptEditSkipTag)) {
            flags |= kEditFlagSkip;
        } else if (tag == QString(kTranscriptEditInsertedTag)) {
            flags |= kEditFlagInserted;
        }
    }
    return flags;
}

QString transcriptCutStateLabel(bool mutableCut)
{
    return mutableCut ? QStringLiteral("Editable Cut") : QStringLiteral("Original (Immutable)");
}

QString transcriptFollowStateLabel(bool enabled)
{
    return enabled ? QStringLiteral("Follow Playback: On") : QStringLiteral("Follow Playback: Off");
}

} // namespace

void TranscriptTab::loadTranscriptFile(const TimelineClip& clip)
{
    m_loadedClipFilePath = clip.filePath;

    QString baseEditablePath;
    if (!ensureEditableTranscriptForClipFile(clip.filePath, &baseEditablePath)) {
        baseEditablePath = transcriptWorkingPathForClipFile(clip.filePath);
    }

    const QString originalPath = originalTranscriptPathForClip(clip.filePath);
    QString transcriptPath = baseEditablePath;
    if (m_widgets.transcriptScriptVersionCombo) {
        const QString selectedPath = m_widgets.transcriptScriptVersionCombo->currentData().toString();
        if (!selectedPath.isEmpty() && QFileInfo::exists(selectedPath)) {
            transcriptPath = selectedPath;
        }
    }
    refreshScriptVersionSelector(clip.filePath, transcriptPath);
    setActiveTranscriptPathForClipFile(clip.filePath, transcriptPath);

    QFile transcriptFile(transcriptPath);
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        if (transcriptPath != baseEditablePath) {
            transcriptPath = baseEditablePath;
            refreshScriptVersionSelector(clip.filePath, transcriptPath);
            setActiveTranscriptPathForClipFile(clip.filePath, transcriptPath);
            transcriptFile.setFileName(transcriptPath);
            if (transcriptFile.open(QIODevice::ReadOnly)) {
                // continue with fallback file
            } else {
                if (m_widgets.transcriptInspectorDetailsLabel) {
                    m_widgets.transcriptInspectorDetailsLabel->setText(QStringLiteral("No transcript file found."));
                }
                return;
            }
        } else {
        if (m_widgets.transcriptInspectorDetailsLabel) {
            m_widgets.transcriptInspectorDetailsLabel->setText(QStringLiteral("No transcript file found."));
        }
        return;
        }
    }

    QJsonParseError parseError;
    const QJsonDocument transcriptDoc = QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
        if (m_widgets.transcriptInspectorDetailsLabel) {
            m_widgets.transcriptInspectorDetailsLabel->setText(QStringLiteral("Invalid transcript JSON file."));
        }
        return;
    }

    m_loadedTranscriptPath = transcriptPath;
    m_loadedTranscriptDoc = transcriptDoc;

    const QJsonArray segments = transcriptDoc.object().value(QStringLiteral("segments")).toArray();
    int totalWords = 0;
    int editedWords = 0;
    for (const QJsonValue& segValue : segments) {
        const QJsonArray words = segValue.toObject().value(QStringLiteral("words")).toArray();
        totalWords += words.size();
        for (const QJsonValue& wordValue : words) {
            if (transcriptEditFlagsFromWordObject(wordValue.toObject()) != TranscriptRow::EditNone) {
                ++editedWords;
            }
        }
    }
    const int deletedEdits = transcriptDoc.object().value(QString(kTranscriptDeletedEditsCountKey)).toInt(0);
    const bool mutableCut = activeCutMutable();
    const QString activeCutName =
        m_widgets.transcriptScriptVersionCombo
            ? m_widgets.transcriptScriptVersionCombo->currentText().trimmed()
            : QString();
    const QString resolvedCutName =
        activeCutName.isEmpty() ? scriptVersionLabelForPath(transcriptPath, clip.filePath) : activeCutName;
    const bool followEnabled =
        m_widgets.transcriptFollowCurrentWordCheckBox &&
        m_widgets.transcriptFollowCurrentWordCheckBox->isChecked();
    const QString speechFilterState =
        (m_widgets.speechFilterEnabledCheckBox && m_widgets.speechFilterEnabledCheckBox->isChecked())
            ? QStringLiteral("Speech Filter: On")
            : QStringLiteral("Speech Filter: Off");
    const int speakerCount = [&segments]() {
        QSet<QString> speakers;
        for (const QJsonValue& segValue : segments) {
            const QJsonObject segmentObj = segValue.toObject();
            const QString segmentSpeaker = segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
            if (!segmentSpeaker.isEmpty()) {
                speakers.insert(segmentSpeaker);
            }
            const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
            for (const QJsonValue& wordValue : words) {
                const QString wordSpeaker =
                    wordValue.toObject().value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
                if (!wordSpeaker.isEmpty()) {
                    speakers.insert(wordSpeaker);
                }
            }
        }
        return speakers.size();
    }();

    if (m_widgets.transcriptInspectorDetailsLabel) {
        m_widgets.transcriptInspectorDetailsLabel->setText(
            QStringLiteral("Now Editing: %1 | %2\n"
                           "Stats: %3 words, %4 segments, %5 speakers, %6 edited, %7 deleted\n"
                           "State: %8 | %9 | %10\n"
                           "Workflow: 1) Select/Create Cut  2) Edit Transcript/Speakers  3) Preview  4) Render")
                .arg(clip.label)
                .arg(resolvedCutName)
                .arg(totalWords)
                .arg(segments.size())
                .arg(speakerCount)
                .arg(editedWords)
                .arg(deletedEdits)
                .arg(transcriptCutStateLabel(mutableCut))
                .arg(transcriptFollowStateLabel(followEnabled))
                .arg(speechFilterState));
    }

    QVector<TranscriptRow> allRows = parseTranscriptRows(segments, m_transcriptPrependMs, m_transcriptPostpendMs);
    adjustOverlappingRows(allRows);
    insertGapRows(&allRows);
    computeRenderFrames(&allRows);
    if (showOutsideCutLinesEnabled() && transcriptPath != originalPath && QFileInfo::exists(originalPath)) {
        QFile originalFile(originalPath);
        if (originalFile.open(QIODevice::ReadOnly)) {
            QJsonParseError originalParseError;
            const QJsonDocument originalDoc = QJsonDocument::fromJson(originalFile.readAll(), &originalParseError);
            if (originalParseError.error == QJsonParseError::NoError && originalDoc.isObject()) {
                const QVector<TranscriptRow> originalRows = parseTranscriptRows(
                    originalDoc.object().value(QStringLiteral("segments")).toArray(),
                    m_transcriptPrependMs,
                    m_transcriptPostpendMs);
                QSet<QString> activeKeys;
                for (const TranscriptRow& row : std::as_const(allRows)) {
                    activeKeys.insert(originalWordKey(row));
                }
                for (TranscriptRow row : originalRows) {
                    if (activeKeys.contains(originalWordKey(row))) {
                        continue;
                    }
                    row.isOutsideActiveCut = true;
                    row.renderStartFrame = -1;
                    row.renderEndFrame = -1;
                    allRows.push_back(row);
                }
            }
        }
    }
    refreshSpeakerFilter(allRows);
    QVector<TranscriptRow> displayRows = filteredRowsForSpeaker(allRows);
    populateTable(displayRows);

    const bool canDragRows = activeSpeakerFilter() == QString(kAllSpeakersFilterValue) &&
                             activeCutMutable() &&
                             !showOutsideCutLinesEnabled();
    if (m_widgets.transcriptTable) {
        const bool mutableCut = activeCutMutable();
        m_widgets.transcriptTable->setDragEnabled(canDragRows);
        m_widgets.transcriptTable->setDragDropMode(
            canDragRows ? QAbstractItemView::InternalMove : QAbstractItemView::NoDragDrop);
        m_widgets.transcriptTable->setEditTriggers(
            mutableCut ? (QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed)
                       : QAbstractItemView::NoEditTriggers);
        m_widgets.transcriptTable->setToolTip(
            canDragRows
                ? QStringLiteral("Drag rows to change render order.")
                : QStringLiteral("Switch speaker filter to All Speakers to reorder rows."));
    }
}

QVector<TranscriptTab::TranscriptRow> TranscriptTab::parseTranscriptRows(const QJsonArray& segments,
                                                                         int prependMs,
                                                                         int postpendMs)
{
    struct OrderedRow {
        TranscriptRow row;
        int renderOrder = -1;
        int fallbackOrder = 0;
    };

    QVector<OrderedRow> orderedRows;
    QVector<TranscriptRow> rows;
    const double prependSeconds = prependMs / 1000.0;
    const double postpendSeconds = postpendMs / 1000.0;
    int fallbackOrder = 0;

    for (int segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
        const QJsonObject segmentObj = segments.at(segmentIndex).toObject();
        const QString segmentSpeaker = segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
            const QJsonObject word = words.at(wordIndex).toObject();
            const QString text = word.value(QStringLiteral("word")).toString();
            const double rawStartTime = word.value(QStringLiteral("start")).toDouble(-1.0);
            const double rawEndTime = word.value(QStringLiteral("end")).toDouble(-1.0);
            if (text.trimmed().isEmpty() || rawStartTime < 0.0 || rawEndTime < rawStartTime) continue;

            const double adjustedStartTime = qMax(0.0, rawStartTime - prependSeconds);
            const double adjustedEndTime = qMax(adjustedStartTime, rawEndTime + postpendSeconds);

            TranscriptRow row;
            row.startFrame = qMax<int64_t>(0, static_cast<int64_t>(std::floor(adjustedStartTime * kTimelineFps)));
            row.endFrame = qMax<int64_t>(row.startFrame,
                                         static_cast<int64_t>(std::ceil(adjustedEndTime * kTimelineFps)) - 1);
            row.speaker = word.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (row.speaker.isEmpty()) {
                row.speaker = segmentSpeaker;
            }
            if (row.speaker.isEmpty()) {
                row.speaker = QStringLiteral("Unknown");
            }
            row.text = text;
            row.isSkipped = word.value(kTranscriptWordSkippedKey).toBool(false);
            row.editFlags = transcriptEditFlagsFromWordObject(word);
            row.segmentIndex = segmentIndex;
            row.wordIndex = wordIndex;
            row.originalSegmentIndex = word.value(QString(kTranscriptWordOriginalSegmentKey)).toInt(segmentIndex);
            row.originalWordIndex = word.value(QString(kTranscriptWordOriginalWordKey)).toInt(wordIndex);
            OrderedRow ordered;
            ordered.row = row;
            ordered.renderOrder = word.value(QString(kTranscriptWordRenderOrderKey)).toInt(-1);
            ordered.fallbackOrder = fallbackOrder++;
            orderedRows.push_back(ordered);
        }
    }

    std::sort(orderedRows.begin(), orderedRows.end(), [](const OrderedRow& a, const OrderedRow& b) {
        const bool aHasOrder = a.renderOrder >= 0;
        const bool bHasOrder = b.renderOrder >= 0;
        if (aHasOrder != bHasOrder) {
            return aHasOrder;
        }
        if (aHasOrder && a.renderOrder != b.renderOrder) {
            return a.renderOrder < b.renderOrder;
        }
        if (a.row.startFrame != b.row.startFrame) {
            return a.row.startFrame < b.row.startFrame;
        }
        return a.fallbackOrder < b.fallbackOrder;
    });

    rows.reserve(orderedRows.size());
    for (const OrderedRow& ordered : std::as_const(orderedRows)) {
        TranscriptRow row = ordered.row;
        row.renderOrder = ordered.renderOrder;
        rows.push_back(row);
    }

    return rows;
}

void TranscriptTab::adjustOverlappingRows(QVector<TranscriptRow>& rows)
{
    for (int i = 1; i < rows.size(); ++i) {
        TranscriptRow& previous = rows[i - 1];
        TranscriptRow& current = rows[i];
        if (current.startFrame <= previous.endFrame) {
            const int64_t overlap = previous.endFrame - current.startFrame + 1;
            const int64_t trimPrevious = overlap / 2;
            const int64_t trimCurrent = overlap - trimPrevious;
            previous.endFrame = qMax(previous.startFrame, previous.endFrame - trimPrevious);
            current.startFrame = qMin(current.endFrame, current.startFrame + trimCurrent);
            if (current.startFrame <= previous.endFrame) {
                current.startFrame = qMin(current.endFrame, previous.endFrame + 1);
            }
        }
    }
}

void TranscriptTab::insertGapRows(QVector<TranscriptRow>* rows) const
{
    if (!rows || rows->isEmpty()) {
        return;
    }

    QVector<TranscriptRow> expanded;
    expanded.reserve(rows->size() * 2);
    expanded.push_back(rows->first());
    for (int i = 1; i < rows->size(); ++i) {
        const TranscriptRow& previous = expanded.constLast();
        const TranscriptRow& current = rows->at(i);
        if (!previous.isOutsideActiveCut &&
            !current.isOutsideActiveCut &&
            current.startFrame > previous.endFrame + 1) {
            const int64_t gapStart = previous.endFrame + 1;
            const int64_t gapEnd = current.startFrame - 1;
            const int64_t gapFrames = qMax<int64_t>(0, gapEnd - gapStart + 1);
            if (gapFrames >= 2) {
                TranscriptRow gap;
                gap.startFrame = gapStart;
                gap.endFrame = gapEnd;
                gap.renderStartFrame = -1;
                gap.renderEndFrame = -1;
                gap.speaker = QStringLiteral("—");
                gap.text = QStringLiteral("[Gap %1 ms]")
                               .arg(static_cast<int>(std::llround(
                                   (static_cast<double>(gapFrames) / static_cast<double>(kTimelineFps)) * 1000.0)));
                gap.isGap = true;
                gap.segmentIndex = -1;
                gap.wordIndex = -1;
                gap.originalSegmentIndex = -1;
                gap.originalWordIndex = -1;
                expanded.push_back(gap);
            }
        }
        expanded.push_back(current);
    }
    *rows = std::move(expanded);
}

void TranscriptTab::populateTable(const QVector<TranscriptRow>& rows)
{
    if (!m_widgets.transcriptTable) return;
    const QString selectedClipId =
        (m_deps.getSelectedClip && m_deps.getSelectedClip()) ? m_deps.getSelectedClip()->id : QString();

    m_widgets.transcriptTable->setRowCount(rows.size());
    int restoreRow = -1;
    for (int row = 0; row < rows.size(); ++row) {
        const TranscriptRow& entry = rows.at(row);
        const double sourceStartTime = static_cast<double>(entry.startFrame) / kTimelineFps;
        const double sourceEndTime = static_cast<double>(entry.endFrame + 1) / kTimelineFps;

        auto* sourceStartItem = new QTableWidgetItem(m_transcriptEngine.secondsToTranscriptTime(sourceStartTime));
        auto* sourceEndItem = new QTableWidgetItem(m_transcriptEngine.secondsToTranscriptTime(sourceEndTime));
        auto* speakerItem = new QTableWidgetItem(entry.speaker);
        auto* textItem = new QTableWidgetItem(entry.text);
        auto* editsItem = new QTableWidgetItem(editLabelsForFlags(entry.editFlags).join(QStringLiteral(", ")));
        speakerItem->setFlags(speakerItem->flags() & ~Qt::ItemIsEditable);
        editsItem->setFlags(editsItem->flags() & ~Qt::ItemIsEditable);

        QTableWidgetItem* rowItems[] = {
            sourceStartItem, sourceEndItem, speakerItem, textItem, editsItem};
        for (QTableWidgetItem* tableItem : rowItems) {
            tableItem->setData(Qt::UserRole, sourceStartTime);
            tableItem->setData(Qt::UserRole + 1, sourceEndTime);
            tableItem->setData(Qt::UserRole + 2, QVariant::fromValue(entry.startFrame));
            tableItem->setData(Qt::UserRole + 3, QVariant::fromValue(entry.endFrame));
            tableItem->setData(Qt::UserRole + 4, entry.isGap);
            tableItem->setData(Qt::UserRole + 5, entry.segmentIndex);
            tableItem->setData(Qt::UserRole + 6, entry.wordIndex);
            tableItem->setData(Qt::UserRole + 7, entry.isSkipped);
            tableItem->setData(Qt::UserRole + 8, entry.editFlags);
            tableItem->setData(Qt::UserRole + 9, entry.speaker);
            tableItem->setData(Qt::UserRole + 10, QVariant::fromValue(entry.renderStartFrame));
            tableItem->setData(Qt::UserRole + 11, QVariant::fromValue(entry.renderEndFrame));
            tableItem->setData(Qt::UserRole + 12, entry.isOutsideActiveCut);
            tableItem->setData(Qt::UserRole + 13, entry.originalSegmentIndex);
            tableItem->setData(Qt::UserRole + 14, entry.originalWordIndex);
            tableItem->setData(Qt::UserRole + 15, entry.renderOrder);
        }

        applyTranscriptRowState(sourceStartItem, sourceEndItem, speakerItem, textItem, editsItem, entry);

        if (!entry.isGap &&
            selectedClipId == m_persistedSelectedClipId &&
                   entry.segmentIndex == m_persistedSelectedSegmentIndex &&
                   entry.wordIndex == m_persistedSelectedWordIndex) {
            restoreRow = row;
        }

        m_widgets.transcriptTable->setItem(row, kTranscriptColSourceStart, sourceStartItem);
        m_widgets.transcriptTable->setItem(row, kTranscriptColSourceEnd, sourceEndItem);
        m_widgets.transcriptTable->setItem(row, kTranscriptColSpeaker, speakerItem);
        m_widgets.transcriptTable->setItem(row, kTranscriptColText, textItem);
        m_widgets.transcriptTable->setItem(row, kTranscriptColEdits, editsItem);
    }

    if (restoreRow >= 0 && m_widgets.transcriptTable->selectionModel()) {
        m_suppressSelectionSideEffects = true;
        QSignalBlocker tableBlocker(m_widgets.transcriptTable);
        QSignalBlocker selectionBlocker(m_widgets.transcriptTable->selectionModel());
        m_widgets.transcriptTable->setCurrentCell(restoreRow, 0);
        m_widgets.transcriptTable->selectRow(restoreRow);
        m_suppressSelectionSideEffects = false;
    }

    rebuildFollowRanges(rows);
}

void TranscriptTab::rebuildFollowRanges(const QVector<TranscriptRow>& rows)
{
    m_followRanges.clear();
    m_followRanges.reserve(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const TranscriptRow& entry = rows.at(row);
        if (entry.isGap || entry.isSkipped || entry.isOutsideActiveCut) {
            continue;
        }
        if (entry.endFrame < entry.startFrame) {
            continue;
        }
        m_followRanges.push_back(FollowRange{entry.startFrame, entry.endFrame, row});
    }
    std::sort(m_followRanges.begin(), m_followRanges.end(), [](const FollowRange& a, const FollowRange& b) {
        if (a.startFrame != b.startFrame) {
            return a.startFrame < b.startFrame;
        }
        if (a.endFrame != b.endFrame) {
            return a.endFrame < b.endFrame;
        }
        return a.row < b.row;
    });
}

void TranscriptTab::applyTranscriptRowState(QTableWidgetItem* startItem,
                                            QTableWidgetItem* endItem,
                                            QTableWidgetItem* speakerItem,
                                            QTableWidgetItem* textItem,
                                            QTableWidgetItem* editsItem,
                                            const TranscriptRow& entry) const
{
    const QColor gapColor(255, 248, 196);
    const QColor skippedColor(214, 255, 214);
    const QColor skippedTextColor(22, 96, 37);
    const QColor outsideCutColor(238, 228, 228);
    const QColor outsideCutTextColor(126, 78, 78);
    const QColor timingColor(255, 233, 205);
    const QColor textColor(222, 236, 255);
    const QColor insertedColor(239, 225, 255);
    const QColor editsColor(232, 238, 247);
    const QColor editedTextColor(20, 33, 49);
    QTableWidgetItem* rowItems[] = {startItem, endItem, speakerItem, textItem, editsItem};

    if (entry.isGap) {
        for (QTableWidgetItem* tableItem : rowItems) {
            tableItem->setBackground(gapColor);
            tableItem->setFlags(tableItem->flags() & ~Qt::ItemIsEditable);
        }
        return;
    }

    if (entry.isSkipped) {
        QFont skippedFont = textItem->font();
        skippedFont.setItalic(true);
        textItem->setFont(skippedFont);
        textItem->setText(QStringLiteral("Skip: %1").arg(entry.text));
        for (QTableWidgetItem* tableItem : rowItems) {
            tableItem->setBackground(skippedColor);
            tableItem->setForeground(skippedTextColor);
        }
    }

    if (entry.isOutsideActiveCut) {
        for (QTableWidgetItem* tableItem : rowItems) {
            tableItem->setBackground(outsideCutColor);
            tableItem->setForeground(outsideCutTextColor);
            tableItem->setFlags(tableItem->flags() & ~Qt::ItemIsEditable);
        }
        textItem->setText(QStringLiteral("[Outside Cut] %1").arg(entry.text));
        return;
    }

    if (unifiedEditColorsEnabled()) {
        if (entry.editFlags & TranscriptRow::EditTiming) {
            startItem->setBackground(timingColor);
            endItem->setBackground(timingColor);
            startItem->setForeground(editedTextColor);
            endItem->setForeground(editedTextColor);
        }
        if (entry.editFlags & TranscriptRow::EditText) {
            textItem->setBackground(textColor);
            textItem->setForeground(editedTextColor);
        }
        if (entry.editFlags & TranscriptRow::EditInserted) {
            textItem->setBackground(insertedColor);
            textItem->setForeground(editedTextColor);
        }
        if (entry.editFlags != TranscriptRow::EditNone) {
            editsItem->setBackground(editsColor);
            editsItem->setForeground(editedTextColor);
            speakerItem->setForeground(editedTextColor);
        }
    }
}

void TranscriptTab::refreshSpeakerFilter(const QVector<TranscriptRow>& rows)
{
    if (!m_widgets.transcriptSpeakerFilterCombo) {
        return;
    }

    QString selectedValue = activeSpeakerFilter();
    const QString pendingFilterValue =
        m_widgets.transcriptSpeakerFilterCombo->property("pendingSpeakerFilterValue").toString();
    if (!pendingFilterValue.isEmpty()) {
        selectedValue = pendingFilterValue;
    }
    if (selectedValue.isEmpty()) {
        selectedValue = QString(kAllSpeakersFilterValue);
    }

    QSet<QString> speakers;
    for (const TranscriptRow& row : rows) {
        if (!row.speaker.trimmed().isEmpty()) {
            speakers.insert(row.speaker.trimmed());
        }
    }
    QStringList speakerList = speakers.values();
    std::sort(speakerList.begin(), speakerList.end(), [](const QString& a, const QString& b) {
        return a.localeAwareCompare(b) < 0;
    });

    QSignalBlocker blocker(m_widgets.transcriptSpeakerFilterCombo);
    m_widgets.transcriptSpeakerFilterCombo->clear();
    m_widgets.transcriptSpeakerFilterCombo->addItem(QStringLiteral("All Speakers"),
                                                    QString(kAllSpeakersFilterValue));
    for (const QString& speaker : speakerList) {
        m_widgets.transcriptSpeakerFilterCombo->addItem(speaker, speaker);
    }

    const int index = m_widgets.transcriptSpeakerFilterCombo->findData(selectedValue);
    m_widgets.transcriptSpeakerFilterCombo->setCurrentIndex(index >= 0 ? index : 0);
    if (!pendingFilterValue.isEmpty()) {
        m_widgets.transcriptSpeakerFilterCombo->setProperty("pendingSpeakerFilterValue", QVariant());
    }
}

QString TranscriptTab::activeSpeakerFilter() const
{
    if (!m_widgets.transcriptSpeakerFilterCombo) {
        return QString(kAllSpeakersFilterValue);
    }
    return m_widgets.transcriptSpeakerFilterCombo->currentData().toString();
}

QString TranscriptTab::activeTranscriptSearchFilter() const
{
    if (!m_widgets.transcriptSearchFilterLineEdit) {
        return QString();
    }
    return m_widgets.transcriptSearchFilterLineEdit->text().trimmed();
}

QVector<TranscriptTab::TranscriptRow> TranscriptTab::filteredRowsForSpeaker(const QVector<TranscriptRow>& rows) const
{
    const QString speakerFilterValue = activeSpeakerFilter();
    const QString searchFilterValue = activeTranscriptSearchFilter();
    const bool hasSpeakerFilter =
        !speakerFilterValue.isEmpty() && speakerFilterValue != QString(kAllSpeakersFilterValue);
    const bool hasSearchFilter = !searchFilterValue.isEmpty();
    if (!hasSpeakerFilter && !hasSearchFilter) {
        return rows;
    }

    QVector<TranscriptRow> filtered;
    filtered.reserve(rows.size());
    for (const TranscriptRow& row : rows) {
        if (hasSearchFilter && row.isGap) {
            continue;
        }
        if (hasSpeakerFilter && !row.isGap && row.speaker != speakerFilterValue) {
            continue;
        }
        if (hasSearchFilter && !row.text.contains(searchFilterValue, Qt::CaseInsensitive)) {
            continue;
        }
        filtered.push_back(row);
    }
    return filtered;
}

QStringList TranscriptTab::editLabelsForFlags(int flags) const
{
    QStringList labels;
    if (flags & TranscriptRow::EditTiming) {
        labels.push_back(QStringLiteral("Timing"));
    }
    if (flags & TranscriptRow::EditText) {
        labels.push_back(QStringLiteral("Text"));
    }
    if (flags & TranscriptRow::EditSkip) {
        labels.push_back(QStringLiteral("Skip"));
    }
    if (flags & TranscriptRow::EditInserted) {
        labels.push_back(QStringLiteral("Inserted"));
    }
    return labels;
}

bool TranscriptTab::unifiedEditColorsEnabled() const
{
    return m_widgets.transcriptUnifiedEditModeCheckBox &&
           m_widgets.transcriptUnifiedEditModeCheckBox->isChecked();
}

void TranscriptTab::refreshScriptVersionSelector(const QString& clipFilePath, const QString& selectedPath)
{
    if (!m_widgets.transcriptScriptVersionCombo) {
        return;
    }

    m_updatingScriptVersionSelector = true;
    QSignalBlocker blocker(m_widgets.transcriptScriptVersionCombo);
    m_widgets.transcriptScriptVersionCombo->clear();

    if (clipFilePath.isEmpty()) {
        m_widgets.transcriptScriptVersionCombo->addItem(QStringLiteral("Original (Immutable)"), QString());
        m_widgets.transcriptScriptVersionCombo->setCurrentIndex(0);
        if (m_widgets.transcriptNewVersionButton) {
            m_widgets.transcriptNewVersionButton->setEnabled(false);
        }
        if (m_widgets.transcriptDeleteVersionButton) {
            m_widgets.transcriptDeleteVersionButton->setEnabled(false);
        }
        m_updatingScriptVersionSelector = false;
        return;
    }

    QString resolvedSelectedPath = selectedPath;
    const QString pendingCutPath = m_widgets.transcriptScriptVersionCombo->property("pendingCutPath").toString();
    if (!pendingCutPath.isEmpty()) {
        resolvedSelectedPath = pendingCutPath;
    }

    const QStringList versionPaths = scriptVersionPathsForClip(clipFilePath);
    for (const QString& path : versionPaths) {
        m_widgets.transcriptScriptVersionCombo->addItem(scriptVersionLabelForPath(path, clipFilePath), path);
    }

    int selectedIndex = m_widgets.transcriptScriptVersionCombo->findData(resolvedSelectedPath);
    if (selectedIndex < 0) {
        selectedIndex = 0;
    }
    m_widgets.transcriptScriptVersionCombo->setCurrentIndex(selectedIndex);
    if (!pendingCutPath.isEmpty()) {
        m_widgets.transcriptScriptVersionCombo->setProperty("pendingCutPath", QVariant());
    }

    if (m_widgets.transcriptNewVersionButton) {
        m_widgets.transcriptNewVersionButton->setEnabled(!versionPaths.isEmpty());
    }
    if (m_widgets.transcriptDeleteVersionButton) {
        const QString selected = m_widgets.transcriptScriptVersionCombo->currentData().toString();
        m_widgets.transcriptDeleteVersionButton->setEnabled(
            !selected.isEmpty() && selected != originalTranscriptPathForClip(clipFilePath));
    }
    m_updatingScriptVersionSelector = false;
}

QStringList TranscriptTab::scriptVersionPathsForClip(const QString& clipFilePath) const
{
    QString baseEditablePath;
    if (!ensureEditableTranscriptForClipFile(clipFilePath, &baseEditablePath)) {
        baseEditablePath = transcriptWorkingPathForClipFile(clipFilePath);
    }

    QStringList paths;
    const QString originalPath = originalTranscriptPathForClip(clipFilePath);
    if (QFileInfo::exists(originalPath)) {
        paths.push_back(originalPath);
    }
    const QFileInfo baseInfo(baseEditablePath);
    if (baseInfo.exists()) {
        paths.push_back(baseInfo.absoluteFilePath());
    }

    const QDir dir(baseInfo.dir());
    const QString clipBaseName = QFileInfo(clipFilePath).completeBaseName();
    const QStringList candidates = dir.entryList(
        QStringList{clipBaseName + QStringLiteral("_editable_v*.json")},
        QDir::Files,
        QDir::Name);
    for (const QString& candidate : candidates) {
        const QString absolute = dir.absoluteFilePath(candidate);
        if (absolute != baseInfo.absoluteFilePath()) {
            paths.push_back(absolute);
        }
    }

    std::sort(paths.begin(), paths.end(), [baseInfo, originalPath](const QString& a, const QString& b) {
        if (a == originalPath) {
            return true;
        }
        if (b == originalPath) {
            return false;
        }
        if (a == baseInfo.absoluteFilePath()) {
            return true;
        }
        if (b == baseInfo.absoluteFilePath()) {
            return false;
        }
        const QRegularExpression re(QStringLiteral("_editable_v(\\d+)\\.json$"));
        const QRegularExpressionMatch ma = re.match(a);
        const QRegularExpressionMatch mb = re.match(b);
        const int va = ma.hasMatch() ? ma.captured(1).toInt() : 0;
        const int vb = mb.hasMatch() ? mb.captured(1).toInt() : 0;
        if (va != vb) {
            return va < vb;
        }
        return a < b;
    });
    return paths;
}

QString TranscriptTab::scriptVersionLabelForPath(const QString& path, const QString& clipFilePath) const
{
    const QString originalPath = originalTranscriptPathForClip(clipFilePath);
    if (path == originalPath) {
        return QStringLiteral("Original (Immutable)");
    }

    // Check for a custom label stored in the transcript JSON
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        file.close();
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonObject root = doc.object();
            if (root.contains(QStringLiteral("cut_label"))) {
                const QString customLabel = root.value(QStringLiteral("cut_label")).toString();
                if (!customLabel.isEmpty()) {
                    return customLabel;
                }
            }
        }
    }

    const QString base = defaultEditablePathForClip(clipFilePath);
    if (path == base) {
        return QStringLiteral("Cut 1");
    }
    const QRegularExpression re(QStringLiteral("_editable_v(\\d+)\\.json$"));
    const QRegularExpressionMatch match = re.match(path);
    if (match.hasMatch()) {
        return QStringLiteral("Cut %1").arg(match.captured(1));
    }
    return QFileInfo(path).fileName();
}

QString TranscriptTab::defaultEditablePathForClip(const QString& clipFilePath) const
{
    return transcriptEditablePathForClipFile(clipFilePath);
}

QString TranscriptTab::originalTranscriptPathForClip(const QString& clipFilePath) const
{
    return transcriptPathForClipFile(clipFilePath);
}

bool TranscriptTab::activeCutMutable() const
{
    if (m_loadedTranscriptPath.isEmpty() || m_loadedClipFilePath.isEmpty()) {
        return false;
    }
    return m_loadedTranscriptPath != originalTranscriptPathForClip(m_loadedClipFilePath);
}

bool TranscriptTab::showOutsideCutLinesEnabled() const
{
    return m_widgets.transcriptShowExcludedLinesCheckBox &&
           m_widgets.transcriptShowExcludedLinesCheckBox->isChecked();
}

QString TranscriptTab::originalWordKey(const TranscriptRow& row) const
{
    if (row.originalSegmentIndex < 0 || row.originalWordIndex < 0) {
        return QStringLiteral("new:%1:%2").arg(row.segmentIndex).arg(row.wordIndex);
    }
    return QStringLiteral("%1:%2").arg(row.originalSegmentIndex).arg(row.originalWordIndex);
}

QString TranscriptTab::nextScriptVersionPathForClip(const QString& clipFilePath) const
{
    const QString defaultPath = defaultEditablePathForClip(clipFilePath);
    const QFileInfo info(defaultPath);
    const QString prefix = info.completeBaseName() + QStringLiteral("_v");
    const QDir dir = info.dir();

    int maxVersion = 1;
    const QRegularExpression re(QStringLiteral("_editable_v(\\d+)$"));
    const QStringList fileEntries = dir.entryList(
        QStringList{info.completeBaseName() + QStringLiteral("_v*.json")},
        QDir::Files,
        QDir::Name);
    for (const QString& fileName : fileEntries) {
        const QString completeBase = QFileInfo(fileName).completeBaseName();
        const QRegularExpressionMatch match = re.match(completeBase);
        if (match.hasMatch()) {
            maxVersion = qMax(maxVersion, match.captured(1).toInt());
        }
    }

    return dir.absoluteFilePath(prefix + QString::number(maxVersion + 1) + QStringLiteral(".json"));
}

void TranscriptTab::computeRenderFrames(QVector<TranscriptRow>* rows) const
{
    if (!rows) {
        return;
    }
    int64_t cursor = 0;
    for (TranscriptRow& row : *rows) {
        if (row.isOutsideActiveCut) {
            row.renderStartFrame = -1;
            row.renderEndFrame = -1;
            continue;
        }
        const int64_t duration = qMax<int64_t>(1, row.endFrame - row.startFrame + 1);
        row.renderStartFrame = cursor;
        row.renderEndFrame = cursor + duration - 1;
        cursor = row.renderEndFrame + 1;
    }
}

void TranscriptTab::persistRenderOrderFromTable()
{
    if (!m_widgets.transcriptTable || m_loadedTranscriptPath.isEmpty() || !m_loadedTranscriptDoc.isObject()) {
        return;
    }
    if (!activeCutMutable()) {
        return;
    }
    if (activeSpeakerFilter() != QString(kAllSpeakersFilterValue)) {
        return;
    }

    QVector<QPair<int, int>> orderedKeys;
    orderedKeys.reserve(m_widgets.transcriptTable->rowCount());
    for (int row = 0; row < m_widgets.transcriptTable->rowCount(); ++row) {
        QTableWidgetItem* sourceItem = m_widgets.transcriptTable->item(row, kTranscriptColSourceStart);
        if (!sourceItem || sourceItem->data(Qt::UserRole + 4).toBool() ||
            sourceItem->data(Qt::UserRole + 12).toBool()) {
            continue;
        }
        const int segmentIndex = sourceItem->data(Qt::UserRole + 5).toInt();
        const int wordIndex = sourceItem->data(Qt::UserRole + 6).toInt();
        if (segmentIndex >= 0 && wordIndex >= 0) {
            orderedKeys.push_back(qMakePair(segmentIndex, wordIndex));
        }
    }
    if (orderedKeys.isEmpty()) {
        return;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    int order = 0;
    for (const auto& key : orderedKeys) {
        if (key.first < 0 || key.first >= segments.size()) {
            continue;
        }
        QJsonObject segmentObj = segments.at(key.first).toObject();
        QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        if (key.second < 0 || key.second >= words.size()) {
            continue;
        }
        QJsonObject wordObj = words.at(key.second).toObject();
        wordObj[QString(kTranscriptWordRenderOrderKey)] = order++;
        words.replace(key.second, wordObj);
        segmentObj[QStringLiteral("words")] = words;
        segments.replace(key.first, segmentObj);
    }
    root[QStringLiteral("segments")] = segments;
    m_loadedTranscriptDoc.setObject(root);

    if (!m_transcriptEngine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
        refresh();
        return;
    }
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    refresh();
}

void TranscriptTab::onTranscriptScriptVersionChanged(int index)
{
    Q_UNUSED(index);
    if (m_updating || m_updatingScriptVersionSelector || !m_widgets.transcriptScriptVersionCombo) {
        return;
    }
    if (m_widgets.transcriptDeleteVersionButton && !m_loadedClipFilePath.isEmpty()) {
        const QString selectedPath = m_widgets.transcriptScriptVersionCombo->currentData().toString();
        m_widgets.transcriptDeleteVersionButton->setEnabled(
            !selectedPath.isEmpty() && selectedPath != originalTranscriptPathForClip(m_loadedClipFilePath));
    }
    if (!m_loadedClipFilePath.isEmpty()) {
        refresh();
        emit transcriptDocumentChanged();
        if (m_deps.scheduleSaveState) {
            m_deps.scheduleSaveState();
        }
    }
}

void TranscriptTab::onTranscriptScriptVersionContextMenu(const QPoint& pos)
{
    if (!m_widgets.transcriptScriptVersionCombo) {
        return;
    }

    const QString selectedPath = m_widgets.transcriptScriptVersionCombo->currentData().toString();
    const bool canDeleteSelection =
        !m_loadedClipFilePath.isEmpty() &&
        !selectedPath.isEmpty() &&
        selectedPath != originalTranscriptPathForClip(m_loadedClipFilePath);

    QMenu menu;
    QAction* deleteCurrentTranscription = menu.addAction(QStringLiteral("Delete Current Transcription"));
    deleteCurrentTranscription->setEnabled(canDeleteSelection);
    if (!canDeleteSelection) {
        QAction* unavailable = menu.addAction(QStringLiteral("Delete unavailable for Original or missing selection"));
        unavailable->setEnabled(false);
    }

    QAction* chosen = menu.exec(m_widgets.transcriptScriptVersionCombo->mapToGlobal(pos));
    if (chosen == deleteCurrentTranscription && canDeleteSelection) {
        onTranscriptDeleteVersion();
    }
}

void TranscriptTab::onTranscriptCreateVersion()
{
    if (m_loadedTranscriptPath.isEmpty() || m_loadedClipFilePath.isEmpty()) {
        return;
    }
    const QString nextPath = nextScriptVersionPathForClip(m_loadedClipFilePath);
    QFile::remove(nextPath);
    if (!QFile::copy(m_loadedTranscriptPath, nextPath)) {
        return;
    }

    QFile copied(nextPath);
    if (copied.open(QIODevice::ReadOnly)) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(copied.readAll(), &parseError);
        copied.close();
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            QJsonObject root = doc.object();
            QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
            int order = 0;
            for (int segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
                QJsonObject segmentObj = segments.at(segmentIndex).toObject();
                QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
                for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
                    QJsonObject wordObj = words.at(wordIndex).toObject();
                    if (!wordObj.contains(QString(kTranscriptWordOriginalSegmentKey))) {
                        wordObj[QString(kTranscriptWordOriginalSegmentKey)] = segmentIndex;
                    }
                    if (!wordObj.contains(QString(kTranscriptWordOriginalWordKey))) {
                        wordObj[QString(kTranscriptWordOriginalWordKey)] = wordIndex;
                    }
                    wordObj[QString(kTranscriptWordRenderOrderKey)] = order++;
                    words.replace(wordIndex, wordObj);
                }
                segmentObj[QStringLiteral("words")] = words;
                segments.replace(segmentIndex, segmentObj);
            }
            root[QStringLiteral("segments")] = segments;
            m_transcriptEngine.saveTranscriptJson(nextPath, QJsonDocument(root));
        }
    }

    refreshScriptVersionSelector(m_loadedClipFilePath, nextPath);
    refresh();
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
}

void TranscriptTab::onTranscriptCutLabelEdited()
{
    if (!m_widgets.transcriptScriptVersionCombo || m_loadedClipFilePath.isEmpty()) {
        return;
    }
    const QString selectedPath = m_widgets.transcriptScriptVersionCombo->currentData().toString();
    if (selectedPath.isEmpty()) {
        return;
    }
    const QString newLabel = m_widgets.transcriptScriptVersionCombo->currentText().trimmed();
    if (newLabel.isEmpty()) {
        // Revert to auto-generated label
        m_widgets.transcriptScriptVersionCombo->setItemText(
            m_widgets.transcriptScriptVersionCombo->currentIndex(),
            scriptVersionLabelForPath(selectedPath, m_loadedClipFilePath));
        return;
    }

    // Persist the custom label into the transcript JSON's root as "cut_label"
    QFile file(selectedPath);
    if (file.open(QIODevice::ReadOnly)) {
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        file.close();
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            QJsonObject root = doc.object();
            root[QStringLiteral("cut_label")] = newLabel;
            m_transcriptEngine.saveTranscriptJson(selectedPath, QJsonDocument(root));
        }
    }

    // Update the combo item text immediately
    m_widgets.transcriptScriptVersionCombo->setItemText(
        m_widgets.transcriptScriptVersionCombo->currentIndex(), newLabel);

    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
}

void TranscriptTab::onTranscriptDeleteVersion()
{
    if (!m_widgets.transcriptScriptVersionCombo || m_loadedClipFilePath.isEmpty()) {
        return;
    }
    const QString selectedPath = m_widgets.transcriptScriptVersionCombo->currentData().toString();
    if (selectedPath.isEmpty() || selectedPath == originalTranscriptPathForClip(m_loadedClipFilePath)) {
        return;
    }
    const QString selectedLabel = m_widgets.transcriptScriptVersionCombo->currentText().trimmed();
    const QString label = selectedLabel.isEmpty() ? QFileInfo(selectedPath).fileName() : selectedLabel;
    const QMessageBox::StandardButton confirm = QMessageBox::warning(
        nullptr,
        QStringLiteral("Delete Current Transcription"),
        QStringLiteral("Delete transcription \"%1\"?\n\nThis permanently removes the selected cut file.")
            .arg(label),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }
    QFile::remove(selectedPath);
    const QString basePath = defaultEditablePathForClip(m_loadedClipFilePath);
    refreshScriptVersionSelector(m_loadedClipFilePath, basePath);
    refresh();
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
}
