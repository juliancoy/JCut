#include "transcript_tab.h"

#include "clip_serialization.h"
#include "editor_shared.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QColor>
#include <QDebug>
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
#include <QMessageBox>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSet>
#include <cmath>
#include <limits>

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

QString transcriptCutStateLabel(bool mutableCut)
{
    return mutableCut ? QStringLiteral("Editable Cut") : QStringLiteral("Original (Immutable)");
}

QString transcriptFollowStateLabel(bool enabled)
{
    return enabled ? QStringLiteral("Follow Playback: On") : QStringLiteral("Follow Playback: Off");
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
    if (m_widgets.transcriptCenterHorizontalButton) {
        connect(m_widgets.transcriptCenterHorizontalButton, &QPushButton::clicked,
                this, &TranscriptTab::onCenterHorizontalClicked);
    }
    if (m_widgets.transcriptCenterVerticalButton) {
        connect(m_widgets.transcriptCenterVerticalButton, &QPushButton::clicked,
                this, &TranscriptTab::onCenterVerticalClicked);
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
    if (m_widgets.transcriptSearchFilterLineEdit) {
        connect(m_widgets.transcriptSearchFilterLineEdit, &QLineEdit::textChanged,
                this, [this](const QString&) { refresh(); });
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
        m_widgets.transcriptScriptVersionCombo->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.transcriptScriptVersionCombo, &QWidget::customContextMenuRequested,
                this, &TranscriptTab::onTranscriptScriptVersionContextMenu);
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
}

void TranscriptTab::refresh()
{
    // (Removed early return to ensure the UI updates to reflect truth when a row is modified)


    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    m_updating = true;
    m_manualSelectionTimer.invalidate();

    const QString previousClipFilePath = m_loadedClipFilePath;
    if (m_widgets.transcriptTable) {
        m_widgets.transcriptTable->clearContents();
        m_widgets.transcriptTable->setRowCount(0);
    }
    m_loadedTranscriptPath.clear();
    m_loadedClipFilePath.clear();
    m_loadedTranscriptDoc = QJsonDocument();
    m_followRanges.clear();
    if (m_widgets.transcriptPrependMsSpin) {
        m_transcriptPrependMs = qMax(0, m_widgets.transcriptPrependMsSpin->value());
    }
    if (m_widgets.transcriptPostpendMsSpin) {
        m_transcriptPostpendMs = qMax(0, m_widgets.transcriptPostpendMsSpin->value());
    }
    if (m_widgets.speechFilterFadeSamplesSpin) {
        m_speechFilterFadeSamples = qMax(0, m_widgets.speechFilterFadeSamplesSpin->value());
    }
    if (m_widgets.speechFilterEnabledCheckBox) {
        m_speechFilterEnabled = m_widgets.speechFilterEnabledCheckBox->isChecked();
    }

    if (!clip || !clipSupportsTranscript(*clip)) {
        clearActiveTranscriptPathForClipFile(previousClipFilePath);
        m_persistedSelectedClipId.clear();
        m_persistedSelectedSegmentIndex = -1;
        m_persistedSelectedWordIndex = -1;
        if (m_widgets.transcriptInspectorClipLabel) {
            m_widgets.transcriptInspectorClipLabel->setText(QString());
            m_widgets.transcriptInspectorClipLabel->setPlaceholderText(QStringLiteral("No transcript selected"));
        }
        if (m_widgets.transcriptInspectorDetailsLabel) {
            m_widgets.transcriptInspectorDetailsLabel->setText(
                QStringLiteral("Select an audio clip with a WhisperX JSON transcript.\n"
                               "Workflow: 1) Select/Create Cut  2) Edit Transcript/Speakers  3) Preview  4) Render"));
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
        const qreal previousX = clip.transcriptOverlay.translationX;
        const qreal previousY = clip.transcriptOverlay.translationY;
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
        if (m_widgets.transcriptAutoScrollCheckBox) {
            clip.transcriptOverlay.autoScroll =
                m_widgets.transcriptAutoScrollCheckBox->isChecked();
        }
        clip.transcriptOverlay.translationX = m_widgets.transcriptOverlayXSpin
            ? m_widgets.transcriptOverlayXSpin->value()
            : 0.0;
        clip.transcriptOverlay.translationY = m_widgets.transcriptOverlayYSpin
            ? m_widgets.transcriptOverlayYSpin->value()
            : 0.0;
        const bool translationChanged =
            (std::abs(clip.transcriptOverlay.translationX - previousX) > 0.0001) ||
            (std::abs(clip.transcriptOverlay.translationY - previousY) > 0.0001);
        if (translationChanged) {
            // Any direct position edit (including Center H/V) switches to explicit manual placement.
            clip.transcriptOverlay.useManualPlacement = true;
        }
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

void TranscriptTab::syncTableToPlayhead(int64_t absolutePlaybackSample,
                                        double sourceSeconds,
                                        int64_t sourceFrame)
{
    if (!m_widgets.transcriptTable || m_updating) return;
    if (sourceFrame < 0) {
        static bool warnedMissingSourceFrame = false;
        if (!warnedMissingSourceFrame) {
            warnedMissingSourceFrame = true;
            qWarning().noquote()
                << QStringLiteral("[TRANSCRIPT TIMING WARN] syncTableToPlayhead called without sourceFrame; falling back to sourceSeconds-derived frame.");
        }
    }
    const int64_t previousSourceFrame = m_lastSyncSourceFrame;
    const int64_t currentSourceFrame = sourceFrame >= 0
        ? sourceFrame
        : qMax<int64_t>(0, static_cast<int64_t>(std::floor(sourceSeconds * kTimelineFps)));
    m_lastSyncSourceFrame = currentSourceFrame;
    if (!m_widgets.transcriptFollowCurrentWordCheckBox ||
        !m_widgets.transcriptFollowCurrentWordCheckBox->isChecked()) {
        m_lastSyncAbsolutePlaybackSample = absolutePlaybackSample;
        return;
    }

    const bool playbackAdvanced =
        (m_lastSyncAbsolutePlaybackSample >= 0 &&
         absolutePlaybackSample != m_lastSyncAbsolutePlaybackSample);
    m_lastSyncAbsolutePlaybackSample = absolutePlaybackSample;

    // Hold manual selection while paused, but resume follow immediately once
    // playback advances so click-to-seek + play tracks words in time.
    if (hasActiveManualSelection() && !playbackAdvanced) {
        return;
    }
    if (playbackAdvanced) {
        m_manualSelectionTimer.invalidate();
    }

    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || !clipSupportsTranscript(*clip) || m_loadedTranscriptPath.isEmpty()) {
        m_widgets.transcriptTable->clearSelection();
        return;
    }

    if (m_followRanges.isEmpty()) {
        m_widgets.transcriptTable->clearSelection();
        return;
    }

    int matchingRow = -1;
    int previousEligibleRow = -1;
    int nextEligibleRow = -1;
    int nearestEligibleRow = -1;
    int64_t nearestDistance = std::numeric_limits<int64_t>::max();

    const auto upperIt = std::upper_bound(
        m_followRanges.cbegin(), m_followRanges.cend(), currentSourceFrame,
        [](int64_t value, const FollowRange& range) { return value < range.startFrame; });
    const int upperIndex = static_cast<int>(std::distance(m_followRanges.cbegin(), upperIt));

    int previousIndex = upperIndex - 1;
    if (previousIndex >= 0) {
        previousEligibleRow = m_followRanges.at(previousIndex).row;
    }
    if (upperIndex < m_followRanges.size()) {
        nextEligibleRow = m_followRanges.at(upperIndex).row;
    }

    for (int i = previousIndex; i >= 0; --i) {
        const FollowRange& range = m_followRanges.at(i);
        if (range.endFrame < currentSourceFrame) {
            break;
        }
        if (currentSourceFrame >= range.startFrame && currentSourceFrame <= range.endFrame) {
            matchingRow = range.row;
            break;
        }
    }
    if (matchingRow < 0) {
        for (int i = upperIndex; i < m_followRanges.size(); ++i) {
            const FollowRange& range = m_followRanges.at(i);
            if (range.startFrame > currentSourceFrame) {
                break;
            }
            if (currentSourceFrame >= range.startFrame && currentSourceFrame <= range.endFrame) {
                matchingRow = range.row;
                break;
            }
        }
    }

    if (previousIndex >= 0) {
        const FollowRange& range = m_followRanges.at(previousIndex);
        const int64_t distance = qMax<int64_t>(0, currentSourceFrame - range.endFrame);
        nearestDistance = distance;
        nearestEligibleRow = range.row;
    }
    if (upperIndex < m_followRanges.size()) {
        const FollowRange& range = m_followRanges.at(upperIndex);
        const int64_t distance = qMax<int64_t>(0, range.startFrame - currentSourceFrame);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearestEligibleRow = range.row;
        }
    }

    if (matchingRow < 0) {
        const int64_t sourceDeltaFrames =
            previousSourceFrame >= 0 ? qAbs(currentSourceFrame - previousSourceFrame) : 0;
        const int64_t maxGapFrames = qMax<int64_t>(1, static_cast<int64_t>(std::ceil(sourceDeltaFrames * 1.5)) + 1);
        const bool forwardMotion = previousSourceFrame >= 0 &&
                                   currentSourceFrame > previousSourceFrame;
        const bool backwardMotion = previousSourceFrame >= 0 &&
                                    currentSourceFrame < previousSourceFrame;
        if (forwardMotion && nextEligibleRow >= 0) {
            QTableWidgetItem* nextItem = m_widgets.transcriptTable->item(nextEligibleRow, 0);
            if (nextItem) {
                const int64_t nextStartFrame = nextItem->data(Qt::UserRole + 2).toLongLong();
                if ((nextStartFrame - currentSourceFrame) <= maxGapFrames) {
                    matchingRow = nextEligibleRow;
                }
            }
        } else if (backwardMotion && previousEligibleRow >= 0) {
            QTableWidgetItem* prevItem = m_widgets.transcriptTable->item(previousEligibleRow, 0);
            if (prevItem) {
                const int64_t prevEndFrame = prevItem->data(Qt::UserRole + 3).toLongLong();
                if ((currentSourceFrame - prevEndFrame) <= maxGapFrames) {
                    matchingRow = previousEligibleRow;
                }
            }
        } else if (nearestEligibleRow >= 0 && nearestDistance <= maxGapFrames) {
            matchingRow = nearestEligibleRow;
        }
    }

    if (matchingRow < 0) {
        m_widgets.transcriptTable->clearSelection();
        return;
    }

    if (!m_widgets.transcriptTable->selectionModel()->isRowSelected(matchingRow, QModelIndex())) {
        m_suppressSelectionSideEffects = true;
        m_widgets.transcriptTable->setCurrentCell(matchingRow, 0);
        m_widgets.transcriptTable->selectRow(matchingRow);
        m_suppressSelectionSideEffects = false;
    }

    if (m_widgets.transcriptTable->item(matchingRow, 0)) {
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

    if (m_widgets.transcriptSearchFilterLineEdit) {
        const QString searchText = m_widgets.transcriptSearchFilterLineEdit->text().trimmed();
        if (!searchText.isEmpty() && m_widgets.transcriptTable) {
            const QTableWidgetItem* textItem = m_widgets.transcriptTable->item(item->row(), kTranscriptColText);
            const QString rowText = textItem ? textItem->text() : item->text();
            if (rowText.contains(searchText, Qt::CaseInsensitive)) {
                QSignalBlocker blocker(m_widgets.transcriptSearchFilterLineEdit);
                m_widgets.transcriptSearchFilterLineEdit->clear();
                refresh();
                return;
            }
        }
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
    return m_manualSelectionTimer.elapsed() < qMax(0, m_manualSelectionHoldMs);
}

void TranscriptTab::setManualSelectionHoldMs(int valueMs)
{
    m_manualSelectionHoldMs = qMax(0, valueMs);
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

void TranscriptTab::onCenterHorizontalClicked()
{
    if (m_updating || !m_widgets.transcriptOverlayXSpin) {
        return;
    }
    if (std::abs(m_widgets.transcriptOverlayXSpin->value()) < 0.0001) {
        return;
    }
    {
        QSignalBlocker block(m_widgets.transcriptOverlayXSpin);
        m_widgets.transcriptOverlayXSpin->setValue(0.0);
    }
    applyOverlayFromInspector(true);
}

void TranscriptTab::onCenterVerticalClicked()
{
    if (m_updating || !m_widgets.transcriptOverlayYSpin) {
        return;
    }
    if (std::abs(m_widgets.transcriptOverlayYSpin->value()) < 0.0001) {
        return;
    }
    {
        QSignalBlocker block(m_widgets.transcriptOverlayYSpin);
        m_widgets.transcriptOverlayYSpin->setValue(0.0);
    }
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
        expandAction = menu.addAction(QStringLiteral("Expand Word Timing"));
        skipAction = menu.addAction(rowSkipped ? QStringLiteral("Unskip Word")
                                               : QStringLiteral("Skip Word"));
        menu.addSeparator();
        deleteAction = menu.addAction(QStringLiteral("Delete Word"));
    } else {
        QAction* immutableNotice = menu.addAction(QStringLiteral("Original Cut (Immutable)"));
        immutableNotice->setEnabled(false);
        QAction* copyNotice = menu.addAction(QStringLiteral("Use + New Cut to edit words"));
        copyNotice->setEnabled(false);
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
    const QVariant renderOrderVariant = currentItem->data(Qt::UserRole + 15);
    int insertRenderOrder = renderOrderVariant.isValid() ? renderOrderVariant.toInt() : -1;
    if (insertRenderOrder < 0) {
        insertRenderOrder = row;
    }
    if (!above) {
        ++insertRenderOrder;
    }
    insertRenderOrder = qMax(0, insertRenderOrder);
    const QString currentSpeaker = currentItem->data(Qt::UserRole + 9).toString().trimmed();

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
    if (!currentSpeaker.isEmpty()) {
        newWordObj[QString(kTranscriptWordSpeakerKey)] = currentSpeaker;
    }
    newWordObj[QStringLiteral("start")] = newWordStart;
    newWordObj[QStringLiteral("end")] = newWordEnd;
    newWordObj[QString(kTranscriptWordRenderOrderKey)] = insertRenderOrder;
    newWordObj[QString(kTranscriptWordOriginalSegmentKey)] = -1;
    newWordObj[QString(kTranscriptWordOriginalWordKey)] = -1;
    transcriptAppendEditTag(&newWordObj, QString(kTranscriptEditInsertedTag));

    // Update the JSON document
    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonArray segments = root.value(QStringLiteral("segments")).toArray();

    // Normalize render_order across all words before insertion so below/above placement
    // remains stable even when older words did not have explicit render_order values.
    {
        const QVector<TranscriptRow> orderedRows = parseTranscriptRows(segments, m_transcriptPrependMs, m_transcriptPostpendMs);
        QHash<quint64, int> desiredOrderByKey;
        desiredOrderByKey.reserve(orderedRows.size());
        int nextOrder = 0;
        for (const TranscriptRow& orderedRow : orderedRows) {
            if (orderedRow.segmentIndex < 0 || orderedRow.wordIndex < 0) {
                continue;
            }
            const quint64 key = (static_cast<quint64>(orderedRow.segmentIndex) << 32) |
                                static_cast<quint32>(orderedRow.wordIndex);
            if (desiredOrderByKey.contains(key)) {
                continue;
            }
            desiredOrderByKey.insert(key, nextOrder++);
        }

        int fallbackOrder = nextOrder;
        for (int segIdx = 0; segIdx < segments.size(); ++segIdx) {
            QJsonObject segmentObj = segments.at(segIdx).toObject();
            QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
            bool segmentChanged = false;
            for (int wordIdx = 0; wordIdx < words.size(); ++wordIdx) {
                QJsonObject wordObj = words.at(wordIdx).toObject();
                const quint64 key = (static_cast<quint64>(segIdx) << 32) |
                                    static_cast<quint32>(wordIdx);
                const int desiredOrder = desiredOrderByKey.contains(key)
                    ? desiredOrderByKey.value(key)
                    : fallbackOrder++;
                if (wordObj.value(QString(kTranscriptWordRenderOrderKey)).toInt(-1) != desiredOrder) {
                    wordObj[QString(kTranscriptWordRenderOrderKey)] = desiredOrder;
                    words.replace(wordIdx, wordObj);
                    segmentChanged = true;
                }
            }
            if (segmentChanged) {
                segmentObj[QStringLiteral("words")] = words;
                segments.replace(segIdx, segmentObj);
            }
        }
    }

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
        if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
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
