#include "../transcript_document_core.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

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

const jcut::TranscriptRow* rowWithText(
    const std::vector<jcut::TranscriptRow>& rows,
    const std::string& text)
{
    for (const jcut::TranscriptRow& row : rows) {
        if (row.text == text) {
            return &row;
        }
    }
    return nullptr;
}

jcut::TranscriptRowBuildOptions unpaddedOptions()
{
    jcut::TranscriptRowBuildOptions options;
    options.timing.framesPerSecond = 30.0;
    options.timing.prependMilliseconds = 0;
    options.timing.postpendMilliseconds = 0;
    options.timing.offsetMilliseconds = 0;
    options.adjustOverlaps = false;
    options.insertGaps = false;
    return options;
}

void testParseOrderingLabelsFlagsAndPreservation()
{
    const json root = {
        {"root_extension", {{"keep", true}}},
        {"speaker_profiles", {
            {"S1", {{"name", "Alice"}, {"profile_extension", 7}}},
            {"S2", {{"name", "Bob"}}},
        }},
        {"segments", json::array({
            {
                {"speaker", " S1 "},
                {"segment_extension", "preserve-me"},
                {"words", json::array({
                    {
                        {"word", "later-explicit"},
                        {"start", 2.0},
                        {"end", 2.2},
                        {"render_order", 2},
                        {"skipped", true},
                        {"transcript_edits", json::array(
                            {" timing ", "text", "skip", "inserted", "future-tag"})},
                        {"word_extension", {{"confidence", 0.91}}},
                    },
                    {
                        {"word", "first-explicit"},
                        {"start", 1.0},
                        {"end", 1.2},
                        {"render_order", 0},
                        {"speaker", "S2"},
                    },
                    {
                        {"word", "unassigned-earlier"},
                        {"start", 0.1},
                        {"end", 0.2},
                    },
                    {
                        {"word", "   "},
                        {"start", 3.0},
                        {"end", 3.1},
                        {"unknown_blank_word_field", 42},
                    },
                })},
            },
        })},
    };

    std::string error;
    const auto document = jcut::TranscriptDocumentCore::fromJson(root, &error);
    expect(document.has_value(), "object transcript parses");
    expect(error.empty(), "successful parse clears error");
    if (!document) {
        return;
    }
    expect(document->root() == root, "all unknown JSON remains structurally equivalent");
    expect(document->wordCount() == 4, "model retains invalid/hidden words without data loss");

    const std::vector<jcut::TranscriptRow> rows = document->rows(unpaddedOptions());
    expect(rows.size() == 3, "blank words are omitted only from the row projection");
    if (rows.size() != 3) {
        return;
    }
    expect(rows[0].text == "first-explicit", "explicit render order sorts first");
    expect(rows[1].text == "later-explicit", "explicit render order value is honored");
    expect(rows[2].text == "unassigned-earlier",
           "words without render order follow explicitly ordered words");
    expect(rows[0].word.renderOrder == 0 && rows[1].word.renderOrder == 1 &&
               rows[2].word.renderOrder == 2,
           "row render order is normalized without mutating source JSON");
    expect(rows[0].speakerId == "S2" && rows[0].speakerLabel == "Bob",
           "word speaker and profile display name are projected");
    expect(rows[1].speakerId == "S1" && rows[1].speakerLabel == "Alice",
           "segment speaker fallback is trimmed and labeled");
    expect(rows[1].editFlags ==
               (jcut::TranscriptEditTiming | jcut::TranscriptEditText |
                jcut::TranscriptEditSkip | jcut::TranscriptEditInserted),
           "known edit tags map to stable flag bits and unknown tags are ignored");
    expect(rows[1].skipped && !rows[1].eligibleForFollow(),
           "skipped rows are not follow candidates");
    expect(rows[0].eligibleForFollow(), "ordinary rows are follow candidates");
}

void testTimingOverlapAndFormatting()
{
    const json root = {
        {"segments", json::array({
            {{"words", json::array({
                {{"word", "a"}, {"start", 0.50}, {"end", 0.79}},
                {{"word", "b"}, {"start", 0.84}, {"end", 0.99}},
            })}},
        })},
    };
    const auto document = jcut::TranscriptDocumentCore::fromJson(root);
    expect(document.has_value(), "timing fixture parses");
    if (!document) {
        return;
    }

    jcut::TranscriptRowBuildOptions options;
    options.timing.framesPerSecond = 30.0;
    options.timing.prependMilliseconds = 150;
    options.timing.postpendMilliseconds = 70;
    options.timing.offsetMilliseconds = 30;
    options.adjustOverlaps = true;
    options.insertGaps = false;
    const std::vector<jcut::TranscriptRow> rows = document->rows(options);
    expect(rows.size() == 2, "timing projection yields both words");
    if (rows.size() == 2) {
        expect(rows[0].sourceStartFrame == 11,
               "start frame applies offset and subtracts prepend padding");
        expect(rows[0].sourceEndFrame == 23 && rows[1].sourceStartFrame == 24,
               "overlap is split deterministically between adjacent rows");
        expect(rows[1].sourceEndFrame == 32,
               "end frame applies offset and postpend padding with inclusive rounding");
        expect(std::abs(rows[0].rawStartSeconds - 0.50) < 1.0e-12 &&
                   std::abs(rows[0].rawEndSeconds - 0.79) < 1.0e-12,
               "raw word timing remains separate from display timing");
    }

    expect(jcut::formatTranscriptTime(0.0) == "00:00.000", "zero time formatting");
    expect(jcut::formatTranscriptTime(65.432) == "01:05.432", "minute time formatting");
    expect(jcut::formatTranscriptTime(-4.0) == "00:00.000", "negative time clamps");
    double seconds = -1.0;
    expect(jcut::parseTranscriptTime(" 01:05.250 ", &seconds) &&
               std::abs(seconds - 65.25) < 1.0e-12,
           "minute time parsing");
    expect(jcut::parseTranscriptTime("2.75", &seconds) &&
               std::abs(seconds - 2.75) < 1.0e-12,
           "numeric time parsing");
    expect(!jcut::parseTranscriptTime("1:2:3", &seconds), "malformed time is rejected");
    expect(!jcut::parseTranscriptTime("nan", &seconds), "non-finite time is rejected");
}

void testGapsAndRenderFrames()
{
    const json root = {
        {"segments", json::array({
            {{"words", json::array({
                {{"word", "a"}, {"start", 0.0}, {"end", 0.1}},
                {{"word", "b"}, {"start", 0.2}, {"end", 0.29}},
            })}},
        })},
    };
    const auto document = jcut::TranscriptDocumentCore::fromJson(root);
    expect(document.has_value(), "gap fixture parses");
    if (!document) {
        return;
    }
    jcut::TranscriptRowBuildOptions options = unpaddedOptions();
    options.insertGaps = true;
    const std::vector<jcut::TranscriptRow> rows = document->rows(options);
    expect(rows.size() == 3, "a two-or-more-frame source gap becomes a row");
    if (rows.size() != 3) {
        return;
    }
    expect(rows[0].sourceStartFrame == 0 && rows[0].sourceEndFrame == 2,
           "first word has inclusive source frames");
    expect(rows[1].gap && rows[1].sourceStartFrame == 3 && rows[1].sourceEndFrame == 5,
           "gap covers the exact missing source frames");
    expect(rows[1].text == "[Gap 100 ms]" && !rows[1].eligibleForFollow(),
           "gap label and follow policy match the Qt table contract");
    expect(rows[0].renderStartFrame == 0 && rows[0].renderEndFrame == 2 &&
               rows[1].renderStartFrame == 3 && rows[1].renderEndFrame == 5 &&
               rows[2].renderStartFrame == 6 && rows[2].renderEndFrame == 8,
           "render frames remain contiguous across words and gaps");
}

void testOutsideActiveCutProjection()
{
    const json originalRoot = {
        {"speaker_profiles", {{"S", {{"name", "Speaker Name"}}}}},
        {"segments", json::array({
            {{"words", json::array({
                {{"word", "a"}, {"start", 0.0}, {"end", 0.1}, {"speaker", "S"}},
                {{"word", "missing"}, {"start", 0.2}, {"end", 0.3}, {"speaker", "S"}},
                {{"word", "c"}, {"start", 0.4}, {"end", 0.5}, {"speaker", "S"}},
            })}},
        })},
    };
    const json activeRoot = {
        {"segments", json::array({
            {{"words", json::array({
                {
                    {"word", "a"}, {"start", 0.0}, {"end", 0.1},
                    {"original_segment_index", 0}, {"original_word_index", 0},
                },
                {
                    {"word", "inserted"}, {"start", 0.15}, {"end", 0.18},
                    {"original_segment_index", -1}, {"original_word_index", -1},
                    {"transcript_edits", json::array({"inserted"})},
                },
                {
                    {"word", "c"}, {"start", 0.4}, {"end", 0.5},
                    {"original_segment_index", 0}, {"original_word_index", 2},
                },
            })}},
        })},
    };

    const auto original = jcut::TranscriptDocumentCore::fromJson(originalRoot);
    const auto active = jcut::TranscriptDocumentCore::fromJson(activeRoot);
    expect(original.has_value() && active.has_value(), "active/original fixtures parse");
    if (!original || !active) {
        return;
    }
    jcut::TranscriptRowBuildOptions options = unpaddedOptions();
    options.includeOutsideActiveCut = true;
    const std::vector<jcut::TranscriptRow> rows = active->rows(options, &*original);
    expect(rows.size() == 4, "only the omitted original word is appended");
    const jcut::TranscriptRow* missing = rowWithText(rows, "missing");
    expect(missing != nullptr, "omitted original word is present");
    if (missing) {
        expect(missing->outsideActiveCut, "omitted original word is marked outside the cut");
        expect(missing->renderStartFrame == -1 && missing->renderEndFrame == -1,
               "outside-cut rows have no render range");
        expect(missing->speakerId == "S" && missing->speakerLabel == "Speaker Name",
               "outside-cut rows use the original document's speaker profiles");
        expect(!missing->eligibleForFollow(), "outside-cut rows cannot follow playback");
    }
    const jcut::TranscriptRow* inserted = rowWithText(rows, "inserted");
    expect(inserted && !inserted->outsideActiveCut,
           "inserted words with no original address stay in the active cut");
}

void testInvalidDocumentsAndTimingFallback()
{
    std::string error;
    expect(!jcut::TranscriptDocumentCore::fromJson(json::array(), &error),
           "non-object root is rejected");
    expect(!error.empty(), "non-object root reports an error");
    expect(!jcut::TranscriptDocumentCore::fromJsonBytes("{broken", &error),
           "malformed bytes are rejected");
    expect(error.find("Invalid transcript JSON") != std::string::npos,
           "malformed bytes report a parse error");

    const auto document = jcut::TranscriptDocumentCore::fromJson({
        {"segments", json::array({
            {{"words", json::array({
                {{"word", "valid"}, {"start", 1.0}, {"end", 1.1}},
                {{"word", "negative"}, {"start", -1.0}, {"end", 0.0}},
                {{"word", "backwards"}, {"start", 3.0}, {"end", 2.0}},
            })}},
        })},
    });
    expect(document.has_value(), "invalid-word fixture still has a valid document root");
    if (!document) {
        return;
    }
    jcut::TranscriptRowBuildOptions options = unpaddedOptions();
    options.timing.framesPerSecond = 0.0;
    const std::vector<jcut::TranscriptRow> rows = document->rows(options);
    expect(rows.size() == 1 && rows.front().sourceStartFrame == 30,
           "invalid words are hidden and invalid FPS falls back to 30");
}

} // namespace

int main()
{
    testParseOrderingLabelsFlagsAndPreservation();
    testTimingOverlapAndFormatting();
    testGapsAndRenderFrames();
    testOutsideActiveCutProjection();
    testInvalidDocumentsAndTimingFallback();

    if (g_failures != 0) {
        std::cerr << g_failures << " transcript document core assertion(s) failed\n";
        return 1;
    }
    std::cout << "transcript document core assertions passed\n";
    return 0;
}
