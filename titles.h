#pragma once

#include "editor_shared.h"

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QRect>
#include <QString>

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
    bool dropShadowEnabled = true;
    QColor dropShadowColor = QColor(Qt::black);
    qreal dropShadowOpacity = 0.6;
    qreal dropShadowOffsetX = 2.0;
    qreal dropShadowOffsetY = 2.0;
    bool windowEnabled = false;
    QColor windowColor = QColor(Qt::black);
    qreal windowOpacity = 0.35;
    qreal windowPadding = 16.0;
    bool windowFrameEnabled = false;
    QColor windowFrameColor = QColor(Qt::white);
    qreal windowFrameOpacity = 1.0;
    qreal windowFrameWidth = 2.0;
    qreal windowFrameGap = 4.0;
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
TitleLayoutMetrics measureTitleLayout(const QFont& font, const QString& text);

// Create a default title clip ready for insertion into the timeline.
// Returns a TimelineClip with ClipMediaType::Title, one default TitleKeyframe,
// and no filePath (titles have no source media).
TimelineClip createDefaultTitleClip(int64_t startFrame, int trackIndex,
                                     int64_t durationFrames = 90);

// Draw the evaluated title onto a QPainter at the given output rectangle.
// The painter should already be set up for the composite canvas.
// outputSize is the logical output resolution (e.g. 1080x1920) — title x/y
// are stored in output-canvas coordinates and scaled to widget pixels at render time.
void drawTitleOverlay(QPainter* painter, const QRect& canvasRect,
                      const EvaluatedTitle& title, const QSize& outputSize);
