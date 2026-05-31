#pragma once

#include "editor_shared.h"
#include "cpu_overlay_render_backend.h"
#include "preview_interaction_state.h"

#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QPixmap>
#include <QString>
#include <QVector>

struct HoverSpeakerProfile {
    QString speakerId;
    QString name;
    QString organization;
    QString description;
    QString imagePath;
};

struct CurrentSpeakerLabel {
    QString speakerId;
    QString name;
    QString organization;
};

const HoverSpeakerProfile* hoverSpeakerProfileFor(const QString& transcriptPath, const QString& speakerId);
QPixmap hoverSpeakerImage(const HoverSpeakerProfile& profile, int edgePx);
QPixmap fallbackSpeakerAvatar(const QString& speakerId, const QString& displayName, int edgePx);

QList<TimelineClip> activeAudioClipsForState(const PreviewInteractionState* state);
CurrentSpeakerLabel currentSpeakerLabelForState(const PreviewInteractionState* state);
QJsonObject currentSpeakerLabelDebugForState(const PreviewInteractionState* state);
render_detail::SpeakerLabelOverlaySpec currentSpeakerLabelOverlaySpecForState(const PreviewInteractionState* state);
QString speakerAtSourceFrame(const QVector<TranscriptSection>& sections, int64_t sourceFrame);
QColor speakerColor(const QString& speakerId, int alpha);
void fillShortUnknownSpeakerGaps(QVector<int>* speakerIndexByBin, int maxGapBins);
