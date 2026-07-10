#include "editor_document_core_json.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace {

using json = nlohmann::json;

template <typename T>
T valueOr(const json& object, const char* key, T fallback)
{
    if (!object.is_object()) {
        return fallback;
    }
    const json::const_iterator it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return fallback;
    }
    return it->get<T>();
}

std::string stringOr(const json& object, const char* key, const std::string& fallback = {})
{
    if (!object.is_object()) {
        return fallback;
    }
    const json::const_iterator it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return fallback;
    }
    return it->get<std::string>();
}

std::string clipKindFromState(const json& clip)
{
    const std::string mediaType = stringOr(clip, "mediaType", "unknown");
    if (!mediaType.empty()) {
        return mediaType;
    }
    return "unknown";
}

bool parseCoreDocument(const json& root, jcut::EditorDocumentCore* document, std::string* errorOut)
{
    if (!root.is_object()) {
        if (errorOut) {
            *errorOut = "document root must be an object";
        }
        return false;
    }

    document->projectName = stringOr(root, "projectName", "Untitled Project");

    const json& mediaItems = root.value("mediaItems", json::array());
    if (!mediaItems.is_array()) {
        if (errorOut) {
            *errorOut = "mediaItems must be an array";
        }
        return false;
    }
    for (const json& mediaItem : mediaItems) {
        if (!mediaItem.is_object()) {
            continue;
        }
        document->mediaItems.push_back({
            stringOr(mediaItem, "id"),
            stringOr(mediaItem, "label"),
            stringOr(mediaItem, "kind", "unknown")
        });
    }

    const json& tracks = root.value("tracks", json::array());
    if (!tracks.is_array()) {
        if (errorOut) {
            *errorOut = "tracks must be an array";
        }
        return false;
    }
    for (const json& track : tracks) {
        if (!track.is_object()) {
            continue;
        }
        document->tracks.push_back({
            valueOr(track, "id", 0),
            stringOr(track, "label"),
            valueOr(track, "selected", false)
        });
    }

    const json& clips = root.value("clips", json::array());
    if (!clips.is_array()) {
        if (errorOut) {
            *errorOut = "clips must be an array";
        }
        return false;
    }
    for (const json& clip : clips) {
        if (!clip.is_object()) {
            continue;
        }
        document->clips.push_back({
            valueOr(clip, "id", 0),
            valueOr(clip, "trackId", 0),
            stringOr(clip, "label"),
            valueOr(clip, "startFrame", 0),
            valueOr(clip, "durationFrames", 0),
            valueOr(clip, "selected", false),
            stringOr(clip, "sourcePath")
        });
    }

    const json& transport = root.value("transport", json::object());
    if (transport.is_object()) {
        document->transport.playbackActive = valueOr(transport, "playbackActive", false);
        document->transport.playbackSpeed = valueOr(transport, "playbackSpeed", 1.0f);
        document->transport.previewZoom = valueOr(transport, "previewZoom", 1.0f);
        document->transport.currentFrame = valueOr(transport, "currentFrame", 0);
    }

    const json& panels = root.value("panels", json::object());
    if (panels.is_object()) {
        document->panels.showWaveform = valueOr(panels, "showWaveform", true);
        document->panels.showTranscript = valueOr(panels, "showTranscript", true);
        document->panels.showScopes = valueOr(panels, "showScopes", false);
    }

    const json& exportRequest = root.value("exportRequest", json::object());
    if (exportRequest.is_object()) {
        document->exportRequest.outputPath = stringOr(exportRequest, "outputPath");
        document->exportRequest.outputFormat = stringOr(exportRequest, "outputFormat", "mp4");
        document->exportRequest.imageSequenceFormat =
            stringOr(exportRequest, "imageSequenceFormat");
        const json& outputSize = exportRequest.value("outputSize", json::object());
        if (outputSize.is_object()) {
            document->exportRequest.outputSize.width = valueOr(outputSize, "width", 1080);
            document->exportRequest.outputSize.height = valueOr(outputSize, "height", 1920);
        }
        document->exportRequest.outputFps = valueOr(exportRequest, "outputFps", 30.0);
        document->exportRequest.playbackSpeed = valueOr(
            exportRequest,
            "playbackSpeed",
            static_cast<double>(document->transport.playbackSpeed));
        document->exportRequest.useProxyMedia = valueOr(exportRequest, "useProxyMedia", false);
        document->exportRequest.bypassGrading = valueOr(exportRequest, "bypassGrading", false);
        document->exportRequest.correctionsEnabled =
            valueOr(exportRequest, "correctionsEnabled", true);
        document->exportRequest.createVideoFromImageSequence =
            valueOr(exportRequest, "createVideoFromImageSequence", false);
        document->exportRequest.disableParallelImageWrite =
            valueOr(exportRequest, "disableParallelImageWrite", false);
        document->exportRequest.exportStartFrame =
            valueOr(exportRequest, "exportStartFrame", std::int64_t{0});
        document->exportRequest.exportEndFrame =
            valueOr(exportRequest, "exportEndFrame", std::int64_t{0});
        document->exportRequest.clipCount = valueOr(exportRequest, "clipCount", std::size_t{0});
        document->exportRequest.trackCount = valueOr(exportRequest, "trackCount", std::size_t{0});
        document->exportRequest.renderSyncMarkerCount =
            valueOr(exportRequest, "renderSyncMarkerCount", std::size_t{0});
        document->exportRequest.exportRangeCount =
            valueOr(exportRequest, "exportRangeCount", std::size_t{0});
        const std::string outputMode = stringOr(exportRequest, "outputMode", "encoded_file");
        if (outputMode == "image_sequence") {
            document->exportRequest.outputMode = jcut::render::RenderOutputMode::ImageSequence;
        } else if (outputMode == "encoded_file_and_image_sequence") {
            document->exportRequest.outputMode =
                jcut::render::RenderOutputMode::EncodedFileAndImageSequence;
        } else {
            document->exportRequest.outputMode = jcut::render::RenderOutputMode::EncodedFile;
        }
    }

    return true;
}

bool parseLegacyStateDocument(const json& root, jcut::EditorDocumentCore* document, std::string* errorOut)
{
    if (!root.is_object()) {
        if (errorOut) {
            *errorOut = "state root must be an object";
        }
        return false;
    }

    document->projectName = stringOr(root, "projectName", "Untitled Project");
    document->transport.currentFrame = valueOr(root, "currentFrame", 0);
    document->transport.playbackActive = valueOr(root, "playing", false);
    document->transport.playbackSpeed = valueOr(root, "playbackSpeed", 1.0f);
    document->transport.previewZoom = valueOr(root, "timelineZoom", 1.0f);
    document->panels.showWaveform = valueOr(root, "audioWaveformVisible", true);
    document->panels.showTranscript = true;
    document->panels.showScopes = false;

    document->exportRequest.outputPath = stringOr(root, "lastRenderOutputPath");
    document->exportRequest.outputFormat = stringOr(root, "outputFormat", "mp4");
    document->exportRequest.outputSize.width = valueOr(root, "outputWidth", 1080);
    document->exportRequest.outputSize.height = valueOr(root, "outputHeight", 1920);
    document->exportRequest.outputFps = valueOr(root, "outputFps", 30.0);
    document->exportRequest.playbackSpeed = valueOr(
        root,
        "exportPlaybackSpeed",
        valueOr(root, "playbackSpeed", 1.0));
    document->exportRequest.useProxyMedia = valueOr(root, "renderUseProxies", false);
    document->exportRequest.bypassGrading = !valueOr(root, "gradingPreview", true);
    document->exportRequest.correctionsEnabled = valueOr(root, "correctionsEnabled", true);
    document->exportRequest.exportStartFrame =
        valueOr(root, "exportStartFrame", std::int64_t{0});
    document->exportRequest.exportEndFrame =
        valueOr(root, "exportEndFrame", std::int64_t{0});

    const json& exportRanges = root.value("exportRanges", json::array());
    if (exportRanges.is_array()) {
        document->exportRequest.exportRangeCount = exportRanges.size();
    }

    const json& renderSyncMarkers = root.value("renderSyncMarkers", json::array());
    if (renderSyncMarkers.is_array()) {
        document->exportRequest.renderSyncMarkerCount = renderSyncMarkers.size();
    }

    const std::string selectedClipId = stringOr(root, "selectedClipId");
    const int selectedTrackIndex = valueOr(root, "selectedTrackIndex", -1);

    const json& tracks = root.value("tracks", json::array());
    if (!tracks.is_array()) {
        if (errorOut) {
            *errorOut = "tracks must be an array";
        }
        return false;
    }
    document->tracks.reserve(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const json& track = tracks.at(i);
        if (!track.is_object()) {
            continue;
        }
        document->tracks.push_back({
            static_cast<int>(i + 1),
            stringOr(track, "name", "Track " + std::to_string(i + 1)),
            static_cast<int>(i) == selectedTrackIndex
        });
    }

    const json& timeline = root.value("timeline", json::array());
    if (!timeline.is_array()) {
        if (errorOut) {
            *errorOut = "timeline must be an array";
        }
        return false;
    }

    std::unordered_set<std::string> seenMediaIds;
    document->clips.reserve(timeline.size());
    int nextClipId = 1;
    for (const json& clip : timeline) {
        if (!clip.is_object()) {
            continue;
        }
        const std::string sourcePath = stringOr(clip, "filePath");
        const std::string label = stringOr(clip, "label", sourcePath.empty()
            ? std::string("clip")
            : sourcePath.substr(sourcePath.find_last_of("/\\") + 1));
        if (!sourcePath.empty() && seenMediaIds.insert(sourcePath).second) {
            document->mediaItems.push_back({
                sourcePath,
                label,
                clipKindFromState(clip)
            });
        }
        const int trackId = std::max(1, valueOr(clip, "trackIndex", 0) + 1);
        document->clips.push_back({
            nextClipId++,
            trackId,
            label,
            valueOr(clip, "startFrame", 0),
            valueOr(clip, "durationFrames", 0),
            stringOr(clip, "id") == selectedClipId,
            sourcePath
        });
    }

    if (document->tracks.empty() && !document->clips.empty()) {
        int maxTrackId = 0;
        for (const jcut::EditorClip& clip : document->clips) {
            maxTrackId = std::max(maxTrackId, clip.trackId);
        }
        for (int trackId = 1; trackId <= maxTrackId; ++trackId) {
            document->tracks.push_back({
                trackId,
                "Track " + std::to_string(trackId),
                trackId == 1
            });
        }
    }

    if (std::none_of(document->tracks.begin(), document->tracks.end(), [](const jcut::EditorTrack& track) {
            return track.selected;
        }) && !document->tracks.empty()) {
        document->tracks.front().selected = true;
    }
    if (std::none_of(document->clips.begin(), document->clips.end(), [](const jcut::EditorClip& clip) {
            return clip.selected;
        }) && !document->clips.empty()) {
        document->clips.front().selected = true;
    }

    document->exportRequest.clipCount = document->clips.size();
    document->exportRequest.trackCount = document->tracks.size();

    const std::string format = document->exportRequest.outputFormat;
    if (format == "png" || format == "jpg" || format == "jpeg") {
        document->exportRequest.imageSequenceFormat = format == "jpg" ? "jpeg" : format;
        document->exportRequest.createVideoFromImageSequence = true;
        document->exportRequest.outputMode =
            jcut::render::RenderOutputMode::EncodedFileAndImageSequence;
    } else {
        document->exportRequest.outputMode = jcut::render::RenderOutputMode::EncodedFile;
    }

    return true;
}

std::string outputModeToString(jcut::render::RenderOutputMode mode)
{
    switch (mode) {
    case jcut::render::RenderOutputMode::EncodedFile:
        return "encoded_file";
    case jcut::render::RenderOutputMode::ImageSequence:
        return "image_sequence";
    case jcut::render::RenderOutputMode::EncodedFileAndImageSequence:
        return "encoded_file_and_image_sequence";
    }
    return "encoded_file";
}

json legacyClipJson(const jcut::EditorClip& clip, const json* baseClip = nullptr)
{
    json out = (baseClip && baseClip->is_object()) ? *baseClip : json::object();
    if (!out.contains("id") || !out["id"].is_string() || out["id"].get<std::string>().empty()) {
        out["id"] = std::string("imgui-clip-") + std::to_string(clip.id);
    }
    out["label"] = clip.label;
    out["filePath"] = clip.sourcePath;
    out["trackIndex"] = std::max(0, clip.trackId - 1);
    out["startFrame"] = clip.startFrame;
    out["durationFrames"] = std::max(1, clip.durationFrames);
    if (!out.contains("mediaType") || !out["mediaType"].is_string()) {
        out["mediaType"] = "video";
    }
    return out;
}

const json* findPreservedClipJson(const json& baseTimeline,
                                  const jcut::EditorClip& clip,
                                  std::vector<bool>* consumed)
{
    if (!baseTimeline.is_array() || !consumed || consumed->size() != baseTimeline.size()) {
        return nullptr;
    }

    auto matchesPath = [&](const json& candidate) {
        return stringOr(candidate, "filePath") == clip.sourcePath ||
               stringOr(candidate, "proxyPath") == clip.sourcePath ||
               stringOr(candidate, "audioSourcePath") == clip.sourcePath;
    };
    auto matchesShape = [&](const json& candidate) {
        return stringOr(candidate, "label") == clip.label &&
               valueOr(candidate, "trackIndex", 0) == std::max(0, clip.trackId - 1) &&
               valueOr(candidate, "startFrame", 0) == clip.startFrame;
    };

    for (std::size_t i = 0; i < baseTimeline.size(); ++i) {
        if ((*consumed)[i] || !baseTimeline.at(i).is_object()) {
            continue;
        }
        if (matchesPath(baseTimeline.at(i))) {
            (*consumed)[i] = true;
            return &baseTimeline.at(i);
        }
    }
    for (std::size_t i = 0; i < baseTimeline.size(); ++i) {
        if ((*consumed)[i] || !baseTimeline.at(i).is_object()) {
            continue;
        }
        if (matchesShape(baseTimeline.at(i))) {
            (*consumed)[i] = true;
            return &baseTimeline.at(i);
        }
    }
    return nullptr;
}

} // namespace

namespace jcut {

nlohmann::json toJson(const EditorDocumentCore& document)
{
    json mediaItems = json::array();
    for (const EditorMediaItem& mediaItem : document.mediaItems) {
        mediaItems.push_back({
            {"id", mediaItem.id},
            {"label", mediaItem.label},
            {"kind", mediaItem.kind}
        });
    }

    json tracks = json::array();
    for (const EditorTrack& track : document.tracks) {
        tracks.push_back({
            {"id", track.id},
            {"label", track.label},
            {"selected", track.selected}
        });
    }

    json clips = json::array();
    for (const EditorClip& clip : document.clips) {
        clips.push_back({
            {"id", clip.id},
            {"trackId", clip.trackId},
            {"label", clip.label},
            {"startFrame", clip.startFrame},
            {"durationFrames", clip.durationFrames},
            {"selected", clip.selected},
            {"sourcePath", clip.sourcePath}
        });
    }

    return {
        {"projectName", document.projectName},
        {"mediaItems", std::move(mediaItems)},
        {"tracks", std::move(tracks)},
        {"clips", std::move(clips)},
        {"transport", {
            {"playbackActive", document.transport.playbackActive},
            {"playbackSpeed", document.transport.playbackSpeed},
            {"previewZoom", document.transport.previewZoom},
            {"currentFrame", document.transport.currentFrame}
        }},
        {"panels", {
            {"showWaveform", document.panels.showWaveform},
            {"showTranscript", document.panels.showTranscript},
            {"showScopes", document.panels.showScopes}
        }},
        {"exportRequest", {
            {"outputPath", document.exportRequest.outputPath},
            {"outputFormat", document.exportRequest.outputFormat},
            {"imageSequenceFormat", document.exportRequest.imageSequenceFormat},
            {"outputSize", {
                {"width", document.exportRequest.outputSize.width},
                {"height", document.exportRequest.outputSize.height}
            }},
            {"outputFps", document.exportRequest.outputFps},
            {"playbackSpeed", document.exportRequest.playbackSpeed},
            {"useProxyMedia", document.exportRequest.useProxyMedia},
            {"bypassGrading", document.exportRequest.bypassGrading},
            {"correctionsEnabled", document.exportRequest.correctionsEnabled},
            {"createVideoFromImageSequence", document.exportRequest.createVideoFromImageSequence},
            {"disableParallelImageWrite", document.exportRequest.disableParallelImageWrite},
            {"exportStartFrame", document.exportRequest.exportStartFrame},
            {"exportEndFrame", document.exportRequest.exportEndFrame},
            {"clipCount", document.exportRequest.clipCount},
            {"trackCount", document.exportRequest.trackCount},
            {"renderSyncMarkerCount", document.exportRequest.renderSyncMarkerCount},
            {"exportRangeCount", document.exportRequest.exportRangeCount},
            {"outputMode", outputModeToString(document.exportRequest.outputMode)}
        }}
    };
}

nlohmann::json toLegacyStateJson(const EditorDocumentCore& document, const nlohmann::json* baseRoot)
{
    json root = (baseRoot && baseRoot->is_object()) ? *baseRoot : json::object();

    root["projectName"] = document.projectName.empty() ? "Untitled Project" : document.projectName;
    root["currentFrame"] = document.transport.currentFrame;
    root["playing"] = document.transport.playbackActive;
    root["playbackSpeed"] = document.transport.playbackSpeed;
    root["exportPlaybackSpeed"] = document.exportRequest.playbackSpeed;
    root["timelineZoom"] = document.transport.previewZoom;
    root["audioWaveformVisible"] = document.panels.showWaveform;
    root["outputWidth"] = document.exportRequest.outputSize.width;
    root["outputHeight"] = document.exportRequest.outputSize.height;
    root["timelineFps"] = document.exportRequest.outputFps;
    root["outputFps"] = document.exportRequest.outputFps;
    root["outputFormat"] = document.exportRequest.outputFormat.empty()
        ? std::string("mp4")
        : document.exportRequest.outputFormat;
    root["lastRenderOutputPath"] = document.exportRequest.outputPath;
    root["renderUseProxies"] = document.exportRequest.useProxyMedia;
    root["gradingPreview"] = !document.exportRequest.bypassGrading;
    root["correctionsEnabled"] = document.exportRequest.correctionsEnabled;
    root["exportStartFrame"] = document.exportRequest.exportStartFrame;
    root["exportEndFrame"] = document.exportRequest.exportEndFrame;

    int selectedTrackIndex = -1;
    json tracks = json::array();
    const json baseTracks = root.value("tracks", json::array());
    for (std::size_t i = 0; i < document.tracks.size(); ++i) {
        const EditorTrack& track = document.tracks[i];
        json trackJson = (baseTracks.is_array() && i < baseTracks.size() && baseTracks.at(i).is_object())
            ? baseTracks.at(i)
            : json::object();
        trackJson["name"] = track.label.empty()
            ? std::string("Track ") + std::to_string(i + 1)
            : track.label;
        tracks.push_back(std::move(trackJson));
        if (track.selected) {
            selectedTrackIndex = static_cast<int>(i);
        }
    }
    root["tracks"] = std::move(tracks);
    root["selectedTrackIndex"] = selectedTrackIndex;

    json timeline = json::array();
    json selectedClip = json::object();
    std::string selectedClipId;
    const json baseTimeline = root.value("timeline", json::array());
    std::vector<bool> consumedBaseClips(baseTimeline.is_array() ? baseTimeline.size() : 0, false);
    for (const EditorClip& clip : document.clips) {
        const json* baseClip = findPreservedClipJson(baseTimeline, clip, &consumedBaseClips);
        json clipJson = legacyClipJson(clip, baseClip);
        timeline.push_back(clipJson);
        if (clip.selected) {
            selectedClipId = clipJson.value("id", std::string{});
            selectedClip = clipJson;
        }
    }
    root["timeline"] = std::move(timeline);
    root["selectedClipId"] = selectedClipId;
    root["selectedClip"] = std::move(selectedClip);
    root["selectedClipIds"] = selectedClipId.empty() ? json::array() : json::array({selectedClipId});

    return root;
}

std::optional<EditorDocumentCore> editorDocumentCoreFromJson(
    const nlohmann::json& root,
    std::string* errorOut)
{
    EditorDocumentCore document;
    const bool looksLikeCoreDocument = root.is_object() &&
        (root.contains("mediaItems") || root.contains("clips") ||
         root.contains("transport") || root.contains("panels") ||
         root.contains("exportRequest"));
    const bool looksLikeLegacyState = root.is_object() && !looksLikeCoreDocument &&
        (root.contains("timeline") || root.contains("selectedClipId") || root.contains("tracks"));
    const bool ok = looksLikeLegacyState
        ? parseLegacyStateDocument(root, &document, errorOut)
        : parseCoreDocument(root, &document, errorOut);
    if (!ok) {
        return std::nullopt;
    }
    if (document.projectName.empty()) {
        document.projectName = "Untitled Project";
    }
    if (document.exportRequest.outputFormat.empty()) {
        document.exportRequest.outputFormat = "mp4";
    }
    if (document.exportRequest.outputSize.width <= 0) {
        document.exportRequest.outputSize.width = 1080;
    }
    if (document.exportRequest.outputSize.height <= 0) {
        document.exportRequest.outputSize.height = 1920;
    }
    if (document.exportRequest.outputFps <= 0.0) {
        document.exportRequest.outputFps = 30.0;
    }
    return document;
}

std::optional<EditorDocumentCore> editorDocumentCoreFromJsonBytes(
    const std::string& bytes,
    std::string* errorOut)
{
    try {
        return editorDocumentCoreFromJson(json::parse(bytes), errorOut);
    } catch (const std::exception& exception) {
        if (errorOut) {
            *errorOut = exception.what();
        }
        return std::nullopt;
    }
}

std::optional<EditorDocumentCore> loadEditorDocumentCoreFromFile(
    const std::string& path,
    std::string* errorOut)
{
    std::ifstream stream(path);
    if (!stream.is_open()) {
        if (errorOut) {
            *errorOut = "failed to open document file: " + path;
        }
        return std::nullopt;
    }

    json root;
    try {
        stream >> root;
    } catch (const std::exception& exception) {
        if (errorOut) {
            *errorOut = "failed to parse document file: " + std::string(exception.what());
        }
        return std::nullopt;
    }

    return editorDocumentCoreFromJson(root, errorOut);
}

bool saveEditorDocumentCoreToFile(
    const EditorDocumentCore& document,
    const std::string& path,
    std::string* errorOut)
{
    std::ofstream stream(path, std::ios::out | std::ios::trunc);
    if (!stream.is_open()) {
        if (errorOut) {
            *errorOut = "failed to open document file for write: " + path;
        }
        return false;
    }

    try {
        stream << toJson(document).dump(2) << '\n';
    } catch (const std::exception& exception) {
        if (errorOut) {
            *errorOut = "failed to serialize document file: " + std::string(exception.what());
        }
        return false;
    }

    if (!stream.good()) {
        if (errorOut) {
            *errorOut = "failed to write document file: " + path;
        }
        return false;
    }

    return true;
}

bool saveLegacyStateDocumentToFile(
    const EditorDocumentCore& document,
    const std::string& path,
    std::string* errorOut,
    const nlohmann::json* baseRoot)
{
    std::ofstream stream(path, std::ios::out | std::ios::trunc);
    if (!stream.is_open()) {
        if (errorOut) {
            *errorOut = "failed to open document file for write: " + path;
        }
        return false;
    }

    try {
        stream << toLegacyStateJson(document, baseRoot).dump(2) << '\n';
    } catch (const std::exception& exception) {
        if (errorOut) {
            *errorOut = "failed to serialize legacy state file: " + std::string(exception.what());
        }
        return false;
    }

    if (!stream.good()) {
        if (errorOut) {
            *errorOut = "failed to write legacy state file: " + path;
        }
        return false;
    }

    return true;
}

} // namespace jcut
