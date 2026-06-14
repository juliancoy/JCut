#pragma once

#include "render.h"

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QString>

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

struct SpeakerLabelOverlaySpec {
    QString name;
    QString organization;
    bool showName = false;
    bool showOrganization = false;
    qreal nameTextScale = 1.0;
    qreal organizationTextScale = 1.0;
    qreal nameVerticalPosition = 0.86;
    qreal organizationVerticalPosition = 0.93;
    QString fontFamily = kDefaultFontFamily;
    QColor nameColor = QColor(QStringLiteral("#f4f8fc"));
    QColor organizationColor = QColor(QStringLiteral("#b9d0e5"));
    QColor backgroundColor = QColor(8, 13, 20, 190);
    QColor borderColor = QColor(225, 236, 247, 120);
    qreal backgroundCornerRadius = 14.0;
    qreal borderWidth = 1.0;
    bool showShadow = true;
    QColor shadowColor = QColor(0, 0, 0, 190);
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

    virtual OverlayImage renderSpeakerLabelOverlay(const QSize& imageSize,
                                                   const SpeakerLabelOverlaySpec& spec) = 0;
};

TitleLayoutMetrics measureOverlayTitleLayout(const EvaluatedTitle& title, qreal fontScale = 1.0);

OverlayRenderBackend& overlayRenderBackend();
void setOverlayRenderBackendForTesting(OverlayRenderBackend* backend);

} // namespace render_detail
