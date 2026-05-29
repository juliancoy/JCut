#include "editor_shared_transcript.h"

#include <QStringList>

#include <atomic>
#include <cmath>
#include <limits>

namespace {
std::atomic<int>& transcriptOverlayPrependMs() {
    static std::atomic<int> value{150};
    return value;
}

std::atomic<int>& transcriptOverlayPostpendMs() {
    static std::atomic<int> value{70};
    return value;
}
} // namespace

void setTranscriptOverlayTimingPaddingMs(int prependMs, int postpendMs) {
    transcriptOverlayPrependMs().store(qMax(0, prependMs));
    transcriptOverlayPostpendMs().store(qMax(0, postpendMs));
}

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

ExportRangeSegment transcriptPaddedWordRange(const TranscriptWord& word,
                                             int prependMs,
                                             int postpendMs) {
    const int64_t startFrame =
        qMax<int64_t>(0, word.startFrame - transcriptPrependFramesForMs(prependMs));
    const int64_t endFrame =
        qMax<int64_t>(startFrame, word.endFrame + transcriptPostpendFramesForMs(postpendMs));
    return ExportRangeSegment{startFrame, endFrame};
}

QVector<ExportRangeSegment> transcriptPaddedWordRanges(const QVector<TranscriptSection>& sections,
                                                       int prependMs,
                                                       int postpendMs) {
    QVector<ExportRangeSegment> ranges;
    for (const TranscriptSection& section : sections) {
        ranges.reserve(ranges.size() + section.words.size());
        for (const TranscriptWord& word : section.words) {
            if (word.skipped || word.text.trimmed().isEmpty()) {
                continue;
            }
            ranges.push_back(transcriptPaddedWordRange(word, prependMs, postpendMs));
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

QPointF transcriptOverlayTranslationForOutput(const TimelineClip& clip,
                                              const QSize& outputSize,
                                              const QString& transcriptPath,
                                              const QVector<TranscriptSection>& sections,
                                              int64_t sourceFrame) {
    qreal translationX = clip.transcriptOverlay.translationX;
    qreal translationY = clip.transcriptOverlay.translationY;
    const QSize safeOutputSize = outputSize.isValid() ? outputSize : QSize(1080, 1920);
    const qreal outputWidth = qMax<qreal>(1.0, static_cast<qreal>(safeOutputSize.width()));
    const qreal outputHeight = qMax<qreal>(1.0, static_cast<qreal>(safeOutputSize.height()));

    auto translationPixelsFromStored = [outputWidth, outputHeight](qreal storedX, qreal storedY) {
        const bool looksAbsolutePixels = std::abs(storedX) > 1.0 || std::abs(storedY) > 1.0;
        if (looksAbsolutePixels) {
            return QPointF(storedX, storedY);
        }
        return QPointF(storedX * (outputWidth * 0.5), storedY * (outputHeight * 0.5));
    };

    if (clip.transcriptOverlay.useManualPlacement) {
        return translationPixelsFromStored(translationX, translationY);
    }
    if (transcriptPath.isEmpty() || sections.isEmpty()) {
        return translationPixelsFromStored(translationX, translationY);
    }
    bool speakerLocationResolved = false;
    const QPointF speakerLocation = transcriptSpeakerLocationForSourceFrame(
        transcriptPath, sections, sourceFrame, &speakerLocationResolved);
    if (!speakerLocationResolved) {
        return translationPixelsFromStored(translationX, translationY);
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
    const qreal estimatedLineHeight = qMax<qreal>(12.0, clip.transcriptOverlay.fontPointSize * 1.35);
    const qreal usableHeight =
        qMax<qreal>(estimatedLineHeight, clip.transcriptOverlay.boxHeight - 28.0);
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
    int64_t sourceFrame) {
    if (sections.isEmpty()) {
        return {};
    }
    const int prependMs = transcriptOverlayPrependMs().load();
    const int postpendMs = transcriptOverlayPostpendMs().load();
    for (const TranscriptSection& section : sections) {
        const TranscriptSection paddedSection =
            transcriptSectionWithWordTimingPadding(section, prependMs, postpendMs);
        if (sourceFrame < paddedSection.startFrame) {
            return {};
        }
        if (sourceFrame <= paddedSection.endFrame) {
            return layoutTranscriptSection(paddedSection,
                                           sourceFrame,
                                           transcriptOverlayEffectiveCharsForBox(clip),
                                           transcriptOverlayEffectiveLinesForBox(clip),
                                           clip.transcriptOverlay.autoScroll);
        }
    }
    return {};
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
