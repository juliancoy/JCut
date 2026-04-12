#pragma once

#include <QObject>
#include <QCheckBox>
#include <QLabel>
#include <QPointF>
#include <QPushButton>
#include <QVector>
#include <functional>

#include "editor_shared.h"

class CorrectionsTab : public QObject {
    Q_OBJECT

public:
    struct Widgets {
        QLabel* correctionsClipLabel = nullptr;
        QLabel* correctionsStatusLabel = nullptr;
        QCheckBox* correctionsDrawModeCheck = nullptr;
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
        std::function<void(bool)> setCorrectionDrawMode;
        std::function<void(const QVector<QPointF>&)> setCorrectionDraftPoints;
    };

    explicit CorrectionsTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~CorrectionsTab() override = default;

    void wire();
    void refresh();
    void handlePreviewPoint(const QString& clipId, qreal xNorm, qreal yNorm);

private:
    void commitDraftPolygon();
    void cancelDraftPolygon();
    void clearDraftFromPreview();
    void syncDraftToPreview();
    void setDrawingEnabled(bool enabled);

    Widgets m_widgets;
    Dependencies m_deps;
    QVector<QPointF> m_draftPoints;
    bool m_updating = false;
};
