#pragma once

class QWidget;
class PreviewSurface;

PreviewSurface* createPreviewWidgetForConfiguredBackend(QWidget* parent = nullptr);
