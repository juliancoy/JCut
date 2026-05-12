#pragma once

#include "preview_surface.h"

#include <QObject>
#include <QListWidget>
#include <QTimer>
#include <QVector>
#include <functional>

class QLabel;

class PipelineTab : public QObject
{
    Q_OBJECT

public:
    struct Widgets
    {
        QListWidget* pipelineStageList = nullptr;
    };

    struct Dependencies
    {
        std::function<QVector<PreviewSurface::PipelineStageSnapshot>()> liveSnapshots;
    };

    explicit PipelineTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~PipelineTab() override = default;

    void refresh();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void showHoverPreview(int row);
    void hideHoverPreview();
    void refreshIfVisible();

    Widgets m_widgets;
    Dependencies m_deps;
    QVector<PreviewSurface::PipelineStageSnapshot> m_snapshots;
    QTimer m_liveRefreshTimer;
    QLabel* m_hoverPreview = nullptr;
    int m_hoverRow = -1;
};
