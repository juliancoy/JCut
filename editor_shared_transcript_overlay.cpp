#include "editor_shared_transcript.h"

#include <QStringList>

#include <cmath>
#include <limits>
#include <utility>

int64_t transcriptPrependFramesForMs(int prependMs) {
    return qMax<int64_t>(
        0,
        static_cast<int64_t>(std::floor((qMax(0, prependMs) / 1000.0) * kTimelineFps)));
}

int64_t transcriptPostpendFramesForMs(int postpendMs) {
    return qMax<int64_t>(
        0,
        static_cast<int64_t>(std::ceil((qMax(0, postpendMs) / 1000.0) * kTimelineFps)));
}

int64_t transcriptOffsetFramesForMs(int offsetMs) {
    return static_cast<int64_t>(
        std::llround((static_cast<double>(offsetMs) / 1000.0) * kTimelineFps));
}

ExportRangeSegment transcriptPaddedWordRange(const TranscriptWord& word,
                                             int prependMs,
                                             int postpendMs,
                                             int offsetMs) {
    const int64_t prependFrames = transcriptPrependFramesForMs(prependMs);
    const int64_t postpendFrames = transcriptPostpendFramesForMs(postpendMs);
    const int64_t offsetFrames = transcriptOffsetFramesForMs(offsetMs);
    const int64_t startFrame =
        qMax<int64_t>(0, word.startFrame + offsetFrames - prependFrames);
    const int64_t endFrame =
        qMax<int64_t>(startFrame, word.endFrame + offsetFrames + postpendFrames);
    return ExportRangeSegment{startFrame, endFrame};
}

QVector<ExportRangeSegment> transcriptPaddedWordRanges(const QVector<TranscriptSection>& sections,
                                                       int prependMs,
                                                       int postpendMs,
                                                       int offsetMs) {
    QVector<ExportRangeSegment> ranges;
    for (const TranscriptSection& section : sections) {
        ranges.reserve(ranges.size() + section.words.size());
        for (const TranscriptWord& word : section.words) {
            if (word.skipped || word.text.trimmed().isEmpty()) {
                continue;
            }
            ranges.push_back(transcriptPaddedWordRange(word, prependMs, postpendMs, offsetMs));
        }
    }
    return ranges;
}

TranscriptSection transcriptSectionWithWordTimingPadding(const TranscriptSection& section,
                                                         int prependMs,
                                                         int postpendMs) {
    TranscriptSection padded = section;
    if (padded.words.isEmpty()) {
        return padded;
    }
    int64_t sectionStart = std::numeric_limits<int64_t>::max();
    int64_t sectionEnd = 0;
    for (TranscriptWord& word : padded.words) {
        const ExportRangeSegment range = transcriptPaddedWordRange(word, prependMs, postpendMs);
        word.startFrame = range.startFrame;
        word.endFrame = range.endFrame;
        sectionStart = qMin(sectionStart, range.startFrame);
        sectionEnd = qMax(sectionEnd, range.endFrame);
    }
    padded.startFrame = sectionStart == std::numeric_limits<int64_t>::max() ? section.startFrame : sectionStart;
    padded.endFrame = qMax(padded.startFrame, sectionEnd);
    return padded;
}

namespace {

ExportRangeSegment transcriptPaddedWordRangeForFrames(const TranscriptWord& word,
                                                      int64_t prependFrames,
                                                      int64_t postpendFrames,
                                                      int64_t offsetFrames)
{
    const int64_t startFrame = qMax<int64_t>(0, word.startFrame + offsetFrames - prependFrames);
    const int64_t endFrame = qMax<int64_t>(startFrame, word.endFrame + offsetFrames + postpendFrames);
    return ExportRangeSegment{startFrame, endFrame};
}

struct ResolvedTranscriptOverlaySection {
    TranscriptSection section;
    int activeWordIndex = -1;
    bool valid = false;
};

constexpr int64_t kTranscriptOverlayEdgeGuardFrames = 1;

int transcriptOverlayActiveWordIndexForSection(const TranscriptSection& section,
                                               int64_t sourceFrame)
{
    int activeWordIndex = -1;
    for (int i = 0; i < section.words.size(); ++i) {
        const TranscriptWord& word = section.words.at(i);
        if (sourceFrame >= word.startFrame && sourceFrame <= word.endFrame) {
            activeWordIndex = i;
            break;
        }
        if (sourceFrame > word.endFrame) {
            activeWordIndex = i;
        } else if (activeWordIndex < 0 && sourceFrame < word.startFrame) {
            activeWordIndex = i;
            break;
        }
    }
    return activeWordIndex;
}

int transcriptOverlayActiveWordIndexForPaddedWords(const TranscriptSection& section,
                                                   int64_t sourceFrame,
                                                   int64_t prependFrames,
                                                   int64_t postpendFrames,
                                                   int64_t offsetFrames)
{
    int activeWordIndex = -1;
    int64_t bestDistance = std::numeric_limits<int64_t>::max();
    for (int i = 0; i < section.words.size(); ++i) {
        const TranscriptWord& word = section.words.at(i);
        const ExportRangeSegment range =
            transcriptPaddedWordRangeForFrames(word, prependFrames, postpendFrames, offsetFrames);
        const int64_t guardedStart =
            qMax<int64_t>(0, range.startFrame - kTranscriptOverlayEdgeGuardFrames);
        const int64_t guardedEnd = range.endFrame + kTranscriptOverlayEdgeGuardFrames;
        if (sourceFrame < guardedStart || sourceFrame > guardedEnd) {
            continue;
        }

        const int64_t distance =
            sourceFrame < word.startFrame + offsetFrames
                ? (word.startFrame + offsetFrames) - sourceFrame
                : (sourceFrame > word.endFrame + offsetFrames ? sourceFrame - (word.endFrame + offsetFrames) : 0);
        if (distance < bestDistance ||
            (distance == bestDistance && activeWordIndex >= 0 &&
             sourceFrame >= word.startFrame + offsetFrames)) {
            activeWordIndex = i;
            bestDistance = distance;
        }
    }
    return activeWordIndex;
}

ResolvedTranscriptOverlaySection resolveTranscriptOverlaySectionAtSourceFrame(
    const QVector<TranscriptSection>& sections,
    int64_t sourceFrame,
    const TranscriptOverlayTiming& timing)
{
    const int64_t prependFrames = transcriptPrependFramesForMs(timing.prependMs);
    const int64_t postpendFrames = transcriptPostpendFramesForMs(timing.postpendMs);
    const int64_t offsetFrames = transcriptOffsetFramesForMs(timing.offsetMs);
    const auto firstPossibleSection = std::lower_bound(
        sections.constBegin(),
        sections.constEnd(),
        sourceFrame,
        [offsetFrames, postpendFrames](const TranscriptSection& section, int64_t frame) {
            const int64_t paddedEndFrame =
                qMax<int64_t>(0, section.endFrame + offsetFrames + postpendFrames + kTranscriptOverlayEdgeGuardFrames);
            return paddedEndFrame < frame;
        });
    for (auto it = firstPossibleSection; it != sections.constEnd(); ++it) {
        const TranscriptSection& section = *it;
        if (section.words.isEmpty()) {
            continue;
        }
        const int64_t paddedStartFrame =
            qMax<int64_t>(0,
                          section.words.constFirst().startFrame +
                              offsetFrames -
                              prependFrames -
                              kTranscriptOverlayEdgeGuardFrames);
        const int64_t paddedEndFrame =
            qMax<int64_t>(paddedStartFrame,
                          section.words.constLast().endFrame +
                              offsetFrames +
                              postpendFrames +
                              kTranscriptOverlayEdgeGuardFrames);
        if (sourceFrame < paddedStartFrame) {
            return {};
        }
        if (sourceFrame > paddedEndFrame) {
            continue;
        }
        const int activeWordIndex =
            transcriptOverlayActiveWordIndexForPaddedWords(section,
                                                           sourceFrame,
                                                           prependFrames,
                                                           postpendFrames,
                                                           offsetFrames);
        if (activeWordIndex < 0) {
            continue;
        }

        ResolvedTranscriptOverlaySection resolved;
        TranscriptSection paddedSection = section;
        paddedSection.startFrame = paddedStartFrame;
        paddedSection.endFrame = paddedEndFrame;
        for (TranscriptWord& word : paddedSection.words) {
            const ExportRangeSegment range =
                transcriptPaddedWordRangeForFrames(word, prependFrames, postpendFrames, offsetFrames);
            word.startFrame = range.startFrame;
            word.endFrame = range.endFrame;
        }
        resolved.activeWordIndex = activeWordIndex;
        resolved.valid = resolved.activeWordIndex >= 0 && resolved.activeWordIndex < paddedSection.words.size();
        resolved.section = std::move(paddedSection);
        return resolved;
    }
    return {};
}

QPointF transcriptOverlayManualTranslationPixels(const TimelineClip& clip,
                                                 qreal outputWidth,
                                                 qreal outputHeight)
{
    const qreal normalizedX =
        qBound<qreal>(-1.0, clip.transcriptOverlay.translationX, 1.0);
    const qreal normalizedY =
        qBound<qreal>(-1.0, clip.transcriptOverlay.translationY, 1.0);
    return QPointF(normalizedX * (outputWidth * 0.5),
                   normalizedY * (outputHeight * 0.5));
}

} // namespace

QPointF transcriptOverlayTranslationForOutput(const TimelineClip& clip,
                                              const QSize& outputSize,
                                              const QString& transcriptPath,
                                              const QVector<TranscriptSection>& sections,
                                              int64_t sourceFrame) {
    const QSize safeOutputSize = outputSize.isValid() ? outputSize : QSize(1080, 1920);
    const qreal outputWidth = qMax<qreal>(1.0, static_cast<qreal>(safeOutputSize.width()));
    const qreal outputHeight = qMax<qreal>(1.0, static_cast<qreal>(safeOutputSize.height()));
    const QPointF manualTranslation =
        transcriptOverlayManualTranslationPixels(clip, outputWidth, outputHeight);

    if (clip.transcriptOverlay.useManualPlacement) {
        return manualTranslation;
    }
    if (transcriptPath.isEmpty() || sections.isEmpty()) {
        return manualTranslation;
    }
    bool speakerLocationResolved = false;
    const QPointF speakerLocation = transcriptSpeakerLocationForSourceFrame(
        transcriptPath, sections, sourceFrame, &speakerLocationResolved);
    if (!speakerLocationResolved) {
        return manualTranslation;
    }
    const qreal centerX = speakerLocation.x() * outputWidth;
    const qreal centerY = speakerLocation.y() * outputHeight;
    return QPointF(centerX - (outputWidth / 2.0), centerY - (outputHeight / 2.0));
}

QRectF transcriptOverlayRectInOutputSpace(const TimelineClip& clip,
                                          const QSize& outputSize,
                                          const QString& transcriptPath,
                                          const QVector<TranscriptSection>& sections,
                                          int64_t sourceFrame) {
    const QSize safeOutputSize = outputSize.isValid() ? outputSize : QSize(1080, 1920);
    const qreal outputWidth = qMax<qreal>(1.0, static_cast<qreal>(safeOutputSize.width()));
    const qreal outputHeight = qMax<qreal>(1.0, static_cast<qreal>(safeOutputSize.height()));
    const QPointF translation = transcriptOverlayTranslationForOutput(
        clip, safeOutputSize, transcriptPath, sections, sourceFrame);
    return QRectF((outputWidth / 2.0) + translation.x() - (clip.transcriptOverlay.boxWidth / 2.0),
                  (outputHeight / 2.0) + translation.y() - (clip.transcriptOverlay.boxHeight / 2.0),
                  clip.transcriptOverlay.boxWidth,
                  clip.transcriptOverlay.boxHeight);
}

int transcriptOverlayEffectiveLinesForBox(const TimelineClip& clip) {
    const qreal estimatedLineHeight = qMax<qreal>(12.0, clip.transcriptOverlay.fontPointSize * 1.55);
    const qreal shadowReserve = clip.transcriptOverlay.showShadow
        ? qMax<qreal>(0.0, clip.transcriptOverlay.fontPointSize * 0.16)
        : 0.0;
    const qreal highlightReserve = qMax<qreal>(0.0, clip.transcriptOverlay.fontPointSize * 0.08);
    const qreal reservedTitleHeight = clip.transcriptOverlay.showSpeakerTitle
        ? qMax<qreal>(0.0,
                      (clip.transcriptOverlay.fontPointSize * 0.62 * 1.55) +
                      (clip.transcriptOverlay.fontPointSize * 0.30) +
                      shadowReserve)
        : 0.0;
    const qreal usableHeight =
        qMax<qreal>(estimatedLineHeight,
                    clip.transcriptOverlay.boxHeight -
                        28.0 -
                        reservedTitleHeight -
                        shadowReserve -
                        highlightReserve);
    const int fittedLines =
        qMax(1, static_cast<int>(std::floor(usableHeight / estimatedLineHeight)));
    return qMax(1, qMin(clip.transcriptOverlay.maxLines, fittedLines));
}

int transcriptOverlayEffectiveCharsForBox(const TimelineClip& clip) {
    const qreal estimatedCharWidth = qMax<qreal>(6.0, clip.transcriptOverlay.fontPointSize * 0.62);
    const qreal usableWidth =
        qMax<qreal>(estimatedCharWidth, clip.transcriptOverlay.boxWidth - 36.0);
    const int fittedChars =
        qMax(1, static_cast<int>(std::floor(usableWidth / estimatedCharWidth)));
    return qMax(1, qMin(clip.transcriptOverlay.maxCharsPerLine, fittedChars));
}

TranscriptOverlayLayout transcriptOverlayLayoutAtSourceFrame(
    const TimelineClip& clip,
    const QVector<TranscriptSection>& sections,
    int64_t sourceFrame,
    const TranscriptOverlayTiming& timing) {
    if (sections.isEmpty()) {
        return {};
    }
    const ResolvedTranscriptOverlaySection resolved =
        resolveTranscriptOverlaySectionAtSourceFrame(sections, sourceFrame, timing);
    if (!resolved.valid) {
        return {};
    }
    return layoutTranscriptSection(resolved.section,
                                   sourceFrame,
                                   transcriptOverlayEffectiveCharsForBox(clip),
                                   transcriptOverlayEffectiveLinesForBox(clip),
                                   clip.transcriptOverlay.autoScroll);
}

QString transcriptOverlaySpeakerAtSourceFrame(const QVector<TranscriptSection>& sections,
                                              int64_t sourceFrame,
                                              ExportRangeSegment* activeRangeOut,
                                              const TranscriptOverlayTiming& timing)
{
    if (activeRangeOut) {
        *activeRangeOut = ExportRangeSegment{-1, -1};
    }
    if (sections.isEmpty()) {
        return QString();
    }
    const ResolvedTranscriptOverlaySection resolved =
        resolveTranscriptOverlaySectionAtSourceFrame(sections, sourceFrame, timing);
    if (!resolved.valid) {
        return QString();
    }
    const TranscriptWord& activeWord = resolved.section.words.at(resolved.activeWordIndex);
    if (activeRangeOut) {
        *activeRangeOut = ExportRangeSegment{
            qMax<int64_t>(0, activeWord.startFrame - kTranscriptOverlayEdgeGuardFrames),
            activeWord.endFrame + kTranscriptOverlayEdgeGuardFrames};
    }
    return activeWord.speaker.trimmed();
}

QString wrappedTranscriptSectionText(const QString& text, int maxCharsPerLine, int maxLines) {
    const int charsPerLine = qMax(1, maxCharsPerLine);
    const int linesAllowed = qMax(1, maxLines);
    const QStringList words = text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty()) {
        return QString();
    }

    QStringList lines;
    QString currentLine;
    for (const QString& word : words) {
        const QString candidate = currentLine.isEmpty() ? word : currentLine + QStringLiteral(" ") + word;
        if (candidate.size() <= charsPerLine || currentLine.isEmpty()) {
            currentLine = candidate;
            continue;
        }
        lines.push_back(currentLine);
        if (lines.size() >= linesAllowed) {
            break;
        }
        currentLine = word;
    }
    if (lines.size() < linesAllowed && !currentLine.isEmpty()) {
        lines.push_back(currentLine);
    }
    if (lines.isEmpty()) {
        return QString();
    }
    return lines.join(QLatin1Char('\n'));
}

TranscriptOverlayLayout layoutTranscriptSection(const TranscriptSection& section,
                                                int64_t sourceFrame,
                                                int maxCharsPerLine,
                                                int maxLines,
                                                bool autoScroll) {
    TranscriptOverlayLayout layout;
    if (section.words.isEmpty()) {
        return layout;
    }

    const int charsPerLine = qMax(1, maxCharsPerLine);
    const int linesAllowed = qMax(1, maxLines);
    const int activeWordIndex = transcriptOverlayActiveWordIndexForSection(section, sourceFrame);

    QVector<TranscriptOverlayLine> allLines;
    TranscriptOverlayLine currentLine;
    int currentLength = 0;
    for (int i = 0; i < section.words.size(); ++i) {
        const QString wordText = section.words.at(i).text.simplified();
        if (wordText.isEmpty()) {
            continue;
        }

        const int candidateLength = currentLine.words.isEmpty()
                                        ? wordText.size()
                                        : currentLength + 1 + wordText.size();
        if (!currentLine.words.isEmpty() && candidateLength > charsPerLine) {
            allLines.push_back(currentLine);
            currentLine = TranscriptOverlayLine();
            currentLength = 0;
        }

        currentLine.words.push_back(wordText);
        if (i == activeWordIndex) {
            currentLine.activeWord = currentLine.words.size() - 1;
        }
        currentLength = currentLine.words.join(QStringLiteral(" ")).size();
    }
    if (!currentLine.words.isEmpty()) {
        allLines.push_back(currentLine);
    }
    if (allLines.isEmpty()) {
        return layout;
    }

    int activeLineIndex = -1;
    for (int i = 0; i < allLines.size(); ++i) {
        if (allLines.at(i).activeWord >= 0) {
            activeLineIndex = i;
            break;
        }
    }

    int startLine = 0;
    if (activeLineIndex >= 0 && allLines.size() > linesAllowed) {
        if (autoScroll) {
            startLine = qBound(0, activeLineIndex - (linesAllowed - 1), allLines.size() - linesAllowed);
        } else {
            startLine = qBound(0,
                               (activeLineIndex / linesAllowed) * linesAllowed,
                               allLines.size() - linesAllowed);
        }
    }

    const int endLine = qMin(allLines.size(), startLine + linesAllowed);
    for (int i = startLine; i < endLine; ++i) {
        layout.lines.push_back(allLines.at(i));
    }
    layout.truncatedTop = startLine > 0;
    layout.truncatedBottom = endLine < allLines.size();
    return layout;
}

QString transcriptOverlayHtml(const TranscriptOverlayLayout& layout,
                              const QColor& textColor,
                              const QColor& highlightTextColor,
                              const QColor& highlightFillColor) {
    if (layout.lines.isEmpty()) {
        return QString();
    }

    QStringList htmlLines;
    htmlLines.reserve(layout.lines.size());
    for (int lineIndex = 0; lineIndex < layout.lines.size(); ++lineIndex) {
        const TranscriptOverlayLine& line = layout.lines.at(lineIndex);
        QStringList htmlWords;
        htmlWords.reserve(line.words.size());
        for (int wordIndex = 0; wordIndex < line.words.size(); ++wordIndex) {
            QString wordHtml = line.words.at(wordIndex).toHtmlEscaped();
            if (wordIndex == line.activeWord) {
                wordHtml = QStringLiteral(
                               "<span style=\"background:%1;color:%2;border-radius:0.28em;padding:0.02em 0.18em;\">%3</span>")
                               .arg(highlightFillColor.name(QColor::HexRgb),
                                    highlightTextColor.name(QColor::HexRgb),
                                    wordHtml);
            }
            htmlWords.push_back(wordHtml);
        }

        htmlLines.push_back(htmlWords.join(QStringLiteral(" ")));
    }

    return QStringLiteral("<div style=\"color:%1;text-align:center;\">%2</div>")
        .arg(textColor.name(QColor::HexRgb), htmlLines.join(QStringLiteral("<br/>")));
}
