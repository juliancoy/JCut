#include "grading_tab.h"
#include "grading_histogram_widget.h"

namespace {

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

QVector<QPointF> threePointCurveFromToneValues(qreal shadows, qreal midtones, qreal highlights)
{
    QVector<QPointF> points;
    points.push_back(QPointF(0.0, qBound<qreal>(0.0, 0.0 + (shadows * 0.25), 1.0)));
    points.push_back(QPointF(0.5, qBound<qreal>(0.0, 0.5 + (midtones * 0.20), 1.0)));
    points.push_back(QPointF(1.0, qBound<qreal>(0.0, 1.0 + (highlights * 0.25), 1.0)));
    return sanitizeGradingCurvePoints(points);
}

void toneValuesFromThreePointCurve(const QVector<QPointF>& points,
                                   qreal* shadowsOut,
                                   qreal* midtonesOut,
                                   qreal* highlightsOut)
{
    if (!shadowsOut || !midtonesOut || !highlightsOut) {
        return;
    }
    QVector<QPointF> sanitized = sanitizeGradingCurvePoints(points);
    if (sanitized.size() < 3) {
        sanitized = defaultGradingCurvePoints();
    }
    const qreal y0 = sanitized.at(0).y();
    const qreal y1 = sanitized.at(sanitized.size() / 2).y();
    const qreal y2 = sanitized.constLast().y();
    *shadowsOut = qBound<qreal>(-2.0, (y0 - 0.0) / 0.25, 2.0);
    *midtonesOut = qBound<qreal>(-2.0, (y1 - 0.5) / 0.20, 2.0);
    *highlightsOut = qBound<qreal>(-2.0, (y2 - 1.0) / 0.25, 2.0);
}

} // namespace

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
    if (m_curveThreePointLock) {
        syncToneSpinsFromCurvePoints(points);
        updateCurveFromInspectorValues();
        applyGradeFromInspector(finalized);
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

    if (m_widgets.gradingHistogramWidget) {
        QColor channelBackground(16, 22, 30, 255);
        if (selectedChannelIndex == 0) {
            channelBackground = QColor(44, 16, 16, 255);
        } else if (selectedChannelIndex == 1) {
            channelBackground = QColor(16, 40, 22, 255);
        } else if (selectedChannelIndex == 2) {
            channelBackground = QColor(16, 24, 44, 255);
        } else if (selectedChannelIndex == 3) {
            channelBackground = QColor(48, 38, 16, 255);
        }
        m_widgets.gradingHistogramWidget->setChartBackgroundColor(channelBackground);
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
    syncCurrentChannelCurveFromToneSpins();
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onShadowsGChanged(double value)
{
    Q_UNUSED(value);
    syncCurrentChannelCurveFromToneSpins();
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onShadowsBChanged(double value)
{
    Q_UNUSED(value);
    syncCurrentChannelCurveFromToneSpins();
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onMidtonesRChanged(double value)
{
    Q_UNUSED(value);
    syncCurrentChannelCurveFromToneSpins();
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onMidtonesGChanged(double value)
{
    Q_UNUSED(value);
    syncCurrentChannelCurveFromToneSpins();
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onMidtonesBChanged(double value)
{
    Q_UNUSED(value);
    syncCurrentChannelCurveFromToneSpins();
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onHighlightsRChanged(double value)
{
    Q_UNUSED(value);
    syncCurrentChannelCurveFromToneSpins();
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onHighlightsGChanged(double value)
{
    Q_UNUSED(value);
    syncCurrentChannelCurveFromToneSpins();
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::onHighlightsBChanged(double value)
{
    Q_UNUSED(value);
    syncCurrentChannelCurveFromToneSpins();
    updateCurveFromInspectorValues();
    applyGradeFromInspector(false);
}

void GradingTab::syncCurrentChannelCurveFromToneSpins()
{
    if (!m_curveThreePointLock || !m_widgets.gradingCurveChannelCombo) {
        return;
    }
    const int channelIndex = qBound(0, m_widgets.gradingCurveChannelCombo->currentIndex(), 3);
    if (channelIndex == 3) {
        return;
    }
    qreal shadows = 0.0;
    qreal midtones = 0.0;
    qreal highlights = 0.0;
    if (channelIndex == 0) {
        shadows = m_widgets.shadowsRSpin ? m_widgets.shadowsRSpin->value() : 0.0;
        midtones = m_widgets.midtonesRSpin ? m_widgets.midtonesRSpin->value() : 0.0;
        highlights = m_widgets.highlightsRSpin ? m_widgets.highlightsRSpin->value() : 0.0;
        m_curvePointsR = threePointCurveFromToneValues(shadows, midtones, highlights);
    } else if (channelIndex == 1) {
        shadows = m_widgets.shadowsGSpin ? m_widgets.shadowsGSpin->value() : 0.0;
        midtones = m_widgets.midtonesGSpin ? m_widgets.midtonesGSpin->value() : 0.0;
        highlights = m_widgets.highlightsGSpin ? m_widgets.highlightsGSpin->value() : 0.0;
        m_curvePointsG = threePointCurveFromToneValues(shadows, midtones, highlights);
    } else {
        shadows = m_widgets.shadowsBSpin ? m_widgets.shadowsBSpin->value() : 0.0;
        midtones = m_widgets.midtonesBSpin ? m_widgets.midtonesBSpin->value() : 0.0;
        highlights = m_widgets.highlightsBSpin ? m_widgets.highlightsBSpin->value() : 0.0;
        m_curvePointsB = threePointCurveFromToneValues(shadows, midtones, highlights);
    }
}

void GradingTab::syncToneSpinsFromCurvePoints(const QVector<QPointF>& points)
{
    if (!m_curveThreePointLock || !m_widgets.gradingCurveChannelCombo) {
        return;
    }
    const int channelIndex = qBound(0, m_widgets.gradingCurveChannelCombo->currentIndex(), 3);
    if (channelIndex == 3) {
        return;
    }
    qreal shadows = 0.0;
    qreal midtones = 0.0;
    qreal highlights = 0.0;
    toneValuesFromThreePointCurve(points, &shadows, &midtones, &highlights);
    QSignalBlocker shadowsRBlock(m_widgets.shadowsRSpin);
    QSignalBlocker shadowsGBlock(m_widgets.shadowsGSpin);
    QSignalBlocker shadowsBBlock(m_widgets.shadowsBSpin);
    QSignalBlocker midtonesRBlock(m_widgets.midtonesRSpin);
    QSignalBlocker midtonesGBlock(m_widgets.midtonesGSpin);
    QSignalBlocker midtonesBBlock(m_widgets.midtonesBSpin);
    QSignalBlocker highlightsRBlock(m_widgets.highlightsRSpin);
    QSignalBlocker highlightsGBlock(m_widgets.highlightsGSpin);
    QSignalBlocker highlightsBBlock(m_widgets.highlightsBSpin);

    if (channelIndex == 0) {
        if (m_widgets.shadowsRSpin) m_widgets.shadowsRSpin->setValue(shadows);
        if (m_widgets.midtonesRSpin) m_widgets.midtonesRSpin->setValue(midtones);
        if (m_widgets.highlightsRSpin) m_widgets.highlightsRSpin->setValue(highlights);
        m_curvePointsR = threePointCurveFromToneValues(shadows, midtones, highlights);
    } else if (channelIndex == 1) {
        if (m_widgets.shadowsGSpin) m_widgets.shadowsGSpin->setValue(shadows);
        if (m_widgets.midtonesGSpin) m_widgets.midtonesGSpin->setValue(midtones);
        if (m_widgets.highlightsGSpin) m_widgets.highlightsGSpin->setValue(highlights);
        m_curvePointsG = threePointCurveFromToneValues(shadows, midtones, highlights);
    } else {
        if (m_widgets.shadowsBSpin) m_widgets.shadowsBSpin->setValue(shadows);
        if (m_widgets.midtonesBSpin) m_widgets.midtonesBSpin->setValue(midtones);
        if (m_widgets.highlightsBSpin) m_widgets.highlightsBSpin->setValue(highlights);
        m_curvePointsB = threePointCurveFromToneValues(shadows, midtones, highlights);
    }
}
