#pragma once

#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QTransform>

#include <QHash>
#include <QVector>

enum class PreviewOverlayKind {
    VisualClip,
    TranscriptOverlay,
};

struct PreviewOverlayInfo {
    PreviewOverlayKind kind = PreviewOverlayKind::VisualClip;
    QRectF bounds;
    QRectF rightHandle;
    QRectF bottomHandle;
    QRectF cornerHandle;
    QTransform clipTransform;
    QSizeF clipPixelSize;
};

struct PreviewOverlayModel {
    QHash<QString, PreviewOverlayInfo> overlays;
    QVector<QString> paintOrder;
};
