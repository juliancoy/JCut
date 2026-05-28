#include "speakers_tab.h"
#include "speakers_tab_internal.h"
#include "timeline_fps.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>

namespace {
QVector<int64_t> speakerSectionSourceFrames(const QJsonObject& transcriptRoot, const QString& speakerId)
{
    QVector<int64_t> frames;
    const QString targetSpeaker = speakerId.trimmed();
    if (targetSpeaker.isEmpty()) {
        return frames;
    }

    QString currentSpeaker;
    bool hasCurrentSection = false;
    auto endSection = [&]() {
        currentSpeaker.clear();
        hasCurrentSection = false;
    };
    auto beginSection = [&](const QString& nextSpeaker, double startSeconds) {
        currentSpeaker = nextSpeaker;
        hasCurrentSection = true;
        if (nextSpeaker == targetSpeaker && startSeconds >= 0.0) {
            frames.push_back(qMax<int64_t>(
                0,
                static_cast<int64_t>(std::floor(startSeconds * kTimelineFps))));
        }
    };

    const QJsonArray segments = transcriptRoot.value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        if (!words.isEmpty()) {
            for (const QJsonValue& wordValue : words) {
                const QJsonObject wordObj = wordValue.toObject();
                if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                    endSection();
                    continue;
                }
                QString wordSpeaker =
                    wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
                if (wordSpeaker.isEmpty()) {
                    wordSpeaker = segmentSpeaker;
                }
                if (wordSpeaker.isEmpty()) {
                    endSection();
                    continue;
                }
                const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
                if (!hasCurrentSection || currentSpeaker != wordSpeaker) {
                    beginSection(wordSpeaker, startSeconds);
                }
            }
            continue;
        }

        if (segmentSpeaker.isEmpty()) {
            endSection();
            continue;
        }
        const double startSeconds = segmentObj.value(QStringLiteral("start")).toDouble(-1.0);
        if (!hasCurrentSection || currentSpeaker != segmentSpeaker) {
            beginSection(segmentSpeaker, startSeconds);
        }
    }

    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}
}

bool SpeakersTab::seekToSpeakerSegmentRelative(const QString& speakerId, int direction)
{
    if (speakerId.isEmpty() || direction == 0 || !m_transcriptSession.hasObjectDocument() || !m_deps.seekToTimelineFrame) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }

    const int64_t currentTimeline = m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : clip->startFrame;
    const int64_t currentSourceFrame = qMax<int64_t>(0, clip->sourceInFrame + (currentTimeline - clip->startFrame));
    QVector<int64_t> speakerFrames = speakerSourceFrames(m_transcriptSession.rootObject(), speakerId);
    if (speakerFrames.isEmpty()) {
        return false;
    }

    int64_t chosenSource = -1;
    int chosenIndex = -1;
    if (direction > 0) {
        for (int i = 0; i < speakerFrames.size(); ++i) {
            if (speakerFrames.at(i) > currentSourceFrame) {
                chosenIndex = i;
                break;
            }
        }
        if (chosenIndex < 0) {
            chosenIndex = 0;
        }
    } else {
        for (int i = speakerFrames.size() - 1; i >= 0; --i) {
            if (speakerFrames.at(i) < currentSourceFrame) {
                chosenIndex = i;
                break;
            }
        }
        if (chosenIndex < 0) {
            chosenIndex = speakerFrames.size() - 1;
        }
    }

    chosenSource = speakerFrames.at(chosenIndex);
    int64_t timelineFrame = clip->startFrame + (chosenSource - clip->sourceInFrame);
    const int64_t clipEndFrame = clip->startFrame + qMax<int64_t>(0, clip->durationFrames - 1);
    timelineFrame = qBound<int64_t>(clip->startFrame, timelineFrame, clipEndFrame);

    // Ensure button presses visibly move when multiple segments exist.
    if (timelineFrame == currentTimeline && speakerFrames.size() > 1) {
        if (direction > 0) {
            chosenIndex = (chosenIndex + 1) % speakerFrames.size();
        } else {
            chosenIndex = (chosenIndex - 1 + speakerFrames.size()) % speakerFrames.size();
        }
        chosenSource = speakerFrames.at(chosenIndex);
        timelineFrame = clip->startFrame + (chosenSource - clip->sourceInFrame);
        timelineFrame = qBound<int64_t>(clip->startFrame, timelineFrame, clipEndFrame);
    }

    m_deps.seekToTimelineFrame(timelineFrame);
    return true;
}

bool SpeakersTab::seekToSpeakerNextSection(const QString& speakerId)
{
    if (speakerId.isEmpty() || !m_transcriptSession.hasObjectDocument() || !m_deps.seekToTimelineFrame) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }

    const int64_t currentTimeline =
        m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : clip->startFrame;
    const int64_t currentSourceFrame = qMax<int64_t>(0, clip->sourceInFrame + (currentTimeline - clip->startFrame));
    const QVector<int64_t> sectionFrames =
        speakerSectionSourceFrames(m_transcriptSession.rootObject(), speakerId);
    if (sectionFrames.isEmpty()) {
        return false;
    }

    int chosenIndex = -1;
    for (int i = 0; i < sectionFrames.size(); ++i) {
        if (sectionFrames.at(i) > currentSourceFrame) {
            chosenIndex = i;
            break;
        }
    }
    if (chosenIndex < 0) {
        chosenIndex = 0;
    }

    int64_t timelineFrame = clip->startFrame + (sectionFrames.at(chosenIndex) - clip->sourceInFrame);
    const int64_t clipEndFrame = clip->startFrame + qMax<int64_t>(0, clip->durationFrames - 1);
    timelineFrame = qBound<int64_t>(clip->startFrame, timelineFrame, clipEndFrame);
    m_deps.seekToTimelineFrame(timelineFrame);
    return true;
}

bool SpeakersTab::seekToSpeakerRandomSentence(const QString& speakerId)
{
    if (speakerId.isEmpty() || !m_transcriptSession.hasObjectDocument() || !m_deps.seekToTimelineFrame) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }
    const QVector<int64_t> speakerFrames = speakerSourceFrames(m_transcriptSession.rootObject(), speakerId);
    if (speakerFrames.isEmpty()) {
        return false;
    }
    const int idx = QRandomGenerator::global()->bounded(speakerFrames.size());
    const int64_t chosenSource = speakerFrames.at(idx);
    int64_t timelineFrame = clip->startFrame + (chosenSource - clip->sourceInFrame);
    const int64_t clipEndFrame = clip->startFrame + qMax<int64_t>(0, clip->durationFrames - 1);
    timelineFrame = qBound<int64_t>(clip->startFrame, timelineFrame, clipEndFrame);
    m_deps.seekToTimelineFrame(timelineFrame);
    return true;
}

bool SpeakersTab::navigateSpeakerSentence(const QString& speakerId, SentenceNavAction action)
{
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return false;
    }
    m_lastSelectedSpeakerIdHint = speakerId;
    switch (action) {
    case SentenceNavAction::Previous:
        return seekToSpeakerSegmentRelative(speakerId, -1);
    case SentenceNavAction::Next:
        return seekToSpeakerSegmentRelative(speakerId, +1);
    case SentenceNavAction::NextSection:
        return seekToSpeakerNextSection(speakerId);
    case SentenceNavAction::Random:
        return seekToSpeakerRandomSentence(speakerId);
    }
    return false;
}

void SpeakersTab::onSpeakerPreviousSentenceClicked()
{
    const QString speakerId = selectedSpeakerId();
    navigateSpeakerSentence(speakerId, SentenceNavAction::Previous);
}

void SpeakersTab::onSpeakerNextSentenceClicked()
{
    const QString speakerId = selectedSpeakerId();
    navigateSpeakerSentence(speakerId, SentenceNavAction::Next);
}

void SpeakersTab::onSpeakerNextSectionClicked()
{
    const QString speakerId = selectedSpeakerId();
    navigateSpeakerSentence(speakerId, SentenceNavAction::NextSection);
}

void SpeakersTab::onSpeakerRandomSentenceClicked()
{
    const QString speakerId = selectedSpeakerId();
    navigateSpeakerSentence(speakerId, SentenceNavAction::Random);
}
