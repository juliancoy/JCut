#pragma once

#include "preview_surface.h"

#include <QObject>
#include <QVulkanInstance>
#include <QListWidget>
#include <QTimer>
#include <QVector>
#include <QPointer>
#include <functional>
#include <memory>

class QLabel;
class QWidget;
class QVulkanInstance;
class QVulkanWindow;

class PipelineTab : public QObject
{
    Q_OBJECT

public:
    struct Widgets
    {
        QWidget* pipelinePreviewHost = nullptr;
        QListWidget* pipelineStageList = nullptr;
    };

    struct Dependencies
    {
        std::function<bool()> useVulkanVisualization;
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
    std::unique_ptr<QVulkanInstance> m_vulkanInstance;
    QPointer<QVulkanWindow> m_vulkanWindow;
    QPointer<QWidget> m_vulkanContainer;
};
