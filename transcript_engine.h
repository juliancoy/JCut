#pragma once

#include <QHash>
#include <QJsonDocument>
#include <QString>
#include <QVector>

#include "editor_playback_types.h"
#include "editor_timeline_types.h"

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
    QString facestreamArtifactPath(const QString &transcriptPath) const;
    bool loadFacestreamArtifact(const QString &transcriptPath, QJsonObject *rootOut) const;
    bool saveFacestreamArtifact(const QString &transcriptPath, const QJsonObject &root) const;
    QString facestreamProcessedArtifactPath(const QString &transcriptPath) const;
    bool loadFacestreamProcessedArtifact(const QString &transcriptPath, QJsonObject *rootOut) const;
    bool saveFacestreamProcessedArtifact(const QString &transcriptPath, const QJsonObject &root) const;
    QString identityArtifactPath(const QString &transcriptPath) const;
    bool loadIdentityArtifact(const QString &transcriptPath, QJsonObject *rootOut) const;
    bool saveIdentityArtifact(const QString &transcriptPath, const QJsonObject &root) const;

    int64_t adjustedLocalFrameForClip(const TimelineClip &clip,
                                      int64_t localTimelineFrame,
                                      const QVector<RenderSyncMarker> &markers) const;

    void appendMergedExportFrame(QVector<ExportRangeSegment> &ranges, int64_t frame) const;

    QVector<ExportRangeSegment> transcriptWordExportRanges(const QVector<ExportRangeSegment> &baseRanges,
                                                           const QVector<TimelineClip> &clips,
                                                           const QVector<RenderSyncMarker> &markers,
                                                           int transcriptPrependMs,
                                                           int transcriptPostpendMs) const;
    QVector<ExportRangeSegment> transcriptWordExportRangesDiscrete(const QVector<ExportRangeSegment> &baseRanges,
                                                                   const QVector<TimelineClip> &clips,
                                                                   const QVector<RenderSyncMarker> &markers,
                                                                   int transcriptPrependMs,
                                                                   int transcriptPostpendMs,
                                                                   int neighborWordRadius = 0) const;

    void invalidateCache();

private:
    struct TranscriptSourceWordRangeCacheEntry {
        qint64 modifiedMs = -1;
        qint64 fileSize = -1;
        int prependMs = 0;
        int postpendMs = 0;
        QVector<ExportRangeSegment> sourceWordRanges;
        bool valid = false;
    };

    bool transcriptSourceWordRanges(const QString& transcriptPath,
                                    int transcriptPrependMs,
                                    int transcriptPostpendMs,
                                    QVector<ExportRangeSegment>* rangesOut) const;

    mutable QHash<QString, TranscriptSourceWordRangeCacheEntry> m_transcriptSourceWordRangesCache;
    mutable QHash<QString, QVector<ExportRangeSegment>> m_transcriptWordRangesCache;
    mutable QString m_transcriptWordRangesCacheSignature;
    mutable QVector<ExportRangeSegment> m_transcriptWordRangesMergedCache;
    mutable QString m_transcriptWordRangesDiscreteCacheSignature;
    mutable QVector<ExportRangeSegment> m_transcriptWordRangesDiscreteCache;
};

} // namespace editor
