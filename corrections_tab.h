#pragma once

#include <QObject>
#include <QCheckBox>
#include <QLabel>
#include <QPointF>
#include <QPushButton>
#include <QTableWidget>
#include <QVector>
#include <functional>

#include "editor_shared.h"
#include "timeline_widget.h"

class CorrectionsTab : public QObject {
    Q_OBJECT

public:
    struct Widgets {
        QLabel* correctionsClipLabel = nullptr;
        QLabel* correctionsStatusLabel = nullptr;
        QCheckBox* correctionsEnabledCheck = nullptr;
        QTableWidget* correctionsPolygonTable = nullptr;
        QTableWidget* correctionsVertexTable = nullptr;
        QCheckBox* correctionsDrawModeCheck = nullptr;
        QPushButton* correctionsDrawPolygonButton = nullptr;
        QPushButton* correctionsClosePolygonButton = nullptr;
        QPushButton* correctionsCancelDraftButton = nullptr;
        QPushButton* correctionsDeleteLastButton = nullptr;
        QPushButton* correctionsClearAllButton = nullptr;
    };

    struct Dependencies {
        std::function<const TimelineClip*()> getSelectedClip;
        std::function<bool(const QString&, const std::function<void(TimelineClip&)>&)> updateClipById;
        std::function<void()> setPreviewTimelineClips;
        std::function<void()> refreshInspector;
        std::function<void()> scheduleSaveState;
        std::function<void()> pushHistorySnapshot;
        std::function<bool(const TimelineClip&)> clipHasVisuals;
        std::function<bool()> correctionsEnabled;
        std::function<void(bool)> setCorrectionsEnabled;
        std::function<void(bool)> setCorrectionDrawMode;
        std::function<void(int)> setSelectedCorrectionPolygon;
        std::function<void(const QVector<QPointF>&)> setCorrectionDraftPoints;
        std::function<void(TimelineWidget::ToolMode)> setTimelineToolMode;
        std::function<TimelineWidget::ToolMode()> getTimelineToolMode;
    };

    explicit CorrectionsTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~CorrectionsTab() override = default;

    void wire();
    void refresh();
    void stopDrawing();
    void handlePreviewPoint(const QString& clipId, qreal xNorm, qreal yNorm);

private:
    void commitDraftPolygon();
    void cancelDraftPolygon();
    void clearDraftFromPreview();
    void syncDraftToPreview();
    void setDrawingEnabled(bool enabled);
    void refreshPolygonTable(const TimelineClip* clip);
    void refreshVertexTable(const TimelineClip* clip);
    void applyPolygonCellEdit(QTableWidgetItem* item);
    void applyVertexCellEdit(QTableWidgetItem* item);
    int selectedPolygonIndex() const;

    Widgets m_widgets;
    Dependencies m_deps;
    QVector<QPointF> m_draftPoints;
    int m_selectedPolygon = -1;
    bool m_updating = false;
    bool m_savedToolModeValid = false;
    TimelineWidget::ToolMode m_savedToolMode;
};
