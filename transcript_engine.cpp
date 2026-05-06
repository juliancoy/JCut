#include "transcript_engine.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStringList>
#include <QDataStream>
#include <QMutex>
#include <QMutexLocker>
#include <QtEndian>

#include <algorithm>
#include <cmath>

namespace editor
{
namespace {
struct BoxstreamDocCacheEntry {
    qint64 modifiedMs = 0;
    qint64 fileSize = 0;
    QJsonDocument doc;
};

QMutex g_boxstreamDocCacheMutex;
QHash<QString, BoxstreamDocCacheEntry> g_boxstreamDocCache;

QString boxstreamPathForTranscriptPath(const QString& transcriptPath)
{
    const QFileInfo info(transcriptPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral("_boxstream.bin"));
}

QString legacyJsonBoxstreamPathForTranscriptPath(const QString& transcriptPath)
{
    const QFileInfo info(transcriptPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral("_boxstream.json"));
}

QString transcriptTextCompanionPath(const QString& transcriptPath)
{
    const QFileInfo info(transcriptPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".txt"));
}

bool tokenNeedsLeadingSpace(const QString& token)
{
    if (token.isEmpty()) {
        return false;
    }
    const QChar first = token.front();
    if (first.isSpace()) {
        return false;
    }
    if (first == QLatin1Char('.') || first == QLatin1Char(',') || first == QLatin1Char('!') ||
        first == QLatin1Char('?') || first == QLatin1Char(':') || first == QLatin1Char(';') ||
        first == QLatin1Char(')') || first == QLatin1Char(']') || first == QLatin1Char('}') ||
        first == QLatin1Char('%') || first == QLatin1Char('\'')) {
        return false;
    }
    return true;
}

QString transcriptTokenText(const QJsonObject& wordObj)
{
    const QString word = wordObj.value(QStringLiteral("word")).toString();
    if (!word.trimmed().isEmpty()) {
        return word;
    }
    return wordObj.value(QStringLiteral("text")).toString();
}

QString buildCompactTranscriptText(const QJsonObject& root)
{
    const QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    QString currentSpeaker;
    QString currentText;
    QStringList lines;
    QHash<QString, int> speakerIndexByToken;
    int nextSpeakerIndex = 1;
    const QString unknownSpeakerToken = QStringLiteral("__UNKNOWN_SPEAKER__");

    auto speakerLabelForToken = [&](const QString& rawToken, int fallbackUnknownIndex) {
        const QString token = rawToken.trimmed();
        if (token.isEmpty()) {
            return QStringLiteral("SPEAKER_%1").arg(fallbackUnknownIndex);
        }
        if (!speakerIndexByToken.contains(token)) {
            speakerIndexByToken.insert(token, nextSpeakerIndex++);
        }
        return QStringLiteral("SPEAKER_%1").arg(speakerIndexByToken.value(token));
    };

    auto flushLine = [&]() {
        const QString text = currentText.simplified();
        if (!currentSpeaker.isEmpty() && !text.isEmpty()) {
            lines.push_back(QStringLiteral("%1: %2").arg(currentSpeaker, text));
        }
        currentSpeaker.clear();
        currentText.clear();
    };

    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const QString segmentSpeakerToken = segmentObj.value(QStringLiteral("speaker")).toString().trimmed();
        const int unknownSpeakerIndexForSegment = 1;
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            QString wordSpeakerToken = wordObj.value(QStringLiteral("speaker"))
                                           .toString(segmentSpeakerToken)
                                           .trimmed();
            if (wordSpeakerToken.isEmpty()) {
                wordSpeakerToken = unknownSpeakerToken;
            }
            const QString speaker =
                speakerLabelForToken(wordSpeakerToken, unknownSpeakerIndexForSegment);
            const QString token = transcriptTokenText(wordObj);
            if (token.trimmed().isEmpty()) {
                continue;
            }
            if (!currentSpeaker.isEmpty() && currentSpeaker != speaker) {
                flushLine();
            }
            if (currentSpeaker.isEmpty()) {
                currentSpeaker = speaker;
            }
            if (!currentText.isEmpty() && tokenNeedsLeadingSpace(token)) {
                currentText += QLatin1Char(' ');
            }
            currentText += token;
        }

        if (words.isEmpty()) {
            QString fallbackSpeakerToken = segmentSpeakerToken;
            if (fallbackSpeakerToken.isEmpty()) {
                fallbackSpeakerToken = unknownSpeakerToken;
            }
            const QString speaker =
                speakerLabelForToken(fallbackSpeakerToken, unknownSpeakerIndexForSegment);
            const QString text = segmentObj.value(QStringLiteral("text")).toString().trimmed();
            if (!text.isEmpty()) {
                if (!currentSpeaker.isEmpty() && currentSpeaker != speaker) {
                    flushLine();
                }
                if (currentSpeaker.isEmpty()) {
                    currentSpeaker = speaker;
                }
                if (!currentText.isEmpty()) {
                    currentText += QLatin1Char(' ');
                }
                currentText += text;
            }
        }
    }

    flushLine();
    return lines.join(QLatin1Char('\n')) + (lines.isEmpty() ? QString() : QStringLiteral("\n"));
}

bool saveTranscriptTextCompanion(const QString& transcriptPath, const QJsonObject& root)
{
    QSaveFile file(transcriptTextCompanionPath(transcriptPath));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray payload = buildCompactTranscriptText(root).toUtf8();
    if (file.write(payload) != payload.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

QByteArray boxstreamMagic()
{
    return QByteArrayLiteral("JCUTBOX1");
}

bool loadBoxstreamDocFromFile(const QString& path, QJsonDocument* outDoc)
{
    if (!outDoc) {
        return false;
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return false;
    }
    const qint64 modifiedMs = info.lastModified().toMSecsSinceEpoch();
    const qint64 fileSize = info.size();
    {
        QMutexLocker locker(&g_boxstreamDocCacheMutex);
        const auto cached = g_boxstreamDocCache.constFind(path);
        if (cached != g_boxstreamDocCache.constEnd() &&
            cached->modifiedMs == modifiedMs &&
            cached->fileSize == fileSize &&
            cached->doc.isObject()) {
            *outDoc = cached->doc;
            return true;
        }
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray payload = file.readAll();
    if (payload.size() < boxstreamMagic().size() + static_cast<int>(sizeof(quint32) * 2)) {
        return false;
    }
    if (!payload.startsWith(boxstreamMagic())) {
        return false;
    }
    const char* raw = payload.constData() + boxstreamMagic().size();
    const quint32 version = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(raw));
    raw += sizeof(quint32);
    const quint32 jsonSize = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(raw));
    raw += sizeof(quint32);
    if (version != 1) {
        return false;
    }
    const int headerSize = boxstreamMagic().size() + static_cast<int>(sizeof(quint32) * 2);
    if (jsonSize > static_cast<quint32>(payload.size() - headerSize)) {
        return false;
    }
    const QByteArray jsonBytes(raw, static_cast<int>(jsonSize));
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    {
        QMutexLocker locker(&g_boxstreamDocCacheMutex);
        g_boxstreamDocCache.insert(path, BoxstreamDocCacheEntry{modifiedMs, fileSize, doc});
    }
    *outDoc = doc;
    return true;
}

bool saveBoxstreamDocToFile(const QString& path, const QJsonDocument& doc)
{
    if (!doc.isObject()) {
        return false;
    }
    const QByteArray jsonBytes = doc.toJson(QJsonDocument::Compact);
    if (jsonBytes.size() > std::numeric_limits<quint32>::max()) {
        return false;
    }
    QByteArray payload;
    payload.reserve(boxstreamMagic().size() + static_cast<int>(sizeof(quint32) * 2) + jsonBytes.size());
    payload.append(boxstreamMagic());
    quint32 version = qToLittleEndian<quint32>(1);
    quint32 jsonSize = qToLittleEndian<quint32>(static_cast<quint32>(jsonBytes.size()));
    payload.append(reinterpret_cast<const char*>(&version), sizeof(quint32));
    payload.append(reinterpret_cast<const char*>(&jsonSize), sizeof(quint32));
    payload.append(jsonBytes);

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    if (out.write(payload) != payload.size()) {
        out.cancelWriting();
        return false;
    }
    if (!out.commit()) {
        return false;
    }
    const QFileInfo info(path);
    QMutexLocker locker(&g_boxstreamDocCacheMutex);
    g_boxstreamDocCache.insert(path, BoxstreamDocCacheEntry{
        info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0,
        info.exists() ? info.size() : 0,
        doc});
    return true;
}

bool mergeBoxstreamSpeakerProfiles(const QString& transcriptPath, QJsonObject* root)
{
    if (!root) {
        return false;
    }
    QJsonDocument boxstreamDoc;
    if (!loadBoxstreamDocFromFile(boxstreamPathForTranscriptPath(transcriptPath), &boxstreamDoc)) {
        QFile legacyFile(legacyJsonBoxstreamPathForTranscriptPath(transcriptPath));
        if (!legacyFile.exists() || !legacyFile.open(QIODevice::ReadOnly)) {
            return false;
        }
        QJsonParseError parseError;
        boxstreamDoc = QJsonDocument::fromJson(legacyFile.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !boxstreamDoc.isObject()) {
            return false;
        }
    }
    if (!boxstreamDoc.isObject()) {
        return false;
    }
    const QJsonObject boxstreamRoot = boxstreamDoc.object();
    const QJsonObject boxstreamProfiles = boxstreamRoot.value(QStringLiteral("speaker_profiles")).toObject();
    if (boxstreamProfiles.isEmpty()) {
        return false;
    }
    QJsonObject transcriptProfiles = root->value(QStringLiteral("speaker_profiles")).toObject();
    for (auto it = boxstreamProfiles.constBegin(); it != boxstreamProfiles.constEnd(); ++it) {
        const QString speakerId = it.key().trimmed();
        if (speakerId.isEmpty()) {
            continue;
        }
        const QJsonObject boxProfile = it.value().toObject();
        if (boxProfile.isEmpty()) {
            continue;
        }
        QJsonObject transcriptProfile = transcriptProfiles.value(speakerId).toObject();
        const QJsonObject framing = boxProfile.value(QStringLiteral("framing")).toObject();
        if (!framing.isEmpty()) {
            transcriptProfile[QStringLiteral("framing")] = framing;
        }
        transcriptProfiles[ speakerId ] = transcriptProfile;
    }
    (*root)[QStringLiteral("speaker_profiles")] = transcriptProfiles;
    return true;
}

QVector<ExportRangeSegment> mergeRanges(const QVector<ExportRangeSegment>& ranges)
{
    if (ranges.isEmpty()) {
        return {};
    }

    QVector<ExportRangeSegment> sorted = ranges;
    std::sort(sorted.begin(), sorted.end(),
              [](const ExportRangeSegment& a, const ExportRangeSegment& b) {
                  if (a.startFrame == b.startFrame) {
                      return a.endFrame < b.endFrame;
                  }
                  return a.startFrame < b.startFrame;
              });

    QVector<ExportRangeSegment> merged;
    for (const ExportRangeSegment& range : sorted) {
        if (merged.isEmpty() || range.startFrame > merged.constLast().endFrame + 1) {
            merged.push_back(range);
        } else {
            merged.last().endFrame = qMax(merged.last().endFrame, range.endFrame);
        }
    }
    return merged;
}

QVector<ExportRangeSegment> subtractRanges(const QVector<ExportRangeSegment>& baseRanges,
                                          const QVector<ExportRangeSegment>& removedRanges)
{
    if (baseRanges.isEmpty()) {
        return {};
    }
    if (removedRanges.isEmpty()) {
        return baseRanges;
    }

    const QVector<ExportRangeSegment> mergedBase = mergeRanges(baseRanges);
    const QVector<ExportRangeSegment> mergedRemoved = mergeRanges(removedRanges);
    QVector<ExportRangeSegment> result;

    int removedIndex = 0;
    for (const ExportRangeSegment& base : mergedBase) {
        int64_t cursor = base.startFrame;
        while (removedIndex < mergedRemoved.size() &&
               mergedRemoved.at(removedIndex).endFrame < base.startFrame) {
            ++removedIndex;
        }

        int currentRemoved = removedIndex;
        while (currentRemoved < mergedRemoved.size()) {
            const ExportRangeSegment& removed = mergedRemoved.at(currentRemoved);
            if (removed.startFrame > base.endFrame) {
                break;
            }
            if (removed.startFrame > cursor) {
                result.push_back(ExportRangeSegment{cursor, removed.startFrame - 1});
            }
            cursor = qMax<int64_t>(cursor, removed.endFrame + 1);
            if (cursor > base.endFrame) {
                break;
            }
            ++currentRemoved;
        }

        if (cursor <= base.endFrame) {
            result.push_back(ExportRangeSegment{cursor, base.endFrame});
        }
    }

    return result;
}

}

QString TranscriptEngine::transcriptPathForClip(const TimelineClip &clip) const
    {
        return activeTranscriptPathForClipFile(clip.filePath);
    }

QString TranscriptEngine::secondsToTranscriptTime(double seconds) const
    {
        const qint64 millis = qMax<qint64>(0, qRound64(seconds * 1000.0));
        const qint64 totalSeconds = millis / 1000;
        const qint64 minutes = totalSeconds / 60;
        const qint64 secs = totalSeconds % 60;
        const qint64 ms = millis % 1000;
        return QStringLiteral("%1:%2.%3")
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'))
            .arg(ms, 3, 10, QLatin1Char('0'));
    }

bool TranscriptEngine::parseTranscriptTime(const QString &text, double *secondsOut) const
    {
        bool ok = false;
        const double numericValue = text.toDouble(&ok);
        if (ok)
        {
            *secondsOut = qMax(0.0, numericValue);
            return true;
        }

        const QString trimmed = text.trimmed();
        const QStringList minuteParts = trimmed.split(QLatin1Char(':'));
        if (minuteParts.size() != 2)
        {
            return false;
        }
        const int minutes = minuteParts[0].toInt(&ok);
        if (!ok)
        {
            return false;
        }
        const double secValue = minuteParts[1].toDouble(&ok);
        if (!ok)
        {
            return false;
        }
        *secondsOut = qMax(0.0, minutes * 60.0 + secValue);
        return true;
    }

bool TranscriptEngine::loadTranscriptJson(const QString &path,
                                          QJsonDocument *docOut,
                                          QString *errorOut) const
    {
        if (!docOut) {
            if (errorOut) {
                *errorOut = QStringLiteral("Null output document pointer.");
            }
            return false;
        }
        QFile transcriptFile(path);
        if (!transcriptFile.open(QIODevice::ReadOnly)) {
            if (errorOut) {
                *errorOut = QStringLiteral("Unable to open transcript file.");
            }
            return false;
        }
        QJsonParseError parseError;
        QJsonDocument transcriptDoc = QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
            if (errorOut) {
                *errorOut = QStringLiteral("Invalid transcript JSON.");
            }
            return false;
        }
        QJsonObject root = transcriptDoc.object();
        mergeBoxstreamSpeakerProfiles(path, &root);
        *docOut = QJsonDocument(root);
        return true;
    }

bool TranscriptEngine::loadBoxstreamArtifact(const QString &transcriptPath, QJsonObject *rootOut) const
    {
        if (!rootOut) {
            return false;
        }
        QJsonDocument boxstreamDoc;
        if (loadBoxstreamDocFromFile(boxstreamPathForTranscriptPath(transcriptPath), &boxstreamDoc) &&
            boxstreamDoc.isObject()) {
            *rootOut = boxstreamDoc.object();
            return true;
        }
        QFile legacyFile(legacyJsonBoxstreamPathForTranscriptPath(transcriptPath));
        if (!legacyFile.exists() || !legacyFile.open(QIODevice::ReadOnly)) {
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument legacyDoc = QJsonDocument::fromJson(legacyFile.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !legacyDoc.isObject()) {
            return false;
        }
        *rootOut = legacyDoc.object();
        return true;
    }

bool TranscriptEngine::saveBoxstreamArtifact(const QString &transcriptPath, const QJsonObject &root) const
    {
        return saveBoxstreamDocToFile(boxstreamPathForTranscriptPath(transcriptPath), QJsonDocument(root));
    }

bool TranscriptEngine::saveTranscriptJson(const QString &path, const QJsonDocument &doc) const
    {
        if (!doc.isObject()) {
            return false;
        }
        QJsonObject root = doc.object();
        QJsonObject transcriptProfiles = root.value(QStringLiteral("speaker_profiles")).toObject();
        QJsonObject boxstreamProfiles;
        for (auto it = transcriptProfiles.begin(); it != transcriptProfiles.end(); ++it) {
            QJsonObject profileObj = it.value().toObject();
            const QJsonObject framingObj = profileObj.value(QStringLiteral("framing")).toObject();
            if (!framingObj.isEmpty()) {
                QJsonObject outProfile;
                outProfile[QStringLiteral("framing")] = framingObj;
                boxstreamProfiles[it.key()] = outProfile;
                profileObj.remove(QStringLiteral("framing"));
                it.value() = profileObj;
            }
        }
        root[QStringLiteral("speaker_profiles")] = transcriptProfiles;

        if (!boxstreamProfiles.isEmpty()) {
            QJsonObject boxstreamRoot;
            loadBoxstreamArtifact(path, &boxstreamRoot);
            boxstreamRoot[QStringLiteral("schema")] = QStringLiteral("jcut_boxstream_v1");
            boxstreamRoot[QStringLiteral("speaker_profiles")] = boxstreamProfiles;
            if (!saveBoxstreamArtifact(path, boxstreamRoot)) {
                return false;
            }
        }

        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            return false;
        }
        const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
        if (file.write(payload) != payload.size())
        {
            file.cancelWriting();
            return false;
        }
        if (!file.commit()) {
            return false;
        }
        return saveTranscriptTextCompanion(path, root);
    }

bool TranscriptEngine::ensureTranscriptTextCompanion(const QString &path) const
    {
        if (path.trimmed().isEmpty()) {
            return false;
        }
        const QFileInfo jsonInfo(path);
        if (!jsonInfo.exists() || !jsonInfo.isFile()) {
            return false;
        }
        const QFileInfo txtInfo(transcriptTextCompanionPath(path));
        if (txtInfo.exists() && txtInfo.isFile() && txtInfo.size() > 0) {
            return true;
        }

        QJsonDocument doc;
        if (!loadTranscriptJson(path, &doc) || !doc.isObject()) {
            return false;
        }
        return saveTranscriptTextCompanion(path, doc.object());
    }

int64_t TranscriptEngine::adjustedLocalFrameForClip(const TimelineClip &clip,
                                      int64_t localTimelineFrame,
                                      const QVector<RenderSyncMarker> &markers) const
    {
        int64_t adjustedLocalFrame = qMax<int64_t>(0, localTimelineFrame);
        int duplicateCarry = 0;
        for (const RenderSyncMarker &marker : markers)
        {
            if (marker.clipId != clip.id)
            {
                continue;
            }
            const int64_t markerLocalFrame = marker.frame - clip.startFrame;
            if (markerLocalFrame < 0 || markerLocalFrame >= localTimelineFrame)
            {
                continue;
            }
            if (duplicateCarry > 0)
            {
                adjustedLocalFrame -= 1;
                duplicateCarry -= 1;
                continue;
            }
            if (marker.action == RenderSyncAction::DuplicateFrame)
            {
                adjustedLocalFrame -= 1;
                duplicateCarry = qMax(0, marker.count - 1);
            }
            else if (marker.action == RenderSyncAction::SkipFrame)
            {
                adjustedLocalFrame += marker.count;
            }
        }
        return adjustedLocalFrame;
    }

void TranscriptEngine::appendMergedExportFrame(QVector<ExportRangeSegment> &ranges, int64_t frame) const
    {
        if (ranges.isEmpty() || frame > ranges.constLast().endFrame + 1)
        {
            ranges.push_back(ExportRangeSegment{frame, frame});
            return;
        }
        ranges.last().endFrame = qMax(ranges.last().endFrame, frame);
    }

QVector<ExportRangeSegment> TranscriptEngine::transcriptWordExportRanges(const QVector<ExportRangeSegment> &baseRanges,
                                                           const QVector<TimelineClip> &clips,
                                                           const QVector<RenderSyncMarker> &markers,
                                                           int transcriptPrependMs,
                                                           int transcriptPostpendMs) const
    {
        QString cacheSignature;
        cacheSignature.reserve(256);
        cacheSignature += QStringLiteral("pre=%1|post=%2|").arg(transcriptPrependMs).arg(transcriptPostpendMs);
        for (const ExportRangeSegment &range : baseRanges)
        {
            cacheSignature += QStringLiteral("base:%1-%2|").arg(range.startFrame).arg(range.endFrame);
        }
        for (const RenderSyncMarker &marker : markers)
        {
            cacheSignature += QStringLiteral("marker:%1:%2:%3:%4|")
                                  .arg(marker.clipId)
                                  .arg(marker.frame)
                                  .arg(static_cast<int>(marker.action))
                                  .arg(marker.count);
        }
        for (const TimelineClip &clip : clips)
        {
            const QFileInfo transcriptInfo(transcriptPathForClip(clip));
            cacheSignature += QStringLiteral("clip:%1:%2:%3:%4:%5:%6:%7:%8:%9:%10|")
                                  .arg(clip.id)
                                  .arg(clip.startFrame)
                                  .arg(clip.startSubframeSamples)
                                  .arg(clip.durationFrames)
                                  .arg(clip.sourceInFrame)
                                  .arg(clip.sourceInSubframeSamples)
                                  .arg(clip.sourceDurationFrames)
                                  .arg(clip.playbackRate, 0, 'g', 12)
                                  .arg(clip.filePath)
                                  .arg(transcriptInfo.exists() ? transcriptInfo.lastModified().toMSecsSinceEpoch() : -1);
        }

        if (m_transcriptWordRangesCacheSignature == cacheSignature)
        {
            return m_transcriptWordRangesMergedCache;
        }

        m_transcriptWordRangesCache.clear();
        m_transcriptWordRangesCacheSignature = cacheSignature;
        m_transcriptWordRangesMergedCache.clear();

        QVector<ExportRangeSegment> resolvedBaseRanges = baseRanges;
        if (resolvedBaseRanges.isEmpty())
        {
            int64_t endFrame = -1;
            for (const TimelineClip &clip : clips)
            {
                if (clip.durationFrames <= 0) {
                    continue;
                }
                endFrame = qMax(endFrame, clip.startFrame + clip.durationFrames - 1);
            }
            if (endFrame < 0) {
                return {};
            }
            resolvedBaseRanges.push_back(ExportRangeSegment{0, endFrame});
        }

        QVector<ExportRangeSegment> allTranscriptRanges;
        QVector<ExportRangeSegment> filteredClipCoverage;

        for (const TimelineClip &clip : clips)
        {
            if ((clip.mediaType != ClipMediaType::Audio && !clip.hasAudio) || clip.durationFrames <= 0)
            {
                continue;
            }

            QJsonDocument transcriptDoc;
            if (!loadTranscriptJson(transcriptPathForClip(clip), &transcriptDoc))
            {
                continue;
            }

            filteredClipCoverage.push_back(ExportRangeSegment{
                clip.startFrame,
                clip.startFrame + clip.durationFrames - 1,
            });

            // Build source word ranges from transcript
            QVector<ExportRangeSegment> sourceWordRanges;
            const QJsonArray segments = transcriptDoc.object().value(QStringLiteral("segments")).toArray();
            for (const QJsonValue &segmentValue : segments)
            {
                const QJsonArray words = segmentValue.toObject().value(QStringLiteral("words")).toArray();
                for (const QJsonValue &wordValue : words)
                {
                    const QJsonObject wordObj = wordValue.toObject();
                    if (wordObj.value(QStringLiteral("skipped")).toBool(false))
                    {
                        continue;
                    }
                    if (wordObj.value(QStringLiteral("word")).toString().trimmed().isEmpty())
                    {
                        continue;
                    }

                    double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
                    const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
                    if (startSeconds < 0.0 || endSeconds < startSeconds)
                    {
                        continue;
                    }

                    const double prependSeconds = transcriptPrependMs / 1000.0;
                    const double postpendSeconds = transcriptPostpendMs / 1000.0;
                    startSeconds = qMax(0.0, startSeconds - prependSeconds);
                    const double adjustedEndSeconds = qMax(startSeconds, endSeconds + postpendSeconds);

                    const int64_t startFrame =
                        qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)));
                    const int64_t endFrame =
                        qMax<int64_t>(startFrame, static_cast<int64_t>(std::ceil(adjustedEndSeconds * kTimelineFps)) - 1);
                    sourceWordRanges.push_back(ExportRangeSegment{startFrame, endFrame});
                }
            }

            if (sourceWordRanges.isEmpty())
            {
                continue;
            }

            // Sort and merge overlapping word ranges at source level
            const QVector<ExportRangeSegment> mergedSourceWordRanges = mergeRanges(sourceWordRanges);

            QVector<ExportRangeSegment> clipTimelineRanges;

            for (const ExportRangeSegment &baseRange : resolvedBaseRanges)
            {
                const int64_t clipStart = qMax<int64_t>(clip.startFrame, baseRange.startFrame);
                const int64_t clipEnd =
                    qMin<int64_t>(clip.startFrame + clip.durationFrames - 1, baseRange.endFrame);
                if (clipEnd < clipStart)
                {
                    continue;
                }

                for (int64_t timelineFrame = clipStart; timelineFrame <= clipEnd; ++timelineFrame)
                {
                    const int64_t localTimelineFrame = timelineFrame - clip.startFrame;
                    if (localTimelineFrame < 0 || localTimelineFrame >= clip.durationFrames)
                    {
                        continue;
                    }
                    const int64_t adjustedLocalFrame = adjustedLocalFrameForClip(clip, localTimelineFrame, markers);
                    const int64_t sourceFrame = clip.sourceInFrame + adjustedLocalFrame;
                    bool inWord = false;
                    for (const ExportRangeSegment &wordRange : std::as_const(mergedSourceWordRanges))
                    {
                        if (sourceFrame >= wordRange.startFrame && sourceFrame <= wordRange.endFrame)
                        {
                            inWord = true;
                            break;
                        }
                    }
                    if (inWord)
                    {
                        appendMergedExportFrame(clipTimelineRanges, timelineFrame);
                    }
                }
            }

            // Cache ranges for this clip
            if (!clipTimelineRanges.isEmpty())
            {
                m_transcriptWordRangesCache[clip.id] = clipTimelineRanges;
                allTranscriptRanges.append(clipTimelineRanges);
            }
        }

        // Sort and merge all ranges from all clips
        const QVector<ExportRangeSegment> passthroughRanges =
            subtractRanges(resolvedBaseRanges, filteredClipCoverage);
        allTranscriptRanges += passthroughRanges;
        m_transcriptWordRangesMergedCache = mergeRanges(allTranscriptRanges);
        return m_transcriptWordRangesMergedCache;
    }

void TranscriptEngine::invalidateCache()
{
    m_transcriptWordRangesCache.clear();
    m_transcriptWordRangesCacheSignature.clear();
    m_transcriptWordRangesMergedCache.clear();
    QMutexLocker locker(&g_boxstreamDocCacheMutex);
    g_boxstreamDocCache.clear();
}

} // namespace editor
