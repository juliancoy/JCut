#include "transcript_tab.h"

#include "clip_serialization.h"
#include "editor_shared.h"

#include <QAbstractItemView>
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
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <memory>

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

} // namespace

quint64 TranscriptTab::transcriptWordKey(int segmentIndex, int wordIndex) const
{
    return (static_cast<quint64>(static_cast<quint32>(segmentIndex)) << 32) |
           static_cast<quint32>(wordIndex);
}

bool TranscriptTab::rebuildInMemoryTranscriptDocument(const QJsonDocument& document)
{
    m_transcriptDocumentSegments.clear();
    m_transcriptWordAddressById.clear();
    m_renderOrderedWordIds.clear();
    m_nextTranscriptWordId = 1;
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
            word.wordId = m_nextTranscriptWordId++;
            word.text = wordObject.value(QStringLiteral("word")).toString();
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
            QJsonObject wordObject;
            wordObject[QStringLiteral("word")] = word.text;
            wordObject[QStringLiteral("start")] = word.startSeconds;
            wordObject[QStringLiteral("end")] = word.endSeconds;
            if (!word.speaker.trimmed().isEmpty()) {
                wordObject[QString(kTranscriptWordSpeakerKey)] = word.speaker.trimmed();
            }
            if (word.skipped) {
                wordObject[QString(kTranscriptWordSkippedKey)] = true;
            }
            if (!word.editTags.isEmpty()) {
                QJsonArray editArray;
                for (const QString& tag : word.editTags) {
                    editArray.push_back(tag);
                }
                wordObject[QString(kTranscriptWordEditsKey)] = editArray;
            }
            if (word.renderOrder >= 0) {
                wordObject[QString(kTranscriptWordRenderOrderKey)] = word.renderOrder;
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

    const bool canReuseLoadedDoc =
        m_transcriptSession.hasObjectDocument() &&
        m_transcriptSession.matches(clip.filePath, transcriptPath);

    if (!canReuseLoadedDoc) {
        if (!QFileInfo::exists(transcriptPath)) {
            if (transcriptPath != baseEditablePath && QFileInfo::exists(baseEditablePath)) {
                transcriptPath = baseEditablePath;
                refreshScriptVersionSelector(clip.filePath, transcriptPath);
                setActiveTranscriptPathForClipFile(clip.filePath, transcriptPath);
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
    if (!rebuildInMemoryTranscriptDocument(m_transcriptSession.document())) {
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

QVector<TranscriptTab::TranscriptRow> TranscriptTab::parseTranscriptRows(int prependMs,
                                                                         int postpendMs) const
{
    QVector<TranscriptRow> rows;
    const double prependSeconds = prependMs / 1000.0;
    const double postpendSeconds = postpendMs / 1000.0;
    rows.reserve(m_renderOrderedWordIds.size());

    for (const int wordId : m_renderOrderedWordIds) {
        const auto addressIt = m_transcriptWordAddressById.constFind(wordId);
        if (addressIt == m_transcriptWordAddressById.cend()) {
            continue;
        }
        const TranscriptWordAddress& address = addressIt.value();
        const TranscriptDocumentWord* word = transcriptWordById(wordId);
        if (!word || address.segmentIndex < 0 || address.segmentIndex >= m_transcriptDocumentSegments.size()) {
            continue;
        }
        const TranscriptDocumentSegment& segment = m_transcriptDocumentSegments.at(address.segmentIndex);
        const QString text = word->text;
        const double rawStartTime = word->startSeconds;
        const double rawEndTime = word->endSeconds;
        if (text.trimmed().isEmpty() || rawStartTime < 0.0 || rawEndTime < rawStartTime) {
            continue;
        }

        const double adjustedStartTime = qMax(0.0, rawStartTime - prependSeconds);
        const double adjustedEndTime = qMax(adjustedStartTime, rawEndTime + postpendSeconds);

        TranscriptRow row;
        row.startFrame = qMax<int64_t>(0, static_cast<int64_t>(std::floor(adjustedStartTime * kTimelineFps)));
        row.endFrame = qMax<int64_t>(row.startFrame,
                                     static_cast<int64_t>(std::ceil(adjustedEndTime * kTimelineFps)) - 1);
        row.speaker = word->speaker.trimmed();
        if (row.speaker.isEmpty()) {
            row.speaker = segment.metadata.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        }
        if (row.speaker.isEmpty()) {
            row.speaker = QStringLiteral("Unknown");
        }
        row.text = text;
        row.isSkipped = word->skipped;
        row.editFlags = kEditFlagNone;
        for (const QString& editTag : word->editTags) {
            if (editTag == QString(kTranscriptEditTimingTag)) {
                row.editFlags |= kEditFlagTiming;
            } else if (editTag == QString(kTranscriptEditTextTag)) {
                row.editFlags |= kEditFlagText;
            } else if (editTag == QString(kTranscriptEditSkipTag)) {
                row.editFlags |= kEditFlagSkip;
            } else if (editTag == QString(kTranscriptEditInsertedTag)) {
                row.editFlags |= kEditFlagInserted;
            }
        }
        row.segmentIndex = address.segmentIndex;
        row.wordIndex = address.wordIndex;
        row.wordId = wordId;
        row.originalSegmentIndex = word->originalSegmentIndex >= 0 ? word->originalSegmentIndex : address.segmentIndex;
        row.originalWordIndex = word->originalWordIndex >= 0 ? word->originalWordIndex : address.wordIndex;
        row.renderOrder = word->renderOrder;
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
    QSignalBlocker tableBlocker(m_widgets.transcriptTable);
    std::unique_ptr<QSignalBlocker> selectionBlocker;
    if (m_widgets.transcriptTable->selectionModel()) {
        selectionBlocker = std::make_unique<QSignalBlocker>(m_widgets.transcriptTable->selectionModel());
    }
    m_widgets.transcriptTable->setUpdatesEnabled(false);

    m_widgets.transcriptTable->setRowCount(rows.size());
    if (QHeaderView* vertical = m_widgets.transcriptTable->verticalHeader()) {
        vertical->setSectionResizeMode(QHeaderView::Fixed);
        vertical->setDefaultSectionSize(kTranscriptTableRowHeight);
        vertical->setMinimumSectionSize(kTranscriptTableRowHeight);
    }
    int restoreRow = -1;
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

        m_widgets.transcriptTable->setItem(row, kTranscriptColSourceStart, sourceStartItem);
        m_widgets.transcriptTable->setItem(row, kTranscriptColSourceEnd, sourceEndItem);
        m_widgets.transcriptTable->setItem(row, kTranscriptColSpeaker, speakerItem);
        m_widgets.transcriptTable->setItem(row, kTranscriptColText, textItem);
        m_widgets.transcriptTable->setItem(row, kTranscriptColEdits, editsItem);
        m_widgets.transcriptTable->setRowHeight(row, kTranscriptTableRowHeight);
    }

    if (restoreRow >= 0 && m_widgets.transcriptTable->selectionModel()) {
        m_suppressSelectionSideEffects = true;
        m_widgets.transcriptTable->setCurrentCell(restoreRow, 0);
        m_widgets.transcriptTable->selectRow(restoreRow);
        m_suppressSelectionSideEffects = false;
    }

    m_widgets.transcriptTable->setUpdatesEnabled(true);
    rebuildFollowRanges(rows);
}

void TranscriptTab::startTranscriptRowsBuildRequest(const QString& originalPath)
{
    const qint64 requestId = ++m_transcriptRowsBuildRequestId;
    const QVector<TranscriptDocumentSegment> segments = m_transcriptDocumentSegments;
    const QHash<int, TranscriptWordAddress> addresses = m_transcriptWordAddressById;
    const QVector<int> renderOrder = m_renderOrderedWordIds;
    const QString activeTranscriptPath = m_transcriptSession.transcriptPath();
    const int prependMs = transcriptPrependMs();
    const int postpendMs = transcriptPostpendMs();
    const bool includeOutsideCut =
        showOutsideCutLinesEnabled() &&
        activeTranscriptPath != originalPath &&
        QFileInfo::exists(originalPath);

    m_transcriptRowsBuildWatcher.setFuture(QtConcurrent::run(
        [requestId,
         segments,
         addresses,
         renderOrder,
         activeTranscriptPath,
         originalPath,
         prependMs,
         postpendMs,
         includeOutsideCut]() {
            auto wordAt = [](const QVector<TranscriptDocumentSegment>& docSegments,
                             int segmentIndex,
                             int wordIndex) -> const TranscriptDocumentWord* {
                if (segmentIndex < 0 || segmentIndex >= docSegments.size()) {
                    return nullptr;
                }
                const QVector<TranscriptDocumentWord>& words = docSegments.at(segmentIndex).words;
                if (wordIndex < 0 || wordIndex >= words.size()) {
                    return nullptr;
                }
                return &words.at(wordIndex);
            };
            auto wordById = [&wordAt](const QVector<TranscriptDocumentSegment>& docSegments,
                                      const QHash<int, TranscriptWordAddress>& docAddresses,
                                      int wordId) -> const TranscriptDocumentWord* {
                const auto it = docAddresses.constFind(wordId);
                if (it == docAddresses.cend()) {
                    return nullptr;
                }
                return wordAt(docSegments, it->segmentIndex, it->wordIndex);
            };
            auto rebuildDoc = [](const QJsonDocument& document,
                                 QVector<TranscriptDocumentSegment>* docSegments,
                                 QHash<int, TranscriptWordAddress>* docAddresses,
                                 QVector<int>* docRenderOrder) {
                if (!document.isObject() || !docSegments || !docAddresses || !docRenderOrder) {
                    return false;
                }
                struct RenderLocation {
                    int wordId = -1;
                    int renderOrder = -1;
                    double startSeconds = 0.0;
                    int fallbackOrder = 0;
                };
                docSegments->clear();
                docAddresses->clear();
                docRenderOrder->clear();
                int nextWordId = 1;
                int fallbackOrder = 0;
                QVector<RenderLocation> renderLocations;
                const QJsonArray jsonSegments = document.object().value(QStringLiteral("segments")).toArray();
                docSegments->reserve(jsonSegments.size());
                for (const QJsonValue& segmentValue : jsonSegments) {
                    const QJsonObject segmentObject = segmentValue.toObject();
                    TranscriptDocumentSegment segment;
                    segment.metadata = segmentObject;
                    segment.metadata.remove(QStringLiteral("words"));
                    const QJsonArray words = segmentObject.value(QStringLiteral("words")).toArray();
                    segment.words.reserve(words.size());
                    const int segmentIndex = docSegments->size();
                    for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
                        const QJsonObject wordObject = words.at(wordIndex).toObject();
                        TranscriptDocumentWord word;
                        word.wordId = nextWordId++;
                        word.text = wordObject.value(QStringLiteral("word")).toString();
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
                            wordObject.value(QString(kTranscriptWordOriginalSegmentKey)).toInt(segmentIndex);
                        word.originalWordIndex =
                            wordObject.value(QString(kTranscriptWordOriginalWordKey)).toInt(wordIndex);
                        segment.words.push_back(word);
                        docAddresses->insert(word.wordId, TranscriptWordAddress{segmentIndex, wordIndex});
                        renderLocations.push_back(
                            RenderLocation{word.wordId, word.renderOrder, word.startSeconds, fallbackOrder++});
                    }
                    docSegments->push_back(segment);
                }
                std::sort(renderLocations.begin(), renderLocations.end(), [](const RenderLocation& a,
                                                                              const RenderLocation& b) {
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
                docRenderOrder->reserve(renderLocations.size());
                for (const RenderLocation& location : std::as_const(renderLocations)) {
                    docRenderOrder->push_back(location.wordId);
                }
                return true;
            };
            auto buildRows = [&](const QVector<TranscriptDocumentSegment>& docSegments,
                                 const QHash<int, TranscriptWordAddress>& docAddresses,
                                 const QVector<int>& docRenderOrder) {
                QVector<TranscriptRow> rows;
                const double prependSeconds = prependMs / 1000.0;
                const double postpendSeconds = postpendMs / 1000.0;
                rows.reserve(docRenderOrder.size());
                for (const int wordId : docRenderOrder) {
                    const auto addressIt = docAddresses.constFind(wordId);
                    if (addressIt == docAddresses.cend()) {
                        continue;
                    }
                    const TranscriptWordAddress& address = addressIt.value();
                    const TranscriptDocumentWord* word = wordById(docSegments, docAddresses, wordId);
                    if (!word || address.segmentIndex < 0 || address.segmentIndex >= docSegments.size()) {
                        continue;
                    }
                    const TranscriptDocumentSegment& segment = docSegments.at(address.segmentIndex);
                    if (word->text.trimmed().isEmpty() ||
                        word->startSeconds < 0.0 ||
                        word->endSeconds < word->startSeconds) {
                        continue;
                    }
                    const double adjustedStartTime = qMax(0.0, word->startSeconds - prependSeconds);
                    const double adjustedEndTime =
                        qMax(adjustedStartTime, word->endSeconds + postpendSeconds);
                    TranscriptRow row;
                    row.startFrame =
                        qMax<int64_t>(0, static_cast<int64_t>(std::floor(adjustedStartTime * kTimelineFps)));
                    row.endFrame =
                        qMax<int64_t>(row.startFrame,
                                      static_cast<int64_t>(std::ceil(adjustedEndTime * kTimelineFps)) - 1);
                    row.speaker = word->speaker.trimmed();
                    if (row.speaker.isEmpty()) {
                        row.speaker =
                            segment.metadata.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
                    }
                    if (row.speaker.isEmpty()) {
                        row.speaker = QStringLiteral("Unknown");
                    }
                    row.text = word->text;
                    row.isSkipped = word->skipped;
                    row.editFlags = kEditFlagNone;
                    for (const QString& editTag : word->editTags) {
                        if (editTag == QString(kTranscriptEditTimingTag)) {
                            row.editFlags |= kEditFlagTiming;
                        } else if (editTag == QString(kTranscriptEditTextTag)) {
                            row.editFlags |= kEditFlagText;
                        } else if (editTag == QString(kTranscriptEditSkipTag)) {
                            row.editFlags |= kEditFlagSkip;
                        } else if (editTag == QString(kTranscriptEditInsertedTag)) {
                            row.editFlags |= kEditFlagInserted;
                        }
                    }
                    row.segmentIndex = address.segmentIndex;
                    row.wordIndex = address.wordIndex;
                    row.wordId = wordId;
                    row.originalSegmentIndex =
                        word->originalSegmentIndex >= 0 ? word->originalSegmentIndex : address.segmentIndex;
                    row.originalWordIndex =
                        word->originalWordIndex >= 0 ? word->originalWordIndex : address.wordIndex;
                    row.renderOrder = word->renderOrder;
                    rows.push_back(row);
                }
                return rows;
            };
            auto adjustRows = [](QVector<TranscriptRow>* rows) {
                if (!rows) {
                    return;
                }
                for (int i = 1; i < rows->size(); ++i) {
                    TranscriptRow& previous = (*rows)[i - 1];
                    TranscriptRow& current = (*rows)[i];
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
            };
            auto insertGaps = [](QVector<TranscriptRow>* rows) {
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
                                               (static_cast<double>(gapFrames) /
                                                static_cast<double>(kTimelineFps)) * 1000.0)));
                            gap.isGap = true;
                            expanded.push_back(gap);
                        }
                    }
                    expanded.push_back(current);
                }
                *rows = std::move(expanded);
            };
            auto computeRender = [](QVector<TranscriptRow>* rows) {
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
            };

            QVector<TranscriptRow> allRows = buildRows(segments, addresses, renderOrder);
            adjustRows(&allRows);
            insertGaps(&allRows);
            computeRender(&allRows);

            if (includeOutsideCut) {
                QJsonDocument originalDoc;
                if (loadTranscriptJsonCached(originalPath, &originalDoc) && originalDoc.isObject()) {
                    QVector<TranscriptDocumentSegment> originalSegments;
                    QHash<int, TranscriptWordAddress> originalAddresses;
                    QVector<int> originalRenderOrder;
                    if (rebuildDoc(originalDoc, &originalSegments, &originalAddresses, &originalRenderOrder)) {
                        QVector<TranscriptRow> originalRows =
                            buildRows(originalSegments, originalAddresses, originalRenderOrder);
                        QSet<QString> activeKeys;
                        for (const TranscriptRow& row : std::as_const(allRows)) {
                            activeKeys.insert(QStringLiteral("%1:%2")
                                                  .arg(row.originalSegmentIndex)
                                                  .arg(row.originalWordIndex));
                        }
                        for (TranscriptRow row : originalRows) {
                            const QString key = QStringLiteral("%1:%2")
                                                    .arg(row.originalSegmentIndex)
                                                    .arg(row.originalWordIndex);
                            if (activeKeys.contains(key)) {
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

            Q_UNUSED(activeTranscriptPath)
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
    return transcriptEditablePathForClipFile(clipFilePath);
}

QString TranscriptTab::originalTranscriptPathForClip(const QString& clipFilePath) const
{
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

    QVector<int> orderedWordIds;
    orderedWordIds.reserve(m_widgets.transcriptTable->rowCount());
    for (int row = 0; row < m_widgets.transcriptTable->rowCount(); ++row) {
        QTableWidgetItem* sourceItem = m_widgets.transcriptTable->item(row, kTranscriptColSourceStart);
        if (!sourceItem || sourceItem->data(Qt::UserRole + 4).toBool() ||
            sourceItem->data(Qt::UserRole + 12).toBool()) {
            continue;
        }
        const int wordId = sourceItem->data(Qt::UserRole + 16).toInt();
        if (wordId >= 0) {
            orderedWordIds.push_back(wordId);
        }
    }
    if (orderedWordIds.isEmpty()) {
        return;
    }

    QSet<int> seen;
    seen.reserve(orderedWordIds.size());
    QVector<int> nextOrder;
    nextOrder.reserve(m_renderOrderedWordIds.size());
    for (const int wordId : std::as_const(orderedWordIds)) {
        if (!seen.contains(wordId) && transcriptWordById(wordId)) {
            seen.insert(wordId);
            nextOrder.push_back(wordId);
        }
    }
    for (const int wordId : std::as_const(m_renderOrderedWordIds)) {
        if (!seen.contains(wordId) && transcriptWordById(wordId)) {
            nextOrder.push_back(wordId);
        }
    }
    if (nextOrder == m_renderOrderedWordIds) {
        return;
    }
    m_renderOrderedWordIds = nextOrder;
    normalizeTranscriptRenderOrder();
    rehydrateLoadedTranscriptDocumentFromMemory();
    saveLoadedTranscriptDocument();
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
    const QString nextPath = nextScriptVersionPathForClip(m_transcriptSession.clipFilePath());
    QJsonDocument nextDoc = serializeInMemoryTranscriptDocument();
    if (!nextDoc.isObject()) {
        return;
    }
    QJsonObject root = nextDoc.object();
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
    QFile::remove(nextPath);
    if (!saveTranscriptDocumentToPath(&m_transcriptEngine, nextPath, root)) {
        return;
    }

    refreshScriptVersionSelector(m_transcriptSession.clipFilePath(), nextPath);
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

    // Persist the custom label into the transcript JSON's root as "cut_label"
    QJsonDocument doc;
    if (selectedPath == m_transcriptSession.transcriptPath()) {
        doc = serializeInMemoryTranscriptDocument();
    } else {
        loadTranscriptJsonCached(selectedPath, &doc);
    }
    if (doc.isObject()) {
        QJsonObject root = doc.object();
        root[QStringLiteral("cut_label")] = newLabel;
        if (saveTranscriptDocumentToPath(&m_transcriptEngine, selectedPath, root) &&
            selectedPath == m_transcriptSession.transcriptPath()) {
            m_transcriptSession.assign(
                m_transcriptSession.clipFilePath(),
                m_transcriptSession.transcriptPath(),
                QJsonDocument(root));
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
    QFile::remove(selectedPath);
    const QString basePath = defaultEditablePathForClip(m_transcriptSession.clipFilePath());
    refreshScriptVersionSelector(m_transcriptSession.clipFilePath(), basePath);
    refresh();
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
}
