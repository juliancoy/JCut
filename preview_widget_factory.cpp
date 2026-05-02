#include "preview_widget_factory.h"

#include "preview_surface_factory.h"
#include "preview_surface.h"

PreviewSurface* createPreviewWidgetForConfiguredBackend(QWidget* parent)
{
    return createPreviewSurfaceForConfiguredBackend(parent);
}
