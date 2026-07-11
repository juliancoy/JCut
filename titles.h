#pragma once

#include "editor_shared.h"
#include "cpu_overlay_render_backend.h"

#include <QColor>
#include <QRect>
#include <QString>

using TitleMaterialStyle = TimelineClip::TitleKeyframe::MaterialStyle;
using TextExtrudeMode = TimelineClip::TitleKeyframe::TextExtrudeMode;

// Evaluated title state at a specific frame — the result of interpolating
// between TitleKeyframes. Used by both the preview renderer and the export pipeline.
struct EvaluatedTitle {
    QString text;
    qreal x = 0.0;
    qreal y = 0.0;
    qreal fontSize = 48.0;
    qreal opacity = 1.0;
    QString fontFamily = kDefaultFontFamily;
    bool bold = true;
    bool italic = false;
    QColor color = QColor(Qt::white);
    QString logoPath;
    TitleMaterialStyle textMaterialStyle = TitleMaterialStyle::Solid;
    QString textPatternImagePath;
    qreal textPatternScale = 1.0;
    bool dropShadowEnabled = true;
    QColor dropShadowColor = QColor(Qt::black);
    qreal dropShadowOpacity = 0.6;
    qreal dropShadowOffsetX = 2.0;
    qreal dropShadowOffsetY = 2.0;
    bool windowEnabled = false;
    QColor windowColor = QColor(Qt::black);
    qreal windowOpacity = 0.35;
    qreal windowPadding = 16.0;
    qreal windowWidth = 0.0;
    bool windowFrameEnabled = false;
    QColor windowFrameColor = QColor(Qt::white);
    qreal windowFrameOpacity = 1.0;
    qreal windowFrameWidth = 2.0;
    qreal windowFrameGap = 4.0;
    TitleMaterialStyle windowFrameMaterialStyle = TitleMaterialStyle::Solid;
    QString windowFramePatternImagePath;
    qreal windowFramePatternScale = 1.0;
    bool vulkan3DEnabled = false;
    bool vulkan3DExtrudeEnabled = false;
    TextExtrudeMode textExtrudeMode = TextExtrudeMode::None;
    qreal vulkan3DExtrudeDepth = 0.0;
    qreal vulkan3DBevelScale = 0.0;
    qreal vulkan3DYawDegrees = 0.0;
    qreal vulkan3DPitchDegrees = 0.0;
    qreal vulkan3DRollDegrees = 0.0;
    qreal vulkan3DDepth = 0.0;
    qreal vulkan3DScale = 1.0;
    bool valid = false;  // false if no keyframes exist
};

struct TitleLayoutMetrics {
    qreal width = 0.0;
    qreal lineHeight = 0.0;
    int lineCount = 0;
    qreal height = 0.0;
};

// Evaluate the title state for a clip at a given local frame (0-based within the clip).
EvaluatedTitle evaluateTitleAtLocalFrame(const TimelineClip& clip, int64_t localFrame);
EvaluatedTitle composeTitleWithOpacity(const EvaluatedTitle& title, qreal opacityMultiplier);
TitleLayoutMetrics measureTitleLayout(const EvaluatedTitle& title, qreal fontScale = 1.0);

// Create a default title clip ready for insertion into the timeline.
// Returns a TimelineClip with ClipMediaType::Title, one default TitleKeyframe,
// and no filePath (titles have no source media).
TimelineClip createDefaultTitleClip(int64_t startFrame, int trackIndex,
                                     int64_t durationFrames = 90);

render_detail::OverlayImage renderTitleOverlay(const QSize& imageSize,
                                               const EvaluatedTitle& title,
                                               const QSize& outputSize);
