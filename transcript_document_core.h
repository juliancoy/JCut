#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace jcut {

struct TranscriptTiming {
    double framesPerSecond = 30.0;
    int prependMilliseconds = 150;
    int postpendMilliseconds = 70;
    int offsetMilliseconds = 0;
};

enum TranscriptEditFlag : std::uint32_t {
    TranscriptEditNone = 0,
    TranscriptEditTiming = 1U << 0,
    TranscriptEditText = 1U << 1,
    TranscriptEditSkip = 1U << 2,
    TranscriptEditInserted = 1U << 3,
};

struct TranscriptWordRef {
    int documentWordId = -1;
    int segmentIndex = -1;
    int wordIndex = -1;
    int originalSegmentIndex = -1;
    int originalWordIndex = -1;
    int renderOrder = -1;
};

struct TranscriptRow {
    TranscriptWordRef word;
    std::int64_t sourceStartFrame = 0;
    std::int64_t sourceEndFrame = 0;
    std::int64_t renderStartFrame = -1;
    std::int64_t renderEndFrame = -1;
    double rawStartSeconds = 0.0;
    double rawEndSeconds = 0.0;
    std::string speakerId;
    std::string speakerLabel;
    std::string text;
    std::uint32_t editFlags = TranscriptEditNone;
    bool skipped = false;
    bool gap = false;
    bool outsideActiveCut = false;

    bool eligibleForFollow() const noexcept
    {
        return !skipped && !gap && !outsideActiveCut &&
            sourceEndFrame >= sourceStartFrame;
    }
};

struct TranscriptRowBuildOptions {
    TranscriptTiming timing;
    bool adjustOverlaps = true;
    bool insertGaps = true;
    bool includeOutsideActiveCut = false;
};

// Qt-free, read-only projection of a WhisperX-style transcript document.
// The complete input JSON value is retained structurally unchanged so unknown
// root, segment, and word fields remain available to later adapters and
// mutation services.
class TranscriptDocumentCore {
public:
    static std::optional<TranscriptDocumentCore> fromJson(
        nlohmann::json root,
        std::string* errorOut = nullptr);
    static std::optional<TranscriptDocumentCore> fromJsonBytes(
        std::string_view bytes,
        std::string* errorOut = nullptr);

    const nlohmann::json& root() const noexcept { return m_root; }
    std::size_t wordCount() const noexcept { return m_words.size(); }

    std::vector<TranscriptRow> rows(
        const TranscriptRowBuildOptions& options = {},
        const TranscriptDocumentCore* originalDocument = nullptr) const;

private:
    struct Word {
        TranscriptWordRef reference;
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        std::string speaker;
        std::string segmentSpeaker;
        std::string text;
        std::uint32_t editFlags = TranscriptEditNone;
        bool skipped = false;
        int storedRenderOrder = -1;
        int fallbackOrder = 0;
    };

    explicit TranscriptDocumentCore(nlohmann::json root);

    std::vector<TranscriptRow> projectRows(const TranscriptTiming& timing) const;
    std::string speakerLabel(const std::string& speakerId) const;

    nlohmann::json m_root;
    std::vector<Word> m_words;
    std::vector<std::size_t> m_renderOrder;
};

std::string formatTranscriptTime(double seconds);
bool parseTranscriptTime(std::string_view text, double* secondsOut);

} // namespace jcut
