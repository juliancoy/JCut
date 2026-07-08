#include "editor.h"

#include "background_fill_effect.h"
#include "export_vulkan_preview_widget.h"
#include "speaker_export_harness.h"

#include <QAbstractItemView>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QProgressBar>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <cmath>

using namespace editor;

namespace {

QString sanitizedExportBaseName(QString value, const QString& fallback)
{
    value = value.simplified();
    value.remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")));
    value.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._ -]+")), QStringLiteral(""));
    value.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral("_"));
    value.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    value = value.trimmed();
    while (value.startsWith(QLatin1Char('.')) || value.startsWith(QLatin1Char('_')) ||
           value.startsWith(QLatin1Char('-'))) {
        value.remove(0, 1);
    }
    while (value.endsWith(QLatin1Char('.')) || value.endsWith(QLatin1Char('_')) ||
           value.endsWith(QLatin1Char('-'))) {
        value.chop(1);
    }
    if (value.isEmpty()) {
        value = fallback.trimmed();
    }
    return value.left(80);
}

QString exportSpeedSuffix(qreal speed)
{
    const qreal normalizedSpeed = std::isfinite(speed) && speed > 0.001 ? speed : 1.0;
    QString value = QString::number(normalizedSpeed, 'f', 3);
    while (value.contains(QLatin1Char('.')) && value.endsWith(QLatin1Char('0'))) {
        value.chop(1);
    }
    if (value.endsWith(QLatin1Char('.'))) {
        value.chop(1);
    }
    return QStringLiteral("_%1x").arg(value);
}

QString stripExportSpeedSuffix(QString baseName)
{
    static const QRegularExpression speedSuffixPattern(
        QStringLiteral("_[0-9]+(?:\\.[0-9]+)?x$"));
    baseName.remove(speedSuffixPattern);
    return baseName.trimmed();
}

QString baseNameWithExportSpeed(const QString& baseName, qreal speed)
{
    const QString stripped = stripExportSpeedSuffix(baseName);
    return (stripped.isEmpty() ? QStringLiteral("render") : stripped) + exportSpeedSuffix(speed);
}

QString coupleWordTitle(QString value, const QString& fallback)
{
    value = value.simplified();
    value.remove(QRegularExpression(QStringLiteral("[\\r\\n]")));
    value.remove(QRegularExpression(QStringLiteral("^[\"'`]+|[\"'`]+$")));
    value.remove(QRegularExpression(QStringLiteral("[.!?;:]+$")));

    const QStringList words = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty()) {
        return fallback;
    }
    QStringList limited;
    for (const QString& word : words) {
        limited.push_back(word);
        if (limited.size() >= 4) {
            break;
        }
    }
    return limited.join(QLatin1Char(' '));
}

QString titleFromAiResponse(const QJsonObject& response)
{
    const QStringList topLevelKeys{
        QStringLiteral("title"),
        QStringLiteral("name"),
        QStringLiteral("filename"),
    };
    for (const QString& key : topLevelKeys) {
        const QString value = response.value(key).toString().trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }

    const QJsonValue resultValue = response.value(QStringLiteral("result"));
    if (resultValue.isString()) {
        return resultValue.toString().trimmed();
    }
    const QJsonObject resultObj = resultValue.toObject();
    for (const QString& key : topLevelKeys) {
        const QString value = resultObj.value(key).toString().trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }

    const QJsonObject payloadObj = response.value(QStringLiteral("payload")).toObject();
    for (const QString& key : topLevelKeys) {
        const QString value = payloadObj.value(key).toString().trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return {};
}

QString speakerSectionFallbackTitle(const QString& speakerDisplayName,
                                    const QString& speakerId,
                                    int sectionOrdinal,
                                    int64_t sourceStartFrame,
                                    int64_t sourceEndFrame)
{
    QString speakerName = speakerDisplayName.simplified();
    if (speakerName.isEmpty()) {
        speakerName = speakerId.simplified();
    }
    if (speakerName.isEmpty()) {
        speakerName = QStringLiteral("Speaker");
    }
    if (sectionOrdinal > 0) {
        return QStringLiteral("%1 %2").arg(speakerName).arg(sectionOrdinal);
    }
    return QStringLiteral("%1 section %2 %3")
        .arg(speakerName)
        .arg(sourceStartFrame)
        .arg(sourceEndFrame);
}

QStringList normalizedSectionTrackIds(const QStringList& trackIds)
{
    QStringList normalized;
    QSet<QString> seen;
    for (const QString& trackIdText : trackIds) {
        bool ok = false;
        const int trackId = trackIdText.trimmed().toInt(&ok);
        if (!ok || trackId < 0) {
            continue;
        }
        const QString normalizedTrackId = QString::number(trackId);
        if (seen.contains(normalizedTrackId)) {
            continue;
        }
        seen.insert(normalizedTrackId);
        normalized.push_back(normalizedTrackId);
    }
    return normalized;
}

QString speakerTrackSectionTitle(const SpeakerSectionExportItem& section)
{
    QString speakerName = section.speakerDisplayName.simplified();
    if (speakerName.isEmpty()) {
        speakerName = section.speakerId.simplified();
    }
    if (speakerName.isEmpty()) {
        speakerName = QStringLiteral("Speaker");
    }

    const QStringList tracks = normalizedSectionTrackIds(section.trackIds);
    if (tracks.isEmpty()) {
        return QStringLiteral("%1 no track").arg(speakerName);
    }
    if (tracks.size() == 1) {
        return QStringLiteral("%1 track %2").arg(speakerName, tracks.constFirst());
    }
    return QStringLiteral("%1 tracks %2").arg(speakerName, tracks.join(QLatin1Char('-')));
}

QVector<SpeakerSectionExportItem> coalescedAdjacentSpeakerSections(
    const QVector<SpeakerSectionExportItem>& sections)
{
    QVector<SpeakerSectionExportItem> coalesced;
    coalesced.reserve(sections.size());
    for (const SpeakerSectionExportItem& section : sections) {
        if (!coalesced.isEmpty() &&
            coalesced.last().speakerId.trimmed() == section.speakerId.trimmed()) {
            SpeakerSectionExportItem& merged = coalesced.last();
            merged.sourceEndFrame = qMax<qint64>(merged.sourceEndFrame, section.sourceEndFrame);
            if (!section.snippet.trimmed().isEmpty()) {
                if (!merged.snippet.trimmed().isEmpty()) {
                    merged.snippet += QStringLiteral(" ");
                }
                merged.snippet += section.snippet.trimmed();
            }
            merged.wordCount += qMax(0, section.wordCount);
            QStringList mergedTracks = normalizedSectionTrackIds(merged.trackIds);
            QSet<QString> seenTracks;
            for (const QString& trackId : std::as_const(mergedTracks)) {
                seenTracks.insert(trackId);
            }
            for (const QString& trackId : normalizedSectionTrackIds(section.trackIds)) {
                if (seenTracks.contains(trackId)) {
                    continue;
                }
                seenTracks.insert(trackId);
                mergedTracks.push_back(trackId);
            }
            merged.trackIds = mergedTracks;
            continue;
        }
        SpeakerSectionExportItem copy = section;
        copy.trackIds = normalizedSectionTrackIds(copy.trackIds);
        coalesced.push_back(copy);
    }
    return coalesced;
}

QVector<ExportRangeSegment> timelineRangesForTranscriptSection(const TimelineClip& clip,
                                                               int64_t sourceStartFrame,
                                                               int64_t sourceEndFrame,
                                                               const QVector<RenderSyncMarker>& markers)
{
    QVector<ExportRangeSegment> timelineRanges;
    const int64_t clipStartFrame = clip.startFrame;
    const int64_t clipEndFrame = clip.startFrame + clip.durationFrames - 1;
    for (int64_t timelineFrame = clipStartFrame; timelineFrame <= clipEndFrame; ++timelineFrame) {
        const int64_t transcriptFrame = transcriptFrameForClipAtTimelineSample(
            clip, frameToSamples(timelineFrame), markers);
        if (transcriptFrame < sourceStartFrame || transcriptFrame > sourceEndFrame) {
            continue;
        }
        if (timelineRanges.isEmpty() || timelineFrame > timelineRanges.constLast().endFrame + 1) {
            timelineRanges.push_back(ExportRangeSegment{timelineFrame, timelineFrame});
        } else {
            timelineRanges.last().endFrame = timelineFrame;
        }
    }
    return timelineRanges;
}

QString uniqueExportPath(const QString& directory,
                         const QString& baseName,
                         const QString& outputFormat,
                         QSet<QString>* reservedPaths)
{
    const QString safeBase = baseName.trimmed().isEmpty() ? QStringLiteral("section") : baseName.trimmed();
    const QString suffix = outputFormat.trimmed().isEmpty() ? QStringLiteral("mp4") : outputFormat.trimmed();
    QDir dir(directory);
    QString candidate = dir.filePath(QStringLiteral("%1.%2").arg(safeBase, suffix));
    int counter = 2;
    while ((reservedPaths && reservedPaths->contains(candidate)) || QFileInfo::exists(candidate)) {
        candidate = dir.filePath(QStringLiteral("%1_%2.%3").arg(safeBase).arg(counter).arg(suffix));
        ++counter;
    }
    if (reservedPaths) {
        reservedPaths->insert(candidate);
    }
    return candidate;
}

QString deterministicExportPath(const QString& directory,
                                const QString& baseName,
                                const QString& outputFormat)
{
    const QString safeBase = baseName.trimmed().isEmpty() ? QStringLiteral("section") : baseName.trimmed();
    const QString suffix = outputFormat.trimmed().isEmpty() ? QStringLiteral("mp4") : outputFormat.trimmed();
    return QDir(directory).filePath(QStringLiteral("%1.%2").arg(safeBase, suffix));
}

void setBulkExportRowStatus(QTableWidget* table,
                            int row,
                            const QString& status,
                            const QColor& background,
                            const QColor& foreground = QColor(QStringLiteral("#111827")))
{
    if (!table || row < 0 || row >= table->rowCount()) {
        return;
    }
    if (auto* statusItem = table->item(row, 1)) {
        statusItem->setText(status);
    }
    for (int column = 0; column < table->columnCount(); ++column) {
        QTableWidgetItem* item = table->item(row, column);
        if (!item) {
            item = new QTableWidgetItem;
            table->setItem(row, column, item);
        }
        item->setBackground(background);
        item->setForeground(foreground);
    }
}

void scrollBulkExportRowIntoView(QTableWidget* table, int row)
{
    if (!table || row < 0 || row >= table->rowCount()) {
        return;
    }
    table->scrollToItem(table->item(row, 0), QAbstractItemView::PositionAtCenter);
    table->selectRow(row);
}

bool confirmContiguousSectionExportPreflight(QWidget* parent,
                                             RenderRequest* request,
                                             int sectionCount)
{
    if (!request) {
        return false;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(sectionCount == 1
                              ? QStringLiteral("Section Export Preflight")
                              : QStringLiteral("Sections Export Preflight"));
    dialog.setModal(true);
    dialog.resize(430, 260);

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    auto* summary = new QLabel(
        sectionCount == 1
            ? QStringLiteral("Confirm render settings for this contiguous transcript section.")
            : QStringLiteral("Confirm render settings for %1 contiguous transcript sections.").arg(sectionCount),
        &dialog);
    summary->setWordWrap(true);
    root->addWidget(summary);

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto* speedSpin = new QDoubleSpinBox(&dialog);
    speedSpin->setRange(0.100, 8.000);
    speedSpin->setSingleStep(0.100);
    speedSpin->setDecimals(3);
    speedSpin->setSuffix(QStringLiteral("x"));
    speedSpin->setValue(std::isfinite(request->playbackSpeed) && request->playbackSpeed > 0.001
                            ? request->playbackSpeed
                            : 1.0);
    form->addRow(QStringLiteral("Speed"), speedSpin);

    auto* fpsSpin = new QDoubleSpinBox(&dialog);
    fpsSpin->setRange(1.0, 240.0);
    fpsSpin->setSingleStep(1.0);
    fpsSpin->setDecimals(3);
    fpsSpin->setValue(std::isfinite(request->outputFps) && request->outputFps > 0.001
                          ? request->outputFps
                          : static_cast<double>(kTimelineFps));
    form->addRow(QStringLiteral("Frame Rate"), fpsSpin);

    auto* widthSpin = new QSpinBox(&dialog);
    widthSpin->setRange(16, 8192);
    widthSpin->setSingleStep(16);
    widthSpin->setValue(qMax(16, request->outputSize.width()));
    auto* heightSpin = new QSpinBox(&dialog);
    heightSpin->setRange(16, 8192);
    heightSpin->setSingleStep(16);
    heightSpin->setValue(qMax(16, request->outputSize.height()));
    auto* sizeRow = new QWidget(&dialog);
    auto* sizeLayout = new QHBoxLayout(sizeRow);
    sizeLayout->setContentsMargins(0, 0, 0, 0);
    sizeLayout->setSpacing(6);
    sizeLayout->addWidget(widthSpin);
    sizeLayout->addWidget(new QLabel(QStringLiteral("x"), sizeRow));
    sizeLayout->addWidget(heightSpin);
    form->addRow(QStringLiteral("Output Window"), sizeRow);

    auto* formatLabel = new QLabel(
        request->outputFormat.trimmed().isEmpty()
            ? QStringLiteral("mp4")
            : request->outputFormat.trimmed(),
        &dialog);
    form->addRow(QStringLiteral("Format"), formatLabel);

    root->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Continue"));
    root->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    request->playbackSpeed = speedSpin->value();
    request->outputFps = fpsSpin->value();
    request->outputSize = QSize(widthSpin->value(), heightSpin->value());
    qInfo().noquote()
        << QStringLiteral("[section-export-preflight] sections=%1 speed=%2 fps=%3 size=%4x%5 format=%6")
               .arg(sectionCount)
               .arg(request->playbackSpeed, 0, 'f', 3)
               .arg(request->outputFps, 0, 'f', 3)
               .arg(request->outputSize.width())
               .arg(request->outputSize.height())
               .arg(request->outputFormat);
    return true;
}

} // namespace

bool EditorWindow::parseSyncActionText(const QString &text, RenderSyncAction *actionOut) const
{
    const QString normalized = text.trimmed().toLower();
    if (normalized == QStringLiteral("duplicate") || normalized == QStringLiteral("dup")) {
        *actionOut = RenderSyncAction::DuplicateFrame;
        return true;
    }
    if (normalized == QStringLiteral("skip")) {
        *actionOut = RenderSyncAction::SkipFrame;
        return true;
    }
    return false;
}

void EditorWindow::refreshSyncInspector()
{
    if (m_syncTab) {
        m_syncTab->refresh();
    }
}

void EditorWindow::onSyncTableSelectionChanged()
{
    // Sync table interactions are handled by SyncTab.
}

void EditorWindow::onSyncTableItemChanged(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    // Sync table interactions are handled by SyncTab.
}

void EditorWindow::onSyncTableItemDoubleClicked(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    // Sync table interactions are handled by SyncTab.
}

void EditorWindow::onSyncTableCustomContextMenu(const QPoint& pos)
{
    Q_UNUSED(pos);
    // Sync table interactions are handled by SyncTab.
}

void EditorWindow::refreshClipInspector()
{
    if (m_propertiesTab) {
        m_propertiesTab->refresh();
    }
}

void EditorWindow::refreshTracksTab()
{
    if (m_tracksTab) {
        m_tracksTab->refresh();
    }
}

void EditorWindow::onTrackTableItemChanged(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    // Tracks table interactions are handled by TracksTab.
}

void EditorWindow::refreshOutputInspector()
{
    if (m_outputTab) {
        m_outputTab->refresh();
    }
}

void EditorWindow::applyOutputRangeFromInspector()
{
    if (m_outputTab) {
        m_outputTab->applyRangeFromInspector();
    }
}

void EditorWindow::renderFromOutputInspector()
{
    if (m_outputTab) {
        m_outputTab->renderFromInspector();
    }
}

void EditorWindow::renderTimelineFromOutputRequestCore(const jcut::render::RenderRequestCore& request)
{
    if (!m_timeline) {
        return;
    }

    jcut::render::TimelineRenderData timelineData;
    const QVector<TimelineClip> clips = m_timeline->clips();
    timelineData.clips.reserve(static_cast<std::size_t>(clips.size()));
    for (const TimelineClip& clip : clips) {
        timelineData.clips.push_back(clip);
    }

    const QVector<TimelineTrack> tracks = m_timeline->tracks();
    timelineData.tracks.reserve(static_cast<std::size_t>(tracks.size()));
    for (const TimelineTrack& track : tracks) {
        timelineData.tracks.push_back(track);
    }

    const QVector<RenderSyncMarker> renderSyncMarkers = m_timeline->renderSyncMarkers();
    timelineData.renderSyncMarkers.reserve(static_cast<std::size_t>(renderSyncMarkers.size()));
    for (const RenderSyncMarker& marker : renderSyncMarkers) {
        timelineData.renderSyncMarkers.push_back(marker);
    }

    const QVector<ExportRangeSegment> exportRanges = effectivePlaybackRanges();
    timelineData.exportRanges.reserve(static_cast<std::size_t>(exportRanges.size()));
    for (const ExportRangeSegment& range : exportRanges) {
        timelineData.exportRanges.push_back(range);
    }

    RenderRequest qtRequest = jcut::render::toQtRenderRequest(request, timelineData);
    qtRequest.transcriptPrependMs = qMax(0, m_transcriptPrependMs);
    qtRequest.transcriptPostpendMs = qMax(0, m_transcriptPostpendMs);
    qtRequest.transcriptOffsetMs = qBound(-10000, m_transcriptOffsetMs, 10000);
    renderTimelineFromOutputRequest(qtRequest);
}

RenderRequest EditorWindow::buildRenderRequestFromOutputControls() const
{
    RenderRequest request;
    request.outputFormat = m_outputFormatCombo
        ? m_outputFormatCombo->currentData().toString()
        : QStringLiteral("mp4");
    if (request.outputFormat.isEmpty()) {
        request.outputFormat = QStringLiteral("mp4");
    }

    request.outputSize = QSize(
        m_outputWidthSpin ? m_outputWidthSpin->value() : 1080,
        m_outputHeightSpin ? m_outputHeightSpin->value() : 1920);
    request.outputFps = m_outputFpsSpin
        ? m_outputFpsSpin->value()
        : static_cast<double>(kTimelineFps);
    request.playbackSpeed = std::isfinite(m_exportPlaybackSpeed) && m_exportPlaybackSpeed > 0.001
        ? m_exportPlaybackSpeed
        : 1.0;
    request.useProxyMedia = m_renderUseProxiesCheckBox &&
                            m_renderUseProxiesCheckBox->isChecked();
    request.backgroundFillEffect =
        backgroundFillEffectFromString(m_backgroundFillEffectCombo
                                           ? m_backgroundFillEffectCombo->currentData().toString()
                                           : QString());
    request.backgroundFillOpacity = m_backgroundFillOpacitySpin
        ? qBound<qreal>(0.0, m_backgroundFillOpacitySpin->value() / 100.0, 1.0)
        : 1.0;
    request.backgroundFillBrightness = m_backgroundFillBrightnessSpin
        ? qBound<qreal>(-1.0, m_backgroundFillBrightnessSpin->value() / 100.0, 1.0)
        : 0.0;
    request.backgroundFillSaturation = m_backgroundFillSaturationSpin
        ? qBound<qreal>(0.0, m_backgroundFillSaturationSpin->value() / 100.0, 3.0)
        : 1.0;
    request.backgroundFillEdgePixels = m_backgroundFillEdgePixelsSlider
        ? qBound(1, m_backgroundFillEdgePixelsSlider->value(), 512)
        : 1;
    request.backgroundFillEdgeProgressive =
        m_backgroundFillEdgeProgressiveCheckBox && m_backgroundFillEdgeProgressiveCheckBox->isChecked();
    request.backgroundFillEdgePower = m_backgroundFillEdgePowerSpin
        ? qBound<qreal>(0.25, m_backgroundFillEdgePowerSpin->value(), 8.0)
        : 2.0;
    request.showCurrentSpeakerName = m_speakerShowCurrentSpeakerNameCheckBox &&
                                     m_speakerShowCurrentSpeakerNameCheckBox->isChecked();
    request.showCurrentSpeakerOrganization =
        m_speakerShowCurrentSpeakerOrganizationCheckBox &&
        m_speakerShowCurrentSpeakerOrganizationCheckBox->isChecked();
    request.currentSpeakerNameTextScale =
        m_speakerCurrentSpeakerNameTextSizeSpin
            ? qBound<qreal>(0.25, m_speakerCurrentSpeakerNameTextSizeSpin->value() / 100.0, 3.0)
            : 1.0;
    request.currentSpeakerOrganizationTextScale =
        m_speakerCurrentSpeakerOrganizationTextSizeSpin
            ? qBound<qreal>(0.25, m_speakerCurrentSpeakerOrganizationTextSizeSpin->value() / 100.0, 3.0)
            : 1.0;
    request.currentSpeakerNameVerticalPosition =
        m_speakerCurrentSpeakerNameYPositionSpin
            ? qBound<qreal>(0.0, m_speakerCurrentSpeakerNameYPositionSpin->value() / 100.0, 1.0)
            : 0.86;
    request.currentSpeakerOrganizationVerticalPosition =
        m_speakerCurrentSpeakerOrganizationYPositionSpin
            ? qBound<qreal>(0.0, m_speakerCurrentSpeakerOrganizationYPositionSpin->value() / 100.0, 1.0)
            : 0.93;
    request.currentSpeakerNameColor = m_speakerCurrentSpeakerNameColor;
    request.currentSpeakerOrganizationColor = m_speakerCurrentSpeakerOrganizationColor;
    request.currentSpeakerBackgroundColor = m_speakerCurrentSpeakerBackgroundColor;
    request.currentSpeakerBorderColor = m_speakerCurrentSpeakerBorderColor;
    request.currentSpeakerBackgroundCornerRadius =
        m_speakerCurrentSpeakerBackgroundRadiusSpin
            ? qBound<qreal>(0.0, m_speakerCurrentSpeakerBackgroundRadiusSpin->value(), 128.0)
            : 14.0;
    request.currentSpeakerBorderWidth =
        m_speakerCurrentSpeakerBorderWidthSpin
            ? qBound<qreal>(0.0, m_speakerCurrentSpeakerBorderWidthSpin->value(), 16.0)
            : 1.0;
    request.currentSpeakerShadowEnabled =
        m_speakerCurrentSpeakerShadowCheckBox
            ? m_speakerCurrentSpeakerShadowCheckBox->isChecked()
            : true;
    request.currentSpeakerShadowColor = m_speakerCurrentSpeakerShadowColor;
    request.transcriptPrependMs = qMax(0, m_transcriptPrependMs);
    request.transcriptPostpendMs = qMax(0, m_transcriptPostpendMs);
    request.transcriptOffsetMs = qBound(-10000, m_transcriptOffsetMs, 10000);
    request.createVideoFromImageSequence = m_createImageSequenceCheckBox &&
                                           m_createImageSequenceCheckBox->isChecked();
    if (request.createVideoFromImageSequence && m_imageSequenceFormatCombo) {
        request.imageSequenceFormat = m_imageSequenceFormatCombo->currentData().toString();
        if (request.imageSequenceFormat.isEmpty()) {
            request.imageSequenceFormat = QStringLiteral("jpeg");
        }
    }
    return request;
}

void EditorWindow::persistExportRequestDefaults(const RenderRequest& request)
{
    bool changed = false;
    const qreal nextSpeed =
        qBound<qreal>(0.1,
                      std::isfinite(request.playbackSpeed) && request.playbackSpeed > 0.001
                          ? request.playbackSpeed
                          : 1.0,
                      8.0);
    if (qAbs(m_exportPlaybackSpeed - nextSpeed) >= 0.0001) {
        m_exportPlaybackSpeed = nextSpeed;
        changed = true;
    }

    const double nextFps =
        qBound(1.0,
               std::isfinite(request.outputFps) && request.outputFps > 0.001
                   ? request.outputFps
                   : static_cast<double>(kTimelineFps),
               240.0);
    if (m_outputFpsSpin && qAbs(m_outputFpsSpin->value() - nextFps) >= 0.0001) {
        QSignalBlocker blocker(m_outputFpsSpin);
        m_outputFpsSpin->setValue(nextFps);
        changed = true;
    }

    const QSize nextSize(qMax(16, request.outputSize.width()),
                         qMax(16, request.outputSize.height()));
    if (m_outputWidthSpin && m_outputWidthSpin->value() != nextSize.width()) {
        QSignalBlocker blocker(m_outputWidthSpin);
        m_outputWidthSpin->setValue(nextSize.width());
        changed = true;
    }
    if (m_outputHeightSpin && m_outputHeightSpin->value() != nextSize.height()) {
        QSignalBlocker blocker(m_outputHeightSpin);
        m_outputHeightSpin->setValue(nextSize.height());
        changed = true;
    }
    if (m_preview && nextSize.isValid() && m_preview->outputSize() != nextSize) {
        m_preview->setOutputSize(nextSize);
    }

    if (changed) {
        scheduleSaveState();
    }
}

void EditorWindow::applyPreviewViewMode(const QString& modeText)
{
    if (!m_featureAudioPreviewMode) {
        const QString nextMode = QStringLiteral("video");
        const bool changed = (m_previewViewMode != nextMode);
        m_previewViewMode = nextMode;
        if (changed && m_preview) {
            m_preview->setViewMode(PreviewSurface::ViewMode::Video);
        }
        return;
    }
    const QString normalized = modeText.trimmed().toLower();
    const QString nextMode = (normalized.contains(QStringLiteral("audio")))
        ? QStringLiteral("audio")
        : QStringLiteral("video");
    const bool changed = (m_previewViewMode != nextMode);
    m_previewViewMode = nextMode;
    if (changed && m_preview) {
        m_preview->setViewMode(m_previewViewMode == QStringLiteral("audio")
                                   ? PreviewSurface::ViewMode::Audio
                                   : PreviewSurface::ViewMode::Video);
    }
    if (!m_loadingState) {
        scheduleSaveState();
    }
}

void EditorWindow::openAudioToolsDialog()
{
    if (!m_featureAudioDynamicsTools) {
        QMessageBox::information(this,
                                 QStringLiteral("Audio Dynamics"),
                                 QStringLiteral("Audio dynamics tools are disabled by feature flag."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Audio Dynamics"));
    dialog.setModal(true);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto makeSpin = [&dialog](qreal min, qreal max, qreal step, int decimals, qreal value) {
        auto* spin = new QDoubleSpinBox(&dialog);
        spin->setRange(min, max);
        spin->setSingleStep(step);
        spin->setDecimals(decimals);
        spin->setValue(value);
        return spin;
    };

    auto* normalizeCheck = new QCheckBox(QStringLiteral("Normalize"), &dialog);
    normalizeCheck->setChecked(m_previewAudioDynamics.normalizeEnabled);
    auto* normalizeTarget = makeSpin(-24.0, 0.0, 0.5, 1, m_previewAudioDynamics.normalizeTargetDb);
    auto* peakReductionCheck = new QCheckBox(QStringLiteral("Peak Reduction"), &dialog);
    peakReductionCheck->setChecked(m_previewAudioDynamics.peakReductionEnabled);
    auto* peakThreshold = makeSpin(-24.0, 0.0, 0.5, 1, m_previewAudioDynamics.peakThresholdDb);
    auto* limiterCheck = new QCheckBox(QStringLiteral("Limiter"), &dialog);
    limiterCheck->setChecked(m_previewAudioDynamics.limiterEnabled);
    auto* limiterThreshold = makeSpin(-12.0, 0.0, 0.1, 1, m_previewAudioDynamics.limiterThresholdDb);
    auto* compressorCheck = new QCheckBox(QStringLiteral("Compressor"), &dialog);
    compressorCheck->setChecked(m_previewAudioDynamics.compressorEnabled);
    auto* compressorThreshold =
        makeSpin(-30.0, -1.0, 0.5, 1, m_previewAudioDynamics.compressorThresholdDb);
    auto* compressorRatio = makeSpin(1.0, 20.0, 0.1, 1, m_previewAudioDynamics.compressorRatio);
    auto* softClipCheck = new QCheckBox(QStringLiteral("Soft Clip"), &dialog);
    softClipCheck->setChecked(m_previewAudioDynamics.softClipEnabled);

    auto* form = new QFormLayout;
    form->addRow(normalizeCheck, normalizeTarget);
    form->addRow(peakReductionCheck, peakThreshold);
    form->addRow(limiterCheck, limiterThreshold);
    form->addRow(compressorCheck, compressorThreshold);
    form->addRow(QStringLiteral("Compressor Ratio"), compressorRatio);
    form->addRow(softClipCheck);
    layout->addLayout(form);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto* cancelBtn = new QPushButton(QStringLiteral("Cancel"), &dialog);
    auto* applyBtn = new QPushButton(QStringLiteral("Apply"), &dialog);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(applyBtn);
    layout->addLayout(btnRow);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(applyBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    m_previewAudioDynamics.normalizeEnabled = normalizeCheck->isChecked();
    m_previewAudioDynamics.normalizeTargetDb = normalizeTarget->value();
    m_previewAudioDynamics.peakReductionEnabled = peakReductionCheck->isChecked();
    m_previewAudioDynamics.peakThresholdDb = peakThreshold->value();
    m_previewAudioDynamics.limiterEnabled = limiterCheck->isChecked();
    m_previewAudioDynamics.limiterThresholdDb = limiterThreshold->value();
    m_previewAudioDynamics.compressorEnabled = compressorCheck->isChecked();
    m_previewAudioDynamics.compressorThresholdDb = compressorThreshold->value();
    m_previewAudioDynamics.compressorRatio = compressorRatio->value();
    m_previewAudioDynamics.softClipEnabled = softClipCheck->isChecked();

    if (m_preview) {
        m_preview->setAudioDynamicsSettings(m_previewAudioDynamics);
    }
    scheduleSaveState();
}


bool EditorWindow::renderTimelineFromOutputRequest(const RenderRequest &request,
                                                   bool showCompletionMessage,
                                                   RenderProgressDialogControls* progressControls)
{
    RenderRequest effectiveRequest = request;
    const bool notifyRenderCompletion =
        showCompletionMessage && !effectiveRequest.suppressCompletionDialog;
    effectiveRequest.correctionsEnabled = m_correctionsEnabled;
    effectiveRequest.playbackTiming =
        speechFilterPlaybackTimingContext(effectiveRequest.exportRanges);
    if (effectiveRequest.useProxyMedia)
    {
        for (TimelineClip &clip : effectiveRequest.clips)
        {
            const QString proxyPath = playbackProxyPathForClip(clip);
            if (!proxyPath.isEmpty())
            {
                clip.filePath = proxyPath;
            }
        }
    }

    int64_t totalFramesToRender = 0;
    for (const ExportRangeSegment &range : std::as_const(effectiveRequest.exportRanges))
    {
        totalFramesToRender += qMax<int64_t>(0, range.endFrame - range.startFrame + 1);
    }
    if (totalFramesToRender <= 0)
    {
        totalFramesToRender = qMax<int64_t>(1, effectiveRequest.exportEndFrame - effectiveRequest.exportStartFrame + 1);
    }

    const bool verticalRenderOutput =
        effectiveRequest.outputSize.height() > effectiveRequest.outputSize.width();

    bool localRenderCancelled = false;
    std::unique_ptr<QDialog> ownedProgressDialog;
    RenderProgressDialogControls localControls;
    if (!progressControls) {
        ownedProgressDialog = std::make_unique<QDialog>(this);
        QDialog* progressDialog = ownedProgressDialog.get();
        progressDialog->setWindowTitle(QStringLiteral("Render Export"));
        progressDialog->setWindowModality(Qt::ApplicationModal);
        progressDialog->setMinimumWidth(verticalRenderOutput ? 920 : 560);
        progressDialog->setStyleSheet(QStringLiteral(
            "QDialog { background: #f6f3ee; }"
            "QLabel { color: #1f2430; font-size: 13px; }"
            "QProgressBar { border: 1px solid #c9c2b8; border-radius: 6px; text-align: center; background: #ffffff; min-height: 20px; }"
            "QProgressBar::chunk { background: #2f7d67; border-radius: 5px; }"
            "QPushButton { min-width: 96px; padding: 6px 14px; }"));
        auto *progressLayout = new QVBoxLayout(progressDialog);
        progressLayout->setContentsMargins(16, 16, 16, 16);
        progressLayout->setSpacing(10);

        auto *renderPreviewWidget = new ExportVulkanPreviewWidget(progressDialog);

        auto *renderStatusLabel = new QLabel(QStringLiteral("Preparing render..."), progressDialog);
        renderStatusLabel->setWordWrap(true);
        renderStatusLabel->setAlignment(Qt::AlignCenter);

        auto *showRenderPreviewCheckBox = new QCheckBox(QStringLiteral("Show Visual Preview"), progressDialog);
        showRenderPreviewCheckBox->setChecked(true);

        auto *renderSourcesLabel = new QLabel(QStringLiteral("Sources In Use (Current Frame)"), progressDialog);
        renderSourcesLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        auto *renderSourcesList = new QPlainTextEdit(progressDialog);
        renderSourcesList->setReadOnly(true);
        renderSourcesList->setMinimumHeight(140);
        renderSourcesList->setPlainText(QStringLiteral("Waiting for first rendered frame..."));

        if (verticalRenderOutput) {
            auto *contentRow = new QHBoxLayout;
            contentRow->setSpacing(12);

            auto *leftColumn = new QVBoxLayout;
            leftColumn->setSpacing(10);
            leftColumn->addWidget(renderStatusLabel);
            leftColumn->addWidget(showRenderPreviewCheckBox, 0, Qt::AlignLeft);
            leftColumn->addWidget(renderSourcesLabel);
            leftColumn->addWidget(renderSourcesList, 1);

            auto *rightColumn = new QVBoxLayout;
            rightColumn->setSpacing(10);
            rightColumn->addWidget(renderPreviewWidget, 1);

            contentRow->addLayout(leftColumn, 3);
            contentRow->addLayout(rightColumn, 2);
            progressLayout->addLayout(contentRow);
        } else {
            progressLayout->addWidget(renderStatusLabel);
            progressLayout->addWidget(showRenderPreviewCheckBox, 0, Qt::AlignLeft);
            progressLayout->addWidget(renderPreviewWidget);
            progressLayout->addWidget(renderSourcesLabel);
            progressLayout->addWidget(renderSourcesList);
        }

        auto *renderProgressBar = new QProgressBar(progressDialog);
        progressLayout->addWidget(renderProgressBar);

        auto *buttonRow = new QHBoxLayout;
        buttonRow->addStretch(1);
        auto *cancelRenderButton = new QPushButton(QStringLiteral("Cancel"), progressDialog);
        buttonRow->addWidget(cancelRenderButton);
        progressLayout->addLayout(buttonRow);

        QObject::connect(cancelRenderButton, &QPushButton::clicked, progressDialog, [&localRenderCancelled, cancelRenderButton]() {
            localRenderCancelled = true;
            cancelRenderButton->setEnabled(false);
        });
        QObject::connect(showRenderPreviewCheckBox, &QCheckBox::toggled, progressDialog, [renderPreviewWidget](bool checked) {
            renderPreviewWidget->setVisible(checked);
        });

        localControls.dialog = progressDialog;
        localControls.statusLabel = renderStatusLabel;
        localControls.previewWidget = renderPreviewWidget;
        localControls.previewCheckBox = showRenderPreviewCheckBox;
        localControls.sourcesList = renderSourcesList;
        localControls.progressBar = renderProgressBar;
        localControls.cancelled = &localRenderCancelled;
        progressControls = &localControls;
        progressDialog->show();
    }

    QDialog* progressDialog = progressControls->dialog;
    QLabel* renderStatusLabel = progressControls->statusLabel;
    QLabel* renderPreviewLabel = progressControls->previewLabel;
    ExportVulkanPreviewWidget* renderPreviewWidget = progressControls->previewWidget;
    QCheckBox* showRenderPreviewCheckBox = progressControls->previewCheckBox;
    QPlainTextEdit* renderSourcesList = progressControls->sourcesList;
    QProgressBar* renderProgressBar = progressControls->progressBar;
    bool* renderCancelled = progressControls->cancelled ? progressControls->cancelled : &localRenderCancelled;
    if (renderProgressBar) {
        renderProgressBar->setRange(0, static_cast<int>(qMin<int64_t>(totalFramesToRender, std::numeric_limits<int>::max())));
        renderProgressBar->setValue(0);
    }
    if (renderStatusLabel) {
        renderStatusLabel->setText(QStringLiteral("Preparing render..."));
    }
    if (renderSourcesList) {
        renderSourcesList->setPlainText(QStringLiteral("Waiting for first rendered frame..."));
    }
    if (renderPreviewLabel) {
        renderPreviewLabel->setText(QStringLiteral("Waiting for first rendered frame..."));
        renderPreviewLabel->setPixmap(QPixmap());
    }
    if (renderPreviewWidget) {
        renderPreviewWidget->clearPreview();
    }
    if (progressDialog) {
        progressDialog->show();
    }

    const QString outputPath = effectiveRequest.outputPath;
    const auto formatEta = [](qint64 remainingMs) -> QString
    {
        if (remainingMs <= 0)
        {
            return QStringLiteral("calculating...");
        }
        const qint64 totalSeconds = remainingMs / 1000;
        const qint64 hours = totalSeconds / 3600;
        const qint64 minutes = (totalSeconds % 3600) / 60;
        const qint64 seconds = totalSeconds % 60;
        if (hours > 0)
        {
            return QStringLiteral("%1h %2m %3s").arg(hours).arg(minutes).arg(seconds);
        }
        if (minutes > 0)
        {
            return QStringLiteral("%1m %2s").arg(minutes).arg(seconds);
        }
        return QStringLiteral("%1s").arg(seconds);
    };
    const auto stageSummary = [](qint64 stageMs, int64_t completedFrames) -> QString
    {
        if (stageMs <= 0 || completedFrames <= 0)
        {
            return QStringLiteral("0 ms");
        }
        return QStringLiteral("%1 ms total (%2 ms/frame)")
            .arg(stageMs)
            .arg(QString::number(static_cast<double>(stageMs) / static_cast<double>(completedFrames), 'f', 2));
    };
    const auto renderProfileFromProgress = [&formatEta](const RenderProgress &progress) -> QJsonObject
    {
        const qint64 completedFrames = qMax<int64_t>(1, progress.framesCompleted);
        const double fps = progress.elapsedMs > 0
                               ? (1000.0 * static_cast<double>(progress.framesCompleted)) / static_cast<double>(progress.elapsedMs)
                               : 0.0;
        return QJsonObject{
            {QStringLiteral("status"), QStringLiteral("running")},
            {QStringLiteral("output_path"), QString()},
            {QStringLiteral("frames_completed"), static_cast<qint64>(progress.framesCompleted)},
            {QStringLiteral("total_frames"), static_cast<qint64>(progress.totalFrames)},
            {QStringLiteral("segment_index"), progress.segmentIndex},
            {QStringLiteral("segment_count"), progress.segmentCount},
            {QStringLiteral("timeline_frame"), static_cast<qint64>(progress.timelineFrame)},
            {QStringLiteral("segment_start_frame"), static_cast<qint64>(progress.segmentStartFrame)},
            {QStringLiteral("segment_end_frame"), static_cast<qint64>(progress.segmentEndFrame)},
            {QStringLiteral("using_gpu"), progress.usingGpu},
            {QStringLiteral("using_hardware_encode"), progress.usingHardwareEncode},
            {QStringLiteral("encoder_label"), progress.encoderLabel},
            {QStringLiteral("export_pipeline"), progress.exportPipeline},
            {QStringLiteral("gpu_transfer_label"), progress.gpuTransferLabel},
            {QStringLiteral("encoder_pixel_format"), progress.encoderPixelFormat},
            {QStringLiteral("encoder_software_pixel_format"), progress.encoderSoftwarePixelFormat},
            {QStringLiteral("cuda_external_memory_status"), progress.cudaExternalMemoryStatus},
            {QStringLiteral("export_path_fallback_reason"), progress.exportPathFallbackReason},
            {QStringLiteral("cuda_external_transfer"), progress.cudaExternalTransfer},
            {QStringLiteral("cuda_external_memory_supported"), progress.cudaExternalMemorySupported},
            {QStringLiteral("encoder_hardware_frames"), progress.encoderHardwareFrames},
            {QStringLiteral("elapsed_ms"), progress.elapsedMs},
            {QStringLiteral("estimated_remaining_ms"), progress.estimatedRemainingMs},
            {QStringLiteral("eta_text"), formatEta(progress.estimatedRemainingMs)},
            {QStringLiteral("fps"), fps},
            {QStringLiteral("render_stage_ms"), progress.renderStageMs},
            {QStringLiteral("render_decode_stage_ms"), progress.renderDecodeStageMs},
            {QStringLiteral("render_texture_stage_ms"), progress.renderTextureStageMs},
            {QStringLiteral("render_composite_stage_ms"), progress.renderCompositeStageMs},
            {QStringLiteral("render_nv12_stage_ms"), progress.renderNv12StageMs},
            {QStringLiteral("gpu_readback_ms"), progress.gpuReadbackMs},
            {QStringLiteral("overlay_stage_ms"), progress.overlayStageMs},
            {QStringLiteral("convert_stage_ms"), progress.convertStageMs},
            {QStringLiteral("encode_stage_ms"), progress.encodeStageMs},
            {QStringLiteral("audio_stage_ms"), progress.audioStageMs},
            {QStringLiteral("audio_setup_ms"), progress.audioSetupMs},
            {QStringLiteral("max_frame_render_stage_ms"), progress.maxFrameRenderStageMs},
            {QStringLiteral("max_frame_decode_stage_ms"), progress.maxFrameDecodeStageMs},
            {QStringLiteral("max_frame_texture_stage_ms"), progress.maxFrameTextureStageMs},
            {QStringLiteral("max_frame_readback_stage_ms"), progress.maxFrameReadbackStageMs},
            {QStringLiteral("max_frame_convert_stage_ms"), progress.maxFrameConvertStageMs},
            {QStringLiteral("skipped_clips"), progress.skippedClips},
            {QStringLiteral("skipped_clip_reason_counts"), progress.skippedClipReasonCounts},
            {QStringLiteral("render_stage_table"), progress.renderStageTable},
            {QStringLiteral("worst_frame_table"), progress.worstFrameTable},
            {QStringLiteral("export_face_transform"), progress.exportFaceTransformDiagnostics},
            {QStringLiteral("render_stage_per_frame_ms"), static_cast<double>(progress.renderStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_decode_stage_per_frame_ms"), static_cast<double>(progress.renderDecodeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_texture_stage_per_frame_ms"), static_cast<double>(progress.renderTextureStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_composite_stage_per_frame_ms"), static_cast<double>(progress.renderCompositeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_nv12_stage_per_frame_ms"), static_cast<double>(progress.renderNv12StageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("gpu_readback_per_frame_ms"), static_cast<double>(progress.gpuReadbackMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("overlay_stage_per_frame_ms"), static_cast<double>(progress.overlayStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("convert_stage_per_frame_ms"), static_cast<double>(progress.convertStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("encode_stage_per_frame_ms"), static_cast<double>(progress.encodeStageMs) / static_cast<double>(completedFrames)}};
    };
    const auto renderProfileFromResult = [&formatEta, &outputPath](const RenderResult &result) -> QJsonObject
    {
        const qint64 completedFrames = qMax<int64_t>(1, result.framesRendered);
        const double fps = result.elapsedMs > 0
                               ? (1000.0 * static_cast<double>(result.framesRendered)) / static_cast<double>(result.elapsedMs)
                               : 0.0;
        return QJsonObject{
            {QStringLiteral("status"), result.success ? QStringLiteral("completed")
                                                      : (result.cancelled ? QStringLiteral("cancelled")
                                                                          : QStringLiteral("failed"))},
            {QStringLiteral("output_path"), QDir::toNativeSeparators(outputPath)},
            {QStringLiteral("frames_completed"), static_cast<qint64>(result.framesRendered)},
            {QStringLiteral("total_frames"), static_cast<qint64>(result.framesRendered)},
            {QStringLiteral("segment_index"), 0},
            {QStringLiteral("segment_count"), 0},
            {QStringLiteral("segment_start_frame"), static_cast<qint64>(0)},
            {QStringLiteral("segment_end_frame"), static_cast<qint64>(0)},
            {QStringLiteral("using_gpu"), result.usedGpu},
            {QStringLiteral("using_hardware_encode"), result.usedHardwareEncode},
            {QStringLiteral("encoder_label"), result.encoderLabel},
            {QStringLiteral("export_pipeline"), result.exportPipeline},
            {QStringLiteral("gpu_transfer_label"), result.gpuTransferLabel},
            {QStringLiteral("encoder_pixel_format"), result.encoderPixelFormat},
            {QStringLiteral("encoder_software_pixel_format"), result.encoderSoftwarePixelFormat},
            {QStringLiteral("cuda_external_memory_status"), result.cudaExternalMemoryStatus},
            {QStringLiteral("export_path_fallback_reason"), result.exportPathFallbackReason},
            {QStringLiteral("cuda_external_transfer"), result.cudaExternalTransfer},
            {QStringLiteral("cuda_external_memory_supported"), result.cudaExternalMemorySupported},
            {QStringLiteral("encoder_hardware_frames"), result.encoderHardwareFrames},
            {QStringLiteral("elapsed_ms"), result.elapsedMs},
            {QStringLiteral("estimated_remaining_ms"), static_cast<qint64>(0)},
            {QStringLiteral("eta_text"), formatEta(0)},
            {QStringLiteral("fps"), fps},
            {QStringLiteral("render_stage_ms"), result.renderStageMs},
            {QStringLiteral("render_decode_stage_ms"), result.renderDecodeStageMs},
            {QStringLiteral("render_texture_stage_ms"), result.renderTextureStageMs},
            {QStringLiteral("render_composite_stage_ms"), result.renderCompositeStageMs},
            {QStringLiteral("render_nv12_stage_ms"), result.renderNv12StageMs},
            {QStringLiteral("gpu_readback_ms"), result.gpuReadbackMs},
            {QStringLiteral("overlay_stage_ms"), result.overlayStageMs},
            {QStringLiteral("convert_stage_ms"), result.convertStageMs},
            {QStringLiteral("encode_stage_ms"), result.encodeStageMs},
            {QStringLiteral("audio_stage_ms"), result.audioStageMs},
            {QStringLiteral("audio_setup_ms"), result.audioSetupMs},
            {QStringLiteral("max_frame_render_stage_ms"), result.maxFrameRenderStageMs},
            {QStringLiteral("max_frame_decode_stage_ms"), result.maxFrameDecodeStageMs},
            {QStringLiteral("max_frame_texture_stage_ms"), result.maxFrameTextureStageMs},
            {QStringLiteral("max_frame_readback_stage_ms"), result.maxFrameReadbackStageMs},
            {QStringLiteral("max_frame_convert_stage_ms"), result.maxFrameConvertStageMs},
            {QStringLiteral("skipped_clips"), result.skippedClips},
            {QStringLiteral("skipped_clip_reason_counts"), result.skippedClipReasonCounts},
            {QStringLiteral("render_stage_table"), result.renderStageTable},
            {QStringLiteral("worst_frame_table"), result.worstFrameTable},
            {QStringLiteral("export_face_transform"), result.exportFaceTransformDiagnostics},
            {QStringLiteral("render_stage_per_frame_ms"), static_cast<double>(result.renderStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_decode_stage_per_frame_ms"), static_cast<double>(result.renderDecodeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_texture_stage_per_frame_ms"), static_cast<double>(result.renderTextureStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_composite_stage_per_frame_ms"), static_cast<double>(result.renderCompositeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_nv12_stage_per_frame_ms"), static_cast<double>(result.renderNv12StageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("gpu_readback_per_frame_ms"), static_cast<double>(result.gpuReadbackMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("overlay_stage_per_frame_ms"), static_cast<double>(result.overlayStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("convert_stage_per_frame_ms"), static_cast<double>(result.convertStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("encode_stage_per_frame_ms"), static_cast<double>(result.encodeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("message"), result.message}};
    };
    m_renderInProgress = true;
    m_liveRenderProfile = QJsonObject{
        {QStringLiteral("status"), QStringLiteral("starting")},
        {QStringLiteral("output_path"), QDir::toNativeSeparators(outputPath)},
        {QStringLiteral("frames_completed"), static_cast<qint64>(0)},
        {QStringLiteral("total_frames"), static_cast<qint64>(totalFramesToRender)}};
    refreshProfileInspector();

    const auto activeRenderSourcesText = [&effectiveRequest](int64_t timelineFrame) -> QString {
        QStringList lines;
        lines.reserve(effectiveRequest.clips.size() * 2);
        for (const TimelineClip& clip : effectiveRequest.clips) {
            if (timelineFrame < clip.startFrame ||
                timelineFrame >= clip.startFrame + qMax<int64_t>(1, clip.durationFrames)) {
                continue;
            }
            const QString clipLabel = clip.label.isEmpty() ? QStringLiteral("(unnamed clip)") : clip.label;
            if (clipVisualPlaybackEnabled(clip, effectiveRequest.tracks) && !clip.filePath.isEmpty()) {
                lines.push_back(QStringLiteral("V | %1 | %2")
                                    .arg(clipLabel, QDir::toNativeSeparators(clip.filePath)));
            }
            if (clipAudioPlaybackEnabled(clip)) {
                const QString audioPath = playbackAudioPathForClip(clip);
                if (!audioPath.isEmpty()) {
                    lines.push_back(QStringLiteral("A | %1 | %2")
                                        .arg(clipLabel, QDir::toNativeSeparators(audioPath)));
                }
            }
        }
        if (lines.isEmpty()) {
            return QStringLiteral("No active clip sources at this frame.");
        }
        lines.removeDuplicates();
        std::sort(lines.begin(), lines.end());
        return lines.join(QLatin1Char('\n'));
    };

    QElapsedTimer progressUiTimer;
    progressUiTimer.start();
    qint64 lastProgressUiUpdateMs = -1000;
    qint64 lastEventPumpMs = -1000;
    const RenderResult result = renderTimelineToFile(
        effectiveRequest,
        [this, renderStatusLabel, renderProgressBar, renderPreviewLabel, renderPreviewWidget,
         renderSourcesList, showRenderPreviewCheckBox, renderCancelled,
         formatEta, stageSummary, renderProfileFromProgress, outputPath,
         activeRenderSourcesText, &progressUiTimer, &lastProgressUiUpdateMs,
         &lastEventPumpMs](const RenderProgress &progress)
        {
            const qint64 nowMs = progressUiTimer.elapsed();
            const bool cancelled = renderCancelled && *renderCancelled;
            const bool finalProgress =
                progress.totalFrames > 0 && progress.framesCompleted >= progress.totalFrames;
            const bool updateUi =
                cancelled || finalProgress || lastProgressUiUpdateMs < 0 ||
                nowMs - lastProgressUiUpdateMs >= 250;
            const bool pumpEvents =
                cancelled || lastEventPumpMs < 0 || nowMs - lastEventPumpMs >= 50;

            if (!updateUi) {
                if (pumpEvents) {
                    lastEventPumpMs = nowMs;
                    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                }
                return !cancelled;
            }

            lastProgressUiUpdateMs = nowMs;
            if (renderProgressBar) {
                renderProgressBar->setMaximum(qMax(1, static_cast<int>(qMin<int64_t>(progress.totalFrames, std::numeric_limits<int>::max()))));
                renderProgressBar->setValue(static_cast<int>(qMin<int64_t>(progress.framesCompleted, std::numeric_limits<int>::max())));
            }
            const QString rendererMode = progress.usingGpu ? QStringLiteral("GPU render") : QStringLiteral("CPU render");
            const QString encoderMode = progress.usingHardwareEncode
                                            ? QStringLiteral("Hardware encode")
                                            : QStringLiteral("Software encode");
            const QString encoderLabel = progress.encoderLabel.isEmpty()
                                             ? QStringLiteral("unknown")
                                             : progress.encoderLabel;
            const QString gpuTransferMetricLabel = progress.gpuTransferLabel.isEmpty()
                                                       ? QStringLiteral("GPU Transfer")
                                                       : progress.gpuTransferLabel;
            m_liveRenderProfile = renderProfileFromProgress(progress);
            m_liveRenderProfile[QStringLiteral("output_path")] = QDir::toNativeSeparators(outputPath);
            refreshProfileInspector();
            const QString metricsTable = QStringLiteral(
                "<table cellspacing='0' cellpadding='2' style='margin: 0 auto;'>"
                "<tr>"
                "<td align='right'><b>Render</b></td><td>%1</td>"
                "<td align='right'><b>Decode</b></td><td>%2</td>"
                "<td align='right'><b>Texture</b></td><td>%3</td>"
                "</tr>"
                "<tr>"
                "<td align='right'><b>Composite</b></td><td>%4</td>"
                "<td align='right'><b>GPU NV12</b></td><td>%5</td>"
                "<td align='right'><b>%6</b></td><td>%7</td>"
                "</tr>"
                "<tr>"
                "<td align='right'><b>Convert</b></td><td>%8</td>"
                "<td align='right'><b>Encode</b></td><td>%9</td>"
                "<td align='right'><b>Audio setup</b></td><td>%10 ms</td>"
                "</tr>"
                "</table>")
                .arg(stageSummary(progress.renderStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderDecodeStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderTextureStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderCompositeStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderNv12StageMs, progress.framesCompleted))
                .arg(gpuTransferMetricLabel)
                .arg(stageSummary(progress.gpuReadbackMs, progress.framesCompleted))
                .arg(stageSummary(progress.convertStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.encodeStageMs, progress.framesCompleted))
                .arg(progress.audioSetupMs);
            if (renderStatusLabel) {
                renderStatusLabel->setText(
                    QStringLiteral("<b>Rendering frame %1 of %2</b><br>"
                                   "Segment %3/%4: %5-%6<br>"
                                   "%7 | %8 (%9)<br>"
                                   "ETA: %10<br>%11")
                        .arg(progress.framesCompleted + 1)
                        .arg(qMax<int64_t>(1, progress.totalFrames))
                        .arg(progress.segmentIndex)
                        .arg(progress.segmentCount)
                        .arg(progress.segmentStartFrame)
                        .arg(progress.segmentEndFrame)
                        .arg(rendererMode)
                        .arg(encoderMode)
                        .arg(encoderLabel)
                        .arg(formatEta(progress.estimatedRemainingMs))
                        .arg(metricsTable));
            }
            if (showRenderPreviewCheckBox && showRenderPreviewCheckBox->isChecked() &&
                !progress.previewFrame.isNull()) {
                if (renderPreviewWidget) {
                    renderPreviewWidget->setPreviewFrame(progress.previewFrame);
                } else if (renderPreviewLabel) {
                    const QPixmap pixmap = QPixmap::fromImage(progress.previewFrame).scaled(
                        renderPreviewLabel->size(),
                        Qt::KeepAspectRatio,
                        Qt::SmoothTransformation);
                    renderPreviewLabel->setPixmap(pixmap);
                    renderPreviewLabel->setText(QString());
                }
            }
            if (renderSourcesList) {
                renderSourcesList->setPlainText(activeRenderSourcesText(progress.timelineFrame));
            }
            lastEventPumpMs = nowMs;
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            return !cancelled;
        });
    if (renderProgressBar) {
        renderProgressBar->setValue(renderProgressBar->maximum());
    }
    if (progressDialog && progressControls->closeOnFinish) {
        progressDialog->close();
    }
    m_renderInProgress = false;
    m_lastRenderProfile = renderProfileFromResult(result);
    m_liveRenderProfile = QJsonObject{};
    refreshProfileInspector();

    if (result.success)
    {
        if (notifyRenderCompletion) {
            QMessageBox::information(this, QStringLiteral("Render Complete"), result.message);
        }
        return true;
    }

    if (!notifyRenderCompletion) {
        return false;
    }
    const QString message = result.message.isEmpty()
                                ? QStringLiteral("Render failed.")
                                : result.message;
    QMessageBox::warning(this,
                         result.cancelled ? QStringLiteral("Render Cancelled") : QStringLiteral("Render Failed"),
                         message);
    return false;
}

void EditorWindow::exportVideoForSpeakersOnSelectedClip(const QStringList& speakerIds)
{
    if (!m_timeline || speakerIds.isEmpty()) {
        return;
    }
    const TimelineClip* clip = m_timeline->selectedClip();
    if (!clip || clip->durationFrames <= 0) {
        QMessageBox::information(this,
                                 QStringLiteral("Export Video"),
                                 QStringLiteral("Select a clip first."));
        return;
    }

    const QString transcriptPath = activeTranscriptPathForClip(*clip);
    QFile transcriptFile(transcriptPath);
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this,
                             QStringLiteral("Export Video"),
                             QStringLiteral("Transcript not found for the selected clip."));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument transcriptDoc =
        QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
        QMessageBox::warning(this,
                             QStringLiteral("Export Video"),
                             QStringLiteral("Transcript JSON is invalid for the selected clip."));
        return;
    }

    auto appendMergedRange = [](QVector<ExportRangeSegment>& ranges, int64_t startFrame, int64_t endFrame) {
        if (endFrame < startFrame) {
            return;
        }
        if (ranges.isEmpty() || startFrame > ranges.constLast().endFrame + 1) {
            ranges.push_back(ExportRangeSegment{startFrame, endFrame});
            return;
        }
        ranges.last().endFrame = qMax(ranges.last().endFrame, endFrame);
    };

    QSet<QString> selectedSpeakerSet;
    selectedSpeakerSet.reserve(speakerIds.size());
    for (const QString& speakerId : speakerIds) {
        const QString trimmed = speakerId.trimmed();
        if (!trimmed.isEmpty()) {
            selectedSpeakerSet.insert(trimmed);
        }
    }
    if (selectedSpeakerSet.isEmpty()) {
        return;
    }

    QVector<ExportRangeSegment> sourceWordRanges;
    const QJsonArray segments = transcriptDoc.object().value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segmentValue : segments) {
        const QJsonObject segmentObj = segmentValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QStringLiteral("speaker")).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            const QString wordText = wordObj.value(QStringLiteral("word")).toString().trimmed();
            if (wordText.isEmpty()) {
                continue;
            }
            QString wordSpeaker = wordObj.value(QStringLiteral("speaker")).toString().trimmed();
            if (wordSpeaker.isEmpty()) {
                wordSpeaker = segmentSpeaker;
            }
            if (!selectedSpeakerSet.contains(wordSpeaker)) {
                continue;
            }
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
            if (startSeconds < 0.0 || endSeconds < startSeconds) {
                continue;
            }
            const int64_t startFrame =
                qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)));
            const int64_t endFrame =
                qMax<int64_t>(startFrame, static_cast<int64_t>(std::ceil(endSeconds * kTimelineFps)) - 1);
            appendMergedRange(sourceWordRanges, startFrame, endFrame);
        }
    }
    if (sourceWordRanges.isEmpty()) {
        QMessageBox::information(this,
                                 QStringLiteral("Export Video"),
                                 QStringLiteral("No spoken words found for the selected speakers in this clip."));
        return;
    }

    const QVector<RenderSyncMarker> markers = m_timeline->renderSyncMarkers();
    QVector<ExportRangeSegment> timelineRanges;
    timelineRanges.reserve(sourceWordRanges.size());
    int sourceRangeIndex = 0;
    const int64_t clipStartFrame = clip->startFrame;
    const int64_t clipEndFrame = clip->startFrame + clip->durationFrames - 1;
    for (int64_t timelineFrame = clipStartFrame; timelineFrame <= clipEndFrame; ++timelineFrame) {
        const int64_t transcriptFrame = transcriptFrameForClipAtTimelineSample(
            *clip, frameToSamples(timelineFrame), markers);
        while (sourceRangeIndex < sourceWordRanges.size() &&
               sourceWordRanges.at(sourceRangeIndex).endFrame < transcriptFrame) {
            ++sourceRangeIndex;
        }
        if (sourceRangeIndex >= sourceWordRanges.size()) {
            break;
        }
        const ExportRangeSegment& sourceRange = sourceWordRanges.at(sourceRangeIndex);
        if (transcriptFrame < sourceRange.startFrame || transcriptFrame > sourceRange.endFrame) {
            continue;
        }
        appendMergedRange(timelineRanges, timelineFrame, timelineFrame);
    }
    if (timelineRanges.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("Export Video"),
            QStringLiteral("Could not map selected speaker words to timeline frames."));
        return;
    }
    timelineRanges = applySpeechFilterToExportRanges(timelineRanges);
    if (timelineRanges.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("Export Video"),
            QStringLiteral("Speech filter removed all frames from the selected speaker export."));
        return;
    }

    setPlaybackActive(false);

    RenderRequest request = buildRenderRequestFromOutputControls();
    const QString suggestedBase = QStringLiteral("speaker_export_%1")
        .arg(selectedSpeakerSet.size() == 1 ? *selectedSpeakerSet.constBegin()
                                            : QStringLiteral("multi"));
    const QString selectedPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Export Video"),
        QDir::current().filePath(
            QStringLiteral("%1.%2").arg(baseNameWithExportSpeed(suggestedBase, request.playbackSpeed),
                                        request.outputFormat)),
        QStringLiteral("Video Files (*.%1);;All Files (*)").arg(request.outputFormat));
    if (selectedPath.isEmpty()) {
        return;
    }

    request.outputPath = selectedPath;
    m_lastRenderOutputPath = selectedPath;
    scheduleSaveState();

    request.clips = m_timeline->clips();
    request.tracks = m_timeline->tracks();
    request.renderSyncMarkers = markers;
    request.exportRanges = timelineRanges;
    request.exportStartFrame = timelineRanges.constFirst().startFrame;
    request.exportEndFrame = timelineRanges.constLast().endFrame;
    renderTimelineFromOutputRequest(request);
}

void EditorWindow::exportVideoForSpeakerSectionOnSelectedClip(const QString& speakerId,
                                                              int64_t sourceStartFrame,
                                                              int64_t sourceEndFrame,
                                                              const QString& snippet,
                                                              const QString& speakerDisplayName,
                                                              int sectionOrdinal)
{
    if (!m_timeline || speakerId.trimmed().isEmpty()) {
        return;
    }
    const TimelineClip* clip = m_timeline->selectedClip();
    if (!clip || clip->durationFrames <= 0) {
        QMessageBox::information(this,
                                 QStringLiteral("Export Section"),
                                 QStringLiteral("Select a clip first."));
        return;
    }
    if (sourceStartFrame < 0 || sourceEndFrame < sourceStartFrame) {
        QMessageBox::information(this,
                                 QStringLiteral("Export Section"),
                                 QStringLiteral("The selected section does not have a valid time range."));
        return;
    }

    const QVector<RenderSyncMarker> markers = m_timeline->renderSyncMarkers();
    QVector<ExportRangeSegment> timelineRanges =
        timelineRangesForTranscriptSection(*clip, sourceStartFrame, sourceEndFrame, markers);
    if (timelineRanges.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("Export Section"),
            QStringLiteral("Could not map the selected section to timeline frames."));
        return;
    }
    timelineRanges = applySpeechFilterToExportRanges(timelineRanges);
    if (timelineRanges.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("Export Section"),
            QStringLiteral("Speech filter removed all frames from the selected section export."));
        return;
    }

    setPlaybackActive(false);

    RenderRequest request = buildRenderRequestFromOutputControls();
    if (!confirmContiguousSectionExportPreflight(this, &request, 1)) {
        return;
    }
    persistExportRequestDefaults(request);
    QString aiTitle;
    QString aiError;
    bool aiOk = false;
    QJsonObject payload;
    payload[QStringLiteral("task")] = QStringLiteral("name_transcript_section");
    payload[QStringLiteral("instruction")] =
        QStringLiteral("Return only a short title for this video section, ideally two to four words. No punctuation.");
    payload[QStringLiteral("speaker_id")] = speakerId.trimmed();
    payload[QStringLiteral("speaker_name")] = speakerDisplayName.simplified();
    payload[QStringLiteral("section_ordinal")] = sectionOrdinal;
    payload[QStringLiteral("section_text")] = snippet.simplified();
    payload[QStringLiteral("source_start_frame")] = QString::number(sourceStartFrame);
    payload[QStringLiteral("source_end_frame")] = QString::number(sourceEndFrame);

    refreshAiIntegrationState();
    if (m_aiIntegrationEnabled && !m_aiAuthToken.trimmed().isEmpty()) {
        const QJsonObject response = runAiAction(
            QStringLiteral("name_transcript_section"), payload, &aiOk, &aiError);
        if (aiOk) {
            aiTitle = titleFromAiResponse(response);
        }
    } else {
        aiError = m_aiAuthToken.trimmed().isEmpty()
            ? QStringLiteral("AI login required.")
            : m_aiIntegrationStatus;
    }

    const QString fallbackTitle = speakerSectionFallbackTitle(
        speakerDisplayName,
        speakerId,
        sectionOrdinal,
        sourceStartFrame,
        sourceEndFrame);
    const QString title = coupleWordTitle(aiTitle, fallbackTitle);
    const QString suggestedBase = sanitizedExportBaseName(title, fallbackTitle);
    if (!aiOk && !aiError.trimmed().isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("Export Section"),
            QStringLiteral("AI could not name this section, so the speaker/section fallback filename will be used.\n\n%1")
                .arg(aiError));
    }

    const QString outputFormat = request.outputFormat.isEmpty()
        ? QStringLiteral("mp4")
        : request.outputFormat;
    QString defaultDir = QDir::currentPath();
    if (!m_lastRenderOutputPath.trimmed().isEmpty()) {
        const QFileInfo previousInfo(m_lastRenderOutputPath);
        if (previousInfo.dir().exists()) {
            defaultDir = previousInfo.dir().absolutePath();
        }
    }
    const QString selectedPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Export Section"),
        QDir(defaultDir).filePath(
            QStringLiteral("%1.%2").arg(baseNameWithExportSpeed(suggestedBase, request.playbackSpeed),
                                        outputFormat)),
        QStringLiteral("Video Files (*.%1);;All Files (*)").arg(outputFormat));
    if (selectedPath.isEmpty()) {
        return;
    }

    request.outputPath = selectedPath;
    m_lastRenderOutputPath = selectedPath;
    scheduleSaveState();

    request.clips = m_timeline->clips();
    request.tracks = m_timeline->tracks();
    request.renderSyncMarkers = markers;
    request.exportRanges = timelineRanges;
    request.exportStartFrame = timelineRanges.constFirst().startFrame;
    request.exportEndFrame = timelineRanges.constLast().endFrame;
    renderTimelineFromOutputRequest(request);
}

void EditorWindow::exportVideoForSpeakerSectionsOnSelectedClip(const QVector<SpeakerSectionExportItem>& sections)
{
    if (!m_timeline || sections.isEmpty()) {
        return;
    }
    const TimelineClip* clip = m_timeline->selectedClip();
    if (!clip || clip->durationFrames <= 0) {
        QMessageBox::information(this,
                                 QStringLiteral("Export Sections"),
                                 QStringLiteral("Select a clip first."));
        return;
    }

    QVector<SpeakerSectionExportItem> validSections;
    validSections.reserve(sections.size());
    for (const SpeakerSectionExportItem& section : sections) {
        if (!section.speakerId.trimmed().isEmpty() &&
            section.sourceStartFrame >= 0 &&
            section.sourceEndFrame >= section.sourceStartFrame) {
            validSections.push_back(section);
        }
    }
    if (validSections.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("Export Sections"),
            QStringLiteral("No contiguous transcript sections meet the current export filter."));
        return;
    }
    const QVector<SpeakerSectionExportItem> exportSections =
        coalescedAdjacentSpeakerSections(validSections);

    RenderRequest baseRequest = buildRenderRequestFromOutputControls();
    if (!confirmContiguousSectionExportPreflight(this, &baseRequest, exportSections.size())) {
        return;
    }
    persistExportRequestDefaults(baseRequest);
    const QString outputFormat = baseRequest.outputFormat.isEmpty()
        ? QStringLiteral("mp4")
        : baseRequest.outputFormat;
    QString defaultDir = QDir::currentPath();
    if (!m_lastRenderOutputPath.trimmed().isEmpty()) {
        const QFileInfo previousInfo(m_lastRenderOutputPath);
        if (previousInfo.dir().exists()) {
            defaultDir = previousInfo.dir().absolutePath();
        }
    }
    const QString outputDir = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Export Sections"),
        defaultDir);
    if (outputDir.isEmpty()) {
        return;
    }

    setPlaybackActive(false);

    const QVector<RenderSyncMarker> markers = m_timeline->renderSyncMarkers();
    struct BulkSectionExportJob {
        SpeakerSectionExportItem section;
        QVector<ExportRangeSegment> timelineRanges;
        QString title;
        QString outputPath;
        bool skip = false;
        bool alreadyExported = false;
        bool unmapped = false;
        int row = -1;
    };

    QVector<BulkSectionExportJob> jobs;
    jobs.reserve(exportSections.size());
    QSet<QString> reservedPaths;
    for (const SpeakerSectionExportItem& section : std::as_const(exportSections)) {
        const QString title = speakerTrackSectionTitle(section);
        const QString fallbackTitle = speakerSectionFallbackTitle(
            section.speakerDisplayName,
            section.speakerId,
            section.sectionOrdinal,
            section.sourceStartFrame,
            section.sourceEndFrame);
        const QString suggestedBase =
            baseNameWithExportSpeed(sanitizedExportBaseName(title, fallbackTitle),
                                    baseRequest.playbackSpeed);
        const QString outputPath = deterministicExportPath(outputDir, suggestedBase, outputFormat);
        QVector<ExportRangeSegment> timelineRanges =
            timelineRangesForTranscriptSection(
                *clip,
                section.sourceStartFrame,
                section.sourceEndFrame,
                markers);

        BulkSectionExportJob job;
        job.section = section;
        job.timelineRanges = applySpeechFilterToExportRanges(timelineRanges);
        job.title = title;
        job.outputPath = outputPath;
        job.unmapped = job.timelineRanges.isEmpty();
        if (reservedPaths.contains(outputPath) || QFileInfo::exists(outputPath)) {
            job.alreadyExported = true;
            job.skip = true;
        } else if (job.unmapped) {
            job.skip = true;
        }
        reservedPaths.insert(outputPath);
        job.row = jobs.size();
        jobs.push_back(job);
    }

    QDialog bulkDialog(this);
    bulkDialog.setWindowTitle(QStringLiteral("Bulk Render Export"));
    bulkDialog.setWindowModality(Qt::ApplicationModal);
    bulkDialog.resize(1180, 760);
    bulkDialog.setStyleSheet(QStringLiteral(
        "QDialog { background: #f6f3ee; }"
        "QLabel { color: #1f2430; font-size: 13px; }"
        "QTableWidget { background: #ffffff; color: #111827; gridline-color: #c9c2b8; }"
        "QHeaderView::section { background: #e9e1d6; color: #1f2430; padding: 5px; border: 1px solid #c9c2b8; }"
        "QProgressBar { border: 1px solid #c9c2b8; border-radius: 6px; text-align: center; background: #ffffff; min-height: 20px; }"
        "QProgressBar::chunk { background: #2f7d67; border-radius: 5px; }"
        "QPushButton { min-width: 96px; padding: 6px 14px; }"));
    auto* bulkLayout = new QVBoxLayout(&bulkDialog);
    bulkLayout->setContentsMargins(16, 16, 16, 16);
    bulkLayout->setSpacing(10);

    auto* renderPreviewWidget = new ExportVulkanPreviewWidget(&bulkDialog);

    auto* renderStatusLabel = new QLabel(QStringLiteral("Preparing bulk export..."), &bulkDialog);
    renderStatusLabel->setWordWrap(true);
    renderStatusLabel->setAlignment(Qt::AlignCenter);

    auto* showRenderPreviewCheckBox = new QCheckBox(QStringLiteral("Show Visual Preview"), &bulkDialog);
    showRenderPreviewCheckBox->setChecked(true);
    QObject::connect(showRenderPreviewCheckBox, &QCheckBox::toggled, &bulkDialog, [renderPreviewWidget](bool checked) {
        renderPreviewWidget->setVisible(checked);
    });

    auto* renderSourcesLabel = new QLabel(QStringLiteral("Sources In Use (Current Frame)"), &bulkDialog);
    renderSourcesLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    auto* renderSourcesList = new QPlainTextEdit(&bulkDialog);
    renderSourcesList->setReadOnly(true);
    renderSourcesList->setMinimumHeight(120);
    renderSourcesList->setPlainText(QStringLiteral("Waiting for first rendered frame..."));

    auto* jobTable = new QTableWidget(jobs.size(), 4, &bulkDialog);
    jobTable->setHorizontalHeaderLabels({
        QStringLiteral("#"),
        QStringLiteral("Status"),
        QStringLiteral("Video"),
        QStringLiteral("Output")
    });
    jobTable->verticalHeader()->setVisible(false);
    jobTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    jobTable->setSelectionMode(QAbstractItemView::SingleSelection);
    jobTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    jobTable->setAlternatingRowColors(false);
    jobTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    jobTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    jobTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    jobTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    jobTable->setMinimumHeight(220);
    for (const BulkSectionExportJob& job : std::as_const(jobs)) {
        const int row = job.row;
        auto* numberItem = new QTableWidgetItem(QString::number(row + 1));
        auto* statusItem = new QTableWidgetItem;
        auto* titleItem = new QTableWidgetItem(job.title);
        auto* outputItem = new QTableWidgetItem(QDir::toNativeSeparators(job.outputPath));
        numberItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        statusItem->setTextAlignment(Qt::AlignCenter);
        for (QTableWidgetItem* item : {numberItem, statusItem, titleItem, outputItem}) {
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        }
        jobTable->setItem(row, 0, numberItem);
        jobTable->setItem(row, 1, statusItem);
        jobTable->setItem(row, 2, titleItem);
        jobTable->setItem(row, 3, outputItem);
        if (job.alreadyExported) {
            setBulkExportRowStatus(jobTable, row, QStringLiteral("Already exported"), QColor(QStringLiteral("#9be7b0")));
        } else if (job.unmapped) {
            setBulkExportRowStatus(jobTable, row, QStringLiteral("Unmapped"), QColor(QStringLiteral("#fca5a5")));
        } else {
            setBulkExportRowStatus(jobTable, row, QStringLiteral("Remaining"), QColor(QStringLiteral("#fecaca")));
        }
    }

    auto* contentRow = new QHBoxLayout;
    contentRow->setSpacing(12);
    auto* leftColumn = new QVBoxLayout;
    leftColumn->setSpacing(10);
    leftColumn->addWidget(renderStatusLabel);
    leftColumn->addWidget(showRenderPreviewCheckBox, 0, Qt::AlignLeft);
    leftColumn->addWidget(renderSourcesLabel);
    leftColumn->addWidget(renderSourcesList, 1);
    auto* rightColumn = new QVBoxLayout;
    rightColumn->setSpacing(10);
    rightColumn->addWidget(renderPreviewWidget, 1);
    contentRow->addLayout(leftColumn, 3);
    contentRow->addLayout(rightColumn, 2);
    bulkLayout->addLayout(contentRow);
    bulkLayout->addWidget(jobTable, 1);

    auto* renderProgressBar = new QProgressBar(&bulkDialog);
    renderProgressBar->setValue(0);
    bulkLayout->addWidget(renderProgressBar);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &bulkDialog);
    buttonRow->addWidget(cancelButton);
    bulkLayout->addLayout(buttonRow);

    bool bulkCancelled = false;
    QObject::connect(cancelButton, &QPushButton::clicked, &bulkDialog, [&bulkCancelled, cancelButton]() {
        bulkCancelled = true;
        cancelButton->setEnabled(false);
    });

    RenderProgressDialogControls bulkControls;
    bulkControls.dialog = &bulkDialog;
    bulkControls.statusLabel = renderStatusLabel;
    bulkControls.previewWidget = renderPreviewWidget;
    bulkControls.previewCheckBox = showRenderPreviewCheckBox;
    bulkControls.sourcesList = renderSourcesList;
    bulkControls.progressBar = renderProgressBar;
    bulkControls.cancelled = &bulkCancelled;
    bulkControls.closeOnFinish = false;
    bulkDialog.show();
    QCoreApplication::processEvents();

    int exportedCount = 0;
    int skippedCount = 0;
    int existingSkippedCount = 0;
    int failedCount = 0;
    for (BulkSectionExportJob& job : jobs) {
        if (bulkCancelled) {
            break;
        }
        if (job.alreadyExported) {
            ++existingSkippedCount;
            continue;
        }
        if (job.unmapped) {
            ++skippedCount;
            continue;
        }

        setBulkExportRowStatus(jobTable, job.row, QStringLiteral("Exporting"), QColor(QStringLiteral("#fde68a")));
        scrollBulkExportRowIntoView(jobTable, job.row);
        QCoreApplication::processEvents();

        RenderRequest request = baseRequest;
        request.suppressCompletionDialog = true;
        request.outputPath = job.outputPath;
        request.clips = m_timeline->clips();
        request.tracks = m_timeline->tracks();
        request.renderSyncMarkers = markers;
        request.exportRanges = job.timelineRanges;
        request.exportStartFrame = job.timelineRanges.constFirst().startFrame;
        request.exportEndFrame = job.timelineRanges.constLast().endFrame;
        m_lastRenderOutputPath = request.outputPath;
        if (renderTimelineFromOutputRequest(request, false, &bulkControls)) {
            ++exportedCount;
            setBulkExportRowStatus(jobTable, job.row, QStringLiteral("Exported"), QColor(QStringLiteral("#9be7b0")));
        } else {
            if (bulkCancelled) {
                setBulkExportRowStatus(jobTable, job.row, QStringLiteral("Cancelled"), QColor(QStringLiteral("#fca5a5")));
                break;
            }
            ++failedCount;
            setBulkExportRowStatus(jobTable, job.row, QStringLiteral("Failed"), QColor(QStringLiteral("#fca5a5")));
        }
        scrollBulkExportRowIntoView(jobTable, job.row);
        QCoreApplication::processEvents();
    }
    cancelButton->setText(QStringLiteral("Close"));
    cancelButton->setEnabled(true);
    QObject::disconnect(cancelButton, nullptr, &bulkDialog, nullptr);
    QObject::connect(cancelButton, &QPushButton::clicked, &bulkDialog, &QDialog::accept);
    if (bulkCancelled) {
        renderStatusLabel->setText(QStringLiteral("Bulk export cancelled."));
    } else {
        renderStatusLabel->setText(QStringLiteral("Bulk export complete."));
    }

    scheduleSaveState();
    QString summary = QStringLiteral("Exported %1 section video(s).").arg(exportedCount);
    if (skippedCount > 0) {
        summary += QStringLiteral("\nSkipped %1 section(s) that could not be mapped to timeline frames.")
                       .arg(skippedCount);
    }
    if (existingSkippedCount > 0) {
        summary += QStringLiteral("\nSkipped %1 section video(s) because the output file already exists.")
                       .arg(existingSkippedCount);
    }
    if (failedCount > 0) {
        summary += QStringLiteral("\n%1 section video(s) failed to export; see the render profile for details.")
                       .arg(failedCount);
    }
    if (bulkCancelled) {
        summary += QStringLiteral("\nBulk export was cancelled.");
    }
    const int mergedCount = validSections.size() - exportSections.size();
    if (mergedCount > 0) {
        summary += QStringLiteral("\nCombined %1 adjacent same-speaker section(s) after filtering.")
                       .arg(mergedCount);
    }
    QMessageBox::information(this, QStringLiteral("Export Sections"), summary);
}
