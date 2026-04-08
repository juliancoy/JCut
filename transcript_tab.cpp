#include "transcript_tab.h"

#include "clip_serialization.h"
#include "editor_shared.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QColor>
#include <QFile>
#include <QFont>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeyEvent>
#include <QMenu>
#include <QSignalBlocker>
#include <cmath>

namespace {
constexpr qint64 kManualTranscriptSelectionHoldMs = 1200;
constexpr QLatin1StringView kTranscriptWordSkippedKey("skipped");
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
    if (m_widgets.transcriptTable) {
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
        m_widgets.transcriptTable->installEventFilter(this);
        if (m_widgets.transcriptTable->viewport()) {
            m_widgets.transcriptTable->viewport()->installEventFilter(this);
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
    m_loadedTranscriptDoc = QJsonDocument();

    if (!clip || clip->mediaType != ClipMediaType::Audio) {
        m_persistedSelectedClipId.clear();
        m_persistedSelectedSegmentIndex = -1;
        m_persistedSelectedWordIndex = -1;
        if (m_widgets.transcriptInspectorClipLabel) {
            m_widgets.transcriptInspectorClipLabel->setText(QStringLiteral("No transcript selected"));
        }
        if (m_widgets.transcriptInspectorDetailsLabel) {
            m_widgets.transcriptInspectorDetailsLabel->setText(
                QStringLiteral("Select an audio clip with a WhisperX JSON transcript."));
        }
        m_updating = false;
        return;
    }

    if (m_widgets.transcriptInspectorClipLabel) {
        m_widgets.transcriptInspectorClipLabel->setText(clip->label);
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
    if (!clip || clip->mediaType != ClipMediaType::Audio || m_loadedTranscriptPath.isEmpty()) {
        m_widgets.transcriptTable->clearSelection();
        return;
    }

    int matchingRow = -1;
    for (int row = 0; row < m_widgets.transcriptTable->rowCount(); ++row) {
        QTableWidgetItem* startItem = m_widgets.transcriptTable->item(row, 0);
        if (!startItem) continue;
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
    if (item->column() == 0 || item->column() == 1) {
        double seconds = 0.0;
        if (!m_transcriptEngine.parseTranscriptTime(item->text(), &seconds)) {
            refresh();
            return;
        }
        if (item->column() == 0) {
            const double currentEnd = wordObj.value(QStringLiteral("end")).toDouble(seconds);
            wordObj[QStringLiteral("start")] = qMin(seconds, currentEnd);
        } else {
            const double currentStart = wordObj.value(QStringLiteral("start")).toDouble(0.0);
            wordObj[QStringLiteral("end")] = qMax(seconds, currentStart);
        }
    } else if (item->column() == 2) {
        wordObj[QStringLiteral("word")] = item->text();
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

    QItemSelectionModel* selectionModel = m_widgets.transcriptTable->selectionModel();
    if (!selectionModel) return;

    const QModelIndexList selectedRows = selectionModel->selectedRows();
    if (selectedRows.isEmpty()) return;

    QSet<quint64> deleteKeys;
    for (const QModelIndex& index : selectedRows) {
        const int row = index.row();
        QTableWidgetItem* item = m_widgets.transcriptTable->item(row, 0);
        if (!item || item->data(Qt::UserRole + 4).toBool()) continue;
        const int segmentIndex = item->data(Qt::UserRole + 5).toInt();
        const int wordIndex = item->data(Qt::UserRole + 6).toInt();
        if (segmentIndex < 0 || wordIndex < 0) continue;
        deleteKeys.insert((static_cast<quint64>(segmentIndex) << 32) |
                          static_cast<quint32>(wordIndex));
    }
    if (deleteKeys.isEmpty()) return;

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    for (int segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
        QJsonObject segmentObj = segments.at(segmentIndex).toObject();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        QJsonArray filteredWords;
        for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
            const quint64 key = (static_cast<quint64>(segmentIndex) << 32) |
                                static_cast<quint32>(wordIndex);
            if (!deleteKeys.contains(key)) {
                filteredWords.push_back(words.at(wordIndex));
            }
        }
        segmentObj[QStringLiteral("words")] = filteredWords;
        segments.replace(segmentIndex, segmentObj);
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

void TranscriptTab::setSelectedRowsSkipped(bool skipped)
{
    if (m_updating || !m_widgets.transcriptTable || m_loadedTranscriptPath.isEmpty() ||
        !m_loadedTranscriptDoc.isObject()) {
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
        if (!item || item->data(Qt::UserRole + 4).toBool()) {
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
    if (!selectedClip || selectedClip->mediaType != ClipMediaType::Audio) {
        return;
    }

    QTableWidgetItem* item = m_widgets.transcriptTable->item(row, 0);
    if (!item || item->data(Qt::UserRole + 4).toBool()) {
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
        item && !item->data(Qt::UserRole + 4).toBool()) {
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
    QString transcriptPath;
    if (!ensureEditableTranscriptForClipFile(clip.filePath, &transcriptPath)) {
        transcriptPath = transcriptWorkingPathForClipFile(clip.filePath);
    }

    QFile transcriptFile(transcriptPath);
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        if (m_widgets.transcriptInspectorDetailsLabel) {
            m_widgets.transcriptInspectorDetailsLabel->setText(QStringLiteral("No transcript file found."));
        }
        return;
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
    for (const QJsonValue& segValue : segments) {
        totalWords += segValue.toObject().value(QStringLiteral("words")).toArray().size();
    }

    if (m_widgets.transcriptInspectorDetailsLabel) {
        m_widgets.transcriptInspectorDetailsLabel->setText(
            QStringLiteral("%1 words across %2 segments, showing normalized speech windows and gaps")
                .arg(totalWords)
                .arg(segments.size()));
    }

    QVector<TranscriptRow> rows = parseTranscriptRows(segments, m_transcriptPrependMs, m_transcriptPostpendMs);
    adjustOverlappingRows(rows);
    populateTable(rows);
}

QVector<TranscriptTab::TranscriptRow> TranscriptTab::parseTranscriptRows(const QJsonArray& segments,
                                                                         int prependMs,
                                                                         int postpendMs)
{
    QVector<TranscriptRow> rows;
    const double prependSeconds = prependMs / 1000.0;
    const double postpendSeconds = postpendMs / 1000.0;

    for (int segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
        const QJsonArray words = segments.at(segmentIndex).toObject().value(QStringLiteral("words")).toArray();
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
            row.text = text;
            row.isSkipped = word.value(kTranscriptWordSkippedKey).toBool(false);
            row.segmentIndex = segmentIndex;
            row.wordIndex = wordIndex;
            rows.push_back(row);
        }
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

    QVector<TranscriptRow> displayRows;
    displayRows.reserve(rows.size() * 2);
    for (const TranscriptRow& wordRow : rows) {
        if (!displayRows.isEmpty()) {
            const TranscriptRow& previous = displayRows.constLast();
            const int64_t gapLength = wordRow.startFrame - previous.endFrame - 1;
            if (gapLength > 1) {
                TranscriptRow gapRow;
                gapRow.startFrame = previous.endFrame + 1;
                gapRow.endFrame = wordRow.startFrame - 1;
                gapRow.text = QStringLiteral("[Gap]");
                gapRow.isGap = true;
                displayRows.push_back(gapRow);
            }
        }
        displayRows.push_back(wordRow);
    }

    m_widgets.transcriptTable->setRowCount(displayRows.size());
    int restoreRow = -1;
    for (int row = 0; row < displayRows.size(); ++row) {
        const TranscriptRow& entry = displayRows.at(row);
        const double startTime = static_cast<double>(entry.startFrame) / kTimelineFps;
        const double endTime = static_cast<double>(entry.endFrame + 1) / kTimelineFps;

        auto* startItem = new QTableWidgetItem(m_transcriptEngine.secondsToTranscriptTime(startTime));
        auto* endItem = new QTableWidgetItem(m_transcriptEngine.secondsToTranscriptTime(endTime));
        auto* textItem = new QTableWidgetItem(entry.text);

        QTableWidgetItem* rowItems[] = {startItem, endItem, textItem};
        for (QTableWidgetItem* tableItem : rowItems) {
            tableItem->setData(Qt::UserRole, startTime);
            tableItem->setData(Qt::UserRole + 1, endTime);
            tableItem->setData(Qt::UserRole + 2, QVariant::fromValue(entry.startFrame));
            tableItem->setData(Qt::UserRole + 3, QVariant::fromValue(entry.endFrame));
            tableItem->setData(Qt::UserRole + 4, entry.isGap);
            tableItem->setData(Qt::UserRole + 5, entry.segmentIndex);
            tableItem->setData(Qt::UserRole + 6, entry.wordIndex);
            tableItem->setData(Qt::UserRole + 7, entry.isSkipped);
        }

        applyTranscriptRowState(startItem, endItem, textItem, entry);

        if (!entry.isGap &&
            selectedClipId == m_persistedSelectedClipId &&
                   entry.segmentIndex == m_persistedSelectedSegmentIndex &&
                   entry.wordIndex == m_persistedSelectedWordIndex) {
            restoreRow = row;
        }

        m_widgets.transcriptTable->setItem(row, 0, startItem);
        m_widgets.transcriptTable->setItem(row, 1, endItem);
        m_widgets.transcriptTable->setItem(row, 2, textItem);
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
                                            QTableWidgetItem* textItem,
                                            const TranscriptRow& entry) const
{
    const QColor gapColor(255, 248, 196);
    const QColor skippedColor(214, 255, 214);
    const QColor skippedTextColor(22, 96, 37);
    QTableWidgetItem* rowItems[] = {startItem, endItem, textItem};

    if (entry.isGap) {
        for (QTableWidgetItem* tableItem : rowItems) {
            tableItem->setBackground(gapColor);
            tableItem->setFlags(tableItem->flags() & ~Qt::ItemIsEditable);
        }
        return;
    }

    if (!entry.isSkipped) {
        return;
    }

    QFont skippedFont = textItem->font();
    skippedFont.setItalic(true);
    textItem->setFont(skippedFont);
    textItem->setText(QStringLiteral("Skip: %1").arg(entry.text));
    for (QTableWidgetItem* tableItem : rowItems) {
        tableItem->setBackground(skippedColor);
        tableItem->setForeground(skippedTextColor);
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
    if (isGap) return;

    QMenu menu;
    QAction* addAbove = menu.addAction(QStringLiteral("Add Word Above"));
    QAction* addBelow = menu.addAction(QStringLiteral("Add Word Below"));
    menu.addSeparator();
    QAction* expandAction = menu.addAction(QStringLiteral("Expand"));
    menu.addSeparator();
    const bool rowSkipped = item->data(Qt::UserRole + 7).toBool();
    QAction* skipAction = menu.addAction(rowSkipped ? QStringLiteral("Unskip Word")
                                                    : QStringLiteral("Skip Word"));
    QAction* deleteAction = menu.addAction(QStringLiteral("Delete Word"));

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

void TranscriptTab::insertWordAtRow(int row, bool above)
{
    if (!m_widgets.transcriptTable || m_loadedTranscriptPath.isEmpty() ||
        !m_loadedTranscriptDoc.isObject()) {
        return;
    }

    // Get the times of the word we're inserting relative to
    QTableWidgetItem* currentItem = m_widgets.transcriptTable->item(row, 0);
    if (!currentItem) return;

    const double currentStart = currentItem->data(Qt::UserRole).toDouble();
    const double currentEnd = currentItem->data(Qt::UserRole + 1).toDouble();
    const int currentSegmentIndex = currentItem->data(Qt::UserRole + 5).toInt();
    const int currentWordIndex = currentItem->data(Qt::UserRole + 6).toInt();

    double newWordStart, newWordEnd;
    int targetSegmentIndex, targetWordIndex;

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

    // Update the JSON document
    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonArray segments = root.value(QStringLiteral("segments")).toArray();

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

    QTableWidgetItem* currentItem = m_widgets.transcriptTable->item(row, 0);
    if (!currentItem) return;

    const bool isGap = currentItem->data(Qt::UserRole + 4).toBool();
    if (isGap) return;

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
        wordObj[QStringLiteral("start")] = newStartTime;
    }
    if (hasNextWord) {
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
