#include "transcript_tab.h"

#include "clip_serialization.h"
#include "editor_shared.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSet>
#include <cmath>

namespace {
constexpr qint64 kManualTranscriptSelectionHoldMs = 1200;
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
const QLatin1String kTranscriptSpeakerProfilesKey("speaker_profiles");
const QLatin1String kTranscriptSpeakerNameKey("name");
const QLatin1String kTranscriptSpeakerLocationKey("location");
const QLatin1String kTranscriptSpeakerLocationXKey("x");
const QLatin1String kTranscriptSpeakerLocationYKey("y");
const QLatin1String kAllSpeakersFilterValue("__all__");

enum TranscriptTableColumn {
    kTranscriptColSourceStart = 0,
    kTranscriptColSourceEnd = 1,
    kTranscriptColRenderStart = 2,
    kTranscriptColRenderEnd = 3,
    kTranscriptColSpeaker = 4,
    kTranscriptColText = 5,
    kTranscriptColEdits = 6
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

void transcriptAppendEditTag(QJsonObject* word, const QString& tag)
{
    if (!word) {
        return;
    }
    QJsonArray editArray = word->value(QString(kTranscriptWordEditsKey)).toArray();
    for (const QJsonValue& value : editArray) {
        if (value.toString() == tag) {
            return;
        }
    }
    editArray.push_back(tag);
    (*word)[QString(kTranscriptWordEditsKey)] = editArray;
}

bool clipSupportsTranscript(const TimelineClip& clip) {
    return clip.mediaType == ClipMediaType::Audio || clip.hasAudio;
}
}

TranscriptTab::TranscriptTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
    m_manualSelectionTimer.invalidate();
    m_deferredSeekTimer.setSingleShot(true);
    connect(&m_deferredSeekTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingSeekTimelineFrame < 0 || !m_deps.seekToTimelineFrame) {
            return;
        }
        m_deps.seekToTimelineFrame(m_pendingSeekTimelineFrame);
        m_pendingSeekTimelineFrame = -1;
    });
}

void TranscriptTab::wire()
{
    // Wire up the editable cut title
    if (m_widgets.transcriptInspectorClipLabel) {
        connect(m_widgets.transcriptInspectorClipLabel, &QLineEdit::editingFinished, this, [this]() {
            if (m_updating || !m_deps.getSelectedClip || !m_deps.updateClipById) {
                return;
            }
            const TimelineClip* clip = m_deps.getSelectedClip();
            if (!clip) {
                return;
            }
            const QString newLabel = m_widgets.transcriptInspectorClipLabel->text().trimmed();
            if (newLabel.isEmpty() || newLabel == clip->label) {
                // Revert to actual clip label if empty or unchanged
                if (newLabel.isEmpty()) {
                    m_widgets.transcriptInspectorClipLabel->setText(clip->label);
                }
                return;
            }
            m_deps.updateClipById(clip->id, [&newLabel](TimelineClip& editableClip) {
                editableClip.label = newLabel;
            });
            if (m_deps.scheduleSaveState) {
                m_deps.scheduleSaveState();
            }
            if (m_deps.pushHistorySnapshot) {
                m_deps.pushHistorySnapshot();
            }
            if (m_deps.refreshInspector) {
                m_deps.refreshInspector();
            }
        });
    }

    if (m_widgets.transcriptTable) {
        m_widgets.transcriptTable->setDragEnabled(true);
        m_widgets.transcriptTable->setAcceptDrops(true);
        m_widgets.transcriptTable->viewport()->setAcceptDrops(true);
        m_widgets.transcriptTable->setDropIndicatorShown(true);
        m_widgets.transcriptTable->setDragDropMode(QAbstractItemView::InternalMove);
        m_widgets.transcriptTable->setDefaultDropAction(Qt::MoveAction);
        connect(m_widgets.transcriptTable, &QTableWidget::itemClicked,
                this, &TranscriptTab::onTranscriptItemClicked);
        connect(m_widgets.transcriptTable, &QTableWidget::itemDoubleClicked,
                this, &TranscriptTab::onTranscriptItemDoubleClicked);
        connect(m_widgets.transcriptTable, &QTableWidget::itemChanged,
                this, &TranscriptTab::applyTableEdit);
        connect(m_widgets.transcriptTable, &QTableWidget::itemSelectionChanged,
                this, &TranscriptTab::onTranscriptSelectionChanged);
        m_widgets.transcriptTable->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.transcriptTable, &QWidget::customContextMenuRequested,
                this, &TranscriptTab::onTranscriptCustomContextMenu);
        if (m_widgets.transcriptTable->horizontalHeader()) {
            m_widgets.transcriptTable->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(m_widgets.transcriptTable->horizontalHeader(), &QHeaderView::customContextMenuRequested,
                    this, &TranscriptTab::onTranscriptHeaderContextMenu);
        }
        m_widgets.transcriptTable->installEventFilter(this);
        if (m_widgets.transcriptTable->viewport()) {
            m_widgets.transcriptTable->viewport()->installEventFilter(this);
        }
        if (m_widgets.transcriptTable->model()) {
            connect(m_widgets.transcriptTable->model(), &QAbstractItemModel::rowsMoved, this,
                    [this](const QModelIndex&, int, int, const QModelIndex&, int) {
                        if (m_updating) {
                            return;
                        }
                        persistRenderOrderFromTable();
                    });
        }
    }
    if (m_widgets.transcriptFollowCurrentWordCheckBox) {
        connect(m_widgets.transcriptFollowCurrentWordCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onFollowCurrentWordToggled);
    }
    if (m_widgets.transcriptOverlayEnabledCheckBox) {
        connect(m_widgets.transcriptOverlayEnabledCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptBackgroundVisibleCheckBox) {
        connect(m_widgets.transcriptBackgroundVisibleCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptMaxLinesSpin) {
        connect(m_widgets.transcriptMaxLinesSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptMaxCharsSpin) {
        connect(m_widgets.transcriptMaxCharsSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptAutoScrollCheckBox) {
        connect(m_widgets.transcriptAutoScrollCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptOverlayXSpin) {
        connect(m_widgets.transcriptOverlayXSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptOverlayYSpin) {
        connect(m_widgets.transcriptOverlayYSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptOverlayWidthSpin) {
        connect(m_widgets.transcriptOverlayWidthSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptOverlayHeightSpin) {
        connect(m_widgets.transcriptOverlayHeightSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptFontFamilyCombo) {
        connect(m_widgets.transcriptFontFamilyCombo, &QFontComboBox::currentFontChanged,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptFontSizeSpin) {
        connect(m_widgets.transcriptFontSizeSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptBoldCheckBox) {
        connect(m_widgets.transcriptBoldCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptItalicCheckBox) {
        connect(m_widgets.transcriptItalicCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptPrependMsSpin) {
        connect(m_widgets.transcriptPrependMsSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onPrependMsChanged);
    }
    if (m_widgets.transcriptPostpendMsSpin) {
        connect(m_widgets.transcriptPostpendMsSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onPostpendMsChanged);
    }
    if (m_widgets.speechFilterEnabledCheckBox) {
        connect(m_widgets.speechFilterEnabledCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onSpeechFilterEnabledToggled);
    }
    if (m_widgets.speechFilterFadeSamplesSpin) {
        connect(m_widgets.speechFilterFadeSamplesSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onSpeechFilterFadeSamplesChanged);
    }
    if (m_widgets.transcriptUnifiedEditModeCheckBox) {
        connect(m_widgets.transcriptUnifiedEditModeCheckBox, &QCheckBox::toggled,
                this, [this](bool) { refresh(); });
    }
    if (m_widgets.transcriptSpeakerFilterCombo) {
        connect(m_widgets.transcriptSpeakerFilterCombo, &QComboBox::currentIndexChanged,
                this, [this](int) { refresh(); });
    }
    if (m_widgets.transcriptShowExcludedLinesCheckBox) {
        connect(m_widgets.transcriptShowExcludedLinesCheckBox, &QCheckBox::toggled,
                this, [this](bool) { refresh(); });
    }
    if (m_widgets.transcriptScriptVersionCombo) {
        connect(m_widgets.transcriptScriptVersionCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &TranscriptTab::onTranscriptScriptVersionChanged);
        // Wire up editable combo for renaming cut versions
        if (m_widgets.transcriptScriptVersionCombo->lineEdit()) {
            connect(m_widgets.transcriptScriptVersionCombo->lineEdit(), &QLineEdit::editingFinished,
                    this, &TranscriptTab::onTranscriptCutLabelEdited);
        }
    }
    if (m_widgets.transcriptNewVersionButton) {
        connect(m_widgets.transcriptNewVersionButton, &QPushButton::clicked,
                this, &TranscriptTab::onTranscriptCreateVersion);
    }
    if (m_widgets.transcriptDeleteVersionButton) {
        connect(m_widgets.transcriptDeleteVersionButton, &QPushButton::clicked,
                this, &TranscriptTab::onTranscriptDeleteVersion);
    }
    if (m_widgets.speakersTable) {
        connect(m_widgets.speakersTable, &QTableWidget::itemChanged,
                this, &TranscriptTab::onSpeakersTableItemChanged);
    }
}

void TranscriptTab::refresh()
{
    // (Removed early return to ensure the UI updates to reflect truth when a row is modified)


    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    m_updating = true;
    m_manualSelectionTimer.invalidate();

    if (m_widgets.transcriptTable) {
        m_widgets.transcriptTable->clearContents();
        m_widgets.transcriptTable->setRowCount(0);
    }
    m_loadedTranscriptPath.clear();
    m_loadedClipFilePath.clear();
    m_loadedTranscriptDoc = QJsonDocument();

    if (!clip || !clipSupportsTranscript(*clip)) {
        m_persistedSelectedClipId.clear();
        m_persistedSelectedSegmentIndex = -1;
        m_persistedSelectedWordIndex = -1;
        if (m_widgets.transcriptInspectorClipLabel) {
            m_widgets.transcriptInspectorClipLabel->setText(QString());
            m_widgets.transcriptInspectorClipLabel->setPlaceholderText(QStringLiteral("No transcript selected"));
        }
        if (m_widgets.transcriptInspectorDetailsLabel) {
            m_widgets.transcriptInspectorDetailsLabel->setText(
                QStringLiteral("Select an audio clip with a WhisperX JSON transcript."));
        }
        if (m_widgets.speakersInspectorClipLabel) {
            m_widgets.speakersInspectorClipLabel->setText(QStringLiteral("No transcript cut selected"));
        }
        if (m_widgets.speakersInspectorDetailsLabel) {
            m_widgets.speakersInspectorDetailsLabel->setText(
                QStringLiteral("Select a transcript cut to name speakers and set on-screen locations."));
        }
        if (m_widgets.speakersTable) {
            m_widgets.speakersTable->clearContents();
            m_widgets.speakersTable->setRowCount(0);
            m_widgets.speakersTable->setEnabled(false);
        }
        refreshScriptVersionSelector(QString(), QString());
        m_updating = false;
        return;
    }

    if (m_widgets.transcriptInspectorClipLabel) {
        m_widgets.transcriptInspectorClipLabel->setText(clip->label);
        m_widgets.transcriptInspectorClipLabel->setPlaceholderText(QString());
    }

    updateOverlayWidgetsFromClip(*clip);
    loadTranscriptFile(*clip);
    m_updating = false;
}

void TranscriptTab::applyOverlayFromInspector(bool pushHistory)
{
    if (m_updating || !m_deps.getSelectedClip || !m_deps.updateClipById) return;

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip) return;

    const bool updated = m_deps.updateClipById(selectedClip->id, [this](TimelineClip& clip) {
        clip.transcriptOverlay.enabled = m_widgets.transcriptOverlayEnabledCheckBox &&
                                         m_widgets.transcriptOverlayEnabledCheckBox->isChecked();
        clip.transcriptOverlay.showBackground = m_widgets.transcriptBackgroundVisibleCheckBox &&
                                                m_widgets.transcriptBackgroundVisibleCheckBox->isChecked();
        clip.transcriptOverlay.maxLines = m_widgets.transcriptMaxLinesSpin
            ? m_widgets.transcriptMaxLinesSpin->value()
            : 2;
        clip.transcriptOverlay.maxCharsPerLine = m_widgets.transcriptMaxCharsSpin
            ? m_widgets.transcriptMaxCharsSpin->value()
            : 28;
        clip.transcriptOverlay.autoScroll = m_widgets.transcriptAutoScrollCheckBox &&
                                            m_widgets.transcriptAutoScrollCheckBox->isChecked();
        clip.transcriptOverlay.translationX = m_widgets.transcriptOverlayXSpin
            ? m_widgets.transcriptOverlayXSpin->value()
            : 0.0;
        clip.transcriptOverlay.translationY = m_widgets.transcriptOverlayYSpin
            ? m_widgets.transcriptOverlayYSpin->value()
            : 640.0;
        clip.transcriptOverlay.boxWidth = m_widgets.transcriptOverlayWidthSpin
            ? m_widgets.transcriptOverlayWidthSpin->value()
            : 900.0;
        clip.transcriptOverlay.boxHeight = m_widgets.transcriptOverlayHeightSpin
            ? m_widgets.transcriptOverlayHeightSpin->value()
            : 220.0;
        clip.transcriptOverlay.fontFamily = m_widgets.transcriptFontFamilyCombo
            ? m_widgets.transcriptFontFamilyCombo->currentFont().family()
            : kDefaultFontFamily;
        clip.transcriptOverlay.fontPointSize = m_widgets.transcriptFontSizeSpin
            ? m_widgets.transcriptFontSizeSpin->value()
            : 42;
        clip.transcriptOverlay.bold = m_widgets.transcriptBoldCheckBox &&
                                      m_widgets.transcriptBoldCheckBox->isChecked();
        clip.transcriptOverlay.italic = m_widgets.transcriptItalicCheckBox &&
                                        m_widgets.transcriptItalicCheckBox->isChecked();
    });

    if (!updated) return;

    if (m_deps.setPreviewTimelineClips) m_deps.setPreviewTimelineClips();
    if (m_deps.refreshInspector) m_deps.refreshInspector();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (pushHistory && m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TranscriptTab::syncTableToPlayhead(int64_t absolutePlaybackSample, int64_t sourceFrame)
{
    Q_UNUSED(absolutePlaybackSample);
    if (!m_widgets.transcriptTable || m_updating) return;
    if (!m_widgets.transcriptFollowCurrentWordCheckBox ||
        !m_widgets.transcriptFollowCurrentWordCheckBox->isChecked()) {
        return;
    }
    
    // Skip sync if table has focus (user is manually selecting a row)
    QWidget* focus = QApplication::focusWidget();
    if (focus && m_widgets.transcriptTable->isAncestorOf(focus)) {
        return;
    }
    if (hasActiveManualSelection()) {
        return;
    }

    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || !clipSupportsTranscript(*clip) || m_loadedTranscriptPath.isEmpty()) {
        m_widgets.transcriptTable->clearSelection();
        return;
    }

    int matchingRow = -1;
    for (int row = 0; row < m_widgets.transcriptTable->rowCount(); ++row) {
        QTableWidgetItem* startItem = m_widgets.transcriptTable->item(row, 0);
        if (!startItem) continue;
        if (startItem->data(Qt::UserRole + 12).toBool()) continue;
        const int64_t startFrame = startItem->data(Qt::UserRole + 2).toLongLong();
        const int64_t endFrame = startItem->data(Qt::UserRole + 3).toLongLong();
        if (sourceFrame >= startFrame && sourceFrame <= endFrame) {
            matchingRow = row;
            break;
        }
    }

    if (matchingRow < 0) {
        return;
    }

    if (!m_widgets.transcriptTable->selectionModel()->isRowSelected(matchingRow, QModelIndex())) {
        m_suppressSelectionSideEffects = true;
        m_widgets.transcriptTable->setCurrentCell(matchingRow, 0);
        m_widgets.transcriptTable->selectRow(matchingRow);
        m_suppressSelectionSideEffects = false;
    }

    if ((!m_widgets.transcriptAutoScrollCheckBox || m_widgets.transcriptAutoScrollCheckBox->isChecked()) &&
        m_widgets.transcriptTable->item(matchingRow, 0)) {
        QTableWidgetItem* item = m_widgets.transcriptTable->item(matchingRow, 0);
        m_widgets.transcriptTable->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    }
}

void TranscriptTab::applyTableEdit(QTableWidgetItem* item)
{
    if (m_updating || !item || m_loadedTranscriptPath.isEmpty() || !m_loadedTranscriptDoc.isObject()) {
        return;
    }
    if (!activeCutMutable()) {
        refresh();
        return;
    }

    const int segmentIndex = item->data(Qt::UserRole + 5).toInt();
    const int wordIndex = item->data(Qt::UserRole + 6).toInt();
    const bool isGap = item->data(Qt::UserRole + 4).toBool();
    if (isGap || segmentIndex < 0 || wordIndex < 0) return;

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    if (segmentIndex >= segments.size()) return;

    QJsonObject segmentObj = segments.at(segmentIndex).toObject();
    QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
    if (wordIndex >= words.size()) return;

    QJsonObject wordObj = words.at(wordIndex).toObject();
    if (item->column() == kTranscriptColSourceStart || item->column() == kTranscriptColSourceEnd) {
        double seconds = 0.0;
        if (!m_transcriptEngine.parseTranscriptTime(item->text(), &seconds)) {
            refresh();
            return;
        }
        if (item->column() == kTranscriptColSourceStart) {
            const double currentEnd = wordObj.value(QStringLiteral("end")).toDouble(seconds);
            const double currentStart = wordObj.value(QStringLiteral("start")).toDouble(seconds);
            if (!qFuzzyCompare(currentStart + 1.0, qMin(seconds, currentEnd) + 1.0)) {
                transcriptAppendEditTag(&wordObj, QString(kTranscriptEditTimingTag));
            }
            wordObj[QStringLiteral("start")] = qMin(seconds, currentEnd);
        } else {
            const double currentStart = wordObj.value(QStringLiteral("start")).toDouble(0.0);
            const double currentEnd = wordObj.value(QStringLiteral("end")).toDouble(currentStart);
            if (!qFuzzyCompare(currentEnd + 1.0, qMax(seconds, currentStart) + 1.0)) {
                transcriptAppendEditTag(&wordObj, QString(kTranscriptEditTimingTag));
            }
            wordObj[QStringLiteral("end")] = qMax(seconds, currentStart);
        }
    } else if (item->column() == kTranscriptColText) {
        if (wordObj.value(QStringLiteral("word")).toString() != item->text()) {
            transcriptAppendEditTag(&wordObj, QString(kTranscriptEditTextTag));
        }
        wordObj[QStringLiteral("word")] = item->text();
    } else {
        return;
    }

    words.replace(wordIndex, wordObj);
    segmentObj[QStringLiteral("words")] = words;
    segments.replace(segmentIndex, segmentObj);
    root[QStringLiteral("segments")] = segments;
    m_loadedTranscriptDoc.setObject(root);

    if (!m_transcriptEngine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
        refresh();
        return;
    }

    refresh();
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
}

void TranscriptTab::deleteSelectedRows()
{
    if (m_updating || !m_widgets.transcriptTable || m_loadedTranscriptPath.isEmpty() ||
        !m_loadedTranscriptDoc.isObject()) {
        return;
    }
    if (!activeCutMutable()) {
        return;
    }

    QItemSelectionModel* selectionModel = m_widgets.transcriptTable->selectionModel();
    if (!selectionModel) return;

    const QModelIndexList selectedRows = selectionModel->selectedRows();
    if (selectedRows.isEmpty()) return;

    QSet<quint64> deleteKeys;
    for (const QModelIndex& index : selectedRows) {
        const int row = index.row();
        QTableWidgetItem* item = m_widgets.transcriptTable->item(row, 0);
        if (!item || item->data(Qt::UserRole + 4).toBool() || item->data(Qt::UserRole + 12).toBool()) continue;
        const int segmentIndex = item->data(Qt::UserRole + 5).toInt();
        const int wordIndex = item->data(Qt::UserRole + 6).toInt();
        if (segmentIndex < 0 || wordIndex < 0) continue;
        deleteKeys.insert((static_cast<quint64>(segmentIndex) << 32) |
                          static_cast<quint32>(wordIndex));
    }
    if (deleteKeys.isEmpty()) return;

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    int deletedCount = 0;
    for (int segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
        QJsonObject segmentObj = segments.at(segmentIndex).toObject();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        QJsonArray filteredWords;
        for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
            const quint64 key = (static_cast<quint64>(segmentIndex) << 32) |
                                static_cast<quint32>(wordIndex);
            if (!deleteKeys.contains(key)) {
                filteredWords.push_back(words.at(wordIndex));
            } else {
                ++deletedCount;
            }
        }
        segmentObj[QStringLiteral("words")] = filteredWords;
        segments.replace(segmentIndex, segmentObj);
    }

    root[QStringLiteral("segments")] = segments;
    if (deletedCount > 0) {
        const int previousDeletedEdits = root.value(QString(kTranscriptDeletedEditsCountKey)).toInt(0);
        root[QString(kTranscriptDeletedEditsCountKey)] = previousDeletedEdits + deletedCount;
    }
    m_loadedTranscriptDoc.setObject(root);
    if (!m_transcriptEngine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
        refresh();
        return;
    }

    refresh();
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TranscriptTab::setSelectedRowsSkipped(bool skipped)
{
    if (m_updating || !m_widgets.transcriptTable || m_loadedTranscriptPath.isEmpty() ||
        !m_loadedTranscriptDoc.isObject()) {
        return;
    }
    if (!activeCutMutable()) {
        return;
    }

    QItemSelectionModel* selectionModel = m_widgets.transcriptTable->selectionModel();
    if (!selectionModel) {
        return;
    }

    const QModelIndexList selectedRows = selectionModel->selectedRows();
    if (selectedRows.isEmpty()) {
        return;
    }

    QSet<quint64> targetKeys;
    for (const QModelIndex& index : selectedRows) {
        QTableWidgetItem* item = m_widgets.transcriptTable->item(index.row(), 0);
        if (!item || item->data(Qt::UserRole + 4).toBool() || item->data(Qt::UserRole + 12).toBool()) {
            continue;
        }
        const int segmentIndex = item->data(Qt::UserRole + 5).toInt();
        const int wordIndex = item->data(Qt::UserRole + 6).toInt();
        if (segmentIndex < 0 || wordIndex < 0) {
            continue;
        }
        targetKeys.insert((static_cast<quint64>(segmentIndex) << 32) |
                          static_cast<quint32>(wordIndex));
    }
    if (targetKeys.isEmpty()) {
        return;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    bool changed = false;
    for (int segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
        QJsonObject segmentObj = segments.at(segmentIndex).toObject();
        QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
            const quint64 key = (static_cast<quint64>(segmentIndex) << 32) |
                                static_cast<quint32>(wordIndex);
            if (!targetKeys.contains(key)) {
                continue;
            }
            QJsonObject wordObj = words.at(wordIndex).toObject();
            const bool currentSkipped = wordObj.value(kTranscriptWordSkippedKey).toBool(false);
            if (currentSkipped == skipped) {
                continue;
            }
            wordObj[QString(kTranscriptWordSkippedKey)] = skipped;
            transcriptAppendEditTag(&wordObj, QString(kTranscriptEditSkipTag));
            words.replace(wordIndex, wordObj);
            changed = true;
        }
        segmentObj[QStringLiteral("words")] = words;
        segments.replace(segmentIndex, segmentObj);
    }

    if (!changed) {
        return;
    }

    root[QStringLiteral("segments")] = segments;
    m_loadedTranscriptDoc.setObject(root);
    if (!m_transcriptEngine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
        refresh();
        return;
    }

    refresh();
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TranscriptTab::scheduleSeekToTranscriptRow(int row)
{
    if (!m_widgets.transcriptTable || !m_deps.getSelectedClip || !m_deps.seekToTimelineFrame) {
        return;
    }

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip || !clipSupportsTranscript(*selectedClip)) {
        return;
    }

    QTableWidgetItem* item = m_widgets.transcriptTable->item(row, 0);
    if (!item || item->data(Qt::UserRole + 4).toBool() || item->data(Qt::UserRole + 12).toBool()) {
        return;
    }

    const int64_t startFrame = item->data(Qt::UserRole + 2).toLongLong();
    const int64_t timelineFrame = qMax<int64_t>(
        selectedClip->startFrame,
        selectedClip->startFrame + (startFrame - selectedClip->sourceInFrame));
    m_pendingSeekTimelineFrame = timelineFrame;
    m_deferredSeekTimer.start(QApplication::doubleClickInterval());
}

void TranscriptTab::onTranscriptItemClicked(QTableWidgetItem* item)
{
    if (m_updating || m_suppressSelectionSideEffects || !item ||
        !m_deps.getSelectedClip || !m_deps.seekToTimelineFrame) {
        return;
    }

    const Qt::KeyboardModifiers modifiers = QApplication::keyboardModifiers();
    if (modifiers.testFlag(Qt::ShiftModifier) ||
        modifiers.testFlag(Qt::ControlModifier) ||
        modifiers.testFlag(Qt::MetaModifier)) {
        return;
    }
    if (m_widgets.transcriptTable && m_widgets.transcriptTable->selectionModel() &&
        m_widgets.transcriptTable->selectionModel()->selectedRows().size() > 1) {
        return;
    }
    scheduleSeekToTranscriptRow(item->row());
}

void TranscriptTab::onTranscriptItemDoubleClicked(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    m_deferredSeekTimer.stop();
    m_pendingSeekTimelineFrame = -1;
}

void TranscriptTab::onTranscriptSelectionChanged()
{
    if (m_updating || m_suppressSelectionSideEffects || !m_widgets.transcriptTable) {
        return;
    }
    QItemSelectionModel* selectionModel = m_widgets.transcriptTable->selectionModel();
    if (!selectionModel) {
        return;
    }

    const QModelIndexList selectedRows = selectionModel->selectedRows();
    if (selectedRows.isEmpty()) {
        return;
    }

    m_manualSelectionTimer.restart();

    if (selectedRows.size() != 1) {
        m_deferredSeekTimer.stop();
        m_pendingSeekTimelineFrame = -1;
        return;
    }

    if (QTableWidgetItem* item = m_widgets.transcriptTable->item(selectedRows.constFirst().row(), 0);
        item && !item->data(Qt::UserRole + 4).toBool() && !item->data(Qt::UserRole + 12).toBool()) {
        if (const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr) {
            m_persistedSelectedClipId = selectedClip->id;
        }
        m_persistedSelectedSegmentIndex = item->data(Qt::UserRole + 5).toInt();
        m_persistedSelectedWordIndex = item->data(Qt::UserRole + 6).toInt();
    }

    const Qt::KeyboardModifiers modifiers = QApplication::keyboardModifiers();
    if (modifiers.testFlag(Qt::ShiftModifier) ||
        modifiers.testFlag(Qt::ControlModifier) ||
        modifiers.testFlag(Qt::MetaModifier)) {
        return;
    }

    scheduleSeekToTranscriptRow(selectedRows.constFirst().row());
}

bool TranscriptTab::hasActiveManualSelection() const
{
    if (!m_widgets.transcriptTable) {
        return false;
    }
    QItemSelectionModel* selectionModel = m_widgets.transcriptTable->selectionModel();
    if (!selectionModel) {
        return false;
    }
    const QModelIndexList selectedRows = selectionModel->selectedRows();
    if (selectedRows.size() > 1) {
        return true;
    }
    if (!m_manualSelectionTimer.isValid()) {
        return false;
    }
    return m_manualSelectionTimer.elapsed() < kManualTranscriptSelectionHoldMs;
}

void TranscriptTab::onFollowCurrentWordToggled(bool checked)
{
    Q_UNUSED(checked);
    if (m_updating) return;
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.refreshInspector) m_deps.refreshInspector();
}

void TranscriptTab::onOverlaySettingChanged()
{
    applyOverlayFromInspector(true);
}

void TranscriptTab::onPrependMsChanged(int value)
{
    m_transcriptPrependMs = qMax(0, value);
    refresh();
    emit speechFilterParametersChanged();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TranscriptTab::onPostpendMsChanged(int value)
{
    m_transcriptPostpendMs = qMax(0, value);
    refresh();
    emit speechFilterParametersChanged();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TranscriptTab::onSpeechFilterEnabledToggled(bool enabled)
{
    m_speechFilterEnabled = enabled;
    emit speechFilterParametersChanged();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TranscriptTab::onSpeechFilterFadeSamplesChanged(int value)
{
    m_speechFilterFadeSamples = qMax(0, value);
    emit speechFilterParametersChanged();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TranscriptTab::updateOverlayWidgetsFromClip(const TimelineClip& clip)
{
    if (!m_widgets.transcriptOverlayEnabledCheckBox) return;

    QSignalBlocker enabledBlock(m_widgets.transcriptOverlayEnabledCheckBox);
    QSignalBlocker backgroundBlock(m_widgets.transcriptBackgroundVisibleCheckBox);
    QSignalBlocker maxLinesBlock(m_widgets.transcriptMaxLinesSpin);
    QSignalBlocker maxCharsBlock(m_widgets.transcriptMaxCharsSpin);
    QSignalBlocker autoScrollBlock(m_widgets.transcriptAutoScrollCheckBox);
    QSignalBlocker xBlock(m_widgets.transcriptOverlayXSpin);
    QSignalBlocker yBlock(m_widgets.transcriptOverlayYSpin);
    QSignalBlocker widthBlock(m_widgets.transcriptOverlayWidthSpin);
    QSignalBlocker heightBlock(m_widgets.transcriptOverlayHeightSpin);
    QSignalBlocker fontBlock(m_widgets.transcriptFontFamilyCombo);
    QSignalBlocker fontSizeBlock(m_widgets.transcriptFontSizeSpin);
    QSignalBlocker boldBlock(m_widgets.transcriptBoldCheckBox);
    QSignalBlocker italicBlock(m_widgets.transcriptItalicCheckBox);

    m_widgets.transcriptOverlayEnabledCheckBox->setChecked(clip.transcriptOverlay.enabled);
    if (m_widgets.transcriptBackgroundVisibleCheckBox) {
        m_widgets.transcriptBackgroundVisibleCheckBox->setChecked(clip.transcriptOverlay.showBackground);
    }
    if (m_widgets.transcriptMaxLinesSpin) {
        m_widgets.transcriptMaxLinesSpin->setValue(clip.transcriptOverlay.maxLines);
    }
    if (m_widgets.transcriptMaxCharsSpin) {
        m_widgets.transcriptMaxCharsSpin->setValue(clip.transcriptOverlay.maxCharsPerLine);
    }
    if (m_widgets.transcriptAutoScrollCheckBox) {
        m_widgets.transcriptAutoScrollCheckBox->setChecked(clip.transcriptOverlay.autoScroll);
    }
    if (m_widgets.transcriptOverlayXSpin) {
        m_widgets.transcriptOverlayXSpin->setValue(clip.transcriptOverlay.translationX);
    }
    if (m_widgets.transcriptOverlayYSpin) {
        m_widgets.transcriptOverlayYSpin->setValue(clip.transcriptOverlay.translationY);
    }
    if (m_widgets.transcriptOverlayWidthSpin) {
        m_widgets.transcriptOverlayWidthSpin->setValue(static_cast<int>(clip.transcriptOverlay.boxWidth));
    }
    if (m_widgets.transcriptOverlayHeightSpin) {
        m_widgets.transcriptOverlayHeightSpin->setValue(static_cast<int>(clip.transcriptOverlay.boxHeight));
    }
    if (m_widgets.transcriptFontFamilyCombo) {
        m_widgets.transcriptFontFamilyCombo->setCurrentFont(QFont(clip.transcriptOverlay.fontFamily));
    }
    if (m_widgets.transcriptFontSizeSpin) {
        m_widgets.transcriptFontSizeSpin->setValue(clip.transcriptOverlay.fontPointSize);
    }
    if (m_widgets.transcriptBoldCheckBox) {
        m_widgets.transcriptBoldCheckBox->setChecked(clip.transcriptOverlay.bold);
    }
    if (m_widgets.transcriptItalicCheckBox) {
        m_widgets.transcriptItalicCheckBox->setChecked(clip.transcriptOverlay.italic);
    }
}

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

    QFile transcriptFile(transcriptPath);
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        if (transcriptPath != baseEditablePath) {
            transcriptPath = baseEditablePath;
            refreshScriptVersionSelector(clip.filePath, transcriptPath);
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

    if (m_widgets.transcriptInspectorDetailsLabel) {
        m_widgets.transcriptInspectorDetailsLabel->setText(
            QStringLiteral("%1 words across %2 segments, %3 edited words, %4 deletions. "
                           "Source time reflects transcript timing; Render time follows row order.")
                .arg(totalWords)
                .arg(segments.size())
                .arg(editedWords)
                .arg(deletedEdits));
    }

    QVector<TranscriptRow> allRows = parseTranscriptRows(segments, m_transcriptPrependMs, m_transcriptPostpendMs);
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
    refreshSpeakersTable(allRows);
    if (m_widgets.speakersTable) {
        m_widgets.speakersTable->setEnabled(activeCutMutable());
    }
    if (m_widgets.speakersInspectorClipLabel) {
        m_widgets.speakersInspectorClipLabel->setText(QStringLiteral("Speakers\n%1").arg(clip.label));
    }
    if (m_widgets.speakersInspectorDetailsLabel) {
        m_widgets.speakersInspectorDetailsLabel->setText(
            activeCutMutable()
                ? QStringLiteral("Edit speaker names and on-screen locations for this cut.")
                : QStringLiteral("Original cut is immutable. Use Copy Cut to edit speaker metadata."));
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
        rows.push_back(ordered.row);
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
        const bool hasRenderTime = entry.renderStartFrame >= 0 && entry.renderEndFrame >= entry.renderStartFrame;
        const double renderStartTime = hasRenderTime ? static_cast<double>(entry.renderStartFrame) / kTimelineFps : -1.0;
        const double renderEndTime = hasRenderTime ? static_cast<double>(entry.renderEndFrame + 1) / kTimelineFps : -1.0;

        auto* sourceStartItem = new QTableWidgetItem(m_transcriptEngine.secondsToTranscriptTime(sourceStartTime));
        auto* sourceEndItem = new QTableWidgetItem(m_transcriptEngine.secondsToTranscriptTime(sourceEndTime));
        auto* renderStartItem = new QTableWidgetItem(
            hasRenderTime ? m_transcriptEngine.secondsToTranscriptTime(renderStartTime) : QStringLiteral("--"));
        auto* renderEndItem = new QTableWidgetItem(
            hasRenderTime ? m_transcriptEngine.secondsToTranscriptTime(renderEndTime) : QStringLiteral("--"));
        auto* speakerItem = new QTableWidgetItem(entry.speaker);
        auto* textItem = new QTableWidgetItem(entry.text);
        auto* editsItem = new QTableWidgetItem(editLabelsForFlags(entry.editFlags).join(QStringLiteral(", ")));
        speakerItem->setFlags(speakerItem->flags() & ~Qt::ItemIsEditable);
        editsItem->setFlags(editsItem->flags() & ~Qt::ItemIsEditable);
        renderStartItem->setFlags(renderStartItem->flags() & ~Qt::ItemIsEditable);
        renderEndItem->setFlags(renderEndItem->flags() & ~Qt::ItemIsEditable);

        QTableWidgetItem* rowItems[] = {
            sourceStartItem, sourceEndItem, renderStartItem, renderEndItem, speakerItem, textItem, editsItem};
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
        m_widgets.transcriptTable->setItem(row, kTranscriptColRenderStart, renderStartItem);
        m_widgets.transcriptTable->setItem(row, kTranscriptColRenderEnd, renderEndItem);
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
        }
        if (entry.editFlags & TranscriptRow::EditText) {
            textItem->setBackground(textColor);
        }
        if (entry.editFlags & TranscriptRow::EditInserted) {
            textItem->setBackground(insertedColor);
        }
        if (entry.editFlags != TranscriptRow::EditNone) {
            editsItem->setBackground(editsColor);
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

QVector<TranscriptTab::TranscriptRow> TranscriptTab::filteredRowsForSpeaker(const QVector<TranscriptRow>& rows) const
{
    const QString filterValue = activeSpeakerFilter();
    if (filterValue.isEmpty() || filterValue == QString(kAllSpeakersFilterValue)) {
        return rows;
    }

    QVector<TranscriptRow> filtered;
    filtered.reserve(rows.size());
    for (const TranscriptRow& row : rows) {
        if (row.speaker == filterValue) {
            filtered.push_back(row);
        }
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

void TranscriptTab::refreshSpeakersTable(const QVector<TranscriptRow>& rows)
{
    if (!m_widgets.speakersTable) {
        return;
    }
    m_updating = true;
    QSignalBlocker blocker(m_widgets.speakersTable);
    m_widgets.speakersTable->clearContents();

    QSet<QString> speakerIds;
    for (const TranscriptRow& row : rows) {
        if (!row.speaker.trimmed().isEmpty()) {
            speakerIds.insert(row.speaker.trimmed());
        }
    }
    QStringList ids = speakerIds.values();
    std::sort(ids.begin(), ids.end(), [](const QString& a, const QString& b) {
        return a.localeAwareCompare(b) < 0;
    });

    const QJsonObject root = m_loadedTranscriptDoc.object();
    const QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();

    m_widgets.speakersTable->setRowCount(ids.size());
    for (int row = 0; row < ids.size(); ++row) {
        const QString id = ids.at(row);
        const QJsonObject profile = profiles.value(id).toObject();
        const QString name = profile.value(QString(kTranscriptSpeakerNameKey)).toString(id);
        const QJsonObject location = profile.value(QString(kTranscriptSpeakerLocationKey)).toObject();
        const double x = location.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5);
        const double y = location.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85);

        auto* idItem = new QTableWidgetItem(id);
        idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
        idItem->setData(Qt::UserRole, id);
        auto* nameItem = new QTableWidgetItem(name);
        nameItem->setData(Qt::UserRole, id);
        auto* xItem = new QTableWidgetItem(QString::number(x, 'f', 3));
        xItem->setData(Qt::UserRole, id);
        auto* yItem = new QTableWidgetItem(QString::number(y, 'f', 3));
        yItem->setData(Qt::UserRole, id);

        m_widgets.speakersTable->setItem(row, 0, idItem);
        m_widgets.speakersTable->setItem(row, 1, nameItem);
        m_widgets.speakersTable->setItem(row, 2, xItem);
        m_widgets.speakersTable->setItem(row, 3, yItem);
    }
    m_updating = false;
}

bool TranscriptTab::saveSpeakerProfileEdit(int tableRow, int column, const QString& valueText)
{
    if (!m_widgets.speakersTable || !m_loadedTranscriptDoc.isObject()) {
        return false;
    }
    QTableWidgetItem* idItem = m_widgets.speakersTable->item(tableRow, 0);
    if (!idItem) {
        return false;
    }
    const QString speakerId = idItem->data(Qt::UserRole).toString().trimmed();
    if (speakerId.isEmpty()) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    QJsonObject location = profile.value(QString(kTranscriptSpeakerLocationKey)).toObject();

    if (column == 1) {
        profile[QString(kTranscriptSpeakerNameKey)] = valueText.trimmed().isEmpty() ? speakerId : valueText.trimmed();
    } else if (column == 2 || column == 3) {
        bool ok = false;
        const double parsed = valueText.toDouble(&ok);
        if (!ok) {
            return false;
        }
        const double bounded = qBound(0.0, parsed, 1.0);
        if (column == 2) {
            location[QString(kTranscriptSpeakerLocationXKey)] = bounded;
        } else {
            location[QString(kTranscriptSpeakerLocationYKey)] = bounded;
        }
        profile[QString(kTranscriptSpeakerLocationKey)] = location;
    } else {
        return false;
    }

    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);
    if (!m_transcriptEngine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
        return false;
    }
    return true;
}

void TranscriptTab::onSpeakersTableItemChanged(QTableWidgetItem* item)
{
    if (m_updating || !item || !activeCutMutable()) {
        return;
    }
    if (!saveSpeakerProfileEdit(item->row(), item->column(), item->text())) {
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


void TranscriptTab::onTranscriptCustomContextMenu(const QPoint& pos)
{
    if (!m_widgets.transcriptTable || m_loadedTranscriptPath.isEmpty() ||
        !m_loadedTranscriptDoc.isObject()) {
        return;
    }

    QTableWidgetItem* item = m_widgets.transcriptTable->itemAt(pos);
    if (!item) return;

    const int row = item->row();
    const bool isGap = m_widgets.transcriptTable->item(row, 0)->data(Qt::UserRole + 4).toBool();
    const bool isOutsideCut = m_widgets.transcriptTable->item(row, 0)->data(Qt::UserRole + 12).toBool();
    if (isGap || isOutsideCut) return;

    QMenu menu;
    QAction* addAbove = nullptr;
    QAction* addBelow = nullptr;
    QAction* expandAction = nullptr;
    QAction* skipAction = nullptr;
    QAction* deleteAction = nullptr;
    const bool rowSkipped = item->data(Qt::UserRole + 7).toBool();
    if (activeCutMutable()) {
        addAbove = menu.addAction(QStringLiteral("Add Word Above"));
        addBelow = menu.addAction(QStringLiteral("Add Word Below"));
        menu.addSeparator();
        expandAction = menu.addAction(QStringLiteral("Expand"));
        menu.addSeparator();
        skipAction = menu.addAction(rowSkipped ? QStringLiteral("Unskip Word")
                                               : QStringLiteral("Skip Word"));
        deleteAction = menu.addAction(QStringLiteral("Delete Word"));
    } else {
        QAction* immutableNotice = menu.addAction(QStringLiteral("Original Cut (Immutable)"));
        immutableNotice->setEnabled(false);
    }

    QAction* chosen = menu.exec(m_widgets.transcriptTable->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == addAbove) {
        insertWordAtRow(row, true);
    } else if (chosen == addBelow) {
        insertWordAtRow(row, false);
    } else if (chosen == expandAction) {
        expandSelectedRow(row);
    } else if (chosen == skipAction) {
        setSelectedRowsSkipped(!rowSkipped);
    } else if (chosen == deleteAction) {
        deleteSelectedRows();
    }
}

void TranscriptTab::onTranscriptHeaderContextMenu(const QPoint& pos)
{
    if (!m_widgets.transcriptTable || !m_widgets.transcriptTable->horizontalHeader()) {
        return;
    }

    QHeaderView* header = m_widgets.transcriptTable->horizontalHeader();
    QMenu menu;
    for (int column = 0; column < m_widgets.transcriptTable->columnCount(); ++column) {
        const QString label = m_widgets.transcriptTable->horizontalHeaderItem(column)
            ? m_widgets.transcriptTable->horizontalHeaderItem(column)->text()
            : QStringLiteral("Column %1").arg(column + 1);
        QAction* action = menu.addAction(label);
        action->setCheckable(true);

        const bool isTextColumn = (column == kTranscriptColText);
        const bool visible = !m_widgets.transcriptTable->isColumnHidden(column);
        action->setChecked(visible);
        if (isTextColumn) {
            action->setEnabled(false);
            action->setToolTip(QStringLiteral("Text column is always visible."));
            continue;
        }

        connect(action, &QAction::toggled, this, [this, column](bool checked) {
            if (!m_widgets.transcriptTable) {
                return;
            }
            m_widgets.transcriptTable->setColumnHidden(column, !checked);
        });
    }

    menu.exec(header->viewport()->mapToGlobal(pos));
}

void TranscriptTab::insertWordAtRow(int row, bool above)
{
    if (!m_widgets.transcriptTable || m_loadedTranscriptPath.isEmpty() ||
        !m_loadedTranscriptDoc.isObject()) {
        return;
    }
    if (!activeCutMutable()) {
        return;
    }

    // Get the times of the word we're inserting relative to
    QTableWidgetItem* currentItem = m_widgets.transcriptTable->item(row, 0);
    if (!currentItem || currentItem->data(Qt::UserRole + 12).toBool()) return;

    const double currentStart = currentItem->data(Qt::UserRole).toDouble();
    const double currentEnd = currentItem->data(Qt::UserRole + 1).toDouble();
    const int currentSegmentIndex = currentItem->data(Qt::UserRole + 5).toInt();
    const int currentWordIndex = currentItem->data(Qt::UserRole + 6).toInt();

    double newWordStart, newWordEnd;
    int targetSegmentIndex, targetWordIndex;
    const int insertRenderOrder = qMax(0, above ? row : (row + 1));

    if (above) {
        // Get previous row's end time
        double prevEnd = 0.0;
        if (row > 0) {
            QTableWidgetItem* prevItem = m_widgets.transcriptTable->item(row - 1, 0);
            if (prevItem && !prevItem->data(Qt::UserRole + 4).toBool()) {
                prevEnd = prevItem->data(Qt::UserRole + 1).toDouble();
            } else if (prevItem) {
                // Previous is a gap, check the one before
                if (row > 1) {
                    QTableWidgetItem* prevPrevItem = m_widgets.transcriptTable->item(row - 2, 0);
                    if (prevPrevItem) {
                        prevEnd = prevPrevItem->data(Qt::UserRole + 1).toDouble();
                    }
                }
            }
        }
        
        // Place new word between prevEnd and currentStart
        newWordStart = (prevEnd + currentStart) / 2.0;
        newWordEnd = qMin(newWordStart + 0.1, currentStart - 0.01);
        if (newWordEnd <= newWordStart) {
            newWordStart = currentStart - 0.2;
            newWordEnd = currentStart - 0.05;
        }
        targetSegmentIndex = currentSegmentIndex;
        targetWordIndex = currentWordIndex;
    } else {
        // Get next row's start time
        double nextStart = currentEnd + 1.0;
        if (row < m_widgets.transcriptTable->rowCount() - 1) {
            QTableWidgetItem* nextItem = m_widgets.transcriptTable->item(row + 1, 0);
            if (nextItem && !nextItem->data(Qt::UserRole + 4).toBool()) {
                nextStart = nextItem->data(Qt::UserRole).toDouble();
            } else if (nextItem) {
                // Next is a gap, check the one after
                if (row < m_widgets.transcriptTable->rowCount() - 2) {
                    QTableWidgetItem* nextNextItem = m_widgets.transcriptTable->item(row + 2, 0);
                    if (nextNextItem) {
                        nextStart = nextNextItem->data(Qt::UserRole).toDouble();
                    }
                }
            }
        }
        
        // Place new word between currentEnd and nextStart
        newWordStart = (currentEnd + nextStart) / 2.0;
        newWordEnd = qMin(newWordStart + 0.1, nextStart - 0.01);
        if (newWordEnd <= newWordStart || newWordStart < currentEnd) {
            newWordStart = currentEnd + 0.05;
            newWordEnd = currentEnd + 0.2;
        }
        targetSegmentIndex = currentSegmentIndex;
        targetWordIndex = currentWordIndex + 1;
    }

    // Ensure valid timing
    newWordStart = qMax(0.0, newWordStart);
    newWordEnd = qMax(newWordStart + 0.01, newWordEnd);

    // Create the new word object
    QJsonObject newWordObj;
    newWordObj[QStringLiteral("word")] = QStringLiteral("[new]");
    newWordObj[QStringLiteral("start")] = newWordStart;
    newWordObj[QStringLiteral("end")] = newWordEnd;
    newWordObj[QString(kTranscriptWordRenderOrderKey)] = insertRenderOrder;
    newWordObj[QString(kTranscriptWordOriginalSegmentKey)] = -1;
    newWordObj[QString(kTranscriptWordOriginalWordKey)] = -1;
    transcriptAppendEditTag(&newWordObj, QString(kTranscriptEditInsertedTag));

    // Update the JSON document
    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonArray segments = root.value(QStringLiteral("segments")).toArray();

    for (int segIdx = 0; segIdx < segments.size(); ++segIdx) {
        QJsonObject segmentObj = segments.at(segIdx).toObject();
        QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        bool segmentChanged = false;
        for (int wordIdx = 0; wordIdx < words.size(); ++wordIdx) {
            QJsonObject wordObj = words.at(wordIdx).toObject();
            const int existingOrder = wordObj.value(QString(kTranscriptWordRenderOrderKey)).toInt(-1);
            if (existingOrder >= insertRenderOrder) {
                wordObj[QString(kTranscriptWordRenderOrderKey)] = existingOrder + 1;
                words.replace(wordIdx, wordObj);
                segmentChanged = true;
            }
        }
        if (segmentChanged) {
            segmentObj[QStringLiteral("words")] = words;
            segments.replace(segIdx, segmentObj);
        }
    }

    if (targetSegmentIndex >= 0 && targetSegmentIndex < segments.size()) {
        QJsonObject segmentObj = segments.at(targetSegmentIndex).toObject();
        QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();

        // Insert at appropriate position
        int insertIndex = qBound(0, targetWordIndex, words.size());
        words.insert(insertIndex, newWordObj);

        segmentObj[QStringLiteral("words")] = words;
        segments.replace(targetSegmentIndex, segmentObj);
        root[QStringLiteral("segments")] = segments;
        m_loadedTranscriptDoc.setObject(root);

        if (!m_transcriptEngine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
            refresh();
            return;
        }

        refresh();
        emit transcriptDocumentChanged();
        if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    }
}

void TranscriptTab::expandSelectedRow(int row)
{
    if (!m_widgets.transcriptTable || m_loadedTranscriptPath.isEmpty() ||
        !m_loadedTranscriptDoc.isObject()) {
        return;
    }
    if (!activeCutMutable()) {
        return;
    }

    QTableWidgetItem* currentItem = m_widgets.transcriptTable->item(row, 0);
    if (!currentItem) return;

    const bool isGap = currentItem->data(Qt::UserRole + 4).toBool();
    const bool isOutsideCut = currentItem->data(Qt::UserRole + 12).toBool();
    if (isGap || isOutsideCut) return;

    const int segmentIndex = currentItem->data(Qt::UserRole + 5).toInt();
    const int wordIndex = currentItem->data(Qt::UserRole + 6).toInt();
    if (segmentIndex < 0 || wordIndex < 0) return;

    // Find the previous word's end time
    double newStartTime = 0.0;
    bool hasPreviousWord = false;
    for (int r = row - 1; r >= 0; --r) {
        QTableWidgetItem* prevItem = m_widgets.transcriptTable->item(r, 0);
        if (!prevItem) continue;
        if (prevItem->data(Qt::UserRole + 4).toBool()) continue; // Skip gaps
        newStartTime = prevItem->data(Qt::UserRole + 1).toDouble(); // Previous word's end time
        hasPreviousWord = true;
        break;
    }

    // Find the next word's start time
    double newEndTime = 0.0;
    bool hasNextWord = false;
    for (int r = row + 1; r < m_widgets.transcriptTable->rowCount(); ++r) {
        QTableWidgetItem* nextItem = m_widgets.transcriptTable->item(r, 0);
        if (!nextItem) continue;
        if (nextItem->data(Qt::UserRole + 4).toBool()) continue; // Skip gaps
        newEndTime = nextItem->data(Qt::UserRole).toDouble(); // Next word's start time
        hasNextWord = true;
        break;
    }

    // Update the JSON document
    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    if (segmentIndex >= segments.size()) return;

    QJsonObject segmentObj = segments.at(segmentIndex).toObject();
    QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
    if (wordIndex >= words.size()) return;

    QJsonObject wordObj = words.at(wordIndex).toObject();

    if (hasPreviousWord) {
        if (!qFuzzyCompare(wordObj.value(QStringLiteral("start")).toDouble(newStartTime) + 1.0,
                           newStartTime + 1.0)) {
            transcriptAppendEditTag(&wordObj, QString(kTranscriptEditTimingTag));
        }
        wordObj[QStringLiteral("start")] = newStartTime;
    }
    if (hasNextWord) {
        if (!qFuzzyCompare(wordObj.value(QStringLiteral("end")).toDouble(newEndTime) + 1.0,
                           newEndTime + 1.0)) {
            transcriptAppendEditTag(&wordObj, QString(kTranscriptEditTimingTag));
        }
        wordObj[QStringLiteral("end")] = newEndTime;
    }

    words.replace(wordIndex, wordObj);
    segmentObj[QStringLiteral("words")] = words;
    segments.replace(segmentIndex, segmentObj);
    root[QStringLiteral("segments")] = segments;
    m_loadedTranscriptDoc.setObject(root);

    if (!m_transcriptEngine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
        refresh();
        return;
    }

    refresh();
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

bool TranscriptTab::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == m_widgets.transcriptTable ||
         (m_widgets.transcriptTable && watched == m_widgets.transcriptTable->viewport())) &&
        event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Delete || keyEvent->key() == Qt::Key_Backspace) {
            deleteSelectedRows();
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}
