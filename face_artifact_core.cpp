#include "face_artifact_core.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

#include <zlib.h>

namespace jcut {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::uint32_t littleEndian32(const unsigned char* bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint32_t bigEndian32(const unsigned char* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
        (static_cast<std::uint32_t>(bytes[1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[2]) << 8U) |
        static_cast<std::uint32_t>(bytes[3]);
}

bool inflateQtCompressed(const unsigned char* bytes,
                         std::size_t size,
                         std::vector<unsigned char>* unpackedOut,
                         std::size_t* consumedOut = nullptr)
{
    if (!unpackedOut || size < 5) return false;
    const std::uint32_t expected = bigEndian32(bytes);
    if (expected == 0 || expected > 128U * 1024U * 1024U) return false;
    unpackedOut->assign(expected, 0);
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(bytes + 4);
    stream.avail_in = static_cast<uInt>(
        std::min<std::size_t>(size - 4, std::numeric_limits<uInt>::max()));
    stream.next_out = unpackedOut->data();
    stream.avail_out = static_cast<uInt>(unpackedOut->size());
    if (inflateInit(&stream) != Z_OK) return false;
    const int result = inflate(&stream, Z_FINISH);
    const std::size_t consumed = static_cast<std::size_t>(stream.total_in) + 4U;
    const bool ok = result == Z_STREAM_END &&
        stream.total_out == unpackedOut->size();
    inflateEnd(&stream);
    if (ok && consumedOut) *consumedOut = consumed;
    return ok;
}

bool loadQtBinaryJson(const fs::path& path, json* rootOut)
{
    std::ifstream input(path, std::ios::binary);
    const std::vector<unsigned char> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>{});
    if (bytes.size() < 13 ||
        bigEndian32(bytes.data()) != 0x4A435554U ||
        bigEndian32(bytes.data() + 4) != 1U) {
        return false;
    }
    const std::uint32_t byteArraySize = bigEndian32(bytes.data() + 8);
    if (byteArraySize > bytes.size() - 12U) return false;
    std::vector<unsigned char> unpacked;
    if (!inflateQtCompressed(
            bytes.data() + 12, byteArraySize, &unpacked)) {
        return false;
    }
    try {
        json root = json::parse(unpacked.begin(), unpacked.end());
        if (!root.is_object()) return false;
        if (rootOut) *rootOut = std::move(root);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<json> loadQtCompressedCborRecords(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    const std::vector<unsigned char> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>{});
    std::vector<json> records;
    std::size_t offset = 0;
    while (offset + 5 <= bytes.size()) {
        std::vector<unsigned char> unpacked;
        std::size_t consumed = 0;
        if (!inflateQtCompressed(
                bytes.data() + offset, bytes.size() - offset,
                &unpacked, &consumed) ||
            consumed == 0) {
            break;
        }
        try {
            json record = json::from_cbor(unpacked);
            if (record.is_object()) records.push_back(std::move(record));
        } catch (const std::exception&) {
            break;
        }
        offset += consumed;
    }
    return records;
}

bool writeJcutBoxJson(const fs::path& path, const json& root)
{
    const std::string payload = root.dump();
    std::string bytes("JCUTBOX1", 8);
    const auto appendLittle = [&](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<char>((value >> shift) & 0xffU));
        }
    };
    appendLittle(1);
    appendLittle(static_cast<std::uint32_t>(payload.size()));
    bytes += payload;
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    const fs::path temporary =
        path.string() + ".tmp-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output.good()) return false;
    }
    fs::rename(temporary, path, error);
    if (error) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

std::string stringValue(const json& object, const char* key)
{
    const auto value = object.find(key);
    return value != object.end() && value->is_string()
        ? value->get<std::string>() : std::string{};
}

double finiteNumber(const json& object, const char* key, double fallback)
{
    const auto value = object.find(key);
    if (value == object.end() || !value->is_number()) return fallback;
    const double number = value->get<double>();
    return std::isfinite(number) ? number : fallback;
}

const json* clipRoot(const json& root,
                     const char* collectionKey,
                     const std::string& requestedClipId,
                     std::string* resolvedClipId,
                     std::string* warning)
{
    const auto collection = root.find(collectionKey);
    if (collection == root.end() || !collection->is_object()) return nullptr;
    const auto exact = collection->find(requestedClipId);
    if (exact != collection->end() && exact->is_object()) {
        if (resolvedClipId) *resolvedClipId = requestedClipId;
        return &*exact;
    }
    if (collection->size() == 1 && collection->begin().value().is_object()) {
        if (resolvedClipId) *resolvedClipId = collection->begin().key();
        if (warning) {
            *warning = "Face artifact used its only clip entry because the persisted clip ID did not match.";
        }
        return &collection->begin().value();
    }
    return nullptr;
}

std::vector<FaceContinuityTrackCore> trackRows(const json& continuity)
{
    const json* rows = nullptr;
    const auto rawTracks = continuity.find("raw_tracks");
    const auto streams = continuity.find("streams");
    if (rawTracks != continuity.end() && rawTracks->is_array()) rows = &*rawTracks;
    else if (streams != continuity.end() && streams->is_array()) rows = &*streams;
    if (!rows) return {};

    std::vector<FaceContinuityTrackCore> tracks;
    for (const json& row : *rows) {
        if (!row.is_object()) continue;
        FaceContinuityTrackCore track;
        track.trackId = row.value("track_id", -1);
        if (track.trackId < 0) continue;
        track.streamId = stringValue(row, "stream_id");
        track.state = stringValue(row, "state");
        track.firstFrame = row.value("first_frame", std::int64_t{-1});
        track.lastFrame = row.value("last_frame", std::int64_t{-1});
        const json* samples = nullptr;
        const auto detections = row.find("detections");
        const auto keyframes = row.find("keyframes");
        if (detections != row.end() && detections->is_array()) samples = &*detections;
        else if (keyframes != row.end() && keyframes->is_array()) samples = &*keyframes;
        if (samples) {
            track.sampleCount = samples->size();
            if (!samples->empty() && samples->front().is_object()) {
                const json& sample = samples->front();
                const std::int64_t frame = sample.value("frame", std::int64_t{-1});
                if (track.firstFrame < 0) track.firstFrame = frame;
                if (track.lastFrame < 0) {
                    track.lastFrame = samples->back().is_object()
                        ? samples->back().value("frame", frame) : frame;
                }
                track.x = std::clamp(finiteNumber(sample, "x", 0.5), 0.0, 1.0);
                track.y = std::clamp(finiteNumber(sample, "y", 0.5), 0.0, 1.0);
                track.box = std::clamp(finiteNumber(sample, "box", 0.2), 0.01, 1.0);
                track.score = std::clamp(
                    finiteNumber(sample, "score",
                        finiteNumber(sample, "confidence", 0.0)),
                    0.0, 1.0);
            }
        } else {
            track.sampleCount = static_cast<std::size_t>(
                std::max(0, row.value("length", 0)));
        }
        tracks.push_back(std::move(track));
    }
    std::sort(tracks.begin(), tracks.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.trackId < rhs.trackId;
    });
    return tracks;
}

} // namespace

std::vector<std::string> faceArtifactCandidatePaths(
    const std::string& transcriptPath,
    bool processed)
{
    const fs::path path(transcriptPath);
    const fs::path directory = path.parent_path();
    const std::string stem = path.stem().string();
    if (processed) {
        return {
            (directory / (stem + "_facedetections_processed.bin")).string(),
            (directory / (stem + "_facestream_processed.bin")).string()};
    }
    return {
        (directory / (stem + "_facedetections.bin")).string(),
        (directory / (stem + "_facestream.bin")).string()};
}

std::string faceIdentityArtifactPath(const std::string& transcriptPath)
{
    const fs::path path(transcriptPath);
    return (path.parent_path() / (path.stem().string() + "_identity.bin")).string();
}

std::int64_t faceTrackAnchorTimelineFrame(
    std::int64_t sourceFrame,
    std::int64_t sourceInFrame,
    std::int64_t clipTimelineStart,
    std::int64_t clipTimelineDuration,
    double playbackRate)
{
    const std::int64_t safeStart = std::max<std::int64_t>(0, clipTimelineStart);
    const std::int64_t safeDuration =
        std::max<std::int64_t>(1, clipTimelineDuration);
    const double safeRate =
        std::isfinite(playbackRate) && playbackRate > 0.001
            ? playbackRate : 1.0;
    const double local = static_cast<double>(
        std::max<std::int64_t>(0, sourceFrame - sourceInFrame)) / safeRate;
    return std::clamp<std::int64_t>(
        safeStart + static_cast<std::int64_t>(std::llround(local)),
        safeStart,
        safeStart + safeDuration - 1);
}

bool loadJcutBoxDocument(const std::string& path,
                         json* rootOut,
                         std::string* errorOut)
{
    if (rootOut) *rootOut = json::object();
    if (errorOut) errorOut->clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (errorOut) *errorOut = "Artifact file not found.";
        return false;
    }
    const std::vector<char> storage(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>{});
    const auto* bytes =
        reinterpret_cast<const unsigned char*>(storage.data());
    constexpr std::array<unsigned char, 8> magic{
        'J', 'C', 'U', 'T', 'B', 'O', 'X', '1'};
    if (storage.size() < 16 ||
        !std::equal(magic.begin(), magic.end(), bytes)) {
        if (errorOut) *errorOut = "Artifact has an invalid JCUTBOX1 header.";
        return false;
    }
    const std::uint32_t version = littleEndian32(bytes + 8);
    const std::uint32_t storedSize = littleEndian32(bytes + 12);
    if (storedSize > storage.size() - 16U) {
        if (errorOut) *errorOut = "Artifact payload is truncated.";
        return false;
    }
    try {
        if (version == 1) {
            const std::string payload(
                reinterpret_cast<const char*>(bytes + 16), storedSize);
            json root = json::parse(payload);
            if (!root.is_object()) throw std::runtime_error("root is not an object");
            if (rootOut) *rootOut = std::move(root);
            return true;
        }
        if (version != 2 || storedSize < 4) {
            if (errorOut) *errorOut = "Artifact version is unsupported.";
            return false;
        }
        const unsigned char* compressed = bytes + 16;
        const std::uint32_t unpackedSize = bigEndian32(compressed);
        if (unpackedSize == 0 ||
            unpackedSize > static_cast<std::uint32_t>(128U * 1024U * 1024U)) {
            if (errorOut) *errorOut = "Artifact decompressed size is invalid.";
            return false;
        }
        std::vector<unsigned char> unpacked(unpackedSize);
        uLongf destinationSize = unpacked.size();
        const int result = uncompress(
            unpacked.data(),
            &destinationSize,
            compressed + 4,
            storedSize - 4);
        if (result != Z_OK || destinationSize != unpacked.size()) {
            if (errorOut) *errorOut = "Artifact decompression failed.";
            return false;
        }
        json root = json::from_cbor(unpacked);
        if (!root.is_object()) throw std::runtime_error("root is not an object");
        if (rootOut) *rootOut = std::move(root);
        return true;
    } catch (const std::exception& exception) {
        if (errorOut) *errorOut =
            std::string("Artifact payload is invalid: ") + exception.what();
        return false;
    }
}

bool importGeneratedFaceArtifacts(const std::string& outputDirectory,
                                  const std::string& transcriptPath,
                                  const std::string& clipId,
                                  std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    const fs::path directory(outputDirectory);
    json generated;
    if (!loadQtBinaryJson(
            directory / "continuity_facedetections.bin", &generated)) {
        if (errorOut) *errorOut =
            "Generated continuity artifact could not be decoded.";
        return false;
    }
    const json& generatedByClip =
        generated.value("continuity_facedetections_by_clip", json::object());
    if (!generatedByClip.is_object() || generatedByClip.empty() ||
        !generatedByClip.begin().value().is_object()) {
        if (errorOut) *errorOut =
            "Generated continuity artifact has no clip payload.";
        return false;
    }
    json continuity = generatedByClip.begin().value();
    json tracks = json::array();
    json frameSummaries = json::array();
    for (json& record :
         loadQtCompressedCborRecords(directory / "tracks.dat")) {
        const std::string type = stringValue(record, "type");
        if (type == "track") tracks.push_back(std::move(record));
        else if (type == "frame_summary") {
            frameSummaries.push_back(std::move(record));
        }
    }
    if (tracks.empty()) {
        if (errorOut) *errorOut =
            "Generated track artifact contains no readable tracks.";
        return false;
    }
    continuity["clip_id"] = clipId;
    continuity["raw_tracks"] = std::move(tracks);
    continuity["raw_tracks_count"] = continuity["raw_tracks"].size();
    continuity["raw_tracks_artifact_path"] =
        (directory / "tracks.idx").string();
    continuity["raw_frames_artifact_path"] =
        (directory / "detections.idx").string();
    continuity["continuity_artifact_path"] =
        (directory / "continuity_facedetections.bin").string();
    continuity["imported_from_artifact_dir"] = directory.string();
    continuity["media_sidecar_dir"] = directory.string();
    continuity["facedetections_part"] =
        (directory / "facedetections.part").string();
    continuity["summary_json"] = (directory / "summary.json").string();
    if (!frameSummaries.empty()) {
        continuity["raw_frame_summaries"] = std::move(frameSummaries);
    }

    json artifact = json::object();
    const std::string destination =
        faceArtifactCandidatePaths(transcriptPath, false).front();
    (void)loadJcutBoxDocument(destination, &artifact, nullptr);
    if (!artifact.is_object()) artifact = json::object();
    artifact["schema"] = "jcut_facedetections_v1";
    json& byClip = artifact["continuity_facedetections_by_clip"];
    if (!byClip.is_object()) byClip = json::object();
    byClip[clipId] = std::move(continuity);
    if (!writeJcutBoxJson(destination, artifact)) {
        if (errorOut) *errorOut =
            "Could not atomically write the transcript face artifact.";
        return false;
    }
    return true;
}

FaceArtifactInspectionCore inspectFaceArtifacts(
    const std::string& transcriptPath,
    const std::string& clipId)
{
    FaceArtifactInspectionCore inspection;
    inspection.transcriptPath = transcriptPath;
    json artifact;
    std::string loadError;
    for (const std::string& candidate :
         faceArtifactCandidatePaths(transcriptPath, false)) {
        if (loadJcutBoxDocument(candidate, &artifact, &loadError)) {
            inspection.facedetectionsPath = candidate;
            break;
        }
    }
    if (inspection.facedetectionsPath.empty()) {
        inspection.error = "No readable face-detection artifact was found.";
        return inspection;
    }
    const json* continuity = clipRoot(
        artifact, "continuity_facedetections_by_clip", clipId,
        &inspection.resolvedClipId, &inspection.warning);
    if (!continuity) {
        continuity = clipRoot(
            artifact, "continuity_facestreams_by_clip", clipId,
            &inspection.resolvedClipId, &inspection.warning);
    }
    if (!continuity) {
        inspection.error = "Face artifact has no continuity entry for this clip.";
        return inspection;
    }
    inspection.detectorMode = stringValue(*continuity, "detector_mode");
    inspection.runId = stringValue(*continuity, "run_id");
    inspection.frameDomain = stringValue(*continuity, "streams_frame_domain");
    if (inspection.frameDomain.empty()) {
        inspection.frameDomain = stringValue(*continuity, "raw_tracks_frame_domain");
    }
    inspection.rawFrameCount = continuity->value(
        "raw_frames_count",
        static_cast<std::int64_t>(
            continuity->value("raw_frames", json::array()).size()));
    inspection.tracks = trackRows(*continuity);
    if (inspection.tracks.empty()) {
        inspection.error = "Face artifact contains no continuity tracks.";
        return inspection;
    }

    inspection.identityPath = faceIdentityArtifactPath(transcriptPath);
    json identity;
    if (loadJcutBoxDocument(inspection.identityPath, &identity, nullptr)) {
        const std::string identityClipId = inspection.resolvedClipId.empty()
            ? clipId : inspection.resolvedClipId;
        const json* clusters = clipRoot(
            identity, "identity_clusters_by_clip", identityClipId, nullptr, nullptr);
        if (clusters) {
            inspection.identityClusterCount =
                clusters->value("clusters", json::array()).size();
        }
        const json* assignments = clipRoot(
            identity, "identity_assignments_by_clip", identityClipId, nullptr, nullptr);
        if (assignments) {
            inspection.identityAssignmentCount =
                assignments->value("track_identity_map", json::array()).size();
        }
    } else {
        inspection.identityPath.clear();
    }
    return inspection;
}

std::vector<SpeakerTrackAssignmentCore> transcriptSpeakerTrackAssignments(
    const json& transcriptRoot,
    const std::string& clipId)
{
    std::vector<SpeakerTrackAssignmentCore> result;
    const json& rows = transcriptRoot
        .value("speaker_flow", json::object())
        .value("clips", json::object())
        .value(clipId, json::object())
        .value("resolved_current", json::object())
        .value("track_identity_map", json::array());
    if (!rows.is_array()) return result;
    for (const json& row : rows) {
        if (!row.is_object()) continue;
        SpeakerTrackAssignmentCore assignment;
        assignment.trackId = row.value("track_id", -1);
        assignment.identityId = stringValue(row, "identity_id");
        assignment.streamId = stringValue(row, "stream_id");
        assignment.sourceFrame = row.value(
            "anchor_source_frame",
            row.value("source_frame", std::int64_t{-1}));
        if (assignment.trackId >= 0 && !assignment.identityId.empty()) {
            result.push_back(std::move(assignment));
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.trackId < rhs.trackId;
    });
    return result;
}

} // namespace jcut
