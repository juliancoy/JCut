#pragma once

#include "editor_shared.h"
#include "preview_interaction_state.h"

#include <QColor>
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

const HoverSpeakerProfile* hoverSpeakerProfileFor(const QString& transcriptPath, const QString& speakerId);
QPixmap hoverSpeakerImage(const HoverSpeakerProfile& profile, int edgePx);
QPixmap fallbackSpeakerAvatar(const QString& speakerId, const QString& displayName, int edgePx);

QList<TimelineClip> activeAudioClipsForState(const PreviewInteractionState* state);
QString speakerAtSourceFrame(const QVector<TranscriptSection>& sections, int64_t sourceFrame);
QColor speakerColor(const QString& speakerId, int alpha);
void fillShortUnknownSpeakerGaps(QVector<int>* speakerIndexByBin, int maxGapBins);
