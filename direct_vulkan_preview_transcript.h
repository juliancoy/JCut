#pragma once

#include "editor_shared_core.h"
#include "preview_interaction_state.h"

#include <QHash>
#include <QRectF>
#include <QSize>
#include <QString>

namespace jcut::direct_vulkan_preview {

struct PreparedTranscriptOverlay {
    TimelineClip clip;
    TranscriptOverlayLayout layout;
    QRectF outputRect;
    QRectF bounds;
    QString speakerTitle;
    bool ready = false;
};

using PreparedTranscriptOverlayMap = QHash<QString, PreparedTranscriptOverlay>;

PreparedTranscriptOverlayMap collectPreparedTranscriptOverlays(const PreviewInteractionState* state,
                                                               const QSize& swapSize);
QString transcriptOverlayTextPrepMaterial(const PreparedTranscriptOverlayMap& overlays,
                                          const QSize& outputSize);

} // namespace jcut::direct_vulkan_preview
