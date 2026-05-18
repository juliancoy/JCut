#pragma once

#include "render.h"

#include <QByteArray>
#include <QHash>
#include <QImage>

struct EvaluatedTitle;
struct TitleLayoutMetrics;

namespace render_detail {

struct OverlayImage {
    int width = 0;
    int height = 0;
    QByteArray rgbaPremultiplied;

    bool isNull() const
    {
        return width <= 0 || height <= 0 || rgbaPremultiplied.size() < (width * height * 4);
    }

    QImage asQImageView() const;
};

class OverlayRenderBackend {
public:
    virtual ~OverlayRenderBackend() = default;

    virtual OverlayImage renderTitleOverlay(const QSize& imageSize,
                                            const EvaluatedTitle& title,
                                            const QSize& outputSize) = 0;

    virtual OverlayImage renderTranscriptOverlay(const QSize& imageSize,
                                                 const RenderRequest& request,
                                                 int64_t timelineFrame,
                                                 const QVector<TimelineClip>& orderedClips,
                                                 QHash<QString, QVector<TranscriptSection>>& transcriptCache) = 0;
};

TitleLayoutMetrics measureOverlayTitleLayout(const EvaluatedTitle& title, qreal fontScale = 1.0);

OverlayRenderBackend& overlayRenderBackend();
void setOverlayRenderBackendForTesting(OverlayRenderBackend* backend);

} // namespace render_detail
