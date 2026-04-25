#include "editor_shared.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QRegularExpression>
#include <QMutex>
#include <QMutexLocker>
#include <QDateTime>
#include <QElapsedTimer>

#include <algorithm>
#include <atomic>
#include <cmath>

QString transcriptPathForClipFile(const QString& filePath) {
    const QFileInfo info(filePath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".json"));
}

namespace {
QMutex& activeTranscriptPathMutex() {
    static QMutex mutex;
    return mutex;
}

QHash<QString, QString>& activeTranscriptPathByClipFile() {
    static QHash<QString, QString> paths;
    return paths;
}

struct SpeakerTrackingKeyframe {
    int64_t frame = 0;
    qreal x = 0.5;
    qreal y = 0.85;
};

struct SpeakerProfileRuntime {
    bool trackingEnabled = false;
    qreal defaultX = 0.5;
    qreal defaultY = 0.85;
    QVector<SpeakerTrackingKeyframe> keyframes;
};

struct SpeakerProfileCacheEntry {
    qint64 mtimeMs = -1;
    QHash<QString, SpeakerProfileRuntime> profilesBySpeaker;
};

QMutex& speakerProfileCacheMutex() {
    static QMutex mutex;
    return mutex;
}

QHash<QString, SpeakerProfileCacheEntry>& speakerProfileCacheByPath() {
    static QHash<QString, SpeakerProfileCacheEntry> cache;
    return cache;
}

std::atomic<qint64>& speakerTrackingLookupCount() {
    static std::atomic<qint64> value{0};
    return value;
}

std::atomic<qint64>& speakerTrackingResolvedCount() {
    static std::atomic<qint64> value{0};
    return value;
}

std::atomic<qint64>& speakerTrackingCacheHitCount() {
    static std::atomic<qint64> value{0};
    return value;
}

std::atomic<qint64>& speakerTrackingCacheMissCount() {
    static std::atomic<qint64> value{0};
    return value;
}

std::atomic<qint64>& speakerTrackingLookupNsTotal() {
    static std::atomic<qint64> value{0};
    return value;
}

std::atomic<int>& speakerTrackingMaxSpeedPermillePerFrame() {
    static std::atomic<int> value{40};
    return value;
}

std::atomic<int>& speakerTrackingSmoothingPermille() {
    static std::atomic<int> value{800};
    return value;
}

QVector<SpeakerTrackingKeyframe> sanitizeTrackingKeyframes(const QVector<SpeakerTrackingKeyframe>& keyframes) {
    if (keyframes.size() <= 2) {
        return keyframes;
    }
    QVector<SpeakerTrackingKeyframe> sanitized;
    sanitized.reserve(keyframes.size());
    sanitized.push_back(keyframes.constFirst());
    const qreal maxSpeedPerFrame = qMax<qreal>(
        0.001, static_cast<qreal>(speakerTrackingMaxSpeedPermillePerFrame().load()) / 1000.0);
    for (int i = 1; i < keyframes.size() - 1; ++i) {
        const SpeakerTrackingKeyframe& prev = sanitized.constLast();
        const SpeakerTrackingKeyframe& current = keyframes.at(i);
        const int64_t dt = qMax<int64_t>(1, current.frame - prev.frame);
        const qreal dx = current.x - prev.x;
        const qreal dy = current.y - prev.y;
        const qreal dist = std::sqrt((dx * dx) + (dy * dy));
        const qreal speed = dist / static_cast<qreal>(dt);
        if (speed <= maxSpeedPerFrame) {
            sanitized.push_back(current);
        }
    }
    sanitized.push_back(keyframes.constLast());
    return sanitized;
}

QHash<QString, SpeakerProfileRuntime> parseSpeakerProfiles(const QString& transcriptPath) {
    QFile transcriptFile(transcriptPath);
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument transcriptDoc = QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
        return {};
    }

    const QJsonObject profilesObj =
        transcriptDoc.object().value(QStringLiteral("speaker_profiles")).toObject();
    QHash<QString, SpeakerProfileRuntime> parsed;
    for (auto it = profilesObj.constBegin(); it != profilesObj.constEnd(); ++it) {
        const QString speakerId = it.key().trimmed();
        if (speakerId.isEmpty()) {
            continue;
        }
        const QJsonObject profileObj = it.value().toObject();
        SpeakerProfileRuntime runtime;

        const QJsonObject locationObj = profileObj.value(QStringLiteral("location")).toObject();
        runtime.defaultX = qBound<qreal>(0.0, locationObj.value(QStringLiteral("x")).toDouble(0.5), 1.0);
        runtime.defaultY = qBound<qreal>(0.0, locationObj.value(QStringLiteral("y")).toDouble(0.85), 1.0);

        const QJsonObject trackingObj = profileObj.value(QStringLiteral("tracking")).toObject();
        const QString trackingMode =
            trackingObj.value(QStringLiteral("mode")).toString(QStringLiteral("Manual")).trimmed();
        runtime.trackingEnabled =
            !trackingObj.isEmpty() &&
            trackingMode.compare(QStringLiteral("manual"), Qt::CaseInsensitive) != 0;
        const QJsonArray keyframes = trackingObj.value(QStringLiteral("keyframes")).toArray();
        runtime.keyframes.reserve(keyframes.size());
        for (const QJsonValue& keyframeValue : keyframes) {
            const QJsonObject keyframeObj = keyframeValue.toObject();
            if (!keyframeObj.contains(QStringLiteral("frame"))) {
                continue;
            }
            SpeakerTrackingKeyframe keyframe;
            keyframe.frame = keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong();
            keyframe.x = qBound<qreal>(0.0, keyframeObj.value(QStringLiteral("x")).toDouble(runtime.defaultX), 1.0);
            keyframe.y = qBound<qreal>(0.0, keyframeObj.value(QStringLiteral("y")).toDouble(runtime.defaultY), 1.0);
            runtime.keyframes.push_back(keyframe);
        }
        std::sort(runtime.keyframes.begin(),
                  runtime.keyframes.end(),
                  [](const SpeakerTrackingKeyframe& a, const SpeakerTrackingKeyframe& b) {
                      return a.frame < b.frame;
                  });
        runtime.keyframes = sanitizeTrackingKeyframes(runtime.keyframes);

        parsed.insert(speakerId, runtime);
    }
    return parsed;
}

QString activeSpeakerForSourceFrame(const QVector<TranscriptSection>& sections, int64_t sourceFrame) {
    for (const TranscriptSection& section : sections) {
        if (sourceFrame < section.startFrame) {
            return QString();
        }
        if (sourceFrame > section.endFrame) {
            continue;
        }
        int bestIndex = -1;
        for (int i = 0; i < section.words.size(); ++i) {
            const TranscriptWord& word = section.words.at(i);
            if (sourceFrame >= word.startFrame && sourceFrame <= word.endFrame) {
                bestIndex = i;
                break;
            }
            if (sourceFrame > word.endFrame) {
                bestIndex = i;
            }
        }
        if (bestIndex < 0 && !section.words.isEmpty()) {
            bestIndex = 0;
        }
        if (bestIndex >= 0 && bestIndex < section.words.size()) {
            return section.words.at(bestIndex).speaker.trimmed();
        }
        return QString();
    }
    return QString();
}

QPointF evaluateSpeakerLocation(const SpeakerProfileRuntime& runtime, int64_t sourceFrame) {
    if (!runtime.trackingEnabled) {
        return QPointF();
    }
    if (runtime.keyframes.isEmpty()) {
        return QPointF(runtime.defaultX, runtime.defaultY);
    }
    if (runtime.keyframes.size() == 1 || sourceFrame <= runtime.keyframes.constFirst().frame) {
        return QPointF(runtime.keyframes.constFirst().x, runtime.keyframes.constFirst().y);
    }
    if (sourceFrame >= runtime.keyframes.constLast().frame) {
        return QPointF(runtime.keyframes.constLast().x, runtime.keyframes.constLast().y);
    }

    for (int i = 1; i < runtime.keyframes.size(); ++i) {
        const SpeakerTrackingKeyframe& prev = runtime.keyframes.at(i - 1);
        const SpeakerTrackingKeyframe& next = runtime.keyframes.at(i);
        if (sourceFrame > next.frame) {
            continue;
        }
        const int64_t span = qMax<int64_t>(1, next.frame - prev.frame);
        qreal t = qBound<qreal>(
            0.0, static_cast<qreal>(sourceFrame - prev.frame) / static_cast<qreal>(span), 1.0);
        const qreal smoothMix =
            qBound<qreal>(0.0, static_cast<qreal>(speakerTrackingSmoothingPermille().load()) / 1000.0, 1.0);
        const qreal smoothT = t * t * (3.0 - (2.0 * t));
        t = (t * (1.0 - smoothMix)) + (smoothT * smoothMix);
        const qreal x = prev.x + ((next.x - prev.x) * t);
        const qreal y = prev.y + ((next.y - prev.y) * t);
        return QPointF(x, y);
    }
    return QPointF(runtime.defaultX, runtime.defaultY);
}
}

QString transcriptEditablePathForClipFile(const QString& filePath) {
    const QFileInfo info(filePath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral("_editable.json"));
}

QString transcriptWorkingPathForClipFile(const QString& filePath) {
    const QString editablePath = transcriptEditablePathForClipFile(filePath);
    if (QFileInfo::exists(editablePath)) {
        return editablePath;
    }
    return transcriptPathForClipFile(filePath);
}

QStringList transcriptCutPathsForClipFile(const QString& filePath) {
    if (filePath.isEmpty()) {
        return {};
    }

    QStringList paths;
    const QFileInfo clipInfo(filePath);
    const QString originalPath = QFileInfo(transcriptPathForClipFile(filePath)).absoluteFilePath();
    const QString editablePath = QFileInfo(transcriptEditablePathForClipFile(filePath)).absoluteFilePath();
    const QString activePath = QFileInfo(activeTranscriptPathForClipFile(filePath)).absoluteFilePath();

    paths << originalPath << editablePath;
    if (!activePath.isEmpty()) {
        paths << activePath;
    }

    const QDir dir = clipInfo.dir();
    const QString clipBaseName = clipInfo.completeBaseName();
    const QStringList versionFiles = dir.entryList(
        QStringList{clipBaseName + QStringLiteral("_editable_v*.json")},
        QDir::Files,
        QDir::Name);
    for (const QString& versionFile : versionFiles) {
        paths.push_back(QFileInfo(dir.filePath(versionFile)).absoluteFilePath());
    }

    paths.removeDuplicates();
    paths.erase(std::remove_if(paths.begin(),
                               paths.end(),
                               [](const QString& path) { return path.isEmpty(); }),
                paths.end());
    return paths;
}

QString activeTranscriptPathForClipFile(const QString& filePath) {
    {
        QMutexLocker locker(&activeTranscriptPathMutex());
        const auto it = activeTranscriptPathByClipFile().constFind(filePath);
        if (it != activeTranscriptPathByClipFile().cend() &&
            !it.value().isEmpty() &&
            QFileInfo::exists(it.value())) {
            return it.value();
        }
    }
    return transcriptWorkingPathForClipFile(filePath);
}

void setActiveTranscriptPathForClipFile(const QString& filePath, const QString& transcriptPath) {
    if (filePath.isEmpty()) {
        return;
    }
    QMutexLocker locker(&activeTranscriptPathMutex());
    if (transcriptPath.isEmpty()) {
        activeTranscriptPathByClipFile().remove(filePath);
        return;
    }
    activeTranscriptPathByClipFile().insert(filePath, transcriptPath);
}

void clearActiveTranscriptPathForClipFile(const QString& filePath) {
    if (filePath.isEmpty()) {
        return;
    }
    QMutexLocker locker(&activeTranscriptPathMutex());
    activeTranscriptPathByClipFile().remove(filePath);
}

void clearAllActiveTranscriptPaths() {
    QMutexLocker locker(&activeTranscriptPathMutex());
    activeTranscriptPathByClipFile().clear();
}

bool ensureEditableTranscriptForClipFile(const QString& filePath, QString* editablePathOut) {
    const QString editablePath = transcriptEditablePathForClipFile(filePath);
    if (editablePathOut) {
        *editablePathOut = editablePath;
    }
    if (QFileInfo::exists(editablePath)) {
        return true;
    }

    const QString originalPath = transcriptPathForClipFile(filePath);
    if (!QFileInfo::exists(originalPath)) {
        return false;
    }
    QFile::remove(editablePath);
    return QFile::copy(originalPath, editablePath);
}

QVector<TranscriptSection> loadTranscriptSections(const QString& transcriptPath) {
    QFile transcriptFile(transcriptPath);
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument transcriptDoc = QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
        return {};
    }

    QVector<TranscriptWord> words;
    const QJsonArray segments = transcriptDoc.object().value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segmentValue : segments) {
        const QJsonObject segmentObj = segmentValue.toObject();
        const QString segmentSpeaker = segmentObj.value(QStringLiteral("speaker")).toString().trimmed();
        const QJsonArray segmentWords = segmentObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : segmentWords) {
            const QJsonObject wordObj = wordValue.toObject();
            const QString text = wordObj.value(QStringLiteral("word")).toString().trimmed();
            if (text.isEmpty()) {
                continue;
            }
            const bool skipped = wordObj.value(QStringLiteral("skipped")).toBool(false);
            if (skipped) {
                continue;
            }
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
            if (startSeconds < 0.0 || endSeconds < startSeconds) {
                continue;
            }
            const int64_t startFrame =
                qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)));
            const int64_t endFrame =
                qMax<int64_t>(startFrame, static_cast<int64_t>(std::ceil(endSeconds * kTimelineFps)) - 1);
            QString speaker = wordObj.value(QStringLiteral("speaker")).toString().trimmed();
            if (speaker.isEmpty()) {
                speaker = segmentSpeaker;
            }
            words.push_back({startFrame, endFrame, speaker, text, false});
        }
    }
    std::sort(words.begin(), words.end(), [](const TranscriptWord& a, const TranscriptWord& b) {
        if (a.startFrame == b.startFrame) {
            return a.endFrame < b.endFrame;
        }
        return a.startFrame < b.startFrame;
    });

    QVector<TranscriptSection> sections;
    TranscriptSection current;
    const QRegularExpression punctuationPattern(QStringLiteral("[\\.!\\?;:]$"));
    for (const TranscriptWord& word : std::as_const(words)) {
        if (current.text.isEmpty()) {
            current.startFrame = word.startFrame;
            current.endFrame = word.endFrame;
            current.text = word.text;
            current.words.push_back(word);
        } else {
            current.endFrame = word.endFrame;
            current.text += QStringLiteral(" ") + word.text;
            current.words.push_back(word);
        }
        if (punctuationPattern.match(word.text).hasMatch()) {
            sections.push_back(current);
            current = TranscriptSection();
        }
    }

    if (!current.text.isEmpty()) {
        sections.push_back(current);
    }
    return sections;
}

QPointF transcriptSpeakerLocationForSourceFrame(const QString& transcriptPath,
                                                const QVector<TranscriptSection>& sections,
                                                int64_t sourceFrame,
                                                bool* okOut) {
    QElapsedTimer lookupTimer;
    lookupTimer.start();
    speakerTrackingLookupCount().fetch_add(1);
    if (okOut) {
        *okOut = false;
    }
    if (transcriptPath.isEmpty() || sections.isEmpty()) {
        return {};
    }

    const QString speakerId = activeSpeakerForSourceFrame(sections, sourceFrame);
    if (speakerId.isEmpty()) {
        return {};
    }

    const QFileInfo info(transcriptPath);
    const qint64 mtimeMs = info.exists() ? info.lastModified().toMSecsSinceEpoch() : -1;
    SpeakerProfileRuntime runtime;
    bool found = false;
    {
        QMutexLocker locker(&speakerProfileCacheMutex());
        SpeakerProfileCacheEntry& entry = speakerProfileCacheByPath()[transcriptPath];
        if (entry.mtimeMs != mtimeMs) {
            speakerTrackingCacheMissCount().fetch_add(1);
            entry.mtimeMs = mtimeMs;
            entry.profilesBySpeaker = parseSpeakerProfiles(transcriptPath);
        } else {
            speakerTrackingCacheHitCount().fetch_add(1);
        }
        auto profileIt = entry.profilesBySpeaker.constFind(speakerId);
        if (profileIt != entry.profilesBySpeaker.constEnd()) {
            runtime = profileIt.value();
            found = true;
        }
    }

    if (!found) {
        return {};
    }

    if (!runtime.trackingEnabled) {
        speakerTrackingLookupNsTotal().fetch_add(qMax<qint64>(0, lookupTimer.nsecsElapsed()));
        return QPointF();
    }
    const QPointF location = evaluateSpeakerLocation(runtime, sourceFrame);
    speakerTrackingResolvedCount().fetch_add(1);
    if (okOut) {
        *okOut = true;
    }
    speakerTrackingLookupNsTotal().fetch_add(qMax<qint64>(0, lookupTimer.nsecsElapsed()));
    return location;
}

void invalidateTranscriptSpeakerProfileCache(const QString& transcriptPath) {
    QMutexLocker locker(&speakerProfileCacheMutex());
    if (transcriptPath.isEmpty()) {
        speakerProfileCacheByPath().clear();
    } else {
        speakerProfileCacheByPath().remove(transcriptPath);
    }
}

QJsonObject transcriptSpeakerTrackingConfigSnapshot() {
    return QJsonObject{
        {QStringLiteral("max_speed_permille_per_frame"), speakerTrackingMaxSpeedPermillePerFrame().load()},
        {QStringLiteral("smoothing_permille"), speakerTrackingSmoothingPermille().load()}
    };
}

bool applyTranscriptSpeakerTrackingConfigPatch(const QJsonObject& patch, QString* errorOut) {
    auto parseBoundedInt = [&](const QString& key, int minValue, int maxValue, std::atomic<int>* target) -> bool {
        if (!patch.contains(key)) {
            return true;
        }
        bool ok = false;
        const int value = patch.value(key).toVariant().toInt(&ok);
        if (!ok || value < minValue || value > maxValue) {
            if (errorOut) {
                *errorOut = QStringLiteral("%1 must be between %2 and %3").arg(key).arg(minValue).arg(maxValue);
            }
            return false;
        }
        target->store(value);
        return true;
    };
    if (!parseBoundedInt(QStringLiteral("max_speed_permille_per_frame"), 1, 1000,
                         &speakerTrackingMaxSpeedPermillePerFrame()) ||
        !parseBoundedInt(QStringLiteral("smoothing_permille"), 0, 1000,
                         &speakerTrackingSmoothingPermille())) {
        return false;
    }
    return true;
}

QJsonObject transcriptSpeakerTrackingProfilingSnapshot() {
    const qint64 lookups = speakerTrackingLookupCount().load();
    const qint64 resolved = speakerTrackingResolvedCount().load();
    const qint64 hits = speakerTrackingCacheHitCount().load();
    const qint64 misses = speakerTrackingCacheMissCount().load();
    const qint64 totalNs = speakerTrackingLookupNsTotal().load();
    return QJsonObject{
        {QStringLiteral("lookup_count"), lookups},
        {QStringLiteral("resolved_count"), resolved},
        {QStringLiteral("cache_hit_count"), hits},
        {QStringLiteral("cache_miss_count"), misses},
        {QStringLiteral("cache_hit_rate"), (hits + misses) > 0 ? static_cast<double>(hits) / static_cast<double>(hits + misses) : 0.0},
        {QStringLiteral("avg_lookup_us"), lookups > 0 ? static_cast<double>(totalNs) / static_cast<double>(lookups) / 1000.0 : 0.0}
    };
}

void resetTranscriptSpeakerTrackingProfiling() {
    speakerTrackingLookupCount().store(0);
    speakerTrackingResolvedCount().store(0);
    speakerTrackingCacheHitCount().store(0);
    speakerTrackingCacheMissCount().store(0);
    speakerTrackingLookupNsTotal().store(0);
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
    int consumedWords = 0;
    for (const QString& word : words) {
        const QString candidate = currentLine.isEmpty() ? word : currentLine + QStringLiteral(" ") + word;
        if (candidate.size() <= charsPerLine || currentLine.isEmpty()) {
            currentLine = candidate;
            ++consumedWords;
            continue;
        }
        lines.push_back(currentLine);
        if (lines.size() >= linesAllowed) {
            break;
        }
        currentLine = word;
        ++consumedWords;
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
                               .arg(highlightFillColor.name(QColor::HexArgb),
                                    highlightTextColor.name(QColor::HexArgb),
                                    wordHtml);
            }
            htmlWords.push_back(wordHtml);
        }

        QString lineHtml = htmlWords.join(QStringLiteral(" "));
        htmlLines.push_back(lineHtml);
    }

    return QStringLiteral("<div style=\"color:%1;text-align:center;\">%2</div>")
        .arg(textColor.name(QColor::HexArgb), htmlLines.join(QStringLiteral("<br/>")));
}
