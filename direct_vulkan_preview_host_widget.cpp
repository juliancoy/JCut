#include "direct_vulkan_preview_backend.h"

#include "direct_vulkan_preview_interaction.h"
#include "preview_view_transform.h"

#include <QWheelEvent>
#include <QWidget>

namespace {

class DirectVulkanPreviewHostWidget final : public QWidget {
public:
    DirectVulkanPreviewHostWidget(PreviewInteractionState* state,
                                  std::function<void()> updateCallback,
                                  QWidget* parent = nullptr)
        : QWidget(parent), m_state(state), m_updateCallback(std::move(updateCallback)) {}

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        if (!event || !m_state || event->angleDelta().y() == 0) {
            QWidget::wheelEvent(event);
            return;
        }
        const QRectF surfaceRect = PreviewViewTransform::rectForWidget(
            this, PreviewSurfaceCoordinateSpace::DeviceSurface);
        const QPointF surfacePosition = PreviewViewTransform::pointForWidgetPoint(
            this, event->position(), PreviewSurfaceCoordinateSpace::DeviceSurface);
        if (m_state->viewMode == PreviewSurface::ViewMode::Audio) {
            if (jcut::direct_vulkan_preview::applyAudioPreviewWheelZoom(
                    m_state, surfaceRect, surfacePosition, event->angleDelta().y())) {
                if (m_updateCallback) {
                    m_updateCallback();
                }
                event->accept();
                return;
            }
            QWidget::wheelEvent(event);
            return;
        }
        if (m_state->viewMode == PreviewSurface::ViewMode::Audio) {
            QWidget::wheelEvent(event);
            return;
        }
        if (jcut::direct_vulkan_preview::applyVideoPreviewWheelZoom(
                m_state, surfaceRect, surfacePosition, event->angleDelta().y())) {
            if (m_updateCallback) {
                m_updateCallback();
            }
            event->accept();
            return;
        }
        QWidget::wheelEvent(event);
    }

private:
    PreviewInteractionState* m_state = nullptr;
    std::function<void()> m_updateCallback;
};

} // namespace

QWidget* createDirectVulkanPreviewHostWidget(PreviewInteractionState* state,
                                             std::function<void()> updateCallback,
                                             QWidget* parent)
{
    return new DirectVulkanPreviewHostWidget(state, std::move(updateCallback), parent);
}
