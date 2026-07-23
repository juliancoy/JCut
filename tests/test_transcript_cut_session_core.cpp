#include "../editor_document_core_json.h"
#include "../editor_runtime.h"
#include "../transcript_cut_session_core.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

int g_failures = 0;

void expect(bool condition, const std::string& message)
{
    if (condition) {
        return;
    }
    ++g_failures;
    std::cerr << "FAIL: " << message << '\n';
}

struct TemporaryDirectory {
    fs::path path;

    TemporaryDirectory()
    {
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            path = fs::temp_directory_path() /
                ("jcut-transcript-session-" + std::to_string(seed) + "-" +
                 std::to_string(attempt));
            std::error_code ec;
            if (fs::create_directory(path, ec) && !ec) {
                return;
            }
        }
        path.clear();
    }

    ~TemporaryDirectory()
    {
        if (!path.empty()) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
};

bool writeBytes(const fs::path& path, const std::string& bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << bytes;
    return stream.good();
}

bool writeJson(const fs::path& path, const json& root)
{
    return writeBytes(path, root.dump(2) + "\n");
}

json transcriptRoot(const std::string& firstWord,
                    bool includeSecondWord,
                    const std::string& cutLabel = {})
{
    json words = json::array({
        {
            {"word", firstWord},
            {"start", 0.0},
            {"end", 0.1},
            {"speaker", "S1"},
            {"original_segment_index", 0},
            {"original_word_index", 0},
            {"transcript_edits", json::array({"text"})},
        },
    });
    if (includeSecondWord) {
        words.push_back({
            {"word", "outside"},
            {"start", 0.3},
            {"end", 0.4},
            {"speaker", "S1"},
            {"original_segment_index", 0},
            {"original_word_index", 1},
        });
    }
    json root = {
        {"speaker_profiles", {{"S1", {{"name", "Alice"}}}}},
        {"segments", json::array({{{"words", std::move(words)}}})},
        {"future_root_field", {{"preserved", true}}},
    };
    if (!cutLabel.empty()) {
        root["cut_label"] = cutLabel;
    }
    return root;
}

void testSourceSelectionMatchesQtPolicy()
{
    TemporaryDirectory temp;
    expect(!temp.path.empty(), "temporary directory created");
    if (temp.path.empty()) {
        return;
    }
    const fs::path mediaDir = temp.path / "media";
    std::error_code ec;
    fs::create_directories(mediaDir, ec);
    writeBytes(mediaDir / "clip.mov", "video");
    writeBytes(mediaDir / "derived.wav", "audio");

    jcut::TranscriptSourceSpec source;
    source.sourceRootPath = temp.path.string();
    source.sourcePath = "media/clip.mov";
    source.audioSourcePath = "media/derived.wav";
    source.audioSourceMode = "generated";
    source.audioSourceStatus = "missing";
    source.audioStreamIndex = 2;
    jcut::TranscriptSourceIdentity identity =
        jcut::resolveTranscriptSourceIdentity(source);
    expect(identity.sourcePath == (mediaDir / "clip.mov").string(),
           "unusable future-mode audio path falls back to clip source");
    expect(identity.fileStem == "clip_audio_stream_2",
           "audio stream suffix is isolated in the transcript stem");

    source.sourceRootPath = fs::relative(temp.path, fs::current_path()).string();
    identity = jcut::resolveTranscriptSourceIdentity(source);
    expect(fs::path(identity.sourcePath).is_absolute() &&
               identity.sourcePath == (mediaDir / "clip.mov").string(),
           "an existing relative source root still resolves to a stable absolute key");
    source.sourceRootPath = temp.path.string();

    source.audioSourceStatus = "ok";
    identity = jcut::resolveTranscriptSourceIdentity(source);
    expect(identity.sourcePath == (mediaDir / "derived.wav").string(),
           "persisted ok status selects audio path for future modes");
    expect(identity.canonicalKey.find("::audio_stream=2") != std::string::npos,
           "canonical source registry key includes stream index");

    source.audioSourceStatus = "missing";
    source.audioSourceMode = "explicit_file";
    identity = jcut::resolveTranscriptSourceIdentity(source);
    expect(identity.sourcePath == (mediaDir / "derived.wav").string(),
           "explicit-file mode selects its audio path independent of status");
}

void testCatalogSelectionRowsAndInvalidation()
{
    TemporaryDirectory temp;
    expect(!temp.path.empty(), "temporary directory created for catalog");
    if (temp.path.empty()) {
        return;
    }
    const fs::path mediaPath = temp.path / "clip.wav";
    const fs::path originalPath = temp.path / "clip.json";
    const fs::path editablePath = temp.path / "clip_editable.json";
    const fs::path v0Path = temp.path / "clip_editable_v0.json";
    const fs::path v1Path = temp.path / "clip_editable_v1.json";
    const fs::path v2Path = temp.path / "clip_editable_v2.json";
    const fs::path v10Path = temp.path / "clip_editable_v10.json";
    writeBytes(mediaPath, "audio");
    expect(writeJson(originalPath, transcriptRoot("kept", true)),
           "original transcript written");
    expect(writeJson(v0Path, transcriptRoot("zero", false)), "manual v0 written");
    expect(writeJson(v1Path, transcriptRoot("one", false)), "manual v1 written");
    expect(writeJson(v2Path, transcriptRoot("two", false)), "v2 written");
    expect(writeJson(v10Path, transcriptRoot("kept", false, "Review Cut")),
           "v10 written");
    expect(writeJson(temp.path / "clip_editable_vx.json", transcriptRoot("ignored", false)),
           "nonnumeric candidate written");

    jcut::TranscriptSourceSpec source;
    source.sourcePath = mediaPath.string();
    jcut::TranscriptCutSessionOptions options;
    options.requestedActivePath = v10Path.string();
    options.includeOutsideActiveCut = true;
    options.ensureEditable = true;
    options.timing.prependMilliseconds = 0;
    options.timing.postpendMilliseconds = 0;
    jcut::TranscriptCutSession session =
        jcut::loadTranscriptCutSession(source, options);

    expect(session.ok(), "requested transcript version loads");
    expect(fs::is_regular_file(editablePath),
           "opening a transcript creates the base editable copy like Qt");
    expect(session.catalog.cuts.size() == 6,
           "catalog includes original, editable, and numeric manual versions only");
    if (session.catalog.cuts.size() == 6) {
        expect(session.catalog.cuts[0].kind == jcut::TranscriptCutKind::Original,
               "original sorts first");
        expect(session.catalog.cuts[1].kind == jcut::TranscriptCutKind::Editable,
               "base editable sorts second");
        expect(session.catalog.cuts[2].version == 0 &&
                   session.catalog.cuts[3].version == 1 &&
                   session.catalog.cuts[4].version == 2 &&
                   session.catalog.cuts[5].version == 10,
               "version discovery is numeric rather than lexicographic and accepts v0/v1");
        expect(session.catalog.cuts[5].label == "Review Cut",
               "cut_label overrides the generated version label");
    }
    expect(session.activePath == v10Path.string() && session.activeCutMutable,
           "valid persisted cut wins and is mutable");
    const auto outside = std::find_if(
        session.rows.begin(), session.rows.end(),
        [](const jcut::TranscriptRow& row) { return row.text == "outside"; });
    expect(outside != session.rows.end() && outside->outsideActiveCut,
           "active/original pair projects omitted words as outside-cut rows");
    const auto kept = std::find_if(
        session.rows.begin(), session.rows.end(),
        [](const jcut::TranscriptRow& row) { return row.text == "kept"; });
    expect(kept != session.rows.end() && kept->speakerLabel == "Alice" &&
               (kept->editFlags & jcut::TranscriptEditText) != 0U,
           "read-only rows retain speaker labels and edit flags");

    const jcut::TranscriptFileStamp before =
        jcut::inspectTranscriptFile(v10Path.string());
    expect(writeJson(v10Path, transcriptRoot("kept and expanded", false, "Review Cut")),
           "active version changed on disk");
    const jcut::TranscriptFileStamp after =
        jcut::inspectTranscriptFile(v10Path.string());
    expect(before != after, "file stamp exposes external transcript changes to the UI cache");

    const fs::path customPath = temp.path / "hand_review.json";
    expect(writeJson(customPath, transcriptRoot("custom", false, "Hand Review")),
           "custom persisted cut written");
    options.requestedActivePath = customPath.string();
    options.includeOutsideActiveCut = false;
    session = jcut::loadTranscriptCutSession(source, options);
    const auto customCut = std::find_if(
        session.catalog.cuts.begin(), session.catalog.cuts.end(),
        [&](const jcut::TranscriptCutEntry& cut) {
            return cut.path == customPath.string();
        });
    expect(session.ok() && session.activePath == customPath.string() &&
               customCut != session.catalog.cuts.end() &&
               customCut->kind == jcut::TranscriptCutKind::Custom &&
               customCut->label == "Hand Review",
           "an existing persisted custom cut stays aligned with rich preview");

    options.requestedActivePath = (temp.path / "stale.json").string();
    options.includeOutsideActiveCut = false;
    session = jcut::loadTranscriptCutSession(source, options);
    expect(session.activePath == editablePath.string(),
           "stale persisted selection falls back to base editable");

    expect(writeBytes(v2Path, "{broken"), "malformed active cut written");
    options.requestedActivePath = v2Path.string();
    session = jcut::loadTranscriptCutSession(source, options);
    expect(!session.ok() && session.error.find("Invalid transcript JSON") != std::string::npos,
           "malformed active cut fails with a useful error");
}

void testNeutralActiveCutRoundTripAndCommand()
{
    jcut::EditorDocumentCore document;
    document.projectName = "Transcript Round Trip";
    document.tracks = {{1, "Audio", true}};
    jcut::EditorClip clip;
    clip.id = 1;
    clip.trackId = 1;
    clip.selected = true;
    clip.sourcePath = "clip.mov";
    clip.audioSourceMode = "generated";
    clip.audioSourcePath = "derived.wav";
    clip.audioSourceStatus = "ok";
    clip.transcriptActiveCutPath = "/cuts/first.json";
    document.clips.push_back(clip);

    jcut::EditorRuntime runtime = jcut::EditorRuntime::fromDocument(document);
    const jcut::CommandResult result = runtime.execute(
        jcut::SetClipTranscriptActiveCutCommand{1, "/cuts/review.json"});
    expect(result.applied, "runtime active-cut command applies");
    const jcut::EditorDocumentCore updated = runtime.snapshot();
    expect(updated.clips.front().transcriptActiveCutPath == "/cuts/review.json",
           "runtime stores active cut on the neutral clip");

    const json coreJson = jcut::toJson(updated);
    std::string error;
    const std::optional<jcut::EditorDocumentCore> coreRoundTrip =
        jcut::editorDocumentCoreFromJson(coreJson, &error);
    expect(coreRoundTrip.has_value() &&
               coreRoundTrip->clips.front().audioSourceStatus == "ok" &&
               coreRoundTrip->clips.front().transcriptActiveCutPath == "/cuts/review.json",
           "neutral JSON preserves source status and active cut");

    const json legacyJson = jcut::toLegacyStateJson(updated);
    expect(legacyJson.value("transcriptActiveCutPath", std::string{}) ==
               "/cuts/review.json",
           "legacy root mirrors the selected neutral clip's active cut");
    const std::optional<jcut::EditorDocumentCore> legacyRoundTrip =
        jcut::editorDocumentCoreFromJson(legacyJson, &error);
    expect(legacyRoundTrip.has_value() &&
               legacyRoundTrip->clips.front().transcriptActiveCutPath ==
                   "/cuts/review.json",
           "legacy active-cut field hydrates the selected neutral clip");
}

void testImGuiAndRichRenderWiringContracts()
{
    const fs::path sourceDir(JCUT_SOURCE_DIR);
    const std::string imgui = [&]() {
        std::ifstream stream(sourceDir / "jcut_imgui_main.cpp", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>());
    }();
    const std::string transcriptEngine = [&]() {
        std::ifstream stream(sourceDir / "transcript_engine.cpp", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>());
    }();
    expect(imgui.find("jcut::loadTranscriptCutSession(source, options)") != std::string::npos,
           "ImGui loads the shared transcript session");
    expect(imgui.find("ImGuiListClipper clipper") != std::string::npos,
           "ImGui clips long transcript tables");
    expect(imgui.find("jcut::inspectTranscriptFile") != std::string::npos,
           "ImGui cache checks transcript file metadata");
    expect(imgui.find("SetClipTranscriptActiveCutCommand") != std::string::npos,
           "cut combo updates the neutral active-cut field");
    expect(imgui.find("lastSavedLegacyExtensionSignature") != std::string::npos,
           "legacy transcript view state participates in dirty tracking");
    expect(transcriptEngine.find("clip.transcriptActiveCutPath.trimmed()") !=
               std::string::npos,
           "rich TranscriptEngine consumes the neutral active-cut field");
}

} // namespace

int main()
{
    testSourceSelectionMatchesQtPolicy();
    testCatalogSelectionRowsAndInvalidation();
    testNeutralActiveCutRoundTripAndCommand();
    testImGuiAndRichRenderWiringContracts();

    if (g_failures != 0) {
        std::cerr << g_failures << " transcript cut session assertion(s) failed\n";
        return 1;
    }
    std::cout << "transcript cut session assertions passed\n";
    return 0;
}
