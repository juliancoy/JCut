#pragma once

#include "editor_shared.h"
#include "facefind_window.h"

#include <QJsonArray>
#include <QString>
#include <QVector>

#include <functional>

namespace jcut::facestream_assignment {

struct CropExtractionRequest {
    TimelineClip clip;
    QVector<RenderSyncMarker> renderSyncMarkers;
    QString mediaPath;
    QString cropsDir;
    QString videoStem;
    QJsonArray streams;
};

struct CropExtractionResult {
    bool ok = false;
    bool canceled = false;
    QString errorMessage;
    QVector<facefind::Candidate> candidates;
    QJsonArray candidateRows;
};

struct ClusterRequest {
    QVector<facefind::Candidate> trackCandidates;
    QString arcfaceParamPath;
    QString arcfaceBinPath;
};

struct TrackIdentityEvidence {
    int trackId = -1;
    QVector<facefind::Candidate> cropSamples;
    std::vector<float> embedding;
    bool hasEmbedding = false;
};

struct ClusterResult {
    bool ok = false;
    QString cancelStageMessage;

    QVector<facefind::Candidate> clusterCandidates;
    QJsonArray clusterRows;
    QJsonArray clusterDiagnosticsRows;

    bool embeddingReady = false;
    QString embeddingError;
    int embeddedTrackCount = 0;
    int autoClusterPairCount = 0;
    int reviewPairCount = 0;
    double autoClusterThreshold = 0.70;
    double reviewThreshold = 0.55;
};

struct AssignmentResolutionResult {
    QJsonArray overrides;
    QJsonArray auditLog;
    QJsonArray resolvedMap;
    QHash<QString, QVector<facefind::Candidate>> assignmentsBySpeaker;
};

QVector<QJsonObject> selectRepresentativeKeyframesForIdentity(
    const QJsonArray& keyframes,
    int maxRepresentativeCrops = 3,
    int minFrameSpacing = 24);

CropExtractionResult extractRepresentativeCrops(
    const CropExtractionRequest& request,
    const std::function<bool(int, const QString&)>& progress);

ClusterResult clusterTrackIdentityEvidence(
    const QVector<TrackIdentityEvidence>& trackEvidence,
    double autoClusterThreshold = 0.70,
    double reviewThreshold = 0.55);

AssignmentResolutionResult resolveTrackIdentityAssignments(
    const QJsonArray& assignmentTableRows,
    const QVector<facefind::Candidate>& trackCandidates,
    const QString& timestampUtc);

ClusterResult clusterFaceTracks(
    const ClusterRequest& request,
    const std::function<bool(int, const QString&)>& progress);

} // namespace jcut::facestream_assignment
