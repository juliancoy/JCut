#include "overlay_text_style.h"

void OverlayTextStyle::normalize()
{
    fontSize = qBound<qreal>(1.0, fontSize, 999.0);
    textOpacity = qBound<qreal>(0.0, textOpacity, 1.0);
    shadowOpacity = qBound<qreal>(0.0, shadowOpacity, 1.0);
    backgroundOpacity = qBound<qreal>(0.0, backgroundOpacity, 1.0);
    backgroundPadding = qBound<qreal>(0.0, backgroundPadding, 400.0);
    backgroundCornerRadius = qBound<qreal>(0.0, backgroundCornerRadius, 128.0);
    frameOpacity = qBound<qreal>(0.0, frameOpacity, 1.0);
    frameWidth = qBound<qreal>(0.0, frameWidth, 120.0);
    frameGap = qBound<qreal>(0.0, frameGap, 200.0);
    outlineOpacity = qBound<qreal>(0.0, outlineOpacity, 1.0);
    outlineWidth = qBound<qreal>(0.0, outlineWidth, 24.0);
}

OverlayTextStyle overlayTextStyleFromTitle(const TimelineClip::TitleKeyframe& k)
{
    OverlayTextStyle s;
    s.fontFamily=k.fontFamily; s.fontSize=k.fontSize; s.bold=k.bold; s.italic=k.italic;
    s.textColor=k.color; s.textOpacity=k.opacity;
    s.shadowEnabled=k.dropShadowEnabled; s.shadowColor=k.dropShadowColor;
    s.shadowOpacity=k.dropShadowOpacity; s.shadowOffsetX=k.dropShadowOffsetX; s.shadowOffsetY=k.dropShadowOffsetY;
    s.backgroundEnabled=k.windowEnabled; s.backgroundColor=k.windowColor;
    s.backgroundOpacity=k.windowOpacity; s.backgroundPadding=k.windowPadding;
    s.frameEnabled=k.windowFrameEnabled; s.frameColor=k.windowFrameColor;
    s.frameOpacity=k.windowFrameOpacity; s.frameWidth=k.windowFrameWidth; s.frameGap=k.windowFrameGap;
    s.normalize(); return s;
}

OverlayTextStyle overlayTextStyleFromTranscript(const TimelineClip::TranscriptOverlaySettings& o)
{
    OverlayTextStyle s;
    s.fontFamily=o.fontFamily; s.fontSize=o.fontPointSize; s.bold=o.bold; s.italic=o.italic;
    s.textColor=o.textColor; s.textOpacity=o.textOpacity;
    s.shadowEnabled=o.showShadow; s.shadowColor=o.shadowColor; s.shadowOpacity=o.shadowOpacity;
    s.shadowOffsetX=o.shadowOffsetX; s.shadowOffsetY=o.shadowOffsetY;
    s.backgroundEnabled=o.showBackground; s.backgroundColor=o.backgroundColor;
    s.backgroundOpacity=o.backgroundOpacity; s.backgroundPadding=o.backgroundPadding;
    s.backgroundCornerRadius=o.backgroundCornerRadius; s.frameEnabled=o.backgroundFrameEnabled;
    s.frameColor=o.backgroundFrameColor; s.frameOpacity=o.backgroundFrameOpacity;
    s.frameWidth=o.backgroundFrameWidth; s.frameGap=o.backgroundFrameGap;
    s.outlineEnabled=o.textOutlineEnabled; s.outlineColor=o.textOutlineColor;
    s.outlineOpacity=o.textOutlineOpacity; s.outlineWidth=o.textOutlineWidth;
    s.normalize(); return s;
}

void applyOverlayTextStyle(const OverlayTextStyle& input, TimelineClip::TitleKeyframe* k)
{
    if (!k) return; OverlayTextStyle s=input; s.normalize();
    k->fontFamily=s.fontFamily; k->fontSize=s.fontSize; k->bold=s.bold; k->italic=s.italic;
    k->color=s.textColor; k->opacity=s.textOpacity; k->dropShadowEnabled=s.shadowEnabled;
    k->dropShadowColor=s.shadowColor; k->dropShadowOpacity=s.shadowOpacity;
    k->dropShadowOffsetX=s.shadowOffsetX; k->dropShadowOffsetY=s.shadowOffsetY;
    k->windowEnabled=s.backgroundEnabled; k->windowColor=s.backgroundColor;
    k->windowOpacity=s.backgroundOpacity; k->windowPadding=s.backgroundPadding;
    k->windowFrameEnabled=s.frameEnabled; k->windowFrameColor=s.frameColor;
    k->windowFrameOpacity=s.frameOpacity; k->windowFrameWidth=s.frameWidth; k->windowFrameGap=s.frameGap;
}

void applyOverlayTextStyle(const OverlayTextStyle& input, TimelineClip::TranscriptOverlaySettings* o)
{
    if (!o) return; OverlayTextStyle s=input; s.normalize();
    o->fontFamily=s.fontFamily; o->fontPointSize=qRound(s.fontSize); o->bold=s.bold; o->italic=s.italic;
    o->textColor=s.textColor; o->textOpacity=s.textOpacity; o->showShadow=s.shadowEnabled;
    o->shadowColor=s.shadowColor; o->shadowOpacity=s.shadowOpacity;
    o->shadowOffsetX=s.shadowOffsetX; o->shadowOffsetY=s.shadowOffsetY;
    o->showBackground=s.backgroundEnabled; o->backgroundColor=s.backgroundColor;
    o->backgroundOpacity=s.backgroundOpacity; o->backgroundPadding=s.backgroundPadding;
    o->backgroundCornerRadius=s.backgroundCornerRadius; o->backgroundFrameEnabled=s.frameEnabled;
    o->backgroundFrameColor=s.frameColor; o->backgroundFrameOpacity=s.frameOpacity;
    o->backgroundFrameWidth=s.frameWidth; o->backgroundFrameGap=s.frameGap;
    o->textOutlineEnabled=s.outlineEnabled; o->textOutlineColor=s.outlineColor;
    o->textOutlineOpacity=s.outlineOpacity; o->textOutlineWidth=s.outlineWidth; o->normalizeReadableBounds();
}
