#include "transcript_document_core.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace jcut {
namespace {

using json = nlohmann::json;

std::string trimAscii(std::string value)
{
    const auto isSpace = [](unsigned char character) {
        return character == ' ' || character == '\t' || character == '\n' ||
            character == '\r' || character == '\f' || character == '\v';
    };
    const auto first = std::find_if_not(value.begin(), value.end(), isSpace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string stringValue(const json& object,
                        const char* key,
                        std::string fallback = {})
{
    if (!object.is_object()) {
        return fallback;
    }
    const auto value = object.find(key);
    return value != object.end() && value->is_string()
        ? value->get<std::string>()
        : fallback;
}

double doubleValue(const json& object, const char* key, double fallback)
{
    if (!object.is_object()) {
        return fallback;
    }
    const auto value = object.find(key);
    if (value == object.end() || !value->is_number()) {
        return fallback;
    }
    try {
        const double result = value->get<double>();
        return std::isfinite(result) ? result : fallback;
    } catch (const json::exception&) {
        return fallback;
    }
}

int intValue(const json& object, const char* key, int fallback)
{
    if (!object.is_object()) {
        return fallback;
    }
    const auto value = object.find(key);
    if (value == object.end() ||
        (!value->is_number_integer() && !value->is_number_unsigned())) {
        return fallback;
    }
    try {
        const std::int64_t result = value->get<std::int64_t>();
        if (result < std::numeric_limits<int>::min() ||
            result > std::numeric_limits<int>::max()) {
            return fallback;
        }
        return static_cast<int>(result);
    } catch (const json::exception&) {
        return fallback;
    }
}

bool boolValue(const json& object, const char* key, bool fallback)
{
    if (!object.is_object()) {
        return fallback;
    }
    const auto value = object.find(key);
    return value != object.end() && value->is_boolean()
        ? value->get<bool>()
        : fallback;
}

bool fuzzyEqual(double lhs, double rhs)
{
    const double shiftedLhs = lhs + 1.0;
    const double shiftedRhs = rhs + 1.0;
    return std::abs(shiftedLhs - shiftedRhs) * 1.0e12 <=
        std::min(std::abs(shiftedLhs), std::abs(shiftedRhs));
}

double normalizedFramesPerSecond(const TranscriptTiming& timing)
{
    return std::isfinite(timing.framesPerSecond) && timing.framesPerSecond > 0.0
        ? timing.framesPerSecond
        : 30.0;
}

void adjustOverlappingRows(std::vector<TranscriptRow>* rows)
{
    if (!rows) {
        return;
    }
    for (std::size_t index = 1; index < rows->size(); ++index) {
        TranscriptRow& previous = (*rows)[index - 1];
        TranscriptRow& current = (*rows)[index];
        if (current.sourceStartFrame > previous.sourceEndFrame) {
            continue;
        }
        const std::int64_t overlap =
            previous.sourceEndFrame - current.sourceStartFrame + 1;
        const std::int64_t trimPrevious = overlap / 2;
        const std::int64_t trimCurrent = overlap - trimPrevious;
        previous.sourceEndFrame = std::max(
            previous.sourceStartFrame,
            previous.sourceEndFrame - trimPrevious);
        current.sourceStartFrame = std::min(
            current.sourceEndFrame,
            current.sourceStartFrame + trimCurrent);
        if (current.sourceStartFrame <= previous.sourceEndFrame) {
            current.sourceStartFrame = std::min(
                current.sourceEndFrame,
                previous.sourceEndFrame + 1);
        }
    }
}

void insertGapRows(std::vector<TranscriptRow>* rows, double framesPerSecond)
{
    if (!rows || rows->empty()) {
        return;
    }
    std::vector<TranscriptRow> expanded;
    expanded.reserve(rows->size() * 2);
    expanded.push_back(rows->front());
    for (std::size_t index = 1; index < rows->size(); ++index) {
        const TranscriptRow& previous = expanded.back();
        const TranscriptRow& current = (*rows)[index];
        if (!previous.outsideActiveCut && !current.outsideActiveCut &&
            current.sourceStartFrame > previous.sourceEndFrame + 1) {
            const std::int64_t gapStart = previous.sourceEndFrame + 1;
            const std::int64_t gapEnd = current.sourceStartFrame - 1;
            const std::int64_t gapFrames = std::max<std::int64_t>(
                0, gapEnd - gapStart + 1);
            if (gapFrames >= 2) {
                TranscriptRow gap;
                gap.sourceStartFrame = gapStart;
                gap.sourceEndFrame = gapEnd;
                gap.rawStartSeconds =
                    static_cast<double>(gapStart) / framesPerSecond;
                gap.rawEndSeconds =
                    static_cast<double>(gapEnd + 1) / framesPerSecond;
                gap.speakerId = "\xE2\x80\x94";
                gap.speakerLabel = gap.speakerId;
                gap.text = "[Gap " + std::to_string(static_cast<long long>(
                    std::llround((static_cast<double>(gapFrames) /
                                  framesPerSecond) * 1000.0))) + " ms]";
                gap.gap = true;
                expanded.push_back(std::move(gap));
            }
        }
        expanded.push_back(current);
    }
    *rows = std::move(expanded);
}

void computeRenderFrames(std::vector<TranscriptRow>* rows)
{
    if (!rows) {
        return;
    }
    std::int64_t cursor = 0;
    for (TranscriptRow& row : *rows) {
        if (row.outsideActiveCut) {
            row.renderStartFrame = -1;
            row.renderEndFrame = -1;
            continue;
        }
        const std::int64_t duration = std::max<std::int64_t>(
            1, row.sourceEndFrame - row.sourceStartFrame + 1);
        row.renderStartFrame = cursor;
        row.renderEndFrame = cursor + duration - 1;
        cursor = row.renderEndFrame + 1;
    }
}

bool parseFiniteNumber(std::string_view text, double* valueOut)
{
    if (!valueOut) {
        return false;
    }
    const std::string valueText = trimAscii(std::string(text));
    if (valueText.empty()) {
        return false;
    }
    try {
        std::size_t consumed = 0;
        const double value = std::stod(valueText, &consumed);
        if (consumed != valueText.size() || !std::isfinite(value)) {
            return false;
        }
        *valueOut = value;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace

TranscriptDocumentCore::TranscriptDocumentCore(json root)
    : m_root(std::move(root))
{
    const json emptyArray = json::array();
    const json& segments = [&]() -> const json& {
        const auto value = m_root.find("segments");
        return value != m_root.end() && value->is_array() ? *value : emptyArray;
    }();

    int nextWordId = 1;
    int fallbackOrder = 0;
    for (std::size_t segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
        const json emptyObject = json::object();
        const json& segment = segments[segmentIndex].is_object()
            ? segments[segmentIndex]
            : emptyObject;
        const std::string segmentSpeaker = trimAscii(stringValue(segment, "speaker"));
        const auto wordsValue = segment.find("words");
        if (wordsValue == segment.end() || !wordsValue->is_array()) {
            continue;
        }
        const json& words = *wordsValue;
        for (std::size_t wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
            const json& wordObject = words[wordIndex].is_object()
                ? words[wordIndex]
                : emptyObject;
            Word word;
            word.reference.documentWordId = nextWordId++;
            word.reference.segmentIndex = static_cast<int>(segmentIndex);
            word.reference.wordIndex = static_cast<int>(wordIndex);
            word.reference.originalSegmentIndex = intValue(
                wordObject, "original_segment_index", static_cast<int>(segmentIndex));
            word.reference.originalWordIndex = intValue(
                wordObject, "original_word_index", static_cast<int>(wordIndex));
            word.startSeconds = doubleValue(wordObject, "start", 0.0);
            word.endSeconds = doubleValue(wordObject, "end", word.startSeconds);
            word.speaker = trimAscii(stringValue(wordObject, "speaker"));
            word.segmentSpeaker = segmentSpeaker;
            word.text = stringValue(wordObject, "word");
            word.skipped = boolValue(wordObject, "skipped", false);
            word.storedRenderOrder = intValue(wordObject, "render_order", -1);
            word.fallbackOrder = fallbackOrder++;

            const auto edits = wordObject.find("transcript_edits");
            if (edits != wordObject.end() && edits->is_array()) {
                for (const json& editValue : *edits) {
                    if (!editValue.is_string()) {
                        continue;
                    }
                    const std::string edit = trimAscii(editValue.get<std::string>());
                    if (edit == "timing") {
                        word.editFlags |= TranscriptEditTiming;
                    } else if (edit == "text") {
                        word.editFlags |= TranscriptEditText;
                    } else if (edit == "skip") {
                        word.editFlags |= TranscriptEditSkip;
                    } else if (edit == "inserted") {
                        word.editFlags |= TranscriptEditInserted;
                    }
                }
            }
            m_words.push_back(std::move(word));
        }
    }

    m_renderOrder.resize(m_words.size());
    for (std::size_t index = 0; index < m_words.size(); ++index) {
        m_renderOrder[index] = index;
    }
    std::sort(m_renderOrder.begin(), m_renderOrder.end(), [this](std::size_t lhsIndex,
                                                                 std::size_t rhsIndex) {
        const Word& lhs = m_words[lhsIndex];
        const Word& rhs = m_words[rhsIndex];
        const bool lhsHasOrder = lhs.storedRenderOrder >= 0;
        const bool rhsHasOrder = rhs.storedRenderOrder >= 0;
        if (lhsHasOrder != rhsHasOrder) {
            return lhsHasOrder;
        }
        if (lhsHasOrder && lhs.storedRenderOrder != rhs.storedRenderOrder) {
            return lhs.storedRenderOrder < rhs.storedRenderOrder;
        }
        if (!fuzzyEqual(lhs.startSeconds, rhs.startSeconds)) {
            return lhs.startSeconds < rhs.startSeconds;
        }
        return lhs.fallbackOrder < rhs.fallbackOrder;
    });
    for (std::size_t renderOrder = 0; renderOrder < m_renderOrder.size(); ++renderOrder) {
        m_words[m_renderOrder[renderOrder]].reference.renderOrder =
            static_cast<int>(renderOrder);
    }
}

std::optional<TranscriptDocumentCore> TranscriptDocumentCore::fromJson(
    json root,
    std::string* errorOut)
{
    if (errorOut) {
        errorOut->clear();
    }
    if (!root.is_object()) {
        if (errorOut) {
            *errorOut = "Transcript JSON root must be an object.";
        }
        return std::nullopt;
    }
    return TranscriptDocumentCore(std::move(root));
}

std::optional<TranscriptDocumentCore> TranscriptDocumentCore::fromJsonBytes(
    std::string_view bytes,
    std::string* errorOut)
{
    if (errorOut) {
        errorOut->clear();
    }
    try {
        return fromJson(json::parse(bytes.begin(), bytes.end()), errorOut);
    } catch (const json::exception& error) {
        if (errorOut) {
            *errorOut = std::string("Invalid transcript JSON: ") + error.what();
        }
        return std::nullopt;
    }
}

std::string TranscriptDocumentCore::speakerLabel(const std::string& speakerId) const
{
    const std::string trimmedSpeaker = trimAscii(speakerId);
    if (trimmedSpeaker.empty()) {
        return trimmedSpeaker;
    }
    const auto profiles = m_root.find("speaker_profiles");
    if (profiles == m_root.end() || !profiles->is_object()) {
        return trimmedSpeaker;
    }
    const auto profile = profiles->find(trimmedSpeaker);
    if (profile == profiles->end() || !profile->is_object()) {
        return trimmedSpeaker;
    }
    const std::string name = trimAscii(stringValue(*profile, "name"));
    return name.empty() ? trimmedSpeaker : name;
}

std::vector<TranscriptRow> TranscriptDocumentCore::projectRows(
    const TranscriptTiming& timing) const
{
    const double framesPerSecond = normalizedFramesPerSecond(timing);
    const double prependSeconds =
        static_cast<double>(std::max(0, timing.prependMilliseconds)) / 1000.0;
    const double postpendSeconds =
        static_cast<double>(std::max(0, timing.postpendMilliseconds)) / 1000.0;
    const double offsetSeconds =
        static_cast<double>(timing.offsetMilliseconds) / 1000.0;

    std::vector<TranscriptRow> result;
    result.reserve(m_renderOrder.size());
    for (std::size_t wordIndex : m_renderOrder) {
        const Word& word = m_words[wordIndex];
        if (trimAscii(word.text).empty() || word.startSeconds < 0.0 ||
            word.endSeconds < word.startSeconds ||
            !std::isfinite(word.startSeconds) || !std::isfinite(word.endSeconds)) {
            continue;
        }
        const double adjustedStart = std::max(
            0.0, word.startSeconds + offsetSeconds - prependSeconds);
        const double adjustedEnd = std::max(
            adjustedStart, word.endSeconds + offsetSeconds + postpendSeconds);

        TranscriptRow row;
        row.word = word.reference;
        row.sourceStartFrame = std::max<std::int64_t>(
            0, static_cast<std::int64_t>(std::floor(adjustedStart * framesPerSecond)));
        row.sourceEndFrame = std::max<std::int64_t>(
            row.sourceStartFrame,
            static_cast<std::int64_t>(std::ceil(adjustedEnd * framesPerSecond)) - 1);
        row.rawStartSeconds = word.startSeconds;
        row.rawEndSeconds = word.endSeconds;
        row.speakerId = !word.speaker.empty()
            ? word.speaker
            : (!word.segmentSpeaker.empty() ? word.segmentSpeaker : "Unknown");
        row.speakerLabel = speakerLabel(row.speakerId);
        row.text = word.text;
        row.editFlags = word.editFlags;
        row.skipped = word.skipped;
        result.push_back(std::move(row));
    }
    return result;
}

std::vector<TranscriptRow> TranscriptDocumentCore::rows(
    const TranscriptRowBuildOptions& options,
    const TranscriptDocumentCore* originalDocument) const
{
    std::vector<TranscriptRow> result = projectRows(options.timing);
    if (options.adjustOverlaps) {
        adjustOverlappingRows(&result);
    }
    const double framesPerSecond = normalizedFramesPerSecond(options.timing);
    if (options.insertGaps) {
        insertGapRows(&result, framesPerSecond);
    }
    computeRenderFrames(&result);

    if (!options.includeOutsideActiveCut || !originalDocument) {
        return result;
    }

    std::set<std::pair<int, int>> activeOriginalWords;
    for (const TranscriptRow& row : result) {
        if (!row.gap) {
            activeOriginalWords.emplace(
                row.word.originalSegmentIndex,
                row.word.originalWordIndex);
        }
    }
    std::vector<TranscriptRow> originalRows =
        originalDocument->projectRows(options.timing);
    for (TranscriptRow& row : originalRows) {
        const auto originalKey = std::make_pair(
            row.word.originalSegmentIndex,
            row.word.originalWordIndex);
        if (activeOriginalWords.contains(originalKey)) {
            continue;
        }
        row.outsideActiveCut = true;
        row.renderStartFrame = -1;
        row.renderEndFrame = -1;
        result.push_back(std::move(row));
    }
    return result;
}

std::string formatTranscriptTime(double seconds)
{
    const double safeSeconds = std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
    const std::int64_t milliseconds = static_cast<std::int64_t>(
        std::llround(safeSeconds * 1000.0));
    const std::int64_t totalSeconds = milliseconds / 1000;
    const std::int64_t minutes = totalSeconds / 60;
    const std::int64_t remainingSeconds = totalSeconds % 60;
    const std::int64_t remainingMilliseconds = milliseconds % 1000;

    std::ostringstream stream;
    stream << std::setfill('0') << std::setw(2) << minutes << ':'
           << std::setw(2) << remainingSeconds << '.'
           << std::setw(3) << remainingMilliseconds;
    return stream.str();
}

bool parseTranscriptTime(std::string_view text, double* secondsOut)
{
    if (!secondsOut) {
        return false;
    }
    const std::string trimmed = trimAscii(std::string(text));
    if (trimmed.empty()) {
        return false;
    }

    const std::size_t separator = trimmed.find(':');
    if (separator == std::string::npos) {
        if (!parseFiniteNumber(trimmed, secondsOut)) {
            return false;
        }
        *secondsOut = std::max(0.0, *secondsOut);
        return true;
    }
    if (separator != trimmed.rfind(':')) {
        return false;
    }

    double minutes = 0.0;
    double seconds = 0.0;
    if (!parseFiniteNumber(
            std::string_view(trimmed).substr(0, separator), &minutes) ||
        !parseFiniteNumber(
            std::string_view(trimmed).substr(separator + 1), &seconds)) {
        return false;
    }
    *secondsOut = std::max(0.0, minutes * 60.0 + seconds);
    return true;
}

} // namespace jcut
