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

struct TranscriptOverlayCollectionStats {
    int candidateCount = 0;
    int preparedCount = 0;
    QString lastSkipReason;
    QString lastPreparedClipId;
    QString lastPreparedTranscriptPath;
    QString lastPreparedTimingSource;
    int64_t lastPreparedTimelineSample = -1;
    int64_t lastPreparedTranscriptFrame = -1;
    int64_t lastPreparedPresentedMediaSourceFrame = -1;
};

PreparedTranscriptOverlayMap collectPreparedTranscriptOverlays(const PreviewInteractionState* state,
                                                               const QSize& swapSize,
                                                               TranscriptOverlayCollectionStats* stats = nullptr);
QString transcriptOverlayTextPrepMaterial(const PreparedTranscriptOverlayMap& overlays,
                                          const QSize& outputSize);

} // namespace jcut::direct_vulkan_preview
