#include "../editor_document_core_json.h"
#include "../editor_runtime.h"
#include "../face_artifact_core.h"
#include "../face_processing_job_core.h"
#include "../proxy_path_core.h"
#include "../speaker_title_core.h"
#include "../transcript_cut_session_core.h"
#include "../transcript_document_mutation_core.h"
#include "../transcript_mining_core.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <zlib.h>

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

bool writeJcutBoxV2(const fs::path& path, const json& root)
{
    const std::vector<std::uint8_t> cbor = json::to_cbor(root);
    uLongf compressedSize = compressBound(cbor.size());
    std::vector<unsigned char> compressed(compressedSize);
    if (compress2(
            compressed.data(), &compressedSize,
            cbor.data(), cbor.size(), 6) != Z_OK) {
        return false;
    }
    compressed.resize(compressedSize);
    std::string bytes("JCUTBOX1", 8);
    const auto appendLittle = [&](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<char>((value >> shift) & 0xffU));
        }
    };
    appendLittle(2);
    appendLittle(static_cast<std::uint32_t>(compressed.size() + 4));
    const std::uint32_t unpacked = static_cast<std::uint32_t>(cbor.size());
    bytes.push_back(static_cast<char>((unpacked >> 24U) & 0xffU));
    bytes.push_back(static_cast<char>((unpacked >> 16U) & 0xffU));
    bytes.push_back(static_cast<char>((unpacked >> 8U) & 0xffU));
    bytes.push_back(static_cast<char>(unpacked & 0xffU));
    bytes.append(
        reinterpret_cast<const char*>(compressed.data()), compressed.size());
    return writeBytes(path, bytes);
}

std::string qtCompressed(const std::vector<std::uint8_t>& payload)
{
    uLongf compressedSize = compressBound(payload.size());
    std::vector<unsigned char> compressed(compressedSize);
    if (compress2(
            compressed.data(), &compressedSize,
            payload.data(), payload.size(), 6) != Z_OK) {
        return {};
    }
    compressed.resize(compressedSize);
    std::string bytes;
    const std::uint32_t size = static_cast<std::uint32_t>(payload.size());
    bytes.push_back(static_cast<char>((size >> 24U) & 0xffU));
    bytes.push_back(static_cast<char>((size >> 16U) & 0xffU));
    bytes.push_back(static_cast<char>((size >> 8U) & 0xffU));
    bytes.push_back(static_cast<char>(size & 0xffU));
    bytes.append(
        reinterpret_cast<const char*>(compressed.data()), compressed.size());
    return bytes;
}

void appendBigEndian(std::string* bytes, std::uint32_t value)
{
    bytes->push_back(static_cast<char>((value >> 24U) & 0xffU));
    bytes->push_back(static_cast<char>((value >> 16U) & 0xffU));
    bytes->push_back(static_cast<char>((value >> 8U) & 0xffU));
    bytes->push_back(static_cast<char>(value & 0xffU));
}

bool writeQtBinaryJson(const fs::path& path, const json& root)
{
    const std::string payload = root.dump();
    const std::string compressed = qtCompressed(
        std::vector<std::uint8_t>(payload.begin(), payload.end()));
    std::string bytes;
    appendBigEndian(&bytes, 0x4A435554U);
    appendBigEndian(&bytes, 1);
    appendBigEndian(
        &bytes, static_cast<std::uint32_t>(compressed.size()));
    bytes += compressed;
    return writeBytes(path, bytes);
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
    const std::string qtTranscriptTab = [&]() {
        std::ifstream stream(
            sourceDir / "transcript_tab.cpp",
            std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>());
    }();
    const std::string qtTranscriptDocument = [&]() {
        std::ifstream stream(
            sourceDir / "transcript_tab_document.cpp",
            std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(stream),
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
    expect(imgui.find("jcut::insertTranscriptWord") != std::string::npos &&
               imgui.find("jcut::restoreTranscriptWord") != std::string::npos &&
               imgui.find("SetTranscriptHistoryDocumentCommand") != std::string::npos,
           "ImGui binds shared row actions into global runtime history");
    expect(imgui.find("jcut::createTranscriptCutVersion") != std::string::npos &&
               imgui.find("jcut::deleteTranscriptCut") != std::string::npos,
           "ImGui binds shared transcript cut lifecycle");
    expect(imgui.find("lastSavedLegacyExtensionSignature") != std::string::npos,
           "legacy transcript view state participates in dirty tracking");
    expect(transcriptEngine.find("clip.transcriptActiveCutPath.trimmed()") !=
               std::string::npos,
           "rich TranscriptEngine consumes the neutral active-cut field");
    expect(
        qtTranscriptTab.find(
            "applyTranscriptDocumentMutation") !=
                std::string::npos &&
            qtTranscriptTab.find(
                "jcut::deleteTranscriptWords") !=
                std::string::npos &&
            qtTranscriptTab.find(
                "jcut::insertTranscriptWord") !=
                std::string::npos &&
            qtTranscriptTab.find(
                "jcut::expandTranscriptWordTiming") !=
                std::string::npos &&
            qtTranscriptTab.find(
                "jcut::restoreTranscriptWord") !=
                std::string::npos,
        "Qt row mutations delegate to the same neutral core as ImGui");
    expect(
        qtTranscriptDocument.find(
            "jcut::reorderTranscriptWords") !=
                std::string::npos &&
            qtTranscriptDocument.find(
                "jcut::createTranscriptCutVersion") !=
                std::string::npos &&
            qtTranscriptDocument.find(
                "jcut::renameTranscriptCut") !=
                std::string::npos &&
            qtTranscriptDocument.find(
                "jcut::deleteTranscriptCut") !=
                std::string::npos,
        "Qt reorder and cut lifecycle delegate to shared neutral services");
}

void testNeutralWordMutationPreservesUnknownFieldsAndSavesAtomically()
{
    TemporaryDirectory temp;
    const fs::path path = temp.path / "clip_editable.json";
    json root = transcriptRoot("hello", true);
    root["segments"][0]["words"][0]["future_word_field"] = "keep";
    const auto document = jcut::TranscriptDocumentCore::fromJson(root);
    expect(document.has_value(), "mutation fixture parses");
    if (!document) return;
    const auto rows = document->rows();
    const auto row = std::find_if(rows.begin(), rows.end(), [](const auto& value) {
        return value.text == "hello";
    });
    expect(row != rows.end(), "mutation target projects to a row");
    if (row == rows.end()) return;

    jcut::TranscriptWordPatch patch;
    patch.startSeconds = 0.05;
    patch.endSeconds = 0.25;
    patch.text = "edited";
    patch.skipped = true;
    std::string error;
    expect(jcut::patchTranscriptWord(&root, row->word, patch, &error),
           "neutral patch changes a projected word");
    const json& word = root["segments"][0]["words"][0];
    expect(word.value("word", std::string{}) == "edited" &&
               word.value("start", 0.0) == 0.05 &&
               word.value("end", 0.0) == 0.25 && word.value("skipped", false),
           "text, timing, and skipped state use Qt-compatible fields");
    expect(word.value("future_word_field", std::string{}) == "keep" &&
               root["future_root_field"].value("preserved", false),
           "unknown root and word fields survive mutation");
    expect(jcut::saveTranscriptDocumentAtomic(path.string(), root, &error),
           "mutated transcript saves atomically");
    expect(!fs::exists(path.string() + ".jcut.tmp"),
           "atomic save leaves no temporary file");

    expect(jcut::deleteTranscriptWord(&root, row->word, &error),
           "neutral deletion removes the addressed word");
    expect(root["segments"][0]["words"].size() == 1 &&
               root.value("transcript_deleted_edits", 0) == 1,
           "deletion matches Qt word-count bookkeeping");

    json batch = transcriptRoot("one", true);
    const auto batchDocument =
        jcut::TranscriptDocumentCore::fromJson(batch);
    expect(batchDocument.has_value(),
           "batch mutation fixture parses");
    if (batchDocument) {
        const auto batchRows = batchDocument->rows();
        std::vector<jcut::TranscriptWordRef> references;
        for (const auto& batchRow : batchRows) {
            if (!batchRow.gap) {
                references.push_back(batchRow.word);
            }
        }
        expect(references.size() == 2 &&
                   jcut::deleteTranscriptWords(
                       &batch, references, &error),
               "batch delete accepts stable projected references");
        expect(batch["segments"][0]["words"].empty() &&
                   batch.value(
                       "transcript_deleted_edits", 0) == 2,
               "batch delete is address-shift safe and counts every word");
    }
}

void testNeutralInsertExpandRestoreAndReorderMatchQtSemantics()
{
    json original = transcriptRoot("first", true);
    original["segments"][0]["words"][0]["render_order"] = 0;
    original["segments"][0]["words"][1]["render_order"] = 1;
    json edited = original;
    auto document = jcut::TranscriptDocumentCore::fromJson(edited);
    expect(document.has_value(), "advanced mutation fixture parses");
    if (!document) return;
    auto rows = document->rows();
    const auto first = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
        return row.text == "first";
    });
    expect(first != rows.end(), "advanced mutation anchor projects");
    if (first == rows.end()) return;

    std::string error;
    const auto inserted =
        jcut::insertTranscriptWord(&edited, first->word, false, &error);
    expect(inserted.has_value(), "word inserts below the selected render row");
    if (!inserted) return;
    const json& insertedWord =
        edited["segments"][static_cast<std::size_t>(inserted->segmentIndex)]["words"]
              [static_cast<std::size_t>(inserted->wordIndex)];
    expect(insertedWord.value("word", std::string{}) == "[new]" &&
               insertedWord.value("original_segment_index", 0) == -1 &&
               insertedWord.value("render_order", -1) == 1,
           "inserted word uses Qt defaults and render position");
    expect(edited["segments"][0]["words"][2].value("render_order", -1) == 2,
           "insertion normalizes following render order");

    expect(jcut::expandTranscriptWordTiming(&edited, *inserted, &error),
           "inserted word expands to adjacent raw boundaries");
    const json& expanded =
        edited["segments"][0]["words"][static_cast<std::size_t>(inserted->wordIndex)];
    expect(expanded.value("start", -1.0) == 0.1 &&
               expanded.value("end", -1.0) == 0.3,
           "expand uses previous end and next start");

    expect(jcut::moveTranscriptWordRenderOrder(&edited, *inserted, 1, &error),
           "word moves down in render order");
    expect(expanded.value("render_order", -1) == 2 &&
               edited["segments"][0]["words"][2].value("render_order", -1) == 1,
           "reorder swaps normalized render positions without moving source words");

    const auto reorderedDocument =
        jcut::TranscriptDocumentCore::fromJson(edited);
    expect(reorderedDocument.has_value(),
           "batch reorder fixture parses");
    if (reorderedDocument) {
        auto reorderedRows = reorderedDocument->rows();
        std::vector<jcut::TranscriptWordRef> desired;
        for (auto it = reorderedRows.rbegin();
             it != reorderedRows.rend(); ++it) {
            if (!it->gap) desired.push_back(it->word);
        }
        expect(jcut::reorderTranscriptWords(
                   &edited, desired, &error),
               "arbitrary table order delegates to neutral reorder");
        const auto afterReorder =
            jcut::TranscriptDocumentCore::fromJson(edited);
        expect(afterReorder.has_value() &&
                   afterReorder->rows().front().word.segmentIndex ==
                       desired.front().segmentIndex &&
                   afterReorder->rows().front().word.wordIndex ==
                       desired.front().wordIndex,
               "neutral reorder persists the requested projected order");
    }

    jcut::TranscriptWordPatch patch;
    patch.text = "changed";
    patch.startSeconds = 0.4;
    patch.endSeconds = 0.5;
    patch.skipped = true;
    expect(jcut::patchTranscriptWord(&edited, first->word, patch, &error),
           "original-backed word can be changed before restore");
    expect(jcut::restoreTranscriptWord(&edited, first->word, original, &error),
           "word restores from original segment/word identity");
    const json& restored = edited["segments"][0]["words"][0];
    expect(restored.value("word", std::string{}) == "first" &&
               restored.value("start", -1.0) == 0.0 &&
               restored.value("end", -1.0) == 0.1 &&
               !restored.value("skipped", false) &&
               !restored.contains("transcript_edits"),
           "restore resets Qt-managed content and edit tags");
}

void testNeutralCutLifecycleMatchesQtGuardsAndMetadata()
{
    TemporaryDirectory temp;
    const fs::path mediaPath = temp.path / "clip.wav";
    const fs::path originalPath = temp.path / "clip.json";
    writeBytes(mediaPath, "audio");
    writeJson(originalPath, transcriptRoot("first", true));
    jcut::TranscriptSourceSpec source;
    source.sourcePath = mediaPath.string();
    jcut::TranscriptCutSessionOptions options;
    options.ensureEditable = true;
    jcut::TranscriptCutSession session =
        jcut::loadTranscriptCutSession(source, options);
    expect(session.ok() && session.activeCutMutable,
           "cut lifecycle starts on the ensured editable cut");

    std::string error;
    const auto version = jcut::createTranscriptCutVersion(session, &error);
    expect(version.has_value() && fs::exists(*version) &&
               fs::path(*version).filename() == "clip_editable_v2.json",
           "new cut uses Qt numeric version naming");
    if (!version) return;
    options.requestedActivePath = *version;
    session = jcut::loadTranscriptCutSession(source, options);
    expect(session.ok() && session.activePath == *version,
           "created cut is discoverable and selectable");
    expect(jcut::renameTranscriptCut(session, "Director Review", &error),
           "mutable cut label saves");
    session = jcut::loadTranscriptCutSession(source, options);
    expect(std::any_of(
               session.catalog.cuts.begin(), session.catalog.cuts.end(),
               [&](const auto& cut) {
                   return cut.path == *version && cut.label == "Director Review";
               }),
           "saved cut label is rediscovered");

    std::string fallback;
    expect(jcut::deleteTranscriptCut(session, &fallback, &error) &&
               !fs::exists(*version) &&
               fallback == session.catalog.editablePath,
           "mutable cut deletion selects the base editable fallback");
    options.requestedActivePath = session.catalog.originalPath;
    session = jcut::loadTranscriptCutSession(source, options);
    expect(!jcut::deleteTranscriptCut(session, &fallback, &error) &&
               fs::exists(originalPath),
           "original cut remains immutable during deletion");
}

void testNeutralSpeakerRosterAndProfileMutation()
{
    json root = transcriptRoot("hello", true);
    root["speaker_profiles"]["S1"]["organization"] = "Example Org";
    root["speaker_profiles"]["S1"]["primary_color"] = "#102030";
    root["speaker_profiles"]["S1"]["secondary_color"] = "#405060";
    root["speaker_profiles"]["S1"]["accent_color"] = "#708090";
    root["speaker_profiles"]["S1"]["location"] = {{"x", 0.2}, {"y", 0.7}};
    root["speaker_profiles"]["S1"]["future_profile_field"] = "keep";
    auto document = jcut::TranscriptDocumentCore::fromJson(root);
    expect(document.has_value(), "speaker roster fixture parses");
    if (!document) return;
    const auto profiles = document->speakerProfiles();
    expect(profiles.size() == 1 && profiles.front().id == "S1" &&
               profiles.front().name == "Alice" &&
               profiles.front().organization == "Example Org" &&
               profiles.front().primaryColor == "#102030" &&
               profiles.front().secondaryColor == "#405060" &&
               profiles.front().accentColor == "#708090" &&
               profiles.front().wordCount == 2 &&
               profiles.front().x == 0.2 && profiles.front().y == 0.7,
           "speaker roster combines profiles, locations, and word counts");

    jcut::TranscriptSpeakerProfilePatch patch;
    patch.name = "Alicia";
    patch.organization = "New Org";
    patch.x = 2.0;
    patch.y = -1.0;
    std::string error;
    expect(jcut::patchTranscriptSpeakerProfile(&root, "S1", patch, &error),
           "speaker profile patch applies");
    const json& profile = root["speaker_profiles"]["S1"];
    expect(profile.value("name", std::string{}) == "Alicia" &&
               profile.value("organization", std::string{}) == "New Org" &&
               profile["location"].value("x", -1.0) == 1.0 &&
               profile["location"].value("y", -1.0) == 0.0 &&
               profile.value("future_profile_field", std::string{}) == "keep",
           "speaker profile patch clamps location and preserves unknown fields");
}

void testNeutralFaceArtifactInspectionAndTrackAssignment()
{
    TemporaryDirectory temp;
    const fs::path transcriptPath = temp.path / "clip_editable.json";
    writeJson(transcriptPath, transcriptRoot("hello", true));
    const std::string clipId = "clip-1";
    const json facedetections = {
        {"schema", "jcut_facedetections_v1"},
        {"continuity_facedetections_by_clip", {
            {clipId, {
                {"run_id", "run-7"},
                {"detector_mode", "scrfd"},
                {"raw_frames_count", 12},
                {"raw_tracks_frame_domain", "source_relative"},
                {"raw_tracks", json::array({
                    {
                        {"track_id", 4},
                        {"first_frame", 10},
                        {"last_frame", 20},
                        {"state", "confirmed"},
                        {"detections", json::array({
                            {{"frame", 10}, {"x", 0.25}, {"y", 0.4},
                             {"box", 0.3}, {"score", 0.92}},
                            {{"frame", 11}, {"x", 0.90}, {"y", 0.4},
                             {"box", 0.3}, {"score", 0.92}},
                            {{"frame", 12}, {"x", 0.27}, {"y", 0.4},
                             {"box", 0.3}, {"score", 0.92}}
                        })}
                    },
                    {
                        {"track_id", 9},
                        {"first_frame", 30},
                        {"last_frame", 40},
                        {"state", "confirmed"},
                        {"detections", json::array({
                            {{"frame", 30}, {"x", 0.7}, {"y", 0.5},
                             {"box", 0.2}, {"score", 0.88}}
                        })}
                    }
                })}
            }}
        }}
    };
    const fs::path artifactPath =
        jcut::faceArtifactCandidatePaths(transcriptPath.string()).front();
    expect(writeJcutBoxV2(artifactPath, facedetections),
           "Qt-compatible compressed face artifact written");
    const json identity = {
        {"schema", "jcut_identity_v1"},
        {"identity_clusters_by_clip", {
            {clipId, {{"clusters", json::array({json::object(), json::object()})}}}
        }},
        {"identity_assignments_by_clip", {
            {clipId, {{"track_identity_map", json::array({
                {{"track_id", 4}, {"identity_id", "S1"}}
            })}}}
        }}
    };
    expect(writeJcutBoxV2(
               jcut::faceIdentityArtifactPath(transcriptPath.string()), identity),
           "Qt-compatible identity artifact written");

    const jcut::FaceArtifactInspectionCore inspection =
        jcut::inspectFaceArtifacts(transcriptPath.string(), clipId);
    expect(inspection.ok() && inspection.tracks.size() == 2 &&
               inspection.detectorMode == "scrfd" &&
               inspection.rawFrameCount == 12 &&
               inspection.identityClusterCount == 2 &&
               inspection.identityAssignmentCount == 1,
           "neutral artifact reader exposes continuity and identity diagnostics");
    if (inspection.tracks.size() == 2) {
        expect(inspection.tracks[0].trackId == 4 &&
                   inspection.tracks[0].sampleCount == 3 &&
                   inspection.tracks[0].x == 0.25 &&
                   inspection.tracks[1].trackId == 9,
               "neutral track summaries retain IDs, coverage, and anchor geometry");
    }

    json transcript = transcriptRoot("hello", true);
    transcript["future_root_field"]["assignment_keep"] = true;
    std::string error;
    const std::vector<jcut::TranscriptTrackAssignmentAnchor> anchors{
        {4, "stream-4", 10, 0.25, 0.4, 0.3},
        {9, "stream-9", 30, 0.7, 0.5, 0.2},
    };
    expect(jcut::setTranscriptSpeakerTrackAssignments(
               &transcript, clipId, "S1", anchors, true,
               "2026-07-22T00:00:00Z", &error),
           "neutral assignment mutation applies selected tracks");
    auto assignments =
        jcut::transcriptSpeakerTrackAssignments(transcript, clipId);
    expect(assignments.size() == 2 &&
               assignments[0].trackId == 4 &&
               assignments[1].trackId == 9 &&
               assignments[0].identityId == "S1" &&
               transcript["future_root_field"].value("assignment_keep", false),
           "assignment projection and mutation preserve compatible schema and unknown fields");
    const auto activeAssignments =
        jcut::transcriptSpeakerTrackAssignmentsAtFrame(
            transcript, clipId, "S1", 10);
    expect(activeAssignments.size() == 2 &&
               activeAssignments.front().trackId == 4,
           "frame-aware neutral assignment resolution falls back to "
           "identity mappings");
    json sectionTranscript = transcript;
    sectionTranscript["speaker_flow"]["clips"][clipId]
                     ["resolved_current"]["section_track_map"] =
        json::array({{
            {"speaker_id", "S1"},
            {"start_frame", 100},
            {"end_frame", 120},
            {"rotation", 12.0},
            {"tracks", json::array({{{"track_id", 4}}})},
        }});
    const auto sectionAssignments =
        jcut::transcriptSpeakerTrackAssignmentsAtFrame(
            sectionTranscript, clipId, "S1", 110);
    expect(sectionAssignments.size() == 1 &&
               sectionAssignments.front().trackId == 4 &&
               std::abs(
                   sectionAssignments.front().rotationDegrees -
                   12.0) < 1.0e-12,
           "active section assignment carries its center/face rotation");
    const jcut::FaceTrackingSampleCore sample =
        jcut::sampleFaceContinuityTrack(
            transcriptPath.string(),
            clipId,
            4,
            {},
            110,
            0.5,
            100,
            10.0,
            0);
    expect(sample.valid &&
               std::abs(sample.x - 0.25) < 1.0e-12 &&
               std::abs(sample.y - 0.4) < 1.0e-12 &&
               std::abs(sample.box - 0.3) < 1.0e-12 &&
               std::abs(sample.score - 0.92) < 1.0e-12,
           "neutral continuity sampling resolves assigned face geometry "
           "with confidence filtering and source-relative mapping");
    const jcut::FaceTrackingSampleCore unsmoothed =
        jcut::sampleFaceContinuityTrack(
            transcriptPath.string(),
            clipId,
            4,
            {},
            111,
            0.5,
            100,
            11.0,
            0);
    const jcut::FaceTrackingSampleCore smoothed =
        jcut::sampleFaceContinuityTrack(
            transcriptPath.string(),
            clipId,
            4,
            {},
            111,
            0.5,
            100,
            11.0,
            0,
            3,
            0,
            0,
            1.0,
            0.0);
    expect(unsmoothed.valid && smoothed.valid &&
               std::abs(unsmoothed.x - 0.90) < 1.0e-12 &&
               smoothed.x < unsmoothed.x &&
               smoothed.x > 0.25,
           "neutral continuity smoothing matches Qt robust outlier "
           "rejection and strength blending");
    expect(jcut::setTranscriptSpeakerTrackAssignments(
               &transcript, clipId, "S1", {}, true,
               "2026-07-22T00:01:00Z", &error),
           "replace-with-empty clears one speaker's assignments");
    expect(jcut::transcriptSpeakerTrackAssignments(transcript, clipId).empty(),
           "speaker assignment clearing is scoped and observable");
}

void testNeutralFaceProcessingJobContract()
{
    expect(jcut::faceTrackAnchorTimelineFrame(
               160, 100, 300, 120, 2.0) == 330,
           "face reference maps source-absolute anchors through clip rate");
    expect(jcut::faceTrackAnchorTimelineFrame(
               9999, 100, 300, 120, 1.0) == 419,
           "face reference navigation clamps to the selected clip");

    const fs::path media = "/project/My Camera.mov";
    const std::string output = jcut::faceProcessingSidecarDirectory(
        media.string(), "clip / 7");
    expect(output == "/project/My_Camera.jcut/facedetections/clip_7",
           "neutral face job uses the Qt media-sidecar and clip-token layout");

    jcut::FaceProcessingJobRequest request;
    request.executablePath = "/build/jcut_vulkan_facedetections_offscreen";
    request.mediaPath = media.string();
    request.clipId = "clip / 7";
    request.outputDirectory = output;
    request.detectorSettingsPath = "/project/My Camera_detectorsettings.json";
    request.startFrame = 12;
    request.maxFrames = 90;
    request.stride = 3;
    request.detectorWorkers = 4;
    request.detectorPipelineSlots = 4;
    request.primaryFaceOnly = true;
    request.smallFaceFallback = false;
    request.scrfdTiling = true;
    const std::vector<std::string> command =
        jcut::faceProcessingCommand(request);
    const auto has = [&](const std::string& value) {
        return std::find(command.begin(), command.end(), value) != command.end();
    };
    expect(!command.empty() && command.front() == request.executablePath &&
               command.size() > 1 && command[1] == request.mediaPath,
           "neutral face job launches the existing offscreen executable directly");
    expect(has("--detector") && has("scrfd-ncnn-vulkan") &&
               has("--params-file") && has("--start-frame") &&
               has("--max-frames") && has("--out-dir") &&
               has("--primary-face-only") &&
               has("--no-small-face-fallback") && has("--scrfd-tiling") &&
               has("--no-control-window") && has("--no-preview-window"),
           "neutral face job retains Qt generator options and headless UI policy");
    request.controlWindow = true;
    request.livePreview = true;
    request.restartFromScratch = true;
    request.applyClipGrading = true;
    request.clipJsonPath = "/project/clip_input.json";
    const std::vector<std::string> interactiveCommand =
        jcut::faceProcessingCommand(request);
    expect(
        std::find(
            interactiveCommand.begin(), interactiveCommand.end(),
            "--control-window") != interactiveCommand.end() &&
            std::find(
                interactiveCommand.begin(), interactiveCommand.end(),
                "--preview-window") != interactiveCommand.end() &&
            std::find(
                interactiveCommand.begin(), interactiveCommand.end(),
                "--apply-clip-grading") != interactiveCommand.end() &&
            std::find(
                interactiveCommand.begin(), interactiveCommand.end(),
                "/project/clip_input.json") != interactiveCommand.end(),
        "neutral face preflight can opt into Qt generator windows and clip grading");

    jcut::EditorClip gradingClip;
    gradingClip.id = 42;
    gradingClip.persistentId = "graded-clip";
    gradingClip.sourcePath = media.string();
    gradingClip.brightness = 0.2;
    gradingClip.contrast = 1.1;
    gradingClip.saturation = 0.8;
    gradingClip.opacity = 0.9;
    const json gradingJson = jcut::toLegacyClipJson(gradingClip);
    expect(
        gradingJson.value("id", std::string{}) == "graded-clip" &&
            std::abs(gradingJson.value("brightness", 0.0) - 0.2) <
                1.0e-12 &&
            gradingJson.contains("gradingKeyframes"),
        "shared legacy clip projection supplies the Qt-compatible detector grading input");

    TemporaryDirectory temporary;
    const fs::path launchDirectory = temporary.path / "launch";
    fs::create_directories(launchDirectory);
    expect(
        writeJson(
            launchDirectory / "launch_control.json",
            json{
                {"schema", "jcut_facedetections_launch_control_v1"},
                {"detector_workers", 2},
                {"detector_pipeline_slots", 2},
                {"last_benchmark",
                 {
                     {"best_detector_workers", 6},
                     {"best_detector_pipeline_slots", 4},
                 }},
            }),
        "face benchmark launch-control fixture written");
    const jcut::FaceProcessingLaunchControl launchControl =
        jcut::loadFaceProcessingLaunchControl(launchDirectory.string());
    expect(
        launchControl.hasRecommendation &&
            launchControl.detectorWorkers == 6 &&
            launchControl.detectorPipelineSlots == 4 &&
            !launchControl.benchmarkJson.empty(),
        "neutral face preflight reloads the Qt-compatible saved benchmark recommendation");

    const fs::path transcriptPath = temporary.path / "session.json";
    const fs::path artifactDirectory = temporary.path / "generated";
    fs::create_directories(artifactDirectory);
    expect(writeJson(transcriptPath, transcriptRoot("hello", true)),
           "face import transcript fixture written");
    const json generated = {
        {"schema", "jcut_facedetections_v1"},
        {"continuity_facedetections_by_clip", {
            {"facedetections-offscreen-source", {
                {"run_id", "offscreen"},
                {"detector_mode", "scrfd"},
                {"streams_frame_domain", "source_absolute"},
                {"future_generator_field", 17}
            }}
        }}
    };
    expect(writeQtBinaryJson(
               artifactDirectory / "continuity_facedetections.bin", generated),
           "Qt binary generator continuity fixture written");
    const json track = {
        {"type", "track"},
        {"track_id", 12},
        {"stream_id", "stream-12"},
        {"state", "confirmed"},
        {"detections", json::array({
            {{"frame", 40}, {"x", 0.3}, {"y", 0.4},
             {"box", 0.2}, {"score", 0.9}}
        })}
    };
    const json summary = {
        {"type", "frame_summary"}, {"frame", 40}, {"count", 1}};
    std::string trackData =
        qtCompressed(json::to_cbor(track)) +
        qtCompressed(json::to_cbor(summary));
    expect(writeBytes(artifactDirectory / "tracks.dat", trackData),
           "Qt compressed-CBOR indexed track records written");
    std::string importError;
    expect(jcut::importGeneratedFaceArtifacts(
               artifactDirectory.string(), transcriptPath.string(),
               "clip-12", &importError),
           "neutral generator importer materializes transcript JCUTBOX1 artifact: " +
               importError);
    const jcut::FaceArtifactInspectionCore imported =
        jcut::inspectFaceArtifacts(transcriptPath.string(), "clip-12");
    expect(imported.ok() && imported.tracks.size() == 1 &&
               imported.tracks.front().trackId == 12 &&
               imported.detectorMode == "scrfd",
           "imported generator tracks are immediately consumable by ImGui and Qt");
    json importedRoot;
    expect(jcut::loadJcutBoxDocument(
               jcut::faceArtifactCandidatePaths(
                   transcriptPath.string()).front(),
               &importedRoot, &importError) &&
               importedRoot["continuity_facedetections_by_clip"]["clip-12"]
                   .value("future_generator_field", 0) == 17,
           "generator import preserves unknown continuity fields");
}

void testNeutralSpeakerTitleGenerationAndReplacement()
{
    json root = {
        {"speaker_profiles", {
            {"S1", {
                {"name", "Alice"},
                {"organization", "Example Org"},
                {"primary_color", "#aabbcc"},
                {"secondary_color", "#112233"},
                {"accent_color", "#44ccff"},
                {"logo_path", "/assets/alice.png"}
            }},
            {"S2", {{"name", "Bob"}}}
        }},
        {"segments", json::array({
            {{"words", json::array({
                {{"word", "hello"}, {"start", 0.0}, {"end", 0.2},
                 {"speaker", "S1"}},
                {{"word", "again"}, {"start", 0.3}, {"end", 0.5},
                 {"speaker", "S1"}},
                {{"word", "response"}, {"start", 1.0}, {"end", 1.3},
                 {"speaker", "S2"}},
                {{"word", "return"}, {"start", 2.0}, {"end", 2.2},
                 {"speaker", "S1"}}
            })}}
        })}
    };
    std::string error;
    auto transcript = jcut::TranscriptDocumentCore::fromJson(root, &error);
    expect(transcript.has_value(),
           "speaker-title transcript fixture parses: " + error);
    if (!transcript) return;

    jcut::EditorClip source;
    source.id = 7;
    source.trackId = 1;
    source.persistentId = "source-7";
    source.startFrame = 100;
    source.durationFrames = 120;
    source.sourceInFrame = 0;
    source.sourceDurationFrames = 120;
    source.playbackRate = 1.0;
    source.mediaKind = "video";
    jcut::SpeakerTitleFlyInSettingsCore settings;
    settings.style = jcut::SpeakerTitleFlyInStyleCore::SlideFromRight;
    settings.showSpeakerOrganization = true;
    const std::vector<jcut::EditorClip> generated =
        jcut::makeSpeakerTitleClipsCore(source, *transcript, 0, settings);
    expect(generated.size() == 3,
           "speaker introductions are generated only at speaker changes");
    if (generated.size() == 3) {
        expect(generated[0].label == "Speaker: Alice\nExample Org" &&
                   generated[0].clipRole == "speaker_title" &&
                   generated[0].linkedSourceClipId == "source-7" &&
                   generated[0].titleKeyframes.size() >= 4 &&
                   generated[0].titleKeyframes[1].color == "#aabbcc" &&
                   generated[1].startFrame == 130 &&
                   generated[2].startFrame == 160,
               "neutral titles retain profile styling, ownership, timing, and fly-in keyframes");
    }

    jcut::EditorDocumentCore document;
    document.projectName = "Speaker titles";
    document.tracks.push_back({1, "Video", true});
    document.clips.push_back(source);
    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    const auto applied = runtime.execute(jcut::EditorCommand{
        jcut::ReplaceSpeakerTitleClipsCommand{7, generated}});
    const jcut::EditorDocumentCore after = runtime.snapshot();
    const auto titleCount = std::count_if(
        after.clips.begin(), after.clips.end(),
        [](const auto& clip) {
            return jcut::canonicalEditorClipRole(clip.clipRole) ==
                "speaker_title";
        });
    expect(applied.applied && titleCount == 3 &&
               after.tracks.size() >= 2,
           "runtime atomically places generated titles on dedicated lanes");
    expect(runtime.execute(
               jcut::EditorCommand{jcut::UndoCommand{}}).applied &&
               runtime.snapshot().clips.size() == 1,
           "speaker-title replacement is one undoable edit");
}

void testNeutralTranscriptMiningProposals()
{
    json root = {
        {"speaker_profiles", {
            {"S1", json::object()},
            {"S2", {{"name", "Existing Name"}}}
        }},
        {"segments", json::array({
            {
                {"speaker", "S1"},
                {"words", json::array({
                    {{"word", "Alice"}, {"speaker", "S1"}},
                    {{"word", "Smith."}, {"speaker", "S1"}},
                    {{"word", "Metro"}, {"speaker", "S1"}},
                    {{"word", "City"}, {"speaker", "S1"}},
                    {{"word", "Council"}, {"speaker", "S1"}},
                    {{"word", "aside"}, {"speaker", "S9"}}
                })}
            }
        })},
        {"future_root_field", {{"mining_keep", true}}}
    };
    const auto names = jcut::mineTranscriptSpeakerNames(root);
    const auto organizations = jcut::mineTranscriptOrganizations(root);
    const auto cleanup = jcut::mineSpuriousSpeakerAssignments(root);
    expect(!names.empty() && names.front().targetId == "S1" &&
               names.front().proposedValue == "Alice Smith",
           "neutral mining finds Qt-compatible person-name candidates");
    expect(!organizations.empty() &&
               organizations.front().proposedValue ==
                   "Metro City Council",
           "neutral mining finds Qt-compatible organization candidates");
    expect(cleanup.size() == 1 &&
               cleanup.front().currentValue == "S9" &&
               cleanup.front().proposedValue == "S1",
           "neutral mining proposes one-off speaker cleanup");
    std::vector<jcut::TranscriptMiningProposal> selected;
    if (!names.empty()) selected.push_back(names.front());
    if (!organizations.empty()) selected.push_back(organizations.front());
    if (!cleanup.empty()) selected.push_back(cleanup.front());
    std::string error;
    expect(jcut::applyTranscriptMiningProposals(
               &root, selected, &error),
           "selected mining proposals apply: " + error);
    expect(root["speaker_profiles"]["S1"].value("name", "") ==
                   "Alice Smith" &&
               root["speaker_profiles"]["S1"].value(
                   "organization", "") == "Metro City Council" &&
               root["segments"][0]["words"][5].value("speaker", "") ==
                   "S1" &&
               root["future_root_field"].value("mining_keep", false),
           "mining application preserves unknown fields and scopes edits");

    const json cloudPayload =
        jcut::buildCloudSpeakerMiningPayload(root);
    expect(
            cloudPayload.value("task", "") == "mine_speaker_profiles" &&
            cloudPayload["speaker_legend"].is_array() &&
            cloudPayload["speaker_legend"].size() == 1,
        "cloud mining payload uses stable speaker tokens");
    const auto cloudProposals =
        jcut::parseCloudSpeakerMiningResponse(
            root,
            json{{"result",
                  {{"speakers",
                    json::array({
                        json{
                            {"Speaker", "S1"},
                            {"Name", "Alicia Smith"},
                            {"Organization", "Regional Council"},
                        },
                    })}}}},
            &error);
    expect(
        cloudProposals.size() == 2 &&
            cloudProposals[0].targetId == "S1" &&
            cloudProposals[0].proposedValue == "Alicia Smith" &&
            cloudProposals[1].proposedValue == "Regional Council",
        "cloud mining response becomes reviewable neutral proposals");
}

void testNeutralProxyDiscoveryAndRuntimeCommand()
{
    TemporaryDirectory temporary;
    expect(!temporary.path.empty(), "proxy test creates temporary directory");
    if (temporary.path.empty()) return;

    const fs::path sourcePath = temporary.path / "interview.mov";
    const fs::path proxyPath = temporary.path / "interview.proxy.mp4";
    expect(writeBytes(sourcePath, "source") &&
               writeBytes(proxyPath, "proxy"),
           "proxy test fixture writes source and proxy files");
    const std::vector<std::string> candidates =
        jcut::proxyCandidatePaths(sourcePath.string());
    expect(candidates.size() == 3 &&
               candidates[1] == proxyPath.string() &&
               jcut::proxyPathIsUsable(proxyPath.string()) &&
               jcut::discoverExistingProxyPath(sourcePath.string()) ==
                   proxyPath.string(),
           "neutral proxy discovery matches Qt-compatible default names");

    jcut::EditorDocumentCore document;
    document.projectName = "Proxy";
    document.tracks.push_back({1, "Video", true});
    jcut::EditorClip source;
    source.id = 1;
    source.trackId = 1;
    source.persistentId = "source-1";
    source.sourcePath = sourcePath.string();
    source.durationFrames = 30;
    document.clips.push_back(source);
    jcut::EditorClip matte = source;
    matte.id = 2;
    matte.persistentId = "matte-2";
    matte.clipRole = "mask_matte";
    matte.linkedSourceClipId = "source-1";
    document.clips.push_back(matte);

    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    const auto attached = runtime.execute(jcut::EditorCommand{
        jcut::SetClipProxyCommand{1, proxyPath.string(), true}});
    const jcut::EditorDocumentCore attachedDocument = runtime.snapshot();
    expect(attached.applied &&
               attachedDocument.clips[0].proxyPath == proxyPath.string() &&
               attachedDocument.clips[0].useProxy &&
               attachedDocument.clips[1].proxyPath == proxyPath.string() &&
               attachedDocument.clips[1].useProxy,
           "proxy attach is undoable and propagates to generated mask children");
    expect(!runtime.execute(jcut::EditorCommand{
                jcut::SetClipProxyCommand{2, {}, false}}).applied,
           "direct mask-child proxy edits are rejected");
    expect(runtime.execute(
               jcut::EditorCommand{jcut::UndoCommand{}}).applied &&
               runtime.snapshot().clips[0].proxyPath.empty(),
           "proxy association undo restores the source and child cache");
}

} // namespace

int main()
{
    testSourceSelectionMatchesQtPolicy();
    testCatalogSelectionRowsAndInvalidation();
    testNeutralActiveCutRoundTripAndCommand();
    testImGuiAndRichRenderWiringContracts();
    testNeutralWordMutationPreservesUnknownFieldsAndSavesAtomically();
    testNeutralInsertExpandRestoreAndReorderMatchQtSemantics();
    testNeutralCutLifecycleMatchesQtGuardsAndMetadata();
    testNeutralSpeakerRosterAndProfileMutation();
    testNeutralFaceArtifactInspectionAndTrackAssignment();
    testNeutralFaceProcessingJobContract();
    testNeutralSpeakerTitleGenerationAndReplacement();
    testNeutralTranscriptMiningProposals();
    testNeutralProxyDiscoveryAndRuntimeCommand();

    if (g_failures != 0) {
        std::cerr << g_failures << " transcript cut session assertion(s) failed\n";
        return 1;
    }
    std::cout << "transcript cut session assertions passed\n";
    return 0;
}
