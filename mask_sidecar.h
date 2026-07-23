#pragma once

#include <QString>
#include <QVector>

#include <cstdint>

struct TimelineClip;

namespace editor::masks {

struct MaskSidecar {
    QString id;
    QString displayName;
    QString directory;
    QString sourceType = QStringLiteral("sam3_binary_frames");
    QString frameDomain;
    int frameCount = 0;
    int64_t firstFrame = -1;
    int64_t lastFrame = -1;
    bool frameIndexMapAvailable = false;
    bool frameIndexMetadataAvailable = false;
    int64_t mappedFrameCount = 0;
    int64_t firstMappedSourceFrame = -1;
    int64_t lastMappedSourceFrame = -1;
    int64_t lastMappedMaskFrame = -1;
    bool frameCoverageComplete = true;
    bool completionConfirmed = true;
    bool decodeOrdinalFrames = false;
    QString readinessIssue;

    bool isValid() const { return !id.isEmpty() && !directory.isEmpty() && frameCount > 0; }
    bool isReadyForTimeline() const
    {
        return isValid() &&
               (!decodeOrdinalFrames ||
                (frameIndexMapAvailable && frameIndexMetadataAvailable &&
                 frameCoverageComplete && completionConfirmed));
    }
};

QString stableMaskSidecarId(const QString& directory);
bool maskSidecarUsesDecodeOrdinalFrames(const QString& directory);
bool maskSidecarCompletionConfirmedForRender(const QString& directory,
                                             int64_t lastMappedMaskFrame,
                                             const QString& sourceMediaPath = {});
MaskSidecar inspectMaskSidecar(const QString& directory,
                               const QString& mediaStem = {},
                               const QString& sourceMediaPath = {});
QVector<MaskSidecar> discoverMaskSidecars(const TimelineClip& clip);
bool hasReadyMaskSidecar(const TimelineClip& clip);

} // namespace editor::masks
