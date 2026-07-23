#include "transcript_tab.h"

#include "clip_serialization.h"
#include "editor_shared.h"
#include "json_io_utils.h"
#include "transcript_document_core.h"
#include "transcript_document_mutation_core.h"
#include "transcript_cut_session_core.h"

#include <QAbstractItemView>
#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
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
#include <QPointer>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

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
constexpr int kTranscriptTableRowHeight = 30;
constexpr int kTranscriptSourceColumnWidth = 108;
constexpr int kTranscriptSpeakerColumnWidth = 96;
constexpr int kTranscriptEditsColumnWidth = 92;
const QLatin1String kTranscriptSpeakerProfilesKey("speaker_profiles");
const QLatin1String kTranscriptSpeakerNameKey("name");
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

void revealTranscriptTableRow(QTableWidget* table, int row)
{
    if (!table || row < 0 || row >= table->rowCount()) {
        return;
    }
    if (QTableWidgetItem* item = table->item(row, kTranscriptColSourceStart)) {
        table->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    }
    QScrollBar* scrollBar = table->verticalScrollBar();
    if (!scrollBar) {
        return;
    }
    const int rowHeight = qMax(1, table->rowHeight(row));
    const int viewportHeight = table->viewport() ? table->viewport()->height() : 0;
    const int centeredValue =
        qMax(0, (row * rowHeight) - (viewportHeight / 2) + (rowHeight / 2));
    scrollBar->setValue(qBound(scrollBar->minimum(), centeredValue, scrollBar->maximum()));
}

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

bool saveTranscriptDocumentToPath(editor::TranscriptEngine* engine,
                                  const QString& path,
                                  const QJsonObject& root)
{
    if (!engine || path.trimmed().isEmpty()) {
        return false;
    }
    return engine->saveTranscriptJson(path, QJsonDocument(root));
}

bool shouldSkipTranscriptDeleteConfirmation()
{
    if (qEnvironmentVariableIntValue("JCUT_AUTOCONFIRM_TRANSCRIPT_DELETE") == 1) {
        return true;
    }
    const QString appName = QCoreApplication::applicationName().trimmed().toLower();
    return appName.startsWith(QStringLiteral("test_")) || appName.contains(QStringLiteral("qtest"));
}

void configureTranscriptHeaderForLargeModel(QTableWidget* table)
{
    if (!table) {
        return;
    }
    QHeaderView* header = table->horizontalHeader();
    if (!header) {
        return;
    }
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    if (table->columnCount() > kTranscriptColSourceStart) {
        header->resizeSection(kTranscriptColSourceStart, kTranscriptSourceColumnWidth);
    }
    if (table->columnCount() > kTranscriptColSourceEnd) {
        header->resizeSection(kTranscriptColSourceEnd, kTranscriptSourceColumnWidth);
    }
    if (table->columnCount() > kTranscriptColSpeaker) {
        header->resizeSection(kTranscriptColSpeaker, kTranscriptSpeakerColumnWidth);
    }
    if (table->columnCount() > kTranscriptColText) {
        header->setSectionResizeMode(kTranscriptColText, QHeaderView::Stretch);
    }
    if (table->columnCount() > kTranscriptColEdits) {
        header->resizeSection(kTranscriptColEdits, kTranscriptEditsColumnWidth);
    }
}

} // namespace

quint64 TranscriptTab::transcriptWordKey(int segmentIndex, int wordIndex) const
{
    return (static_cast<quint64>(static_cast<quint32>(segmentIndex)) << 32) |
           static_cast<quint32>(wordIndex);
}

bool TranscriptTab::rebuildInMemoryTranscriptDocument(const QJsonDocument& document)
{
    QHash<quint64, int> priorOriginalWordIds;
    int nextWordId = qMax(1, m_nextTranscriptWordId);
    for (const TranscriptDocumentSegment& segment :
         std::as_const(m_transcriptDocumentSegments)) {
        for (const TranscriptDocumentWord& word :
             segment.words) {
            if (word.originalSegmentIndex >= 0 &&
                word.originalWordIndex >= 0) {
                priorOriginalWordIds.insert(
                    transcriptWordKey(
                        word.originalSegmentIndex,
                        word.originalWordIndex),
                    word.wordId);
            }
            nextWordId = qMax(nextWordId, word.wordId + 1);
        }
    }
    m_transcriptDocumentSegments.clear();
    m_transcriptWordAddressById.clear();
    m_renderOrderedWordIds.clear();
    m_nextTranscriptWordId = nextWordId;
    if (!document.isObject()) {
        return false;
    }
    struct RenderLocation {
        int wordId = -1;
        int renderOrder = -1;
        double startSeconds = 0.0;
        int fallbackOrder = 0;
    };
    const QJsonObject root = document.object();
    const QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    m_transcriptDocumentSegments.reserve(segments.size());
    QVector<RenderLocation> renderLocations;
    int fallbackOrder = 0;
    for (const QJsonValue& segmentValue : segments) {
        const QJsonObject segmentObject = segmentValue.toObject();
        TranscriptDocumentSegment segment;
        segment.metadata = segmentObject;
        segment.metadata.remove(QStringLiteral("words"));
        const QJsonArray words = segmentObject.value(QStringLiteral("words")).toArray();
        segment.words.reserve(words.size());
        for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
            const QJsonObject wordObject = words.at(wordIndex).toObject();
            TranscriptDocumentWord word;
            word.metadata = wordObject;
            word.text = wordObject.value(QStringLiteral("word")).toString(
                wordObject.value(QStringLiteral("text")).toString());
            word.startSeconds = wordObject.value(QStringLiteral("start")).toDouble(0.0);
            word.endSeconds = wordObject.value(QStringLiteral("end")).toDouble(word.startSeconds);
            word.speaker = wordObject.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            word.skipped = wordObject.value(QString(kTranscriptWordSkippedKey)).toBool(false);
            const QJsonArray editArray = wordObject.value(QString(kTranscriptWordEditsKey)).toArray();
            for (const QJsonValue& editValue : editArray) {
                const QString editTag = editValue.toString().trimmed();
                if (!editTag.isEmpty()) {
                    word.editTags.push_back(editTag);
                }
            }
            word.renderOrder = wordObject.value(QString(kTranscriptWordRenderOrderKey)).toInt(-1);
            word.originalSegmentIndex =
                wordObject.value(QString(kTranscriptWordOriginalSegmentKey)).toInt(m_transcriptDocumentSegments.size());
            word.originalWordIndex = wordObject.value(QString(kTranscriptWordOriginalWordKey)).toInt(wordIndex);
            const auto priorId = priorOriginalWordIds.constFind(
                transcriptWordKey(
                    word.originalSegmentIndex,
                    word.originalWordIndex));
            word.wordId =
                priorId == priorOriginalWordIds.constEnd()
                ? m_nextTranscriptWordId++
                : priorId.value();
            segment.words.push_back(word);
            renderLocations.push_back(RenderLocation{
                word.wordId,
                word.renderOrder,
                word.startSeconds,
                fallbackOrder++
            });
        }
        m_transcriptDocumentSegments.push_back(segment);
    }
    std::sort(renderLocations.begin(), renderLocations.end(), [](const RenderLocation& a, const RenderLocation& b) {
        const bool aHasOrder = a.renderOrder >= 0;
        const bool bHasOrder = b.renderOrder >= 0;
        if (aHasOrder != bHasOrder) {
            return aHasOrder;
        }
        if (aHasOrder && a.renderOrder != b.renderOrder) {
            return a.renderOrder < b.renderOrder;
        }
        if (!qFuzzyCompare(a.startSeconds + 1.0, b.startSeconds + 1.0)) {
            return a.startSeconds < b.startSeconds;
        }
        return a.fallbackOrder < b.fallbackOrder;
    });
    m_renderOrderedWordIds.reserve(renderLocations.size());
    for (const RenderLocation& location : std::as_const(renderLocations)) {
        m_renderOrderedWordIds.push_back(location.wordId);
    }
    rebuildTranscriptWordAddressIndex();
    normalizeTranscriptRenderOrder();
    return true;
}

QJsonDocument TranscriptTab::serializeInMemoryTranscriptDocument() const
{
    if (!m_transcriptSession.hasObjectDocument()) {
        return QJsonDocument();
    }
    QJsonObject root = m_transcriptSession.rootObject();
    QJsonArray segments;
    for (const TranscriptDocumentSegment& segment : m_transcriptDocumentSegments) {
        QJsonObject segmentObject = segment.metadata;
        QJsonArray words;
        for (const TranscriptDocumentWord& word : segment.words) {
            QJsonObject wordObject = word.metadata;
            const QString textKey =
                wordObject.contains(QStringLiteral("text")) &&
                    !wordObject.contains(QStringLiteral("word"))
                ? QStringLiteral("text") : QStringLiteral("word");
            wordObject[textKey] = word.text;
            wordObject[QStringLiteral("start")] = word.startSeconds;
            wordObject[QStringLiteral("end")] = word.endSeconds;
            if (!word.speaker.trimmed().isEmpty()) {
                wordObject[QString(kTranscriptWordSpeakerKey)] = word.speaker.trimmed();
            } else {
                wordObject.remove(QString(kTranscriptWordSpeakerKey));
            }
            if (word.skipped) {
                wordObject[QString(kTranscriptWordSkippedKey)] = true;
            } else {
                wordObject.remove(QString(kTranscriptWordSkippedKey));
            }
            if (!word.editTags.isEmpty()) {
                QJsonArray editArray;
                for (const QString& tag : word.editTags) {
                    editArray.push_back(tag);
                }
                wordObject[QString(kTranscriptWordEditsKey)] = editArray;
            } else {
                wordObject.remove(QString(kTranscriptWordEditsKey));
            }
            if (word.renderOrder >= 0) {
                wordObject[QString(kTranscriptWordRenderOrderKey)] = word.renderOrder;
            } else {
                wordObject.remove(QString(kTranscriptWordRenderOrderKey));
            }
            wordObject[QString(kTranscriptWordOriginalSegmentKey)] = word.originalSegmentIndex;
            wordObject[QString(kTranscriptWordOriginalWordKey)] = word.originalWordIndex;
            words.push_back(wordObject);
        }
        segmentObject[QStringLiteral("words")] = words;
        segments.push_back(segmentObject);
    }
    root[QStringLiteral("segments")] = segments;
    return QJsonDocument(root);
}

TranscriptTab::TranscriptDocumentWord* TranscriptTab::transcriptWordAt(int segmentIndex, int wordIndex)
{
    if (segmentIndex < 0 || segmentIndex >= m_transcriptDocumentSegments.size()) {
        return nullptr;
    }
    auto& words = m_transcriptDocumentSegments[segmentIndex].words;
    if (wordIndex < 0 || wordIndex >= words.size()) {
        return nullptr;
    }
    return &words[wordIndex];
}

TranscriptTab::TranscriptDocumentWord* TranscriptTab::transcriptWordById(const int wordId)
{
    const auto it = m_transcriptWordAddressById.constFind(wordId);
    if (it == m_transcriptWordAddressById.cend()) {
        return nullptr;
    }
    return transcriptWordAt(it->segmentIndex, it->wordIndex);
}

const TranscriptTab::TranscriptDocumentWord* TranscriptTab::transcriptWordAt(int segmentIndex, int wordIndex) const
{
    if (segmentIndex < 0 || segmentIndex >= m_transcriptDocumentSegments.size()) {
        return nullptr;
    }
    const auto& words = m_transcriptDocumentSegments[segmentIndex].words;
    if (wordIndex < 0 || wordIndex >= words.size()) {
        return nullptr;
    }
    return &words[wordIndex];
}

const TranscriptTab::TranscriptDocumentWord* TranscriptTab::transcriptWordById(const int wordId) const
{
    const auto it = m_transcriptWordAddressById.constFind(wordId);
    if (it == m_transcriptWordAddressById.cend()) {
        return nullptr;
    }
    return transcriptWordAt(it->segmentIndex, it->wordIndex);
}

int TranscriptTab::transcriptWordCount() const
{
    int count = 0;
    for (const TranscriptDocumentSegment& segment : m_transcriptDocumentSegments) {
        count += segment.words.size();
    }
    return count;
}

void TranscriptTab::normalizeTranscriptRenderOrder()
{
    int nextOrder = 0;
    for (const int wordId : std::as_const(m_renderOrderedWordIds)) {
        if (TranscriptDocumentWord* word = transcriptWordById(wordId)) {
            word->renderOrder = nextOrder++;
        }
    }
}

void TranscriptTab::rehydrateLoadedTranscriptDocumentFromMemory()
{
    m_transcriptSession.assign(
        m_transcriptSession.clipFilePath(),
        m_transcriptSession.transcriptPath(),
        serializeInMemoryTranscriptDocument());
}

void TranscriptTab::rebuildTranscriptWordAddressIndex()
{
    m_transcriptWordAddressById.clear();
    for (int segmentIndex = 0; segmentIndex < m_transcriptDocumentSegments.size(); ++segmentIndex) {
        const auto& words = m_transcriptDocumentSegments.at(segmentIndex).words;
        for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
            m_transcriptWordAddressById.insert(words.at(wordIndex).wordId,
                                               TranscriptWordAddress{segmentIndex, wordIndex});
        }
    }
}

void TranscriptTab::loadTranscriptFile(const TimelineClip& clip)
{
    QString baseEditablePath;
    if (!ensureEditableTranscriptForClip(clip, &baseEditablePath)) {
        baseEditablePath = transcriptWorkingPathForClip(clip);
    }

    const QString originalPath = transcriptPathForClip(clip);
    QString transcriptPath = baseEditablePath;
    if (m_widgets.transcriptScriptVersionCombo) {
        const QString selectedPath = m_widgets.transcriptScriptVersionCombo->currentData().toString();
        if (!selectedPath.isEmpty() && QFileInfo::exists(selectedPath)) {
            transcriptPath = selectedPath;
        }
    }
    refreshScriptVersionSelector(clip.filePath, transcriptPath);
    setActiveTranscriptPathForClip(clip, transcriptPath);

    const bool canReuseLoadedDoc =
        m_transcriptSession.hasObjectDocument() &&
        m_transcriptSession.matches(clip.filePath, transcriptPath);

    if (!canReuseLoadedDoc) {
        if (!QFileInfo::exists(transcriptPath)) {
            if (transcriptPath != baseEditablePath && QFileInfo::exists(baseEditablePath)) {
                transcriptPath = baseEditablePath;
                refreshScriptVersionSelector(clip.filePath, transcriptPath);
                setActiveTranscriptPathForClip(clip, transcriptPath);
            } else {
                if (m_widgets.transcriptInspectorDetailsLabel) {
                    m_widgets.transcriptInspectorDetailsLabel->setText(QStringLiteral("No transcript file found."));
                }
                return;
            }
        }

        m_transcriptSession.assign(clip.filePath, transcriptPath, QJsonDocument());
        startTranscriptLoadRequest(clip.filePath, transcriptPath);
        return;
    }

    applyLoadedTranscriptDocumentData(clip, originalPath);
}

void TranscriptTab::applyLoadedTranscriptDocumentData(const TimelineClip& clip, const QString& originalPath)
{
    struct UpdatingGuard {
        bool& flag;
        const bool previous;
        explicit UpdatingGuard(bool& updatingFlag)
            : flag(updatingFlag)
            , previous(updatingFlag)
        {
            flag = true;
        }
        ~UpdatingGuard() { flag = previous; }
    } updatingGuard(m_updating);

    if (!m_transcriptSession.hasObjectDocument()) {
        if (m_widgets.transcriptInspectorDetailsLabel) {
            m_widgets.transcriptInspectorDetailsLabel->setText(QStringLiteral("Invalid transcript JSON file."));
        }
        return;
    }
    const bool canReuseInMemoryDocument =
        serializeInMemoryTranscriptDocument() == m_transcriptSession.document();
    if (!canReuseInMemoryDocument &&
        !rebuildInMemoryTranscriptDocument(m_transcriptSession.document())) {
        if (m_widgets.transcriptInspectorDetailsLabel) {
            m_widgets.transcriptInspectorDetailsLabel->setText(QStringLiteral("Invalid transcript JSON file."));
        }
        return;
    }

    const QJsonArray segments = serializeInMemoryTranscriptDocument().object().value(QStringLiteral("segments")).toArray();
    const int totalWords = transcriptWordCount();
    int editedWords = 0;
    for (const TranscriptDocumentSegment& segment : m_transcriptDocumentSegments) {
        for (const TranscriptDocumentWord& word : segment.words) {
            if (!word.editTags.isEmpty()) {
                ++editedWords;
            }
        }
    }
    const int deletedEdits =
        m_transcriptSession.rootObject().value(QString(kTranscriptDeletedEditsCountKey)).toInt(0);
    const bool mutableCut = activeCutMutable();
    const QString activeCutName =
        m_widgets.transcriptScriptVersionCombo
            ? m_widgets.transcriptScriptVersionCombo->currentText().trimmed()
            : QString();
    const QString resolvedCutName =
        activeCutName.isEmpty()
            ? scriptVersionLabelForPath(m_transcriptSession.transcriptPath(), clip.filePath)
            : activeCutName;
    const bool followEnabled =
        m_widgets.transcriptFollowCurrentWordCheckBox &&
        m_widgets.transcriptFollowCurrentWordCheckBox->isChecked();
    const QString speechFilterState =
        (m_widgets.speechFilterFadeModeCombo &&
         m_widgets.speechFilterFadeModeCombo->currentData().toString() != QStringLiteral("none"))
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

    startTranscriptRowsBuildRequest(originalPath);

    const bool canDragRows = activeSpeakerFilter() == QString(kAllSpeakersFilterValue) &&
                             activeCutMutable() &&
                             !showOutsideCutLinesEnabled();
    if (m_widgets.transcriptTable) {
        const bool mutableCutTable = activeCutMutable();
        m_widgets.transcriptTable->setDragEnabled(canDragRows);
        m_widgets.transcriptTable->setDragDropMode(
            canDragRows ? QAbstractItemView::InternalMove : QAbstractItemView::NoDragDrop);
        m_widgets.transcriptTable->setEditTriggers(
            mutableCutTable ? (QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed)
                            : QAbstractItemView::NoEditTriggers);
        m_widgets.transcriptTable->setToolTip(
            canDragRows
                ? QStringLiteral("Drag rows to change render order.")
                : QStringLiteral("Switch speaker filter to All Speakers to reorder rows."));
    }
}

void TranscriptTab::rebuildWordEditIndex(const QVector<TranscriptRow>& rows)
{
    m_wordEditIndex.clear();
    int previousWordRow = -1;
    for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const TranscriptRow& row = rows.at(rowIndex);
        if (row.isGap || row.isOutsideActiveCut || row.wordId < 0) {
            continue;
        }
        WordEditNeighborInfo info;
        info.renderOrder = row.renderOrder;
        info.speaker = row.speaker;
        if (previousWordRow >= 0) {
            const TranscriptRow& previousRow = rows.at(previousWordRow);
            info.hasPreviousWord = true;
            info.previousWordEndSeconds =
                static_cast<double>(previousRow.endFrame + 1) / static_cast<double>(kTimelineFps);
            info.previousWordId = previousRow.wordId;
        }
        m_wordEditIndex.insert(row.wordId, info);
        if (previousWordRow >= 0) {
            const TranscriptRow& previousRow = rows.at(previousWordRow);
            auto previousIt = m_wordEditIndex.find(previousRow.wordId);
            if (previousIt != m_wordEditIndex.end()) {
                previousIt->hasNextWord = true;
                previousIt->nextWordStartSeconds =
                    static_cast<double>(row.startFrame) / static_cast<double>(kTimelineFps);
                previousIt->nextWordId = row.wordId;
            }
        }
        previousWordRow = rowIndex;
    }
}

void TranscriptTab::populateTable(const QVector<TranscriptRow>& rows)
{
    if (!m_widgets.transcriptTable) return;
    QTableWidget* table = m_widgets.transcriptTable;
    const QString selectedClipId =
        (m_deps.getSelectedClip && m_deps.getSelectedClip()) ? m_deps.getSelectedClip()->id : QString();
    const bool sortingWasEnabled = table->isSortingEnabled();
    const bool updatesWereEnabled = table->updatesEnabled();
    QSignalBlocker tableBlocker(table);
    std::unique_ptr<QSignalBlocker> selectionBlocker;
    if (table->selectionModel()) {
        selectionBlocker = std::make_unique<QSignalBlocker>(table->selectionModel());
    }
    table->setUpdatesEnabled(false);
    table->setSortingEnabled(false);
    table->clearContents();
    configureTranscriptHeaderForLargeModel(table);

    table->setRowCount(rows.size());
    if (QHeaderView* vertical = table->verticalHeader()) {
        vertical->setSectionResizeMode(QHeaderView::Fixed);
        vertical->setDefaultSectionSize(kTranscriptTableRowHeight);
        vertical->setMinimumSectionSize(kTranscriptTableRowHeight);
    }
    int restoreRow = -1;
    int playheadRestoreRow = -1;
    int64_t nearestPlayheadDistance = std::numeric_limits<int64_t>::max();
    const bool followCurrentWord =
        m_widgets.transcriptFollowCurrentWordCheckBox &&
        m_widgets.transcriptFollowCurrentWordCheckBox->isChecked();
    const bool canRestoreFromPlayhead =
        followCurrentWord &&
        !selectedClipId.isEmpty() &&
        selectedClipId == m_lastSyncClipId &&
        m_lastSyncSourceFrame >= 0;
    for (int row = 0; row < rows.size(); ++row) {
        const TranscriptRow& entry = rows.at(row);
        const double sourceStartTime = static_cast<double>(entry.startFrame) / kTimelineFps;
        const double sourceEndTime = static_cast<double>(entry.endFrame + 1) / kTimelineFps;

        auto* sourceStartItem = new QTableWidgetItem(m_transcriptEngine.secondsToTranscriptTime(sourceStartTime));
        auto* sourceEndItem = new QTableWidgetItem(m_transcriptEngine.secondsToTranscriptTime(sourceEndTime));
        auto* speakerItem = new QTableWidgetItem(entry.isGap ? entry.speaker
                                                             : speakerDisplayLabel(entry.speaker));
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
            tableItem->setData(Qt::UserRole + 16, entry.wordId);
        }

        applyTranscriptRowState(sourceStartItem, sourceEndItem, speakerItem, textItem, editsItem, entry);

        if (!entry.isGap &&
            selectedClipId == m_persistedSelectedClipId &&
            ((m_persistedSelectedWordId >= 0 && entry.wordId == m_persistedSelectedWordId) ||
             (entry.segmentIndex == m_persistedSelectedSegmentIndex &&
              entry.wordIndex == m_persistedSelectedWordIndex))) {
            restoreRow = row;
        }
        if (canRestoreFromPlayhead &&
            !entry.isGap &&
            !entry.isOutsideActiveCut &&
            !entry.isSkipped &&
            entry.wordId >= 0) {
            const bool containsPlayhead =
                entry.startFrame <= m_lastSyncSourceFrame &&
                m_lastSyncSourceFrame <= entry.endFrame;
            const int64_t distance = containsPlayhead
                ? 0
                : qMin(qAbs(m_lastSyncSourceFrame - entry.startFrame),
                       qAbs(m_lastSyncSourceFrame - entry.endFrame));
            if (distance < nearestPlayheadDistance) {
                nearestPlayheadDistance = distance;
                playheadRestoreRow = row;
            }
        }

        table->setItem(row, kTranscriptColSourceStart, sourceStartItem);
        table->setItem(row, kTranscriptColSourceEnd, sourceEndItem);
        table->setItem(row, kTranscriptColSpeaker, speakerItem);
        table->setItem(row, kTranscriptColText, textItem);
        table->setItem(row, kTranscriptColEdits, editsItem);
        table->setRowHeight(row, kTranscriptTableRowHeight);
    }

    if (playheadRestoreRow >= 0) {
        restoreRow = playheadRestoreRow;
    }
    const int rowToReveal = restoreRow;
    if (restoreRow >= 0 && table->selectionModel()) {
        m_suppressSelectionSideEffects = true;
        table->setCurrentCell(restoreRow, 0);
        table->selectRow(restoreRow);
        m_suppressSelectionSideEffects = false;
    }

    table->setSortingEnabled(sortingWasEnabled);
    table->setUpdatesEnabled(updatesWereEnabled);
    if (table->viewport()) {
        table->viewport()->update();
    }
    if (rowToReveal >= 0) {
        QPointer<QTableWidget> tablePtr(table);
        QMetaObject::invokeMethod(
            table,
            [tablePtr, rowToReveal]() {
                if (tablePtr) {
                    revealTranscriptTableRow(tablePtr, rowToReveal);
                }
            },
            Qt::QueuedConnection);
    }
    rebuildFollowRanges(rows);
}

void TranscriptTab::startTranscriptRowsBuildRequest(const QString& originalPath)
{
    const qint64 requestId = ++m_transcriptRowsBuildRequestId;
    const QVector<TranscriptDocumentSegment> segments = m_transcriptDocumentSegments;
    const QByteArray activeDocumentBytes =
        serializeInMemoryTranscriptDocument().toJson(QJsonDocument::Compact);
    const QString activeTranscriptPath = m_transcriptSession.transcriptPath();
    const int prependMs = transcriptPrependMs();
    const int postpendMs = transcriptPostpendMs();
    const int offsetMs = transcriptOffsetMs();
    const bool includeOutsideCut =
        showOutsideCutLinesEnabled() &&
        activeTranscriptPath != originalPath &&
        QFileInfo::exists(originalPath);

    m_transcriptRowsBuildWatcher.setFuture(QtConcurrent::run(
        [requestId,
         segments,
         activeDocumentBytes,
         originalPath,
         prependMs,
         postpendMs,
         offsetMs,
         includeOutsideCut]() {
            const auto activeDocument = jcut::TranscriptDocumentCore::fromJsonBytes(
                std::string_view(activeDocumentBytes.constData(),
                                 static_cast<std::size_t>(activeDocumentBytes.size())));
            if (!activeDocument.has_value()) {
                return TranscriptRowsBuildResult{requestId, {}};
            }

            std::optional<jcut::TranscriptDocumentCore> originalDocument;
            if (includeOutsideCut) {
                QJsonDocument originalDoc;
                if (loadTranscriptJsonCached(originalPath, &originalDoc) && originalDoc.isObject()) {
                    const QByteArray originalBytes = originalDoc.toJson(QJsonDocument::Compact);
                    originalDocument = jcut::TranscriptDocumentCore::fromJsonBytes(
                        std::string_view(originalBytes.constData(),
                                         static_cast<std::size_t>(originalBytes.size())));
                }
            }

            jcut::TranscriptRowBuildOptions options;
            options.timing.framesPerSecond = static_cast<double>(kTimelineFps);
            options.timing.prependMilliseconds = prependMs;
            options.timing.postpendMilliseconds = postpendMs;
            options.timing.offsetMilliseconds = offsetMs;
            options.adjustOverlaps = true;
            options.insertGaps = true;
            options.includeOutsideActiveCut =
                includeOutsideCut && originalDocument.has_value();
            const std::vector<jcut::TranscriptRow> coreRows = activeDocument->rows(
                options,
                originalDocument.has_value() ? &*originalDocument : nullptr);

            QVector<TranscriptRow> allRows;
            allRows.reserve(static_cast<qsizetype>(coreRows.size()));
            for (const jcut::TranscriptRow& coreRow : coreRows) {
                TranscriptRow row;
                row.startFrame = coreRow.sourceStartFrame;
                row.endFrame = coreRow.sourceEndFrame;
                row.renderStartFrame = coreRow.renderStartFrame;
                row.renderEndFrame = coreRow.renderEndFrame;
                row.speaker = QString::fromStdString(coreRow.speakerId);
                row.text = QString::fromStdString(coreRow.text);
                row.isGap = coreRow.gap;
                row.isOutsideActiveCut = coreRow.outsideActiveCut;
                row.isSkipped = coreRow.skipped;
                row.editFlags = static_cast<int>(coreRow.editFlags);
                row.segmentIndex = coreRow.word.segmentIndex;
                row.wordIndex = coreRow.word.wordIndex;
                row.wordId = coreRow.word.documentWordId;
                row.originalSegmentIndex = coreRow.word.originalSegmentIndex;
                row.originalWordIndex = coreRow.word.originalWordIndex;
                row.renderOrder = coreRow.word.renderOrder;

                if (!coreRow.outsideActiveCut &&
                    coreRow.word.segmentIndex >= 0 &&
                    coreRow.word.segmentIndex < segments.size()) {
                    const QVector<TranscriptDocumentWord>& words =
                        segments.at(coreRow.word.segmentIndex).words;
                    if (coreRow.word.wordIndex >= 0 &&
                        coreRow.word.wordIndex < words.size()) {
                        const TranscriptDocumentWord& activeWord =
                            words.at(coreRow.word.wordIndex);
                        row.wordId = activeWord.wordId;
                        row.renderOrder = activeWord.renderOrder;
                        row.originalSegmentIndex = activeWord.originalSegmentIndex >= 0
                            ? activeWord.originalSegmentIndex
                            : coreRow.word.segmentIndex;
                        row.originalWordIndex = activeWord.originalWordIndex >= 0
                            ? activeWord.originalWordIndex
                            : coreRow.word.wordIndex;
                    }
                }
                allRows.push_back(std::move(row));
            }

            return TranscriptRowsBuildResult{requestId, allRows};
        }));
}

void TranscriptTab::applyTranscriptRowsBuildResult(const TranscriptRowsBuildResult& result)
{
    if (result.requestId != m_transcriptRowsBuildRequestId) {
        return;
    }
    m_allTranscriptRows = result.rows;
    rebuildWordEditIndex(m_allTranscriptRows);
    refreshSpeakerFilter(m_allTranscriptRows);
    rebuildFollowRanges(m_allTranscriptRows);
    if (m_deps.playbackActive && m_deps.playbackActive()) {
        m_transcriptTableRefreshPending = true;
        return;
    }
    m_transcriptTableRefreshPending = false;
    populateTable(filteredRowsForSpeaker(m_allTranscriptRows));
}

void TranscriptTab::rebuildFollowRanges(const QVector<TranscriptRow>& rows)
{
    m_followRanges.clear();
    m_excludedRanges.clear();
    m_followRanges.reserve(rows.size());
    m_excludedRanges.reserve(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const TranscriptRow& entry = rows.at(row);
        if (entry.endFrame < entry.startFrame) {
            continue;
        }
        if (entry.isSkipped || entry.isOutsideActiveCut) {
            m_excludedRanges.push_back(ExcludedRange{entry.startFrame, entry.endFrame});
            continue;
        }
        if (entry.isGap) {
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
    std::sort(m_excludedRanges.begin(), m_excludedRanges.end(), [](const ExcludedRange& a, const ExcludedRange& b) {
        if (a.startFrame != b.startFrame) {
            return a.startFrame < b.startFrame;
        }
        return a.endFrame < b.endFrame;
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
    std::sort(speakerList.begin(), speakerList.end(), [this](const QString& a, const QString& b) {
        const QString aLabel = speakerDisplayLabel(a);
        const QString bLabel = speakerDisplayLabel(b);
        const int byLabel = aLabel.localeAwareCompare(bLabel);
        if (byLabel != 0) {
            return byLabel < 0;
        }
        return a.localeAwareCompare(b) < 0;
    });

    QSignalBlocker blocker(m_widgets.transcriptSpeakerFilterCombo);
    m_widgets.transcriptSpeakerFilterCombo->clear();
    m_widgets.transcriptSpeakerFilterCombo->addItem(QStringLiteral("All Speakers"),
                                                    QString(kAllSpeakersFilterValue));
    for (const QString& speaker : speakerList) {
        m_widgets.transcriptSpeakerFilterCombo->addItem(speakerDisplayLabel(speaker), speaker);
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

QString TranscriptTab::speakerDisplayLabel(const QString& speakerId) const
{
    const QString trimmedSpeakerId = speakerId.trimmed();
    if (trimmedSpeakerId.isEmpty() || !m_transcriptSession.hasObjectDocument()) {
        return trimmedSpeakerId;
    }

    const QJsonObject profiles =
        m_transcriptSession.rootObject().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QString displayName =
        profiles.value(trimmedSpeakerId).toObject().value(QString(kTranscriptSpeakerNameKey)).toString().trimmed();
    return displayName.isEmpty() ? trimmedSpeakerId : displayName;
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
        if (hasSearchFilter && !transcriptSearchMatchesText(searchFilterValue, row.text)) {
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
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    const bool useSelectedClip =
        selectedClip && selectedClip->filePath == clipFilePath;

    QString baseEditablePath;
    if (useSelectedClip) {
        if (!ensureEditableTranscriptForClip(*selectedClip, &baseEditablePath)) {
            baseEditablePath = transcriptWorkingPathForClip(*selectedClip);
        }
    } else if (!ensureEditableTranscriptForClipFile(clipFilePath, &baseEditablePath)) {
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
    QString transcriptStem = baseInfo.completeBaseName();
    if (transcriptStem.endsWith(QStringLiteral("_editable"))) {
        transcriptStem.chop(QStringLiteral("_editable").size());
    }
    const QStringList candidates = dir.entryList(
        QStringList{transcriptStem + QStringLiteral("_editable_v*.json")},
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
    QJsonDocument doc;
    if (loadTranscriptJsonCached(path, &doc) && doc.isObject()) {
        const QJsonObject root = doc.object();
        if (root.contains(QStringLiteral("cut_label"))) {
            const QString customLabel = root.value(QStringLiteral("cut_label")).toString();
            if (!customLabel.isEmpty()) {
                return customLabel;
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
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (selectedClip && selectedClip->filePath == clipFilePath) {
        return transcriptEditablePathForClip(*selectedClip);
    }
    return transcriptEditablePathForClipFile(clipFilePath);
}

QString TranscriptTab::originalTranscriptPathForClip(const QString& clipFilePath) const
{
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (selectedClip && selectedClip->filePath == clipFilePath) {
        return transcriptPathForClip(*selectedClip);
    }
    return transcriptPathForClipFile(clipFilePath);
}

bool TranscriptTab::activeCutMutable() const
{
    if (m_transcriptSession.transcriptPath().isEmpty() || m_transcriptSession.clipFilePath().isEmpty()) {
        return false;
    }
    return m_transcriptSession.transcriptPath() !=
           originalTranscriptPathForClip(m_transcriptSession.clipFilePath());
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

void TranscriptTab::persistRenderOrderFromTable()
{
    if (!m_widgets.transcriptTable || m_transcriptSession.transcriptPath().isEmpty() ||
        !m_transcriptSession.hasObjectDocument()) {
        return;
    }
    if (!activeCutMutable()) {
        return;
    }
    if (activeSpeakerFilter() != QString(kAllSpeakersFilterValue)) {
        return;
    }

    std::vector<jcut::TranscriptWordRef> references;
    references.reserve(static_cast<std::size_t>(
        m_widgets.transcriptTable->rowCount()));
    for (int row = 0; row < m_widgets.transcriptTable->rowCount(); ++row) {
        QTableWidgetItem* sourceItem = m_widgets.transcriptTable->item(row, kTranscriptColSourceStart);
        if (!sourceItem || sourceItem->data(Qt::UserRole + 4).toBool() ||
            sourceItem->data(Qt::UserRole + 12).toBool()) {
            continue;
        }
        const int wordId = sourceItem->data(Qt::UserRole + 16).toInt();
        if (wordId >= 0) {
            references.push_back({
                sourceItem->data(Qt::UserRole + 5).toInt(),
                sourceItem->data(Qt::UserRole + 6).toInt(),
                sourceItem->data(Qt::UserRole + 13).toInt(),
                sourceItem->data(Qt::UserRole + 14).toInt(),
                sourceItem->data(Qt::UserRole + 15).toInt()});
        }
    }
    if (references.empty()) {
        return;
    }
    (void)applyTranscriptDocumentMutation(
        [&](nlohmann::json* root, std::string* error) {
            return jcut::reorderTranscriptWords(
                root, references, error);
        });
}

void TranscriptTab::onTranscriptScriptVersionChanged(int index)
{
    Q_UNUSED(index);
    if (m_updating || m_updatingScriptVersionSelector || !m_widgets.transcriptScriptVersionCombo) {
        return;
    }
    if (m_widgets.transcriptDeleteVersionButton && !m_transcriptSession.clipFilePath().isEmpty()) {
        const QString selectedPath = m_widgets.transcriptScriptVersionCombo->currentData().toString();
        m_widgets.transcriptDeleteVersionButton->setEnabled(
            !selectedPath.isEmpty() &&
            selectedPath != originalTranscriptPathForClip(m_transcriptSession.clipFilePath()));
    }
    if (!m_transcriptSession.clipFilePath().isEmpty()) {
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
        !m_transcriptSession.clipFilePath().isEmpty() &&
        !selectedPath.isEmpty() &&
        selectedPath != originalTranscriptPathForClip(m_transcriptSession.clipFilePath());

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
    if (m_transcriptSession.transcriptPath().isEmpty() || m_transcriptSession.clipFilePath().isEmpty()) {
        return;
    }
    QJsonDocument nextDoc = serializeInMemoryTranscriptDocument();
    if (!nextDoc.isObject()) {
        return;
    }
    jcut::TranscriptSourceSpec source;
    source.sourcePath =
        m_transcriptSession.clipFilePath().toStdString();
    jcut::TranscriptCutSessionOptions options;
    options.requestedActivePath =
        m_transcriptSession.transcriptPath().toStdString();
    options.ensureEditable = false;
    jcut::TranscriptCutSession session =
        jcut::loadTranscriptCutSession(source, options);
    session.activeDocument =
        jcut::TranscriptDocumentCore::fromJson(
            jcut::jsonio::toJson(
                QJsonValue(nextDoc.object())));
    std::string createError;
    const std::optional<std::string> nextPath =
        jcut::createTranscriptCutVersion(
            session, &createError);
    if (!nextPath) {
        if (!createError.empty()) {
            qWarning() << "Create transcript cut failed:"
                       << QString::fromStdString(createError);
        }
        return;
    }

    refreshScriptVersionSelector(
        m_transcriptSession.clipFilePath(),
        QString::fromStdString(*nextPath));
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
    if (!m_widgets.transcriptScriptVersionCombo || m_transcriptSession.clipFilePath().isEmpty()) {
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
            scriptVersionLabelForPath(selectedPath, m_transcriptSession.clipFilePath()));
        return;
    }

    QJsonDocument doc;
    if (selectedPath == m_transcriptSession.transcriptPath()) {
        doc = serializeInMemoryTranscriptDocument();
    } else {
        loadTranscriptJsonCached(selectedPath, &doc);
    }
    if (!doc.isObject()) return;
    jcut::TranscriptSourceSpec source;
    source.sourcePath =
        m_transcriptSession.clipFilePath().toStdString();
    jcut::TranscriptCutSessionOptions options;
    options.requestedActivePath = selectedPath.toStdString();
    options.ensureEditable = false;
    jcut::TranscriptCutSession session =
        jcut::loadTranscriptCutSession(source, options);
    session.activePath = selectedPath.toStdString();
    session.activeCutMutable =
        selectedPath != originalTranscriptPathForClip(
            m_transcriptSession.clipFilePath());
    session.activeDocument =
        jcut::TranscriptDocumentCore::fromJson(
            jcut::jsonio::toJson(
                QJsonValue(doc.object())));
    std::string renameError;
    if (!jcut::renameTranscriptCut(
            session, newLabel.toStdString(),
            &renameError)) {
        if (!renameError.empty()) {
            qWarning() << "Rename transcript cut failed:"
                       << QString::fromStdString(renameError);
        }
        return;
    }
    if (selectedPath ==
        m_transcriptSession.transcriptPath()) {
        QJsonObject root = doc.object();
        root[QStringLiteral("cut_label")] = newLabel;
        m_transcriptSession.assign(
            m_transcriptSession.clipFilePath(),
            m_transcriptSession.transcriptPath(),
            QJsonDocument(root));
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
    if (!m_widgets.transcriptScriptVersionCombo || m_transcriptSession.clipFilePath().isEmpty()) {
        return;
    }
    const QString selectedPath = m_widgets.transcriptScriptVersionCombo->currentData().toString();
    if (selectedPath.isEmpty() ||
        selectedPath == originalTranscriptPathForClip(m_transcriptSession.clipFilePath())) {
        return;
    }
    const QString selectedLabel = m_widgets.transcriptScriptVersionCombo->currentText().trimmed();
    const QString label = selectedLabel.isEmpty() ? QFileInfo(selectedPath).fileName() : selectedLabel;
    if (!shouldSkipTranscriptDeleteConfirmation()) {
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
    }
    jcut::TranscriptSourceSpec source;
    source.sourcePath =
        m_transcriptSession.clipFilePath().toStdString();
    jcut::TranscriptCutSessionOptions options;
    options.requestedActivePath = selectedPath.toStdString();
    options.ensureEditable = false;
    jcut::TranscriptCutSession session =
        jcut::loadTranscriptCutSession(source, options);
    session.activePath = selectedPath.toStdString();
    session.activeCutMutable = true;
    std::string fallbackPath;
    std::string deleteError;
    if (!jcut::deleteTranscriptCut(
            session, &fallbackPath, &deleteError)) {
        if (!deleteError.empty()) {
            qWarning() << "Delete transcript cut failed:"
                       << QString::fromStdString(deleteError);
        }
        return;
    }
    refreshScriptVersionSelector(
        m_transcriptSession.clipFilePath(),
        fallbackPath.empty()
            ? defaultEditablePathForClip(
                  m_transcriptSession.clipFilePath())
            : QString::fromStdString(fallbackPath));
    refresh();
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
}
