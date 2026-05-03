#pragma once

#include <QString>

class QWidget;
class PreviewSurface;

struct PreviewBackendDecision {
    QString requested;
    QString effective;
    QString reason;
    bool fallbackApplied = false;
};

PreviewSurface* createPreviewSurfaceForConfiguredBackend(QWidget* parent = nullptr);
PreviewSurface* createPreviewSurfaceForConfiguredBackend(QWidget* parent,
                                                         PreviewBackendDecision* decisionOut);
