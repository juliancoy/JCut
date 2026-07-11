#pragma once

#include <QColor>
#include <QString>
#include "editor_timeline_types.h"

// Renderer-neutral style contract shared by authored titles, generated speaker
// titles, and transcript captions. Format-specific behavior remains in its
// owning model; common editing, presets, and comparisons use this value type.
struct OverlayTextStyle {
    QString fontFamily;
    qreal fontSize = 48.0;
    bool bold = true;
    bool italic = false;
    QColor textColor = Qt::white;
    qreal textOpacity = 1.0;

    bool shadowEnabled = true;
    QColor shadowColor = Qt::black;
    qreal shadowOpacity = 0.6;
    qreal shadowOffsetX = 2.0;
    qreal shadowOffsetY = 2.0;

    bool backgroundEnabled = false;
    QColor backgroundColor = Qt::black;
    qreal backgroundOpacity = 0.35;
    qreal backgroundPadding = 16.0;
    qreal backgroundCornerRadius = 0.0;

    bool frameEnabled = false;
    QColor frameColor = Qt::white;
    qreal frameOpacity = 1.0;
    qreal frameWidth = 2.0;
    qreal frameGap = 4.0;

    bool outlineEnabled = false;
    QColor outlineColor = Qt::black;
    qreal outlineOpacity = 0.8;
    qreal outlineWidth = 0.0;

    void normalize();
};

OverlayTextStyle overlayTextStyleFromTitle(const TimelineClip::TitleKeyframe& keyframe);
OverlayTextStyle overlayTextStyleFromTranscript(const TimelineClip::TranscriptOverlaySettings& settings);
void applyOverlayTextStyle(const OverlayTextStyle& style, TimelineClip::TitleKeyframe* keyframe);
void applyOverlayTextStyle(const OverlayTextStyle& style, TimelineClip::TranscriptOverlaySettings* settings);
