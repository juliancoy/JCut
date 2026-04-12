#include "corrections_tab.h"
#include "timeline_widget.h"

#include <QSignalBlocker>

CorrectionsTab::CorrectionsTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps) {
}

void CorrectionsTab::wire() {
    if (m_widgets.correctionsDrawModeCheck) {
        connect(m_widgets.correctionsDrawModeCheck, &QCheckBox::toggled, this, [this](bool enabled) {
            if (m_updating) {
                return;
            }
            setDrawingEnabled(enabled);
        });
    }
    if (m_widgets.correctionsDrawPolygonButton) {
        connect(m_widgets.correctionsDrawPolygonButton, &QPushButton::toggled, this, [this](bool checked) {
            if (m_updating) {
                return;
            }
            setDrawingEnabled(checked);
        });
    }
    if (m_widgets.correctionsClosePolygonButton) {
        connect(m_widgets.correctionsClosePolygonButton, &QPushButton::clicked, this, [this]() {
            commitDraftPolygon();
        });
    }
    if (m_widgets.correctionsCancelDraftButton) {
        connect(m_widgets.correctionsCancelDraftButton, &QPushButton::clicked, this, [this]() {
            cancelDraftPolygon();
        });
    }
    if (m_widgets.correctionsDeleteLastButton) {
        connect(m_widgets.correctionsDeleteLastButton, &QPushButton::clicked, this, [this]() {
            const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
            if (!clip || clip->correctionPolygons.isEmpty()) {
                return;
            }
            if (!m_deps.updateClipById(clip->id, [](TimelineClip& editable) {
                    if (!editable.correctionPolygons.isEmpty()) {
                        editable.correctionPolygons.removeLast();
                    }
                })) {
                return;
            }
            if (m_deps.setPreviewTimelineClips) {
                m_deps.setPreviewTimelineClips();
            }
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
    if (m_widgets.correctionsClearAllButton) {
        connect(m_widgets.correctionsClearAllButton, &QPushButton::clicked, this, [this]() {
            const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
            if (!clip || clip->correctionPolygons.isEmpty()) {
                return;
            }
            if (!m_deps.updateClipById(clip->id, [](TimelineClip& editable) {
                    editable.correctionPolygons.clear();
                })) {
                return;
            }
            if (m_deps.setPreviewTimelineClips) {
                m_deps.setPreviewTimelineClips();
            }
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
}

void CorrectionsTab::setDrawingEnabled(bool enabled) {
    if (enabled) {
        if (m_deps.getTimelineToolMode && m_deps.setTimelineToolMode) {
            m_savedToolMode = m_deps.getTimelineToolMode();
            m_savedToolModeValid = true;
            m_deps.setTimelineToolMode(TimelineWidget::ToolMode::Select);
        }
    } else {
        if (m_deps.setTimelineToolMode && m_savedToolModeValid) {
            m_deps.setTimelineToolMode(m_savedToolMode);
            m_savedToolModeValid = false;
        }
    }
    if (m_deps.setCorrectionDrawMode) {
        m_deps.setCorrectionDrawMode(enabled);
    }
    if (!enabled) {
        cancelDraftPolygon();
    }
    refresh();
}

void CorrectionsTab::clearDraftFromPreview() {
    if (m_deps.setCorrectionDraftPoints) {
        m_deps.setCorrectionDraftPoints({});
    }
}

void CorrectionsTab::syncDraftToPreview() {
    if (m_deps.setCorrectionDraftPoints) {
        m_deps.setCorrectionDraftPoints(m_draftPoints);
    }
}

void CorrectionsTab::cancelDraftPolygon() {
    m_draftPoints.clear();
    clearDraftFromPreview();
    refresh();
}

void CorrectionsTab::commitDraftPolygon() {
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || m_draftPoints.size() < 3) {
        return;
    }

    const QVector<QPointF> polygonPoints = m_draftPoints;
    if (!m_deps.updateClipById(clip->id, [polygonPoints](TimelineClip& editable) {
            TimelineClip::CorrectionPolygon polygon;
            polygon.enabled = true;
            polygon.pointsNormalized = polygonPoints;
            editable.correctionPolygons.push_back(polygon);
        })) {
        return;
    }

    m_draftPoints.clear();
    clearDraftFromPreview();
    if (m_deps.setPreviewTimelineClips) {
        m_deps.setPreviewTimelineClips();
    }
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    if (m_deps.refreshInspector) {
        m_deps.refreshInspector();
    }
}

void CorrectionsTab::handlePreviewPoint(const QString& clipId, qreal xNorm, qreal yNorm) {
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || clip->id != clipId) {
        return;
    }
    const bool drawEnabled = (m_widgets.correctionsDrawModeCheck && m_widgets.correctionsDrawModeCheck->isChecked())
        || (m_widgets.correctionsDrawPolygonButton && m_widgets.correctionsDrawPolygonButton->isChecked());
    if (!drawEnabled) {
        return;
    }

    m_draftPoints.push_back(QPointF(qBound<qreal>(0.0, xNorm, 1.0), qBound<qreal>(0.0, yNorm, 1.0)));
    syncDraftToPreview();
    refresh();
}

void CorrectionsTab::refresh() {
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    const bool drawEnabled = (m_widgets.correctionsDrawModeCheck && m_widgets.correctionsDrawModeCheck->isChecked())
        || (m_widgets.correctionsDrawPolygonButton && m_widgets.correctionsDrawPolygonButton->isChecked());

    m_updating = true;
    if (m_widgets.correctionsClipLabel) {
        if (!clip) {
            m_widgets.correctionsClipLabel->setText(QStringLiteral("No clip selected"));
        } else {
            m_widgets.correctionsClipLabel->setText(clip->label);
        }
    }

    const bool clipSupportsCorrections = clip && m_deps.clipHasVisuals && m_deps.clipHasVisuals(*clip);
    const int polygonCount = clip ? clip->correctionPolygons.size() : 0;
    const QString draftStatus = m_draftPoints.isEmpty()
        ? QStringLiteral("No draft polygon")
        : QStringLiteral("Draft points: %1").arg(m_draftPoints.size());
    if (m_widgets.correctionsStatusLabel) {
        m_widgets.correctionsStatusLabel->setText(
            clipSupportsCorrections
                ? QStringLiteral("Polygons: %1\n%2").arg(polygonCount).arg(draftStatus)
                : QStringLiteral("Select a visual clip to add erase polygons."));
    }

    if (m_widgets.correctionsDrawModeCheck) {
        QSignalBlocker block(m_widgets.correctionsDrawModeCheck);
        m_widgets.correctionsDrawModeCheck->setEnabled(clipSupportsCorrections);
        if (!clipSupportsCorrections) {
            m_widgets.correctionsDrawModeCheck->setChecked(false);
        }
    }

    if (m_widgets.correctionsDrawPolygonButton) {
        QSignalBlocker block(m_widgets.correctionsDrawPolygonButton);
        m_widgets.correctionsDrawPolygonButton->setEnabled(clipSupportsCorrections);
        if (!clipSupportsCorrections) {
            m_widgets.correctionsDrawPolygonButton->setChecked(false);
        }
    }

    if (m_widgets.correctionsClosePolygonButton) {
        m_widgets.correctionsClosePolygonButton->setEnabled(drawEnabled && m_draftPoints.size() >= 3);
    }
    if (m_widgets.correctionsCancelDraftButton) {
        m_widgets.correctionsCancelDraftButton->setEnabled(!m_draftPoints.isEmpty());
    }
    if (m_widgets.correctionsDeleteLastButton) {
        m_widgets.correctionsDeleteLastButton->setEnabled(clipSupportsCorrections && polygonCount > 0);
    }
    if (m_widgets.correctionsClearAllButton) {
        m_widgets.correctionsClearAllButton->setEnabled(clipSupportsCorrections && polygonCount > 0);
    }

    if (!clipSupportsCorrections || !drawEnabled) {
        if (!m_draftPoints.isEmpty()) {
            m_draftPoints.clear();
        }
        clearDraftFromPreview();
    } else {
        syncDraftToPreview();
    }

    m_updating = false;
}
