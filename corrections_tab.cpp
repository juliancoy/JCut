#include "corrections_tab.h"
#include "timeline_widget.h"

#include <QSignalBlocker>

CorrectionsTab::CorrectionsTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps) {
}

void CorrectionsTab::wire() {
    if (m_widgets.correctionsEnabledCheck) {
        connect(m_widgets.correctionsEnabledCheck, &QCheckBox::toggled, this, [this](bool enabled) {
            if (m_updating) {
                return;
            }
            if (m_deps.setCorrectionsEnabled) {
                m_deps.setCorrectionsEnabled(enabled);
            }
            refresh();
        });
    }
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
    if (m_widgets.correctionsPolygonTable) {
        connect(m_widgets.correctionsPolygonTable, &QTableWidget::itemSelectionChanged, this, [this]() {
            if (m_updating) {
                return;
            }
            m_selectedPolygon = selectedPolygonIndex();
            if (m_deps.setSelectedCorrectionPolygon) {
                m_deps.setSelectedCorrectionPolygon(m_selectedPolygon);
            }
            refresh();
        });
        connect(m_widgets.correctionsPolygonTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
            if (m_updating) {
                return;
            }
            applyPolygonCellEdit(item);
        });
    }
    if (m_widgets.correctionsVertexTable) {
        connect(m_widgets.correctionsVertexTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
            if (m_updating) {
                return;
            }
            applyVertexCellEdit(item);
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
            polygon.startFrame = 0;
            polygon.endFrame = -1;
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
    m_selectedPolygon = clip->correctionPolygons.size();
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
    const bool correctionsEnabled = !m_deps.correctionsEnabled || m_deps.correctionsEnabled();
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
        if (!clipSupportsCorrections) {
            m_widgets.correctionsStatusLabel->setText(QStringLiteral("Select a visual clip to add erase polygons."));
        } else if (correctionsEnabled) {
            m_widgets.correctionsStatusLabel->setText(
                QStringLiteral("Polygons: %1 (GPU applied)\n%2").arg(polygonCount).arg(draftStatus));
        } else {
            m_widgets.correctionsStatusLabel->setText(
                QStringLiteral("Polygons: %1 (disabled)\n%2").arg(polygonCount).arg(draftStatus));
        }
    }

    if (m_widgets.correctionsEnabledCheck) {
        QSignalBlocker block(m_widgets.correctionsEnabledCheck);
        m_widgets.correctionsEnabledCheck->setChecked(correctionsEnabled);
        m_widgets.correctionsEnabledCheck->setEnabled(true);
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

    if (m_widgets.correctionsPolygonTable) {
        m_widgets.correctionsPolygonTable->setEnabled(clipSupportsCorrections);
    }
    if (m_widgets.correctionsVertexTable) {
        m_widgets.correctionsVertexTable->setEnabled(clipSupportsCorrections && polygonCount > 0);
    }
    refreshPolygonTable(clip);
    refreshVertexTable(clip);
    if (m_deps.setSelectedCorrectionPolygon) {
        m_deps.setSelectedCorrectionPolygon(m_selectedPolygon);
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

void CorrectionsTab::stopDrawing() {
    const bool wasEnabled =
        (m_widgets.correctionsDrawModeCheck && m_widgets.correctionsDrawModeCheck->isChecked()) ||
        (m_widgets.correctionsDrawPolygonButton && m_widgets.correctionsDrawPolygonButton->isChecked());
    if (!wasEnabled && m_draftPoints.isEmpty()) {
        return;
    }

    m_updating = true;
    if (m_widgets.correctionsDrawModeCheck) {
        QSignalBlocker block(m_widgets.correctionsDrawModeCheck);
        m_widgets.correctionsDrawModeCheck->setChecked(false);
    }
    if (m_widgets.correctionsDrawPolygonButton) {
        QSignalBlocker block(m_widgets.correctionsDrawPolygonButton);
        m_widgets.correctionsDrawPolygonButton->setChecked(false);
    }
    m_updating = false;
    if (m_deps.setSelectedCorrectionPolygon) {
        m_deps.setSelectedCorrectionPolygon(m_selectedPolygon);
    }
    setDrawingEnabled(false);
}

int CorrectionsTab::selectedPolygonIndex() const {
    if (!m_widgets.correctionsPolygonTable) {
        return -1;
    }
    const int row = m_widgets.correctionsPolygonTable->currentRow();
    if (row < 0) {
        return -1;
    }
    if (QTableWidgetItem* item = m_widgets.correctionsPolygonTable->item(row, 0)) {
        return item->data(Qt::UserRole).toInt();
    }
    return row;
}

void CorrectionsTab::refreshPolygonTable(const TimelineClip* clip) {
    if (!m_widgets.correctionsPolygonTable) {
        return;
    }
    QSignalBlocker block(m_widgets.correctionsPolygonTable);
    m_widgets.correctionsPolygonTable->clearContents();
    const int rowCount = clip ? clip->correctionPolygons.size() : 0;
    m_widgets.correctionsPolygonTable->setRowCount(rowCount);
    for (int row = 0; row < rowCount; ++row) {
        const TimelineClip::CorrectionPolygon& polygon = clip->correctionPolygons[row];
        auto* enabledItem = new QTableWidgetItem;
        enabledItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        enabledItem->setCheckState(polygon.enabled ? Qt::Checked : Qt::Unchecked);
        enabledItem->setData(Qt::UserRole, row);
        m_widgets.correctionsPolygonTable->setItem(row, 0, enabledItem);

        auto* startItem = new QTableWidgetItem(QString::number(qMax<int64_t>(0, polygon.startFrame)));
        startItem->setData(Qt::UserRole, row);
        m_widgets.correctionsPolygonTable->setItem(row, 1, startItem);

        auto* endItem = new QTableWidgetItem(
            polygon.endFrame < 0 ? QStringLiteral("clip_end") : QString::number(polygon.endFrame));
        endItem->setData(Qt::UserRole, row);
        m_widgets.correctionsPolygonTable->setItem(row, 2, endItem);

        auto* pointsItem = new QTableWidgetItem(QString::number(polygon.pointsNormalized.size()));
        pointsItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        pointsItem->setData(Qt::UserRole, row);
        m_widgets.correctionsPolygonTable->setItem(row, 3, pointsItem);
    }

    if (rowCount <= 0) {
        m_selectedPolygon = -1;
        return;
    }
    if (m_selectedPolygon < 0 || m_selectedPolygon >= rowCount) {
        m_selectedPolygon = rowCount - 1;
    }
    m_widgets.correctionsPolygonTable->selectRow(m_selectedPolygon);
}

void CorrectionsTab::refreshVertexTable(const TimelineClip* clip) {
    if (!m_widgets.correctionsVertexTable) {
        return;
    }
    QSignalBlocker block(m_widgets.correctionsVertexTable);
    m_widgets.correctionsVertexTable->clearContents();
    if (!clip || m_selectedPolygon < 0 || m_selectedPolygon >= clip->correctionPolygons.size()) {
        m_widgets.correctionsVertexTable->setRowCount(0);
        return;
    }
    const auto& points = clip->correctionPolygons[m_selectedPolygon].pointsNormalized;
    m_widgets.correctionsVertexTable->setRowCount(points.size());
    for (int i = 0; i < points.size(); ++i) {
        auto* xItem = new QTableWidgetItem(QString::number(points[i].x(), 'f', 4));
        xItem->setData(Qt::UserRole, i);
        m_widgets.correctionsVertexTable->setItem(i, 0, xItem);
        auto* yItem = new QTableWidgetItem(QString::number(points[i].y(), 'f', 4));
        yItem->setData(Qt::UserRole, i);
        m_widgets.correctionsVertexTable->setItem(i, 1, yItem);
    }
}

void CorrectionsTab::applyPolygonCellEdit(QTableWidgetItem* item) {
    if (!item || !m_deps.getSelectedClip || !m_deps.updateClipById) {
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip();
    if (!clip) {
        return;
    }
    const int polygonIndex = item->data(Qt::UserRole).toInt();
    const int column = item->column();
    const bool changed = m_deps.updateClipById(clip->id, [polygonIndex, column, item](TimelineClip& editable) {
        if (polygonIndex < 0 || polygonIndex >= editable.correctionPolygons.size()) {
            return;
        }
        auto& polygon = editable.correctionPolygons[polygonIndex];
        if (column == 0) {
            polygon.enabled = item->checkState() == Qt::Checked;
            return;
        }
        if (column == 1) {
            polygon.startFrame = qMax<int64_t>(0, item->text().toLongLong());
            if (polygon.endFrame >= 0 && polygon.endFrame < polygon.startFrame) {
                polygon.endFrame = polygon.startFrame;
            }
            return;
        }
        if (column == 2) {
            const QString normalized = item->text().trimmed().toLower();
            if (normalized.isEmpty() || normalized == QStringLiteral("clip_end") ||
                normalized == QStringLiteral("end") || normalized == QStringLiteral("inf")) {
                polygon.endFrame = -1;
                return;
            }
            bool ok = false;
            const qlonglong value = normalized.toLongLong(&ok);
            if (!ok) {
                return;
            }
            polygon.endFrame = qMax<int64_t>(polygon.startFrame, value);
        }
    });
    if (!changed) {
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
    refresh();
}

void CorrectionsTab::applyVertexCellEdit(QTableWidgetItem* item) {
    if (!item || !m_deps.getSelectedClip || !m_deps.updateClipById) {
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip();
    if (!clip || m_selectedPolygon < 0 || m_selectedPolygon >= clip->correctionPolygons.size()) {
        return;
    }
    const int pointIndex = item->data(Qt::UserRole).toInt();
    const int column = item->column();
    bool ok = false;
    const qreal value = item->text().toDouble(&ok);
    if (!ok) {
        refresh();
        return;
    }
    const qreal clamped = qBound<qreal>(0.0, value, 1.0);
    const bool changed = m_deps.updateClipById(clip->id, [this, pointIndex, column, clamped](TimelineClip& editable) {
        if (m_selectedPolygon < 0 || m_selectedPolygon >= editable.correctionPolygons.size()) {
            return;
        }
        auto& points = editable.correctionPolygons[m_selectedPolygon].pointsNormalized;
        if (pointIndex < 0 || pointIndex >= points.size()) {
            return;
        }
        QPointF point = points[pointIndex];
        if (column == 0) {
            point.setX(clamped);
        } else if (column == 1) {
            point.setY(clamped);
        } else {
            return;
        }
        points[pointIndex] = point;
    });
    if (!changed) {
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
    refresh();
}
