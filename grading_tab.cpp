#include "grading_tab.h"
#include "clip_serialization.h"
#include "decoder_context.h"
#include "editor_shared.h"
#include "grading_histogram_widget.h"
#include "keyframe_table_shared.h"

#include <QMenu>
#include <QHeaderView>
#include <QSignalBlocker>
#include <QMessageBox>
#include <QInputDialog>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QColor>
#include <QImage>
#include <QFormLayout>
#include <QSpinBox>
#include <cmath>

namespace {

struct GradeProbeSample {
    int64_t localFrame = 0;
    double lumaMean = 0.0;
    double saturationMean = 0.0;
    double contrastSpread = 0.0;
};

struct OpposeGradeEvent {
    int64_t localFrame = 0;
    double brightnessDelta = 0.0;
    double contrastMul = 1.0;
    double saturationMul = 1.0;
};

bool probeFrameGradeStats(const QImage& image, GradeProbeSample* outSample)
{
    if (!outSample || image.isNull()) {
        return false;
    }
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull() || rgba.width() <= 0 || rgba.height() <= 0) {
        return false;
    }

    const int width = rgba.width();
    const int height = rgba.height();
    const int pixelCount = width * height;
    const int step =
        qMax(1, static_cast<int>(std::sqrt(static_cast<double>(qMax(1, pixelCount / 32000)))));

    double sumLuma = 0.0;
    double sumSat = 0.0;
    double sumLumaSq = 0.0;
    int samples = 0;
    for (int y = 0; y < height; y += step) {
        const uchar* row = rgba.constScanLine(y);
        for (int x = 0; x < width; x += step) {
            const int idx = x * 4;
            const double r = static_cast<double>(row[idx]) / 255.0;
            const double g = static_cast<double>(row[idx + 1]) / 255.0;
            const double b = static_cast<double>(row[idx + 2]) / 255.0;
            const double luma = (0.2126 * r) + (0.7152 * g) + (0.0722 * b);
            const double maxv = qMax(r, qMax(g, b));
            const double minv = qMin(r, qMin(g, b));
            const double sat = maxv - minv;
            sumLuma += luma;
            sumLumaSq += luma * luma;
            sumSat += sat;
            ++samples;
        }
    }
    if (samples <= 0) {
        return false;
    }

    const double invCount = 1.0 / static_cast<double>(samples);
    outSample->lumaMean = sumLuma * invCount;
    outSample->saturationMean = sumSat * invCount;
    const double varLuma = qMax(0.0, (sumLumaSq * invCount) - (outSample->lumaMean * outSample->lumaMean));
    outSample->contrastSpread = std::sqrt(varLuma);
    return true;
}

QVector<OpposeGradeEvent> detectOpposeGradeEvents(const QVector<GradeProbeSample>& samples,
                                                  int minFrameGap,
                                                  int maxEvents,
                                                  double jumpLumaThreshold,
                                                  double jumpSaturationThreshold,
                                                  double jumpContrastThreshold,
                                                  double brightnessStrength)
{
    QVector<OpposeGradeEvent> events;
    if (samples.size() < 2) {
        return events;
    }

    const int gap = qMax(1, minFrameGap);
    int64_t lastEventFrame = -1000000;

    double targetLuma = samples.constFirst().lumaMean;
    double targetSat = samples.constFirst().saturationMean;
    double targetSpread = samples.constFirst().contrastSpread;

    for (int i = 1; i < samples.size(); ++i) {
        const GradeProbeSample& previous = samples.at(i - 1);
        const GradeProbeSample& current = samples.at(i);

        const double jumpLuma = std::abs(current.lumaMean - previous.lumaMean);
        const double jumpSat = std::abs(current.saturationMean - previous.saturationMean);
        const double jumpSpread = std::abs(current.contrastSpread - previous.contrastSpread);
        const bool majorJump = (jumpLuma >= jumpLumaThreshold) ||
                               (jumpSat >= jumpSaturationThreshold) ||
                               (jumpSpread >= jumpContrastThreshold);
        if (!majorJump) {
            targetLuma = (targetLuma * 0.96) + (current.lumaMean * 0.04);
            targetSat = (targetSat * 0.96) + (current.saturationMean * 0.04);
            targetSpread = (targetSpread * 0.96) + (current.contrastSpread * 0.04);
            continue;
        }

        if (current.localFrame - lastEventFrame < gap) {
            continue;
        }

        OpposeGradeEvent event;
        event.localFrame = current.localFrame;
        event.brightnessDelta =
            qBound(-2.0, -(current.lumaMean - targetLuma) * brightnessStrength, 2.0);
        event.saturationMul = qBound(0.45, targetSat / qMax(0.03, current.saturationMean), 2.2);
        event.contrastMul = qBound(0.50, targetSpread / qMax(0.02, current.contrastSpread), 2.0);

        const bool hasMeaningfulAdjustment = std::abs(event.brightnessDelta) >= 0.04 ||
                                             std::abs(event.contrastMul - 1.0) >= 0.06 ||
                                             std::abs(event.saturationMul - 1.0) >= 0.06;
        if (!hasMeaningfulAdjustment) {
            continue;
        }

        events.push_back(event);
        lastEventFrame = current.localFrame;
        if (events.size() >= maxEvents) {
            break;
        }
    }
    return events;
}

bool comboHasAlphaItem(const QComboBox* combo)
{
    return combo && combo->findText(QStringLiteral("Alpha")) >= 0;
}

bool comboAlphaSelected(const QComboBox* combo)
{
    if (!combo) {
        return false;
    }
    const int idx = combo->currentIndex();
    return idx >= 0 && combo->itemText(idx).compare(QStringLiteral("Alpha"), Qt::CaseInsensitive) == 0;
}

void setToneSpinGroupVisible(QDoubleSpinBox* r, QDoubleSpinBox* g, QDoubleSpinBox* b, int channelIndex)
{
    if (r) r->setVisible(channelIndex == 0);
    if (g) g->setVisible(channelIndex == 1);
    if (b) b->setVisible(channelIndex == 2);
}

} // namespace

GradingTab::GradingTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : KeyframeTabBase(deps, parent)
    , m_widgets(widgets)
    , m_gradingDeps(deps)
{
    m_curvePointsR = defaultGradingCurvePoints();
    m_curvePointsG = defaultGradingCurvePoints();
    m_curvePointsB = defaultGradingCurvePoints();
    m_curvePointsLuma = defaultGradingCurvePoints();
    m_deferredSeekTimer.setSingleShot(true);
    connect(&m_deferredSeekTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingSeekTimelineFrame < 0 || !m_deps.seekToTimelineFrame) {
            return;
        }
        m_deps.seekToTimelineFrame(m_pendingSeekTimelineFrame);
        m_pendingSeekTimelineFrame = -1;
    });
}

void GradingTab::wire()
{
    if (m_widgets.brightnessSpin) {
        connect(m_widgets.brightnessSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &GradingTab::onBrightnessChanged);
        connect(m_widgets.brightnessSpin, &QDoubleSpinBox::editingFinished,
                this, &GradingTab::onBrightnessEditingFinished);
    }
    if (m_widgets.contrastSpin) {
        connect(m_widgets.contrastSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &GradingTab::onContrastChanged);
        connect(m_widgets.contrastSpin, &QDoubleSpinBox::editingFinished,
                this, &GradingTab::onContrastEditingFinished);
    }
    if (m_widgets.saturationSpin) {
        connect(m_widgets.saturationSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &GradingTab::onSaturationChanged);
        connect(m_widgets.saturationSpin, &QDoubleSpinBox::editingFinished,
                this, &GradingTab::onSaturationEditingFinished);
    }
    // Shadows/Midtones/Highlights connections
    auto connectToneSpin = [this](QDoubleSpinBox* spin, void (GradingTab::*changedSlot)(double)) {
        if (spin) {
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, changedSlot);
            connect(spin, &QDoubleSpinBox::editingFinished, this, &GradingTab::onBrightnessEditingFinished);
        }
    };
    connectToneSpin(m_widgets.shadowsRSpin, &GradingTab::onShadowsRChanged);
    connectToneSpin(m_widgets.shadowsGSpin, &GradingTab::onShadowsGChanged);
    connectToneSpin(m_widgets.shadowsBSpin, &GradingTab::onShadowsBChanged);
    connectToneSpin(m_widgets.midtonesRSpin, &GradingTab::onMidtonesRChanged);
    connectToneSpin(m_widgets.midtonesGSpin, &GradingTab::onMidtonesGChanged);
    connectToneSpin(m_widgets.midtonesBSpin, &GradingTab::onMidtonesBChanged);
    connectToneSpin(m_widgets.highlightsRSpin, &GradingTab::onHighlightsRChanged);
    connectToneSpin(m_widgets.highlightsGSpin, &GradingTab::onHighlightsGChanged);
    connectToneSpin(m_widgets.highlightsBSpin, &GradingTab::onHighlightsBChanged);
    
    if (m_widgets.gradingAutoScrollCheckBox) {
        connect(m_widgets.gradingAutoScrollCheckBox, &QCheckBox::toggled,
                this, &GradingTab::onAutoScrollToggled);
    }
    if (m_widgets.gradingFollowCurrentCheckBox) {
        connect(m_widgets.gradingFollowCurrentCheckBox, &QCheckBox::toggled,
                this, &GradingTab::onFollowCurrentToggled);
    }
    if (m_widgets.gradingKeyAtPlayheadButton) {
        connect(m_widgets.gradingKeyAtPlayheadButton, &QPushButton::clicked,
                this, &GradingTab::onKeyAtPlayheadClicked);
    }
    if (m_widgets.gradingAutoOpposeButton) {
        connect(m_widgets.gradingAutoOpposeButton, &QPushButton::clicked,
                this, &GradingTab::onAutoOpposeGradeChangesClicked);
    }
    if (m_widgets.gradingCurveChannelCombo) {
        connect(m_widgets.gradingCurveChannelCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &GradingTab::onCurveChannelChanged);
    }
    if (m_widgets.gradingCurveThreePointLockCheckBox) {
        connect(m_widgets.gradingCurveThreePointLockCheckBox, &QCheckBox::toggled,
                this, &GradingTab::onCurveThreePointLockToggled);
    }
    if (m_widgets.gradingCurveSmoothingCheckBox) {
        connect(m_widgets.gradingCurveSmoothingCheckBox, &QCheckBox::toggled,
                this, &GradingTab::onCurveSmoothingToggled);
    }
    if (m_widgets.gradingHistogramWidget) {
        connect(m_widgets.gradingHistogramWidget, &GradingHistogramWidget::curvePointsAdjusted,
                this, &GradingTab::onCurveAdjusted);
    }
    if (m_widgets.gradingKeyframeTable) {
        connect(m_widgets.gradingKeyframeTable, &QTableWidget::itemSelectionChanged,
                this, &GradingTab::onTableSelectionChanged);
        connect(m_widgets.gradingKeyframeTable, &QTableWidget::itemChanged,
                this, &GradingTab::onTableItemChanged);
        connect(m_widgets.gradingKeyframeTable, &QTableWidget::itemClicked,
                this, &GradingTab::onTableItemClicked);
        connect(m_widgets.gradingKeyframeTable, &QTableWidget::itemDoubleClicked,
                this, [this](QTableWidgetItem*) {});
        m_widgets.gradingKeyframeTable->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.gradingKeyframeTable, &QWidget::customContextMenuRequested,
                this, &GradingTab::onTableCustomContextMenu);
        installTableHandlers(m_widgets.gradingKeyframeTable);
    }
}

void GradingTab::refresh()
{
    if (!m_widgets.gradingPathLabel || !m_widgets.brightnessSpin || !m_widgets.contrastSpin ||
        !m_widgets.saturationSpin || !m_widgets.gradingKeyframeTable) {
        return;
    }
    
    // (Removed early return to ensure the UI updates to reflect truth when a row is modified)


    const TimelineClip* clip = m_deps.getSelectedClip();
    m_updating = true;

    QSignalBlocker brightnessBlock(m_widgets.brightnessSpin);
    QSignalBlocker contrastBlock(m_widgets.contrastSpin);
    QSignalBlocker saturationBlock(m_widgets.saturationSpin);
    QSignalBlocker shadowsRBlock(m_widgets.shadowsRSpin);
    QSignalBlocker shadowsGBlock(m_widgets.shadowsGSpin);
    QSignalBlocker shadowsBBlock(m_widgets.shadowsBSpin);
    QSignalBlocker midtonesRBlock(m_widgets.midtonesRSpin);
    QSignalBlocker midtonesGBlock(m_widgets.midtonesGSpin);
    QSignalBlocker midtonesBBlock(m_widgets.midtonesBSpin);
    QSignalBlocker highlightsRBlock(m_widgets.highlightsRSpin);
    QSignalBlocker highlightsGBlock(m_widgets.highlightsGSpin);
    QSignalBlocker highlightsBBlock(m_widgets.highlightsBSpin);
    QSignalBlocker curveLockBlock(m_widgets.gradingCurveThreePointLockCheckBox);
    QSignalBlocker curveSmoothingBlock(m_widgets.gradingCurveSmoothingCheckBox);
    QSignalBlocker tableBlocker(m_widgets.gradingKeyframeTable);

    m_widgets.gradingKeyframeTable->clearContents();
    m_widgets.gradingKeyframeTable->setRowCount(0);

    if (!clip || !m_deps.clipHasVisuals(*clip)) {
        m_widgets.gradingPathLabel->setText(QStringLiteral("No visual clip selected"));
        m_widgets.gradingPathLabel->setToolTip(QString());
        m_widgets.brightnessSpin->setValue(0.0);
        m_widgets.contrastSpin->setValue(1.0);
        m_widgets.saturationSpin->setValue(1.0);
        // Reset shadows/midtones/highlights
        if (m_widgets.shadowsRSpin) m_widgets.shadowsRSpin->setValue(0.0);
        if (m_widgets.shadowsGSpin) m_widgets.shadowsGSpin->setValue(0.0);
        if (m_widgets.shadowsBSpin) m_widgets.shadowsBSpin->setValue(0.0);
        if (m_widgets.midtonesRSpin) m_widgets.midtonesRSpin->setValue(0.0);
        if (m_widgets.midtonesGSpin) m_widgets.midtonesGSpin->setValue(0.0);
        if (m_widgets.midtonesBSpin) m_widgets.midtonesBSpin->setValue(0.0);
        if (m_widgets.highlightsRSpin) m_widgets.highlightsRSpin->setValue(0.0);
        if (m_widgets.highlightsGSpin) m_widgets.highlightsGSpin->setValue(0.0);
        if (m_widgets.highlightsBSpin) m_widgets.highlightsBSpin->setValue(0.0);
        m_curvePointsR = defaultGradingCurvePoints();
        m_curvePointsG = defaultGradingCurvePoints();
        m_curvePointsB = defaultGradingCurvePoints();
        m_curvePointsLuma = defaultGradingCurvePoints();
        m_curveThreePointLock = false;
        m_curveSmoothingEnabled = true;
        if (m_widgets.gradingCurveThreePointLockCheckBox) {
            m_widgets.gradingCurveThreePointLockCheckBox->setChecked(m_curveThreePointLock);
        }
        if (m_widgets.gradingCurveSmoothingCheckBox) {
            m_widgets.gradingCurveSmoothingCheckBox->setChecked(m_curveSmoothingEnabled);
        }
        m_selectedKeyframeFrame = -1;
        m_selectedKeyframeFrames.clear();
        updateHistogramAndCurve(true);
        m_updating = false;
        return;
    }

    const QString nativePath = QDir::toNativeSeparators(m_deps.getClipFilePath(*clip));
    const QString sourceLabel = QStringLiteral("%1 | %2")
                                    .arg(clipMediaTypeLabel(clip->mediaType),
                                         mediaSourceKindLabel(clip->sourceKind));
    m_widgets.gradingPathLabel->setText(QStringLiteral("%1\n%2").arg(clip->label, sourceLabel));
    m_widgets.gradingPathLabel->setToolTip(nativePath);

    populateTable(*clip);

    if (m_selectedKeyframeFrame < 0) {
        const int selectedIndex = clip->gradingKeyframes.isEmpty() ? 0 : nearestKeyframeIndex(*clip);
        m_selectedKeyframeFrame = selectedIndex >= 0 && selectedIndex < clip->gradingKeyframes.size()
                                     ? clip->gradingKeyframes[selectedIndex].frame
                                     : 0;
        m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
    } else if (m_selectedKeyframeFrames.isEmpty()) {
        m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
    }

    m_suppressSyncForTimelineFrame = -1;

    GradingKeyframeDisplay displayed = evaluateDisplayedGrading(*clip, clip->startFrame);
    const int selectedIndex = selectedKeyframeIndex(*clip);
    if (selectedIndex >= 0) {
        const auto& keyframe = clip->gradingKeyframes[selectedIndex];
        displayed.frame = keyframe.frame;
        displayed.brightness = keyframe.brightness;
        displayed.contrast = keyframe.contrast;
        displayed.saturation = keyframe.saturation;
        displayed.shadowsR = keyframe.shadowsR;
        displayed.shadowsG = keyframe.shadowsG;
        displayed.shadowsB = keyframe.shadowsB;
        displayed.midtonesR = keyframe.midtonesR;
        displayed.midtonesG = keyframe.midtonesG;
        displayed.midtonesB = keyframe.midtonesB;
        displayed.highlightsR = keyframe.highlightsR;
        displayed.highlightsG = keyframe.highlightsG;
        displayed.highlightsB = keyframe.highlightsB;
        displayed.curvePointsR = keyframe.curvePointsR;
        displayed.curvePointsG = keyframe.curvePointsG;
        displayed.curvePointsB = keyframe.curvePointsB;
        displayed.curvePointsLuma = keyframe.curvePointsLuma;
        displayed.curveThreePointLock = keyframe.curveThreePointLock;
        displayed.curveSmoothingEnabled = keyframe.curveSmoothingEnabled;
        displayed.linearInterpolation = keyframe.linearInterpolation;
    }
    updateSpinBoxesFromKeyframe(displayed);

    editor::restoreSelectionByFrameRole(m_widgets.gradingKeyframeTable, m_selectedKeyframeFrames);

    updateHistogramAndCurve(true);
    m_updating = false;
    syncTableToPlayhead();
}

void GradingTab::applyGradeFromInspector(bool pushHistory)
{
    if (m_updating) return;

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip || !m_deps.clipHasVisuals(*selectedClip)) return;

    int64_t targetFrame = m_selectedKeyframeFrame;
    if (targetFrame < 0) {
        targetFrame = qBound<int64_t>(0,
                                      m_deps.getCurrentTimelineFrame() - selectedClip->startFrame,
                                      qMax<int64_t>(0, selectedClip->durationFrames - 1));
    }

    const bool updated = m_deps.updateClipById(selectedClip->id, [this, targetFrame](TimelineClip& clip) {
        TimelineClip::GradingKeyframe keyframe;
        keyframe.frame = targetFrame;
        keyframe.brightness = m_widgets.brightnessSpin->value();
        keyframe.contrast = m_widgets.contrastSpin->value();
        keyframe.saturation = m_widgets.saturationSpin->value();
        // Shadows/Midtones/Highlights
        keyframe.shadowsR = m_widgets.shadowsRSpin ? m_widgets.shadowsRSpin->value() : 0.0;
        keyframe.shadowsG = m_widgets.shadowsGSpin ? m_widgets.shadowsGSpin->value() : 0.0;
        keyframe.shadowsB = m_widgets.shadowsBSpin ? m_widgets.shadowsBSpin->value() : 0.0;
        keyframe.midtonesR = m_widgets.midtonesRSpin ? m_widgets.midtonesRSpin->value() : 0.0;
        keyframe.midtonesG = m_widgets.midtonesGSpin ? m_widgets.midtonesGSpin->value() : 0.0;
        keyframe.midtonesB = m_widgets.midtonesBSpin ? m_widgets.midtonesBSpin->value() : 0.0;
        keyframe.highlightsR = m_widgets.highlightsRSpin ? m_widgets.highlightsRSpin->value() : 0.0;
        keyframe.highlightsG = m_widgets.highlightsGSpin ? m_widgets.highlightsGSpin->value() : 0.0;
        keyframe.highlightsB = m_widgets.highlightsBSpin ? m_widgets.highlightsBSpin->value() : 0.0;
        keyframe.curvePointsR = sanitizeGradingCurvePoints(m_curvePointsR);
        keyframe.curvePointsG = sanitizeGradingCurvePoints(m_curvePointsG);
        keyframe.curvePointsB = sanitizeGradingCurvePoints(m_curvePointsB);
        keyframe.curvePointsLuma = sanitizeGradingCurvePoints(m_curvePointsLuma);
        keyframe.curveThreePointLock = m_curveThreePointLock;
        keyframe.curveSmoothingEnabled = m_curveSmoothingEnabled;
        keyframe.linearInterpolation = true;

        bool replaced = false;
        for (TimelineClip::GradingKeyframe& existing : clip.gradingKeyframes) {
            if (existing.frame == targetFrame) {
                keyframe.linearInterpolation = existing.linearInterpolation;
                existing = keyframe;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            clip.gradingKeyframes.push_back(keyframe);
        }
        normalizeClipGradingKeyframes(clip);
    });

    if (!updated) return;

    m_selectedKeyframeFrame = targetFrame;
    m_selectedKeyframeFrames = {targetFrame};
    if (m_deps.setPreviewTimelineClips) {
        m_deps.setPreviewTimelineClips();
    }
    if (pushHistory) {
        m_deps.refreshInspector();
    }
    m_deps.scheduleSaveState();
    if (pushHistory) {
        m_deps.pushHistorySnapshot();
    }
    emit gradeApplied();
}

void GradingTab::upsertKeyframeAtPlayhead()
{
    const TimelineClip* clip = m_deps.getSelectedClip();
    if (!clip || !m_deps.clipHasVisuals(*clip)) return;

    m_selectedKeyframeFrame = qBound<int64_t>(0,
                                              m_deps.getCurrentTimelineFrame() - clip->startFrame,
                                              qMax<int64_t>(0, clip->durationFrames - 1));
    m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
    applyGradeFromInspector(true);
    emit keyframeAdded();
}

void GradingTab::syncTableToPlayhead()
{
    if (shouldSkipSyncToPlayhead(m_widgets.gradingKeyframeTable, m_widgets.gradingFollowCurrentCheckBox)) {
        return;
    }

    const TimelineClip* clip = m_deps.getSelectedClip();
    if (!clip || !m_deps.clipHasVisuals(*clip) || m_widgets.gradingKeyframeTable->rowCount() <= 0) {
        m_widgets.gradingKeyframeTable->clearSelection();
        return;
    }

    const int64_t localFrame = calculateLocalFrame(clip);

    int matchingRow = -1;
    int64_t matchingFrame = -1;
    for (int row = 0; row < m_widgets.gradingKeyframeTable->rowCount(); ++row) {
        QTableWidgetItem* item = m_widgets.gradingKeyframeTable->item(row, 0);
        if (!item) continue;
        const int64_t frame = item->data(Qt::UserRole).toLongLong();
        if (frame <= localFrame && frame >= matchingFrame) {
            matchingFrame = frame;
            matchingRow = row;
        }
    }
    if (matchingRow < 0) {
        matchingRow = 0;
    }

    applySyncedRowSelection(m_widgets.gradingKeyframeTable,
                            matchingRow,
                            m_widgets.gradingAutoScrollCheckBox &&
                                m_widgets.gradingAutoScrollCheckBox->isChecked());
    
    // Also update the spin boxes to match the new selection
    // This is needed because onTableSelectionChanged() might be suppressed
    // when m_syncingTableSelection is true
    QTableWidgetItem* item = m_widgets.gradingKeyframeTable->item(matchingRow, 0);
    if (item) {
        const int64_t primaryFrame = item->data(Qt::UserRole).toLongLong();
        m_selectedKeyframeFrame = primaryFrame;
        m_selectedKeyframeFrames = {primaryFrame};
        
        // Update spin boxes with the selected keyframe's values
        GradingKeyframeDisplay displayed = evaluateDisplayedGrading(*clip, clip->startFrame + primaryFrame);
        for (const TimelineClip::GradingKeyframe& keyframe : clip->gradingKeyframes) {
            if (keyframe.frame == primaryFrame) {
                displayed.frame = keyframe.frame;
                displayed.brightness = keyframe.brightness;
                displayed.contrast = keyframe.contrast;
                displayed.saturation = keyframe.saturation;
                displayed.shadowsR = keyframe.shadowsR;
                displayed.shadowsG = keyframe.shadowsG;
                displayed.shadowsB = keyframe.shadowsB;
                displayed.midtonesR = keyframe.midtonesR;
                displayed.midtonesG = keyframe.midtonesG;
                displayed.midtonesB = keyframe.midtonesB;
                displayed.highlightsR = keyframe.highlightsR;
                displayed.highlightsG = keyframe.highlightsG;
                displayed.highlightsB = keyframe.highlightsB;
                displayed.curvePointsR = keyframe.curvePointsR;
                displayed.curvePointsG = keyframe.curvePointsG;
                displayed.curvePointsB = keyframe.curvePointsB;
                displayed.curvePointsLuma = keyframe.curvePointsLuma;
                displayed.curveThreePointLock = keyframe.curveThreePointLock;
                displayed.curveSmoothingEnabled = keyframe.curveSmoothingEnabled;
                displayed.linearInterpolation = keyframe.linearInterpolation;
                break;
            }
        }
        updateSpinBoxesFromKeyframe(displayed);
    }
    updateHistogramAndCurve();
}

void GradingTab::onAutoScrollToggled(bool checked)
{
    Q_UNUSED(checked);
    if (m_updating) return;
    m_deps.scheduleSaveState();
}

void GradingTab::onFollowCurrentToggled(bool checked)
{
    Q_UNUSED(checked);
    if (m_updating) return;
    if (m_widgets.gradingFollowCurrentCheckBox && m_widgets.gradingFollowCurrentCheckBox->isChecked()) {
        syncTableToPlayhead();
    }
    m_deps.scheduleSaveState();
}

void GradingTab::onKeyAtPlayheadClicked()
{
    upsertKeyframeAtPlayhead();
}

bool GradingTab::configureAutoOpposeSettings(AutoOpposeSettings* settings)
{
    if (!settings) {
        return false;
    }

    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Auto Oppose Grade Changes"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    layout->addLayout(form);

    auto* sampleTargetSpin = new QSpinBox(&dialog);
    sampleTargetSpin->setRange(30, 2000);
    sampleTargetSpin->setValue(settings->sampleTarget);
    sampleTargetSpin->setSuffix(QStringLiteral(" samples"));

    auto* minEventGapSpin = new QSpinBox(&dialog);
    minEventGapSpin->setRange(1, 300);
    minEventGapSpin->setValue(settings->minEventGapFrames);
    minEventGapSpin->setSuffix(QStringLiteral(" frames"));

    auto* maxEventsSpin = new QSpinBox(&dialog);
    maxEventsSpin->setRange(1, 200);
    maxEventsSpin->setValue(settings->maxEvents);

    auto* lumaJumpSpin = new QDoubleSpinBox(&dialog);
    lumaJumpSpin->setRange(0.01, 0.5);
    lumaJumpSpin->setDecimals(3);
    lumaJumpSpin->setSingleStep(0.01);
    lumaJumpSpin->setValue(settings->jumpLumaThreshold);

    auto* satJumpSpin = new QDoubleSpinBox(&dialog);
    satJumpSpin->setRange(0.01, 0.5);
    satJumpSpin->setDecimals(3);
    satJumpSpin->setSingleStep(0.01);
    satJumpSpin->setValue(settings->jumpSaturationThreshold);

    auto* contrastJumpSpin = new QDoubleSpinBox(&dialog);
    contrastJumpSpin->setRange(0.01, 0.5);
    contrastJumpSpin->setDecimals(3);
    contrastJumpSpin->setSingleStep(0.01);
    contrastJumpSpin->setValue(settings->jumpContrastThreshold);

    auto* brightnessStrengthSpin = new QDoubleSpinBox(&dialog);
    brightnessStrengthSpin->setRange(0.5, 6.0);
    brightnessStrengthSpin->setDecimals(2);
    brightnessStrengthSpin->setSingleStep(0.1);
    brightnessStrengthSpin->setValue(settings->brightnessStrength);

    form->addRow(QStringLiteral("Analysis Density"), sampleTargetSpin);
    form->addRow(QStringLiteral("Min Event Gap"), minEventGapSpin);
    form->addRow(QStringLiteral("Max Events"), maxEventsSpin);
    form->addRow(QStringLiteral("Luma Jump Threshold"), lumaJumpSpin);
    form->addRow(QStringLiteral("Saturation Jump Threshold"), satJumpSpin);
    form->addRow(QStringLiteral("Contrast Jump Threshold"), contrastJumpSpin);
    form->addRow(QStringLiteral("Brightness Oppose Strength"), brightnessStrengthSpin);

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    settings->sampleTarget = sampleTargetSpin->value();
    settings->minEventGapFrames = minEventGapSpin->value();
    settings->maxEvents = maxEventsSpin->value();
    settings->jumpLumaThreshold = lumaJumpSpin->value();
    settings->jumpSaturationThreshold = satJumpSpin->value();
    settings->jumpContrastThreshold = contrastJumpSpin->value();
    settings->brightnessStrength = brightnessStrengthSpin->value();
    return true;
}

void GradingTab::onAutoOpposeGradeChangesClicked()
{
    if (m_updating) {
        return;
    }
    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip || !m_deps.clipHasVisuals(*selectedClip)) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Auto Oppose Grade Changes"),
                                 QStringLiteral("Select a visual clip first."));
        return;
    }
    if (selectedClip->mediaType == ClipMediaType::Title) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Auto Oppose Grade Changes"),
                                 QStringLiteral("This tool works on decoded media clips, not generated titles."));
        return;
    }

    AutoOpposeSettings settings = m_autoOpposeSettings;
    if (!configureAutoOpposeSettings(&settings)) {
        return;
    }
    m_autoOpposeSettings = settings;

    const int64_t duration = qMax<int64_t>(1, selectedClip->durationFrames);
    const int targetSamples = qMax(30, settings.sampleTarget);
    const int sampleStep = qMax<int>(1, static_cast<int>(duration / targetSamples));
    const int minEventGap = qMax(1, settings.minEventGapFrames);
    const int maxEvents = qMax(1, settings.maxEvents);

    editor::DecoderContext decoder(m_deps.getClipFilePath(*selectedClip));
    if (!decoder.initialize()) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Auto Oppose Grade Changes"),
                             QStringLiteral("Could not initialize decoder for this clip."));
        return;
    }

    QVector<GradeProbeSample> samples;
    samples.reserve(targetSamples + 4);
    for (int64_t localFrame = 0; localFrame < duration; localFrame += sampleStep) {
        const int64_t timelineFrame = selectedClip->startFrame + localFrame;
        const int64_t sourceFrame =
            sourceFrameForClipAtTimelinePosition(*selectedClip, static_cast<qreal>(timelineFrame), {});
        const editor::FrameHandle frame = decoder.decodeFrame(sourceFrame);
        if (frame.isNull() || !frame.hasCpuImage()) {
            continue;
        }
        GradeProbeSample sample;
        sample.localFrame = localFrame;
        if (!probeFrameGradeStats(frame.cpuImage(), &sample)) {
            continue;
        }
        samples.push_back(sample);
        if (QCoreApplication::instance()) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 2);
        }
    }

    if (samples.size() < 2) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Auto Oppose Grade Changes"),
                                 QStringLiteral("Not enough decodable frames were available for analysis."));
        return;
    }

    const QVector<OpposeGradeEvent> events =
        detectOpposeGradeEvents(samples,
                                minEventGap,
                                maxEvents,
                                settings.jumpLumaThreshold,
                                settings.jumpSaturationThreshold,
                                settings.jumpContrastThreshold,
                                settings.brightnessStrength);
    if (events.isEmpty()) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Auto Oppose Grade Changes"),
                                 QStringLiteral("No major grade-change events were detected."));
        return;
    }

    const bool updated = m_deps.updateClipById(selectedClip->id, [events](TimelineClip& clip) {
        auto upsertKeyframe = [](QVector<TimelineClip::GradingKeyframe>& keyframes,
                                 const TimelineClip::GradingKeyframe& incoming) {
            for (TimelineClip::GradingKeyframe& existing : keyframes) {
                if (existing.frame == incoming.frame) {
                    existing = incoming;
                    return;
                }
            }
            keyframes.push_back(incoming);
        };

        for (const OpposeGradeEvent& event : events) {
            const int64_t timelineFrame = clip.startFrame + event.localFrame;
            TimelineClip::GradingKeyframe key = evaluateClipGradingAtFrame(clip, timelineFrame);
            key.frame = qBound<int64_t>(0, event.localFrame, qMax<int64_t>(0, clip.durationFrames - 1));
            key.brightness = qBound<qreal>(-10.0, key.brightness + event.brightnessDelta, 10.0);
            key.contrast = qBound<qreal>(0.05, key.contrast * event.contrastMul, 10.0);
            key.saturation = qBound<qreal>(0.0, key.saturation * event.saturationMul, 10.0);
            upsertKeyframe(clip.gradingKeyframes, key);
        }
        normalizeClipGradingKeyframes(clip);
    });

    if (!updated) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Auto Oppose Grade Changes"),
                             QStringLiteral("Failed to apply generated grading keyframes."));
        return;
    }

    m_selectedKeyframeFrame = events.constFirst().localFrame;
    m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
    if (m_deps.setPreviewTimelineClips) {
        m_deps.setPreviewTimelineClips();
    }
    refresh();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();

    QMessageBox::information(
        nullptr,
        QStringLiteral("Auto Oppose Grade Changes"),
        QStringLiteral("Detected %1 major grade-change events and generated opposing keyframes.")
            .arg(events.size()));
}

void GradingTab::onTableSelectionChanged()
{
    if (m_updating || m_syncingTableSelection) return;

    // Use base class method for deferred seek to timeline frame
    onTableSelectionChangedBase(m_widgets.gradingKeyframeTable, &m_deferredSeekTimer, &m_pendingSeekTimelineFrame);
    // Note: Don't call refresh() here - it disrupts multi-selection. 
    // The UI update happens through other refresh mechanisms.

    const QSet<int64_t> selectedFrames =
        editor::collectSelectedFrameRoles(m_widgets.gradingKeyframeTable);
    const int64_t primaryFrame =
        editor::primarySelectedFrameRole(m_widgets.gradingKeyframeTable);

    if (primaryFrame < 0) {
        m_selectedKeyframeFrame = -1;
        m_selectedKeyframeFrames.clear();
        return;
    }

    m_selectedKeyframeFrame = primaryFrame;
    m_selectedKeyframeFrames = selectedFrames;
    
    // Suppress auto-sync for this timeline frame to prevent the table from
    // jumping immediately after user clicks a row (which seeks to that frame).
    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (selectedClip) {
        m_suppressSyncForTimelineFrame = selectedClip->startFrame + primaryFrame;
    }

    const TimelineClip* clip = m_deps.getSelectedClip();
    if (clip && m_deps.clipHasVisuals(*clip)) {
        GradingKeyframeDisplay displayed = evaluateDisplayedGrading(*clip, clip->startFrame + primaryFrame);
        for (const TimelineClip::GradingKeyframe& keyframe : clip->gradingKeyframes) {
            if (keyframe.frame == primaryFrame) {
                displayed.frame = keyframe.frame;
                displayed.brightness = keyframe.brightness;
                displayed.contrast = keyframe.contrast;
                displayed.saturation = keyframe.saturation;
                displayed.shadowsR = keyframe.shadowsR;
                displayed.shadowsG = keyframe.shadowsG;
                displayed.shadowsB = keyframe.shadowsB;
                displayed.midtonesR = keyframe.midtonesR;
                displayed.midtonesG = keyframe.midtonesG;
                displayed.midtonesB = keyframe.midtonesB;
                displayed.highlightsR = keyframe.highlightsR;
                displayed.highlightsG = keyframe.highlightsG;
                displayed.highlightsB = keyframe.highlightsB;
                displayed.curvePointsR = keyframe.curvePointsR;
                displayed.curvePointsG = keyframe.curvePointsG;
                displayed.curvePointsB = keyframe.curvePointsB;
                displayed.curvePointsLuma = keyframe.curvePointsLuma;
                displayed.curveThreePointLock = keyframe.curveThreePointLock;
                displayed.curveSmoothingEnabled = keyframe.curveSmoothingEnabled;
                displayed.linearInterpolation = keyframe.linearInterpolation;
                break;
            }
        }
        updateSpinBoxesFromKeyframe(displayed);
    }
    updateHistogramAndCurve();

    if (m_deps.onKeyframeSelectionChanged) {
        m_deps.onKeyframeSelectionChanged();
    }
}

void GradingTab::onTableItemChanged(QTableWidgetItem* changedItem)
{
    if (m_updating || !changedItem) {
        if (m_deps.onKeyframeItemChanged && changedItem) {
            m_deps.onKeyframeItemChanged(changedItem);
        }
        return;
    }

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip || !m_deps.clipHasVisuals(*selectedClip)) return;

    const int row = changedItem->row();
    if (row < 0 || row >= m_widgets.gradingKeyframeTable->rowCount()) return;

    auto tableText = [this, row](int column) -> QString {
        QTableWidgetItem* item = m_widgets.gradingKeyframeTable->item(row, column);
        return item ? item->text().trimmed() : QString();
    };

    bool ok = false;
    TimelineClip::GradingKeyframe edited;
    edited.frame = tableText(0).toLongLong(&ok);
    if (!ok) { refresh(); return; }
    edited.brightness = tableText(1).toDouble(&ok);
    if (!ok) { refresh(); return; }
    edited.contrast = tableText(2).toDouble(&ok);
    if (!ok) { refresh(); return; }
    edited.saturation = tableText(3).toDouble(&ok);
    if (!ok) { refresh(); return; }
    if (!parseVideoInterpolationText(tableText(4), &edited.linearInterpolation)) {
        refresh();
        return;
    }
    edited.shadowsR = 0.0;
    edited.shadowsG = 0.0;
    edited.shadowsB = 0.0;
    edited.midtonesR = 0.0;
    edited.midtonesG = 0.0;
    edited.midtonesB = 0.0;
    edited.highlightsR = 0.0;
    edited.highlightsG = 0.0;
    edited.highlightsB = 0.0;
    edited.curvePointsR = defaultGradingCurvePoints();
    edited.curvePointsG = defaultGradingCurvePoints();
    edited.curvePointsB = defaultGradingCurvePoints();
    edited.curvePointsLuma = defaultGradingCurvePoints();
    edited.curveThreePointLock = false;
    edited.curveSmoothingEnabled = true;

    edited.frame = qBound<int64_t>(0, edited.frame, qMax<int64_t>(0, selectedClip->durationFrames - 1));
    const int64_t originalFrame = changedItem->data(Qt::UserRole).toLongLong();

    const bool updated = m_deps.updateClipById(selectedClip->id, [edited, originalFrame](TimelineClip& clip) mutable {
        TimelineClip::GradingKeyframe originalKeyframe;
        bool foundOriginal = false;
        for (const TimelineClip::GradingKeyframe& existing : clip.gradingKeyframes) {
            if (existing.frame == originalFrame) {
                originalKeyframe = existing;
                foundOriginal = true;
                break;
            }
        }

        if (foundOriginal) {
            edited.shadowsR = originalKeyframe.shadowsR;
            edited.shadowsG = originalKeyframe.shadowsG;
            edited.shadowsB = originalKeyframe.shadowsB;
            edited.midtonesR = originalKeyframe.midtonesR;
            edited.midtonesG = originalKeyframe.midtonesG;
            edited.midtonesB = originalKeyframe.midtonesB;
            edited.highlightsR = originalKeyframe.highlightsR;
            edited.highlightsG = originalKeyframe.highlightsG;
            edited.highlightsB = originalKeyframe.highlightsB;
            edited.curvePointsR = originalKeyframe.curvePointsR;
            edited.curvePointsG = originalKeyframe.curvePointsG;
            edited.curvePointsB = originalKeyframe.curvePointsB;
            edited.curvePointsLuma = originalKeyframe.curvePointsLuma;
            edited.curveThreePointLock = originalKeyframe.curveThreePointLock;
            edited.curveSmoothingEnabled = originalKeyframe.curveSmoothingEnabled;
        }

        bool replaced = false;
        for (TimelineClip::GradingKeyframe& existing : clip.gradingKeyframes) {
            if (existing.frame == originalFrame) {
                existing = edited;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            clip.gradingKeyframes.push_back(edited);
        }

        // Grading always maintains a frame-0 base state. If the edited keyframe
        // was the frame-0 key and the user moves it later in time, preserve the
        // original grade as the new base key instead of silently snapping back.
        if (foundOriginal && originalFrame == 0 && edited.frame > 0) {
            bool hasBaseAtZero = false;
            for (const TimelineClip::GradingKeyframe& existing : clip.gradingKeyframes) {
                if (existing.frame == 0 && !(existing.frame == edited.frame &&
                                             existing.brightness == edited.brightness &&
                                             existing.contrast == edited.contrast &&
                                             existing.saturation == edited.saturation &&
                                             existing.linearInterpolation == edited.linearInterpolation)) {
                    hasBaseAtZero = true;
                    break;
                }
            }
            if (!hasBaseAtZero) {
                originalKeyframe.frame = 0;
                clip.gradingKeyframes.push_back(originalKeyframe);
            }
        }

        normalizeClipGradingKeyframes(clip);
    });

    if (!updated) {
        refresh();
        return;
    }

    m_selectedKeyframeFrame = edited.frame;
    m_selectedKeyframeFrames = {edited.frame};
    if (m_deps.setPreviewTimelineClips) {
        m_deps.setPreviewTimelineClips();
    }
    if (m_deps.onKeyframeItemChanged) {
        m_deps.onKeyframeItemChanged(changedItem);
    }
    refresh();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
}

void GradingTab::onTableItemClicked(QTableWidgetItem* item)
{
    if (m_updating || !item) return;
    if (item->column() != 4) return;
    item->setText(nextVideoInterpolationLabel(item->text()));
}

QString GradingTab::videoInterpolationLabel(bool linearInterpolation) const
{
    return linearInterpolation ? QStringLiteral("Linear") : QStringLiteral("Step");
}

QString GradingTab::nextVideoInterpolationLabel(const QString& text) const
{
    bool linearInterpolation = true;
    if (!parseVideoInterpolationText(text, &linearInterpolation)) {
        linearInterpolation = true;
    }
    return videoInterpolationLabel(!linearInterpolation);
}

bool GradingTab::parseVideoInterpolationText(const QString& text, bool* linearInterpolationOut) const
{
    const QString normalized = text.trimmed().toLower();
    if (normalized.isEmpty() || normalized == QStringLiteral("step")) {
        *linearInterpolationOut = false;
        return true;
    }
    if (normalized == QStringLiteral("linear") || normalized == QStringLiteral("smooth")) {
        *linearInterpolationOut = true;
        return true;
    }
    return false;
}

int GradingTab::selectedKeyframeIndex(const TimelineClip& clip) const
{
    for (int i = 0; i < clip.gradingKeyframes.size(); ++i) {
        if (clip.gradingKeyframes[i].frame == m_selectedKeyframeFrame) {
            return i;
        }
    }
    return -1;
}

QList<int64_t> GradingTab::selectedKeyframeFramesForClip(const TimelineClip& clip) const
{
    QList<int64_t> frames;
    for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
        if (m_selectedKeyframeFrames.contains(keyframe.frame)) {
            frames.push_back(keyframe.frame);
        }
    }
    return frames;
}

int GradingTab::nearestKeyframeIndex(const TimelineClip& clip) const
{
    if (!m_deps.getSelectedClip() || clip.gradingKeyframes.isEmpty()) {
        return -1;
    }
    const int64_t localFrame = qBound<int64_t>(0,
                                               m_deps.getCurrentTimelineFrame() - clip.startFrame,
                                               qMax<int64_t>(0, clip.durationFrames - 1));
    int nearestIndex = 0;
    int64_t nearestDistance = std::numeric_limits<int64_t>::max();
    for (int i = 0; i < clip.gradingKeyframes.size(); ++i) {
        const int64_t distance = std::abs(clip.gradingKeyframes[i].frame - localFrame);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearestIndex = i;
        }
    }
    return nearestIndex;
}

bool GradingTab::hasRemovableKeyframeSelection(const TimelineClip& clip) const
{
    for (const int64_t frame : selectedKeyframeFramesForClip(clip)) {
        if (frame > 0) {
            return true;
        }
    }
    return false;
}

GradingTab::GradingKeyframeDisplay GradingTab::evaluateDisplayedGrading(const TimelineClip& clip, int64_t localFrame) const
{
    GradingKeyframeDisplay result;
    result.brightness = 0.0;
    result.contrast = 1.0;
    result.saturation = 1.0;
    result.shadowsR = 0.0; result.shadowsG = 0.0; result.shadowsB = 0.0;
    result.midtonesR = 0.0; result.midtonesG = 0.0; result.midtonesB = 0.0;
    result.highlightsR = 0.0; result.highlightsG = 0.0; result.highlightsB = 0.0;
    result.curvePointsR = defaultGradingCurvePoints();
    result.curvePointsG = defaultGradingCurvePoints();
    result.curvePointsB = defaultGradingCurvePoints();
    result.curvePointsLuma = defaultGradingCurvePoints();
    result.curveThreePointLock = false;
    result.curveSmoothingEnabled = true;
    result.linearInterpolation = true;

    if (clip.gradingKeyframes.isEmpty()) {
        return result;
    }

    // Find the keyframe at or before localFrame
    int beforeIndex = -1;
    for (int i = clip.gradingKeyframes.size() - 1; i >= 0; --i) {
        if (clip.gradingKeyframes[i].frame <= localFrame) {
            beforeIndex = i;
            break;
        }
    }

    if (beforeIndex < 0) {
        // Use first keyframe
        const auto& kf = clip.gradingKeyframes[0];
        result.brightness = kf.brightness;
        result.contrast = kf.contrast;
        result.saturation = kf.saturation;
        result.shadowsR = kf.shadowsR; result.shadowsG = kf.shadowsG; result.shadowsB = kf.shadowsB;
        result.midtonesR = kf.midtonesR; result.midtonesG = kf.midtonesG; result.midtonesB = kf.midtonesB;
        result.highlightsR = kf.highlightsR; result.highlightsG = kf.highlightsG; result.highlightsB = kf.highlightsB;
        result.curvePointsR = kf.curvePointsR;
        result.curvePointsG = kf.curvePointsG;
        result.curvePointsB = kf.curvePointsB;
        result.curvePointsLuma = kf.curvePointsLuma;
        result.curveThreePointLock = kf.curveThreePointLock;
        result.curveSmoothingEnabled = kf.curveSmoothingEnabled;
        result.linearInterpolation = kf.linearInterpolation;
        return result;
    }

    const auto& before = clip.gradingKeyframes[beforeIndex];
    
    // If this is the last keyframe or we're exactly at it
    if (beforeIndex == clip.gradingKeyframes.size() - 1 || before.frame == localFrame) {
        result.brightness = before.brightness;
        result.contrast = before.contrast;
        result.saturation = before.saturation;
        result.shadowsR = before.shadowsR; result.shadowsG = before.shadowsG; result.shadowsB = before.shadowsB;
        result.midtonesR = before.midtonesR; result.midtonesG = before.midtonesG; result.midtonesB = before.midtonesB;
        result.highlightsR = before.highlightsR; result.highlightsG = before.highlightsG; result.highlightsB = before.highlightsB;
        result.curvePointsR = before.curvePointsR;
        result.curvePointsG = before.curvePointsG;
        result.curvePointsB = before.curvePointsB;
        result.curvePointsLuma = before.curvePointsLuma;
        result.curveThreePointLock = before.curveThreePointLock;
        result.curveSmoothingEnabled = before.curveSmoothingEnabled;
        result.linearInterpolation = before.linearInterpolation;
        return result;
    }

    // Find the next keyframe
    int afterIndex = beforeIndex + 1;
    const auto& after = clip.gradingKeyframes[afterIndex];

    // If not interpolating, just use the before keyframe
    if (!before.linearInterpolation) {
        result.brightness = before.brightness;
        result.contrast = before.contrast;
        result.saturation = before.saturation;
        result.shadowsR = before.shadowsR; result.shadowsG = before.shadowsG; result.shadowsB = before.shadowsB;
        result.midtonesR = before.midtonesR; result.midtonesG = before.midtonesG; result.midtonesB = before.midtonesB;
        result.highlightsR = before.highlightsR; result.highlightsG = before.highlightsG; result.highlightsB = before.highlightsB;
        result.curvePointsR = before.curvePointsR;
        result.curvePointsG = before.curvePointsG;
        result.curvePointsB = before.curvePointsB;
        result.curvePointsLuma = before.curvePointsLuma;
        result.curveThreePointLock = before.curveThreePointLock;
        result.curveSmoothingEnabled = before.curveSmoothingEnabled;
        result.linearInterpolation = before.linearInterpolation;
        return result;
    }

    // Interpolate
    const int64_t range = after.frame - before.frame;
    if (range <= 0) {
        result.brightness = before.brightness;
        result.contrast = before.contrast;
        result.saturation = before.saturation;
        result.shadowsR = before.shadowsR; result.shadowsG = before.shadowsG; result.shadowsB = before.shadowsB;
        result.midtonesR = before.midtonesR; result.midtonesG = before.midtonesG; result.midtonesB = before.midtonesB;
        result.highlightsR = before.highlightsR; result.highlightsG = before.highlightsG; result.highlightsB = before.highlightsB;
        result.curvePointsR = before.curvePointsR;
        result.curvePointsG = before.curvePointsG;
        result.curvePointsB = before.curvePointsB;
        result.curvePointsLuma = before.curvePointsLuma;
        result.curveThreePointLock = before.curveThreePointLock;
        result.curveSmoothingEnabled = before.curveSmoothingEnabled;
        return result;
    }

    const double t = static_cast<double>(localFrame - before.frame) / static_cast<double>(range);
    result.brightness = before.brightness + (after.brightness - before.brightness) * t;
    result.contrast = before.contrast + (after.contrast - before.contrast) * t;
    result.saturation = before.saturation + (after.saturation - before.saturation) * t;
    result.shadowsR = before.shadowsR + (after.shadowsR - before.shadowsR) * t;
    result.shadowsG = before.shadowsG + (after.shadowsG - before.shadowsG) * t;
    result.shadowsB = before.shadowsB + (after.shadowsB - before.shadowsB) * t;
    result.midtonesR = before.midtonesR + (after.midtonesR - before.midtonesR) * t;
    result.midtonesG = before.midtonesG + (after.midtonesG - before.midtonesG) * t;
    result.midtonesB = before.midtonesB + (after.midtonesB - before.midtonesB) * t;
    result.highlightsR = before.highlightsR + (after.highlightsR - before.highlightsR) * t;
    result.highlightsG = before.highlightsG + (after.highlightsG - before.highlightsG) * t;
    result.highlightsB = before.highlightsB + (after.highlightsB - before.highlightsB) * t;
    // Keep curve data keyframe-agnostic for now (hold previous curve between keys).
    result.curvePointsR = before.curvePointsR;
    result.curvePointsG = before.curvePointsG;
    result.curvePointsB = before.curvePointsB;
    result.curvePointsLuma = before.curvePointsLuma;
    result.curveThreePointLock = before.curveThreePointLock;
    result.curveSmoothingEnabled = before.curveSmoothingEnabled;
    result.linearInterpolation = after.linearInterpolation;

    return result;
}

void GradingTab::updateSpinBoxesFromKeyframe(const GradingKeyframeDisplay& keyframe)
{
    QSignalBlocker brightnessBlock(m_widgets.brightnessSpin);
    QSignalBlocker contrastBlock(m_widgets.contrastSpin);
    QSignalBlocker saturationBlock(m_widgets.saturationSpin);
    QSignalBlocker shadowsRBlock(m_widgets.shadowsRSpin);
    QSignalBlocker shadowsGBlock(m_widgets.shadowsGSpin);
    QSignalBlocker shadowsBBlock(m_widgets.shadowsBSpin);
    QSignalBlocker midtonesRBlock(m_widgets.midtonesRSpin);
    QSignalBlocker midtonesGBlock(m_widgets.midtonesGSpin);
    QSignalBlocker midtonesBBlock(m_widgets.midtonesBSpin);
    QSignalBlocker highlightsRBlock(m_widgets.highlightsRSpin);
    QSignalBlocker highlightsGBlock(m_widgets.highlightsGSpin);
    QSignalBlocker highlightsBBlock(m_widgets.highlightsBSpin);

    m_widgets.brightnessSpin->setValue(keyframe.brightness);
    m_widgets.contrastSpin->setValue(keyframe.contrast);
    m_widgets.saturationSpin->setValue(keyframe.saturation);
    
    if (m_widgets.shadowsRSpin) m_widgets.shadowsRSpin->setValue(keyframe.shadowsR);
    if (m_widgets.shadowsGSpin) m_widgets.shadowsGSpin->setValue(keyframe.shadowsG);
    if (m_widgets.shadowsBSpin) m_widgets.shadowsBSpin->setValue(keyframe.shadowsB);
    if (m_widgets.midtonesRSpin) m_widgets.midtonesRSpin->setValue(keyframe.midtonesR);
    if (m_widgets.midtonesGSpin) m_widgets.midtonesGSpin->setValue(keyframe.midtonesG);
    if (m_widgets.midtonesBSpin) m_widgets.midtonesBSpin->setValue(keyframe.midtonesB);
    if (m_widgets.highlightsRSpin) m_widgets.highlightsRSpin->setValue(keyframe.highlightsR);
    if (m_widgets.highlightsGSpin) m_widgets.highlightsGSpin->setValue(keyframe.highlightsG);
    if (m_widgets.highlightsBSpin) m_widgets.highlightsBSpin->setValue(keyframe.highlightsB);
    m_curvePointsR = sanitizeGradingCurvePoints(keyframe.curvePointsR);
    m_curvePointsG = sanitizeGradingCurvePoints(keyframe.curvePointsG);
    m_curvePointsB = sanitizeGradingCurvePoints(keyframe.curvePointsB);
    m_curvePointsLuma = sanitizeGradingCurvePoints(keyframe.curvePointsLuma);
    m_curveThreePointLock = keyframe.curveThreePointLock;
    m_curveSmoothingEnabled = keyframe.curveSmoothingEnabled;
    if (m_widgets.gradingCurveThreePointLockCheckBox) {
        QSignalBlocker lockBlock(m_widgets.gradingCurveThreePointLockCheckBox);
        m_widgets.gradingCurveThreePointLockCheckBox->setChecked(m_curveThreePointLock);
    }
    if (m_widgets.gradingCurveSmoothingCheckBox) {
        QSignalBlocker smoothBlock(m_widgets.gradingCurveSmoothingCheckBox);
        m_widgets.gradingCurveSmoothingCheckBox->setChecked(m_curveSmoothingEnabled);
    }
}

void GradingTab::populateTable(const TimelineClip& clip)
{
    QList<int64_t> frames;
    if (clip.gradingKeyframes.isEmpty()) {
        frames.push_back(0);
    } else {
        frames.reserve(clip.gradingKeyframes.size());
        for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
            frames.push_back(keyframe.frame);
        }
        std::sort(frames.begin(), frames.end());
    }

    m_widgets.gradingKeyframeTable->setRowCount(frames.size());
    
    for (int row = 0; row < frames.size(); ++row) {
        const int64_t frame = frames[row];
        TimelineClip::GradingKeyframe displayedFrame;
        
        if (clip.gradingKeyframes.isEmpty()) {
            displayedFrame = evaluateClipGradingAtFrame(clip, clip.startFrame);
            displayedFrame.frame = 0;
        } else {
            for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
                if (keyframe.frame == frame) {
                    displayedFrame = keyframe;
                    break;
                }
            }
        }

        const QStringList rowValues = {
            QString::number(frame),
            QString::number(displayedFrame.brightness, 'f', 3),
            QString::number(displayedFrame.contrast, 'f', 3),
            QString::number(displayedFrame.saturation, 'f', 3),
            videoInterpolationLabel(displayedFrame.linearInterpolation)
        };

        for (int column = 0; column < rowValues.size(); ++column) {
            auto* item = new QTableWidgetItem(rowValues[column]);
            item->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(frame)));
            m_widgets.gradingKeyframeTable->setItem(row, column, item);
        }
    }
}

void GradingTab::removeSelectedKeyframes()
{
    if (m_selectedKeyframeFrames.isEmpty()) return;

    const QString clipId = m_deps.getSelectedClipId();
    if (clipId.isEmpty()) return;

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip) return;

    QList<int64_t> selectedFrames = selectedKeyframeFramesFromTable(m_widgets.gradingKeyframeTable);
    selectedFrames.erase(std::remove_if(selectedFrames.begin(),
                                        selectedFrames.end(),
                                        [](int64_t frame) { return frame <= 0; }),
                         selectedFrames.end());

    if (selectedFrames.isEmpty()) {
        refresh();
        return;
    }

    const bool updated = m_deps.updateClipById(clipId, [selectedFrames](TimelineClip& clip) {
        clip.gradingKeyframes.erase(
            std::remove_if(clip.gradingKeyframes.begin(),
                           clip.gradingKeyframes.end(),
                           [&selectedFrames](const TimelineClip::GradingKeyframe& keyframe) {
                               return selectedFrames.contains(keyframe.frame);
                           }),
            clip.gradingKeyframes.end());
        normalizeClipGradingKeyframes(clip);
    });

    if (!updated) return;

    m_selectedKeyframeFrame = 0;
    m_selectedKeyframeFrames = {0};
    m_deps.setPreviewTimelineClips();
    refresh();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
    emit keyframesRemoved();
}

void GradingTab::onBrightnessEditingFinished()
{
    if (m_updating) return;
    applyGradeFromInspector(true);
}

void GradingTab::onContrastEditingFinished()
{
    if (m_updating) return;
    applyGradeFromInspector(true);
}

void GradingTab::onSaturationEditingFinished()
{
    if (m_updating) return;
    applyGradeFromInspector(true);
}

void GradingTab::onTableCustomContextMenu(const QPoint& pos)
{
    if (!m_widgets.gradingKeyframeTable) return;

    int row = -1;
    QTableWidgetItem* item =
        editor::ensureContextRowSelected(m_widgets.gradingKeyframeTable, pos, &row);
    if (!item) return;

    const int64_t anchorFrame = item->data(Qt::UserRole).toLongLong();
    const int64_t previousFrame = editor::rowFrameRole(m_widgets.gradingKeyframeTable, row - 1);
    const int64_t nextFrame = editor::rowFrameRole(m_widgets.gradingKeyframeTable, row + 1);
    
    QMenu menu;
    ContextMenuActions actions = buildStandardContextMenu(menu, m_widgets.gradingKeyframeTable, row, m_deps.getSelectedClip());
    // Grading tab doesn't use copy actions, so we ignore those

    QAction* chosen = menu.exec(m_widgets.gradingKeyframeTable->viewport()->mapToGlobal(pos));
    if (chosen == actions.addAbove && actions.addAbove->isEnabled()) {
        const int64_t midpointFrame = previousFrame + ((anchorFrame - previousFrame) / 2);
        if (midpointFrame > previousFrame && midpointFrame < anchorFrame) {
            const auto findKeyframeAt = [](const TimelineClip& clip, int64_t frame) -> const TimelineClip::GradingKeyframe* {
                for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
                    if (keyframe.frame == frame) return &keyframe;
                }
                return nullptr;
            };
            const TimelineClip* selectedClip = m_deps.getSelectedClip();
            if (selectedClip) {
                const TimelineClip::GradingKeyframe* earlier = findKeyframeAt(*selectedClip, previousFrame);
                const TimelineClip::GradingKeyframe* later = findKeyframeAt(*selectedClip, anchorFrame);
                if (earlier && later) {
                    const double t = static_cast<double>(midpointFrame - previousFrame) / static_cast<double>(anchorFrame - previousFrame);
                    TimelineClip::GradingKeyframe midpoint;
                    midpoint.frame = midpointFrame;
                    midpoint.brightness = earlier->brightness + ((later->brightness - earlier->brightness) * t);
                    midpoint.contrast = earlier->contrast + ((later->contrast - earlier->contrast) * t);
                    midpoint.saturation = earlier->saturation + ((later->saturation - earlier->saturation) * t);
                    midpoint.shadowsR = earlier->shadowsR + ((later->shadowsR - earlier->shadowsR) * t);
                    midpoint.shadowsG = earlier->shadowsG + ((later->shadowsG - earlier->shadowsG) * t);
                    midpoint.shadowsB = earlier->shadowsB + ((later->shadowsB - earlier->shadowsB) * t);
                    midpoint.midtonesR = earlier->midtonesR + ((later->midtonesR - earlier->midtonesR) * t);
                    midpoint.midtonesG = earlier->midtonesG + ((later->midtonesG - earlier->midtonesG) * t);
                    midpoint.midtonesB = earlier->midtonesB + ((later->midtonesB - earlier->midtonesB) * t);
                    midpoint.highlightsR = earlier->highlightsR + ((later->highlightsR - earlier->highlightsR) * t);
                    midpoint.highlightsG = earlier->highlightsG + ((later->highlightsG - earlier->highlightsG) * t);
                    midpoint.highlightsB = earlier->highlightsB + ((later->highlightsB - earlier->highlightsB) * t);
                    midpoint.curvePointsR = earlier->curvePointsR;
                    midpoint.curvePointsG = earlier->curvePointsG;
                    midpoint.curvePointsB = earlier->curvePointsB;
                    midpoint.curvePointsLuma = earlier->curvePointsLuma;
                    midpoint.curveThreePointLock = earlier->curveThreePointLock;
                    midpoint.curveSmoothingEnabled = earlier->curveSmoothingEnabled;
                    midpoint.linearInterpolation = later->linearInterpolation;
                    const bool updated = m_deps.updateClipById(selectedClip->id, [midpoint](TimelineClip& clip) {
                        for (TimelineClip::GradingKeyframe& existing : clip.gradingKeyframes) {
                            if (existing.frame == midpoint.frame) {
                                existing = midpoint;
                                normalizeClipGradingKeyframes(clip);
                                return;
                            }
                        }
                        clip.gradingKeyframes.push_back(midpoint);
                        normalizeClipGradingKeyframes(clip);
                    });
                    if (updated) {
                        m_selectedKeyframeFrame = midpoint.frame;
                        m_selectedKeyframeFrames = {midpoint.frame};
                        m_deps.setPreviewTimelineClips();
                        refresh();
                        m_deps.scheduleSaveState();
                        m_deps.pushHistorySnapshot();
                        m_deps.seekToTimelineFrame(selectedClip->startFrame + midpoint.frame);
                    }
                }
            }
        }
    } else if (chosen == actions.addBelow && actions.addBelow->isEnabled()) {
        const int64_t midpointFrame = anchorFrame + ((nextFrame - anchorFrame) / 2);
        if (midpointFrame > anchorFrame && midpointFrame < nextFrame) {
            const auto findKeyframeAt = [](const TimelineClip& clip, int64_t frame) -> const TimelineClip::GradingKeyframe* {
                for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
                    if (keyframe.frame == frame) return &keyframe;
                }
                return nullptr;
            };
            const TimelineClip* selectedClip = m_deps.getSelectedClip();
            if (selectedClip) {
                const TimelineClip::GradingKeyframe* earlier = findKeyframeAt(*selectedClip, anchorFrame);
                const TimelineClip::GradingKeyframe* later = findKeyframeAt(*selectedClip, nextFrame);
                if (earlier && later) {
                    const double t = static_cast<double>(midpointFrame - anchorFrame) / static_cast<double>(nextFrame - anchorFrame);
                    TimelineClip::GradingKeyframe midpoint;
                    midpoint.frame = midpointFrame;
                    midpoint.brightness = earlier->brightness + ((later->brightness - earlier->brightness) * t);
                    midpoint.contrast = earlier->contrast + ((later->contrast - earlier->contrast) * t);
                    midpoint.saturation = earlier->saturation + ((later->saturation - earlier->saturation) * t);
                    midpoint.shadowsR = earlier->shadowsR + ((later->shadowsR - earlier->shadowsR) * t);
                    midpoint.shadowsG = earlier->shadowsG + ((later->shadowsG - earlier->shadowsG) * t);
                    midpoint.shadowsB = earlier->shadowsB + ((later->shadowsB - earlier->shadowsB) * t);
                    midpoint.midtonesR = earlier->midtonesR + ((later->midtonesR - earlier->midtonesR) * t);
                    midpoint.midtonesG = earlier->midtonesG + ((later->midtonesG - earlier->midtonesG) * t);
                    midpoint.midtonesB = earlier->midtonesB + ((later->midtonesB - earlier->midtonesB) * t);
                    midpoint.highlightsR = earlier->highlightsR + ((later->highlightsR - earlier->highlightsR) * t);
                    midpoint.highlightsG = earlier->highlightsG + ((later->highlightsG - earlier->highlightsG) * t);
                    midpoint.highlightsB = earlier->highlightsB + ((later->highlightsB - earlier->highlightsB) * t);
                    midpoint.curvePointsR = earlier->curvePointsR;
                    midpoint.curvePointsG = earlier->curvePointsG;
                    midpoint.curvePointsB = earlier->curvePointsB;
                    midpoint.curvePointsLuma = earlier->curvePointsLuma;
                    midpoint.curveThreePointLock = earlier->curveThreePointLock;
                    midpoint.curveSmoothingEnabled = earlier->curveSmoothingEnabled;
                    midpoint.linearInterpolation = later->linearInterpolation;
                    const bool updated = m_deps.updateClipById(selectedClip->id, [midpoint](TimelineClip& clip) {
                        for (TimelineClip::GradingKeyframe& existing : clip.gradingKeyframes) {
                            if (existing.frame == midpoint.frame) {
                                existing = midpoint;
                                normalizeClipGradingKeyframes(clip);
                                return;
                            }
                        }
                        clip.gradingKeyframes.push_back(midpoint);
                        normalizeClipGradingKeyframes(clip);
                    });
                    if (updated) {
                        m_selectedKeyframeFrame = midpoint.frame;
                        m_selectedKeyframeFrames = {midpoint.frame};
                        m_deps.setPreviewTimelineClips();
                        refresh();
                        m_deps.scheduleSaveState();
                        m_deps.pushHistorySnapshot();
                        m_deps.seekToTimelineFrame(selectedClip->startFrame + midpoint.frame);
                    }
                }
            }
        }
    } else if (chosen == actions.deleteRows && actions.deleteRows->isEnabled()) {
        removeSelectedKeyframes();
    }
}

void GradingTab::onCurveChannelChanged(int index)
{
    Q_UNUSED(index);
    if (m_updating || !m_widgets.gradingHistogramWidget) {
        return;
    }
    updateCurveFromInspectorValues();
}

void GradingTab::onCurveAdjusted(const QVector<QPointF>& points, bool finalized)
{
    if (m_updating) {
        return;
    }
    if (comboAlphaSelected(m_widgets.gradingCurveChannelCombo)) {
        return;
    }
    applyCurvePointsToCurrentChannel(points);
    applyGradeFromInspector(finalized);
}

void GradingTab::onCurveThreePointLockToggled(bool checked)
{
    if (m_updating) {
        return;
    }
    m_curveThreePointLock = checked;
    if (m_widgets.gradingHistogramWidget) {
        m_widgets.gradingHistogramWidget->setThreePointLockEnabled(checked);
        applyCurvePointsToCurrentChannel(m_widgets.gradingHistogramWidget->curvePoints());
    }
    applyGradeFromInspector(true);
}

void GradingTab::onCurveSmoothingToggled(bool checked)
{
    if (m_updating) {
        return;
    }
    m_curveSmoothingEnabled = checked;
    if (m_widgets.gradingHistogramWidget) {
        m_widgets.gradingHistogramWidget->setCurveSmoothingEnabled(checked);
        if (m_curveThreePointLock) {
            applyCurvePointsToCurrentChannel(m_widgets.gradingHistogramWidget->curvePoints());
        }
    }
    applyGradeFromInspector(true);
}

void GradingTab::updateHistogramAndCurve(bool forceHistogramRefresh)
{
    if (!m_widgets.gradingHistogramWidget) {
        return;
    }

    const bool paused = !m_gradingDeps.isPlaybackPaused || m_gradingDeps.isPlaybackPaused();
    m_widgets.gradingHistogramWidget->setVisible(paused);
    updateCurveFromInspectorValues();
    if (!paused) {
        return;
    }

    if (!m_gradingDeps.getCurrentFrameImage) {
        if (forceHistogramRefresh) {
            m_widgets.gradingHistogramWidget->clearHistogram();
            if (m_widgets.gradingCurveChannelCombo) {
                const int alphaIndex = m_widgets.gradingCurveChannelCombo->findText(QStringLiteral("Alpha"));
                if (alphaIndex >= 0) {
                    const bool wasSelected =
                        (m_widgets.gradingCurveChannelCombo->currentIndex() == alphaIndex);
                    m_widgets.gradingCurveChannelCombo->removeItem(alphaIndex);
                    if (wasSelected) {
                        m_widgets.gradingCurveChannelCombo->setCurrentIndex(3);
                    }
                }
            }
        }
        return;
    }

    const QImage frameImage = m_gradingDeps.getCurrentFrameImage();
    if (frameImage.isNull()) {
        if (forceHistogramRefresh) {
            m_lastHistogramImageKey = 0;
            m_widgets.gradingHistogramWidget->clearHistogram();
            if (m_widgets.gradingCurveChannelCombo) {
                const int alphaIndex = m_widgets.gradingCurveChannelCombo->findText(QStringLiteral("Alpha"));
                if (alphaIndex >= 0) {
                    const bool wasSelected =
                        (m_widgets.gradingCurveChannelCombo->currentIndex() == alphaIndex);
                    m_widgets.gradingCurveChannelCombo->removeItem(alphaIndex);
                    if (wasSelected) {
                        m_widgets.gradingCurveChannelCombo->setCurrentIndex(3);
                    }
                }
            }
        }
        return;
    }

    const int64_t imageKey = static_cast<int64_t>(frameImage.cacheKey());
    if (!forceHistogramRefresh && imageKey == m_lastHistogramImageKey) {
        return;
    }
    m_lastHistogramImageKey = imageKey;
    m_widgets.gradingHistogramWidget->setHistogramFromImage(frameImage);
    if (m_widgets.gradingCurveChannelCombo) {
        const bool wantAlpha = m_widgets.gradingHistogramWidget->hasAlphaHistogram();
        const int alphaIndex = m_widgets.gradingCurveChannelCombo->findText(QStringLiteral("Alpha"));
        if (wantAlpha && alphaIndex < 0) {
            m_widgets.gradingCurveChannelCombo->addItem(QStringLiteral("Alpha"));
        } else if (!wantAlpha && alphaIndex >= 0) {
            const bool wasSelected = (m_widgets.gradingCurveChannelCombo->currentIndex() == alphaIndex);
            m_widgets.gradingCurveChannelCombo->removeItem(alphaIndex);
            if (wasSelected) {
                m_widgets.gradingCurveChannelCombo->setCurrentIndex(3);
            }
        }
    }
}

void GradingTab::updateCurveFromInspectorValues()
{
    if (!m_widgets.gradingHistogramWidget) {
        return;
    }

    int selectedChannelIndex = 0;
    if (m_widgets.gradingCurveChannelCombo) {
        const int maxChannel = comboHasAlphaItem(m_widgets.gradingCurveChannelCombo) ? 4 : 3;
        const int channelIndex = qBound(0, m_widgets.gradingCurveChannelCombo->currentIndex(), maxChannel);
        selectedChannelIndex = channelIndex;
        GradingHistogramWidget::Channel channel = GradingHistogramWidget::Channel::Red;
        if (channelIndex == 1) {
            channel = GradingHistogramWidget::Channel::Green;
        } else if (channelIndex == 2) {
            channel = GradingHistogramWidget::Channel::Blue;
        } else if (channelIndex == 3) {
            channel = GradingHistogramWidget::Channel::Brightness;
        } else if (channelIndex == 4) {
            channel = GradingHistogramWidget::Channel::Alpha;
        }
        m_widgets.gradingHistogramWidget->setSelectedChannel(channel);
    }

    // Show only tone controls for the selected RGB channel.
    setToneSpinGroupVisible(m_widgets.shadowsRSpin,
                            m_widgets.shadowsGSpin,
                            m_widgets.shadowsBSpin,
                            selectedChannelIndex);
    setToneSpinGroupVisible(m_widgets.midtonesRSpin,
                            m_widgets.midtonesGSpin,
                            m_widgets.midtonesBSpin,
                            selectedChannelIndex);
    setToneSpinGroupVisible(m_widgets.highlightsRSpin,
                            m_widgets.highlightsGSpin,
                            m_widgets.highlightsBSpin,
                            selectedChannelIndex);

    m_widgets.gradingHistogramWidget->setCurveSmoothingEnabled(m_curveSmoothingEnabled);
    m_widgets.gradingHistogramWidget->setThreePointLockEnabled(m_curveThreePointLock);
    if (comboAlphaSelected(m_widgets.gradingCurveChannelCombo)) {
        m_widgets.gradingHistogramWidget->setEnabled(false);
        m_widgets.gradingHistogramWidget->setCurvePoints(defaultGradingCurvePoints());
    } else {
        m_widgets.gradingHistogramWidget->setEnabled(true);
        m_widgets.gradingHistogramWidget->setCurvePoints(currentChannelCurvePoints());
    }
}

QVector<QPointF> GradingTab::currentChannelCurvePoints() const
{
    const int channelIndex = m_widgets.gradingCurveChannelCombo
                                 ? qBound(0, m_widgets.gradingCurveChannelCombo->currentIndex(), 3)
                                 : 0;
    if (channelIndex == 1) {
        return sanitizeGradingCurvePoints(m_curvePointsG);
    }
    if (channelIndex == 2) {
        return sanitizeGradingCurvePoints(m_curvePointsB);
    }
    if (channelIndex == 3) {
        return sanitizeGradingCurvePoints(m_curvePointsLuma);
    }
    return sanitizeGradingCurvePoints(m_curvePointsR);
}

void GradingTab::applyCurvePointsToCurrentChannel(const QVector<QPointF>& points)
{
    const QVector<QPointF> sanitized = sanitizeGradingCurvePoints(points);
    const int channelIndex = m_widgets.gradingCurveChannelCombo
                                 ? qBound(0, m_widgets.gradingCurveChannelCombo->currentIndex(), 3)
                                 : 0;
    if (channelIndex == 1) {
        m_curvePointsG = sanitized;
        return;
    }
    if (channelIndex == 2) {
        m_curvePointsB = sanitized;
        return;
    }
    if (channelIndex == 3) {
        m_curvePointsLuma = sanitized;
        return;
    }
    m_curvePointsR = sanitized;
}

void GradingTab::onBrightnessChanged(double value)
{
    Q_UNUSED(value);
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onContrastChanged(double value)
{
    Q_UNUSED(value);
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onSaturationChanged(double value)
{
    Q_UNUSED(value);
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onShadowsRChanged(double value)
{
    Q_UNUSED(value);
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onShadowsGChanged(double value)
{
    Q_UNUSED(value);
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onShadowsBChanged(double value)
{
    Q_UNUSED(value);
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onMidtonesRChanged(double value)
{
    Q_UNUSED(value);
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onMidtonesGChanged(double value)
{
    Q_UNUSED(value);
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onMidtonesBChanged(double value)
{
    Q_UNUSED(value);
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onHighlightsRChanged(double value)
{
    Q_UNUSED(value);
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onHighlightsGChanged(double value)
{
    Q_UNUSED(value);
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onHighlightsBChanged(double value)
{
    Q_UNUSED(value);
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}
