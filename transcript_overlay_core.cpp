#include "transcript_overlay_core.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jcut {
namespace {

std::string trimAscii(std::string value)
{
    const auto isSpace = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    const auto first = std::find_if_not(value.begin(), value.end(), isSpace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
    return first < last ? std::string(first, last) : std::string{};
}

int utf8Length(const std::string& value)
{
    int result = 0;
    for (unsigned char c : value) {
        if ((c & 0xc0) != 0x80) ++result;
    }
    return result;
}

std::int64_t framesForMilliseconds(int milliseconds, double framesPerSecond)
{
    const double fps = std::isfinite(framesPerSecond) && framesPerSecond > 0.0
        ? framesPerSecond : 30.0;
    return static_cast<std::int64_t>(std::llround(milliseconds * fps / 1000.0));
}

} // namespace

TranscriptOverlayLayoutCore transcriptOverlayLayoutForRows(
    const std::vector<TranscriptRow>& rows,
    std::int64_t sourceFrame,
    const TranscriptOverlayLayoutOptions& options)
{
    TranscriptOverlayLayoutCore layout;
    const std::int64_t prepend = std::max<std::int64_t>(
        0, framesForMilliseconds(options.timing.prependMilliseconds,
                                 options.timing.framesPerSecond));
    const std::int64_t postpend = std::max<std::int64_t>(
        0, framesForMilliseconds(options.timing.postpendMilliseconds,
                                 options.timing.framesPerSecond));
    const std::int64_t offset = framesForMilliseconds(
        options.timing.offsetMilliseconds, options.timing.framesPerSecond);

    std::size_t sectionStart = 0;
    while (sectionStart < rows.size()) {
        while (sectionStart < rows.size() &&
               (!rows[sectionStart].eligibleForFollow() ||
                rows[sectionStart].word.segmentIndex < 0)) {
            ++sectionStart;
        }
        if (sectionStart >= rows.size()) break;
        const int segment = rows[sectionStart].word.segmentIndex;
        std::size_t sectionEnd = sectionStart + 1;
        while (sectionEnd < rows.size() &&
               rows[sectionEnd].word.segmentIndex == segment) {
            ++sectionEnd;
        }
        std::vector<const TranscriptRow*> words;
        for (std::size_t index = sectionStart; index < sectionEnd; ++index) {
            if (rows[index].eligibleForFollow() && !trimAscii(rows[index].text).empty()) {
                words.push_back(&rows[index]);
            }
        }
        sectionStart = sectionEnd;
        if (words.empty()) continue;
        const std::int64_t paddedStart = std::max<std::int64_t>(
            0, words.front()->sourceStartFrame + offset - prepend - 1);
        const std::int64_t paddedEnd = std::max(
            paddedStart, words.back()->sourceEndFrame + offset + postpend + 1);
        if (sourceFrame < paddedStart) break;
        if (sourceFrame > paddedEnd) continue;

        int activeIndex = -1;
        std::int64_t bestDistance = std::numeric_limits<std::int64_t>::max();
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::int64_t guardedStart = std::max<std::int64_t>(
                0, words[index]->sourceStartFrame + offset - prepend - 1);
            const std::int64_t guardedEnd =
                words[index]->sourceEndFrame + offset + postpend + 1;
            if (sourceFrame < guardedStart || sourceFrame > guardedEnd) continue;
            const std::int64_t wordStart = words[index]->sourceStartFrame + offset;
            const std::int64_t wordEnd = words[index]->sourceEndFrame + offset;
            const std::int64_t distance = sourceFrame < wordStart
                ? wordStart - sourceFrame
                : (sourceFrame > wordEnd ? sourceFrame - wordEnd : 0);
            if (distance < bestDistance ||
                (distance == bestDistance && activeIndex >= 0 && sourceFrame >= wordStart)) {
                activeIndex = static_cast<int>(index);
                bestDistance = distance;
            }
        }
        if (activeIndex < 0) continue;

        layout.speakerId = words[static_cast<std::size_t>(activeIndex)]->speakerId;
        layout.speakerLabel = words[static_cast<std::size_t>(activeIndex)]->speakerLabel;
        layout.speakerTitle = words[static_cast<std::size_t>(activeIndex)]->speakerTitle;
        const int charsPerLine = std::max(1, options.maxCharsPerLine);
        std::vector<TranscriptOverlayLineCore> allLines;
        TranscriptOverlayLineCore current;
        int currentLength = 0;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::string text = trimAscii(words[index]->text);
            const int textLength = utf8Length(text);
            const int candidateLength = current.words.empty()
                ? textLength : currentLength + 1 + textLength;
            if (!current.words.empty() && candidateLength > charsPerLine) {
                allLines.push_back(std::move(current));
                current = {};
                currentLength = 0;
            }
            current.words.push_back(text);
            if (static_cast<int>(index) == activeIndex) {
                current.activeWord = static_cast<int>(current.words.size()) - 1;
            }
            currentLength += (current.words.size() > 1 ? 1 : 0) + textLength;
        }
        if (!current.words.empty()) allLines.push_back(std::move(current));
        int activeLine = -1;
        for (std::size_t index = 0; index < allLines.size(); ++index) {
            if (allLines[index].activeWord >= 0) {
                activeLine = static_cast<int>(index);
                break;
            }
        }
        const int linesAllowed = std::max(1, options.maxLines);
        int startLine = 0;
        if (activeLine >= 0 && static_cast<int>(allLines.size()) > linesAllowed) {
            startLine = options.autoScroll
                ? std::clamp(activeLine - (linesAllowed - 1), 0,
                             static_cast<int>(allLines.size()) - linesAllowed)
                : std::clamp((activeLine / linesAllowed) * linesAllowed, 0,
                             static_cast<int>(allLines.size()) - linesAllowed);
        }
        const int endLine = std::min(
            static_cast<int>(allLines.size()), startLine + linesAllowed);
        layout.lines.assign(
            allLines.begin() + startLine, allLines.begin() + endLine);
        layout.truncatedTop = startLine > 0;
        layout.truncatedBottom = endLine < static_cast<int>(allLines.size());
        return layout;
    }
    return layout;
}

} // namespace jcut
