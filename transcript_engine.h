#pragma once

#include <QHash>
#include <QJsonDocument>
#include <QString>
#include <QVector>

#include "editor_shared.h"

namespace editor
{

class TranscriptEngine
{
public:
    QString transcriptPathForClip(const TimelineClip &clip) const;
    QString secondsToTranscriptTime(double seconds) const;
    bool parseTranscriptTime(const QString &text, double *secondsOut) const;
    bool loadTranscriptJson(const QString &path, QJsonDocument *docOut, QString *errorOut = nullptr) const;
    bool saveTranscriptJson(const QString &path, const QJsonDocument &doc) const;
    bool ensureTranscriptTextCompanion(const QString &path) const;
    bool loadBoxstreamArtifact(const QString &transcriptPath, QJsonObject *rootOut) const;
    bool saveBoxstreamArtifact(const QString &transcriptPath, const QJsonObject &root) const;

    int64_t adjustedLocalFrameForClip(const TimelineClip &clip,
                                      int64_t localTimelineFrame,
                                      const QVector<RenderSyncMarker> &markers) const;

    void appendMergedExportFrame(QVector<ExportRangeSegment> &ranges, int64_t frame) const;

    QVector<ExportRangeSegment> transcriptWordExportRanges(const QVector<ExportRangeSegment> &baseRanges,
                                                           const QVector<TimelineClip> &clips,
                                                           const QVector<RenderSyncMarker> &markers,
                                                           int transcriptPrependMs,
                                                           int transcriptPostpendMs) const;

    void invalidateCache();

private:
    mutable QHash<QString, QVector<ExportRangeSegment>> m_transcriptWordRangesCache;
    mutable QString m_transcriptWordRangesCacheSignature;
    mutable QVector<ExportRangeSegment> m_transcriptWordRangesMergedCache;
};

} // namespace editor
