#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace jcut {

struct FaceContinuityTrackCore {
    int trackId = -1;
    std::string streamId;
    std::string state;
    std::int64_t firstFrame = -1;
    std::int64_t lastFrame = -1;
    std::size_t sampleCount = 0;
    double x = 0.5;
    double y = 0.5;
    double box = 0.2;
    double score = 0.0;
};

struct FaceArtifactInspectionCore {
    std::string transcriptPath;
    std::string facedetectionsPath;
    std::string identityPath;
    std::string resolvedClipId;
    std::string detectorMode;
    std::string runId;
    std::string frameDomain;
    std::int64_t rawFrameCount = 0;
    std::size_t identityClusterCount = 0;
    std::size_t identityAssignmentCount = 0;
    std::vector<FaceContinuityTrackCore> tracks;
    std::string warning;
    std::string error;

    bool ok() const noexcept { return error.empty() && !tracks.empty(); }
};

struct SpeakerTrackAssignmentCore {
    int trackId = -1;
    std::string identityId;
    std::string streamId;
    std::int64_t sourceFrame = -1;
    double rotationDegrees = 0.0;
};

struct FaceTrackingSampleCore {
    double x = 0.5;
    double y = 0.5;
    double box = -1.0;
    double score = 0.0;
    bool valid = false;
};

std::vector<std::string> faceArtifactCandidatePaths(
    const std::string& transcriptPath,
    bool processed = false);
std::string faceIdentityArtifactPath(const std::string& transcriptPath);
std::int64_t faceTrackAnchorTimelineFrame(
    std::int64_t sourceFrame,
    std::int64_t sourceInFrame,
    std::int64_t clipTimelineStart,
    std::int64_t clipTimelineDuration,
    double playbackRate);
bool loadJcutBoxDocument(const std::string& path,
                         nlohmann::json* rootOut,
                         std::string* errorOut = nullptr);
bool importGeneratedFaceArtifacts(const std::string& outputDirectory,
                                  const std::string& transcriptPath,
                                  const std::string& clipId,
                                  std::string* errorOut = nullptr);
FaceArtifactInspectionCore inspectFaceArtifacts(
    const std::string& transcriptPath,
    const std::string& clipId);
std::vector<SpeakerTrackAssignmentCore> transcriptSpeakerTrackAssignments(
    const nlohmann::json& transcriptRoot,
    const std::string& clipId);
std::vector<SpeakerTrackAssignmentCore>
transcriptSpeakerTrackAssignmentsAtFrame(
    const nlohmann::json& transcriptRoot,
    const std::string& clipId,
    const std::string& speakerId,
    std::int64_t sourceFrame);
FaceTrackingSampleCore sampleFaceContinuityTrack(
    const std::string& transcriptPath,
    const std::string& clipId,
    int trackId,
    const std::string& streamId,
    std::int64_t sourceFrame,
    double minConfidence,
    std::int64_t sourceInFrame = 0,
    double localTimelineFrame = -1.0,
    int gapHoldFrames = 0,
    int centerSmoothingFrames = 0,
    int zoomSmoothingFrames = 0,
    int smoothingMode = 0,
    double centerSmoothingStrength = 1.0,
    double zoomSmoothingStrength = 1.0);

} // namespace jcut
