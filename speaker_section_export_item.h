#pragma once

#include <QString>

#include <cstdint>

struct SpeakerSectionExportItem {
    QString speakerId;
    qint64 sourceStartFrame = -1;
    qint64 sourceEndFrame = -1;
    QString snippet;
    QString speakerDisplayName;
    int sectionOrdinal = 0;
    int wordCount = 0;
};
