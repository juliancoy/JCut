#include "transcript_engine.h"

#include "editor_shared.h"
#include "json_io_utils.h"

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
struct FacestreamDocCacheEntry {
    qint64 modifiedMs = 0;
    qint64 fileSize = 0;
    QJsonDocument doc;
};

QMutex g_facedetectionsDocCacheMutex;
QHash<QString, FacestreamDocCacheEntry> g_facedetectionsDocCache;

QString originalTranscriptPathForEditableTranscriptPath(const QString& transcriptPath)
{
    const QFileInfo info(transcriptPath);
    const QString base = info.completeBaseName();
    if (!base.endsWith(QStringLiteral("_editable"))) {
        return QString();
    }
    return info.dir().filePath(base.left(base.size() - QStringLiteral("_editable").size()) +
                               QStringLiteral(".json"));
}

QString editableTranscriptPathForTranscriptPath(const QString& transcriptPath)
{
    const QFileInfo info(transcriptPath);
    const QString base = info.completeBaseName();
    if (base.endsWith(QStringLiteral("_editable"))) {
        return info.absoluteFilePath();
    }
    return info.dir().filePath(base + QStringLiteral("_editable.json"));
}

QStringList transcriptSidecarCandidatePaths(const QString& transcriptPath)
{
    QStringList paths;
    const QString exact = QFileInfo(transcriptPath).absoluteFilePath();
    const QString original = QFileInfo(originalTranscriptPathForEditableTranscriptPath(exact)).absoluteFilePath();
    const QString editable = QFileInfo(editableTranscriptPathForTranscriptPath(exact)).absoluteFilePath();
    paths << exact << original << editable;
    paths.removeDuplicates();
    paths.erase(std::remove_if(paths.begin(),
                               paths.end(),
                               [](const QString& path) {
                                   return path.trimmed().isEmpty() || !QFileInfo::exists(path);
                               }),
                paths.end());
    return paths;
}

bool facestreamArtifactRootHasUsablePayload(const QJsonObject& root)
{
    const auto clipRootHasTrackPayload = [](const QJsonObject& clipRoot) {
        return !clipRoot.value(QStringLiteral("streams")).toArray().isEmpty() ||
               !clipRoot.value(QStringLiteral("raw_tracks")).toArray().isEmpty();
    };
    const auto byClipHasTrackPayload = [&](const QJsonObject& byClip) {
        for (auto it = byClip.constBegin(); it != byClip.constEnd(); ++it) {
            if (clipRootHasTrackPayload(it.value().toObject())) {
                return true;
            }
        }
        return false;
    };
    const QJsonObject current =
        root.value(QStringLiteral("continuity_facedetections_by_clip")).toObject();
    if (byClipHasTrackPayload(current)) {
        return true;
    }
    return byClipHasTrackPayload(root.value(QStringLiteral("continuity_facestreams_by_clip")).toObject());
}

QString facedetectionsPathForTranscriptPath(const QString& transcriptPath)
{
    const QFileInfo info(transcriptPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral("_facedetections.bin"));
}

QString facestreamPathForTranscriptPath(const QString& transcriptPath)
{
    const QFileInfo info(transcriptPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral("_facestream.bin"));
}

QString facedetectionsProcessedPathForTranscriptPath(const QString& transcriptPath)
{
    const QFileInfo info(transcriptPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral("_facedetections_processed.bin"));
}

QString facestreamProcessedPathForTranscriptPath(const QString& transcriptPath)
{
    const QFileInfo info(transcriptPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral("_facestream_processed.bin"));
}

QStringList facestreamArtifactCandidatePaths(const QString& transcriptPath, bool processed)
{
    QStringList paths;
    if (processed) {
        paths << facedetectionsProcessedPathForTranscriptPath(transcriptPath)
              << facestreamProcessedPathForTranscriptPath(transcriptPath);
    } else {
        paths << facedetectionsPathForTranscriptPath(transcriptPath)
              << facestreamPathForTranscriptPath(transcriptPath);
    }
    paths.removeDuplicates();
    return paths;
}

QString identityPathForTranscriptPath(const QString& transcriptPath)
{
    const QFileInfo info(transcriptPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral("_identity.bin"));
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

QByteArray facedetectionsMagic()
{
    return QByteArrayLiteral("JCUTBOX1");
}

bool parseFacestreamCborDocument(const QByteArray& cborBytes, QJsonDocument* outDoc)
{
    if (!outDoc) {
        return false;
    }
    try {
        const auto cbor = jcut::jsonio::Json::from_cbor(cborBytes.begin(), cborBytes.end());
        if (!cbor.is_object()) {
            return false;
        }
        const QJsonObject object = jcut::jsonio::fromJson(cbor).toObject();
        *outDoc = QJsonDocument(object);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool loadFacestreamDocFromFile(const QString& path, QJsonDocument* outDoc)
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
        QMutexLocker locker(&g_facedetectionsDocCacheMutex);
        const auto cached = g_facedetectionsDocCache.constFind(path);
        if (cached != g_facedetectionsDocCache.constEnd() &&
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
    const QByteArray magic = facedetectionsMagic();
    if (file.size() < magic.size() + static_cast<int>(sizeof(quint32) * 2)) {
        return false;
    }
    if (file.read(magic.size()) != magic) {
        return false;
    }
    const QByteArray versionBytes = file.read(static_cast<qint64>(sizeof(quint32)));
    const QByteArray sizeBytes = file.read(static_cast<qint64>(sizeof(quint32)));
    if (versionBytes.size() != static_cast<int>(sizeof(quint32)) ||
        sizeBytes.size() != static_cast<int>(sizeof(quint32))) {
        return false;
    }
    const quint32 version =
        qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(versionBytes.constData()));
    const quint32 storedSize =
        qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(sizeBytes.constData()));
    const qint64 headerSize = magic.size() + static_cast<qint64>(sizeof(quint32) * 2);
    if (storedSize > static_cast<quint64>(file.size() - headerSize)) {
        return false;
    }
    if (storedSize > static_cast<quint32>(std::numeric_limits<int>::max())) {
        return false;
    }
    const QByteArray storedBytes = file.read(static_cast<qint64>(storedSize));
    if (storedBytes.size() != static_cast<int>(storedSize)) {
        return false;
    }

    QJsonDocument doc;
    if (version == 1) {
        QJsonParseError parseError;
        doc = QJsonDocument::fromJson(storedBytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            return false;
        }
    } else if (version == 2) {
        const QByteArray cborBytes = qUncompress(storedBytes);
        if (cborBytes.isEmpty() && !storedBytes.isEmpty()) {
            return false;
        }
        if (!parseFacestreamCborDocument(cborBytes, &doc)) {
            return false;
        }
    } else {
        return false;
    }
    {
        QMutexLocker locker(&g_facedetectionsDocCacheMutex);
        g_facedetectionsDocCache.insert(path, FacestreamDocCacheEntry{modifiedMs, fileSize, doc});
    }
    *outDoc = doc;
    return true;
}

bool saveFacestreamDocToFile(const QString& path, const QJsonDocument& doc)
{
    if (!doc.isObject()) {
        return false;
    }
    const auto cborVector = jcut::jsonio::Json::to_cbor(jcut::jsonio::toJson(doc.object()));
    const QByteArray cborBytes(reinterpret_cast<const char*>(cborVector.data()),
                               static_cast<int>(cborVector.size()));
    const QByteArray compressedBytes = qCompress(cborBytes, 6);
    if (compressedBytes.size() > std::numeric_limits<quint32>::max()) {
        return false;
    }
    QByteArray payload;
    payload.reserve(facedetectionsMagic().size() + static_cast<int>(sizeof(quint32) * 2) + compressedBytes.size());
    payload.append(facedetectionsMagic());
    quint32 version = qToLittleEndian<quint32>(2);
    quint32 storedSize = qToLittleEndian<quint32>(static_cast<quint32>(compressedBytes.size()));
    payload.append(reinterpret_cast<const char*>(&version), sizeof(quint32));
    payload.append(reinterpret_cast<const char*>(&storedSize), sizeof(quint32));
    payload.append(compressedBytes);

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
    QMutexLocker locker(&g_facedetectionsDocCacheMutex);
    g_facedetectionsDocCache.insert(path, FacestreamDocCacheEntry{
        info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0,
        info.exists() ? info.size() : 0,
        doc});
    return true;
}

bool mergeFacestreamSpeakerProfiles(const QString& transcriptPath, QJsonObject* root)
{
    if (!root) {
        return false;
    }
    QJsonDocument facedetectionsDoc;
    if (!loadFacestreamDocFromFile(facedetectionsPathForTranscriptPath(transcriptPath), &facedetectionsDoc)) {
        return false;
    }
    if (!facedetectionsDoc.isObject()) {
        return false;
    }
    const QJsonObject facedetectionsRoot = facedetectionsDoc.object();
    const QJsonObject facedetectionsProfiles = facedetectionsRoot.value(QStringLiteral("speaker_profiles")).toObject();
    if (facedetectionsProfiles.isEmpty()) {
        return false;
    }
    QJsonObject transcriptProfiles = root->value(QStringLiteral("speaker_profiles")).toObject();
    for (auto it = facedetectionsProfiles.constBegin(); it != facedetectionsProfiles.constEnd(); ++it) {
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
        return activeTranscriptPathForClip(clip);
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
        mergeFacestreamSpeakerProfiles(path, &root);
        *docOut = QJsonDocument(root);
        return true;
    }

bool TranscriptEngine::loadFacestreamArtifact(const QString &transcriptPath, QJsonObject *rootOut) const
    {
        if (!rootOut) {
            return false;
        }
        QJsonObject fallbackRoot;
        bool haveFallback = false;
        for (const QString& candidatePath : transcriptSidecarCandidatePaths(transcriptPath)) {
            for (const QString& artifactPath : facestreamArtifactCandidatePaths(candidatePath, false)) {
                QJsonDocument facedetectionsDoc;
                if (loadFacestreamDocFromFile(artifactPath, &facedetectionsDoc) &&
                    facedetectionsDoc.isObject()) {
                    const QJsonObject root = facedetectionsDoc.object();
                    if (facestreamArtifactRootHasUsablePayload(root)) {
                        *rootOut = root;
                        return true;
                    }
                    if (!haveFallback && candidatePath == QFileInfo(transcriptPath).absoluteFilePath()) {
                        fallbackRoot = root;
                        haveFallback = true;
                    }
                }
            }
        }
        if (haveFallback) {
            *rootOut = fallbackRoot;
            return true;
        }
        return false;
    }

QString TranscriptEngine::facedetectionsArtifactPath(const QString &transcriptPath) const
    {
        return QFileInfo(facedetectionsPathForTranscriptPath(transcriptPath)).absoluteFilePath();
    }

bool TranscriptEngine::saveFacestreamArtifact(const QString &transcriptPath, const QJsonObject &root) const
    {
        return saveFacestreamDocToFile(facedetectionsArtifactPath(transcriptPath), QJsonDocument(root));
    }

QString TranscriptEngine::facedetectionsProcessedArtifactPath(const QString &transcriptPath) const
    {
        return QFileInfo(facedetectionsProcessedPathForTranscriptPath(transcriptPath)).absoluteFilePath();
    }

bool TranscriptEngine::loadFacestreamProcessedArtifact(const QString &transcriptPath, QJsonObject *rootOut) const
    {
        if (!rootOut) {
            return false;
        }
        QJsonObject fallbackRoot;
        bool haveFallback = false;
        for (const QString& candidatePath : transcriptSidecarCandidatePaths(transcriptPath)) {
            for (const QString& artifactPath : facestreamArtifactCandidatePaths(candidatePath, true)) {
                QJsonDocument processedDoc;
                if (loadFacestreamDocFromFile(artifactPath, &processedDoc) &&
                    processedDoc.isObject()) {
                    const QJsonObject root = processedDoc.object();
                    if (facestreamArtifactRootHasUsablePayload(root)) {
                        *rootOut = root;
                        return true;
                    }
                    if (!haveFallback && candidatePath == QFileInfo(transcriptPath).absoluteFilePath()) {
                        fallbackRoot = root;
                        haveFallback = true;
                    }
                }
            }
        }
        if (haveFallback) {
            *rootOut = fallbackRoot;
            return true;
        }
        return false;
    }

bool TranscriptEngine::saveFacestreamProcessedArtifact(const QString &transcriptPath, const QJsonObject &root) const
    {
        return saveFacestreamDocToFile(facedetectionsProcessedArtifactPath(transcriptPath), QJsonDocument(root));
    }

QString TranscriptEngine::identityArtifactPath(const QString &transcriptPath) const
    {
        return identityPathForTranscriptPath(transcriptPath);
    }

bool TranscriptEngine::loadIdentityArtifact(const QString &transcriptPath, QJsonObject *rootOut) const
    {
        if (!rootOut) {
            return false;
        }
        QJsonDocument identityDoc;
        if (loadFacestreamDocFromFile(identityPathForTranscriptPath(transcriptPath), &identityDoc) &&
            identityDoc.isObject()) {
            *rootOut = identityDoc.object();
            return true;
        }
        return false;
    }

bool TranscriptEngine::saveIdentityArtifact(const QString &transcriptPath, const QJsonObject &root) const
    {
        return saveFacestreamDocToFile(identityPathForTranscriptPath(transcriptPath), QJsonDocument(root));
    }

bool TranscriptEngine::saveTranscriptJson(const QString &path, const QJsonDocument &doc) const
    {
        if (!doc.isObject()) {
            return false;
        }
        QJsonObject root = doc.object();
        QJsonObject transcriptProfiles = root.value(QStringLiteral("speaker_profiles")).toObject();
        QJsonObject facedetectionsProfiles;
        for (auto it = transcriptProfiles.begin(); it != transcriptProfiles.end(); ++it) {
            QJsonObject profileObj = it.value().toObject();
            const QJsonObject framingObj = profileObj.value(QStringLiteral("framing")).toObject();
            if (!framingObj.isEmpty()) {
                QJsonObject outProfile;
                outProfile[QStringLiteral("framing")] = framingObj;
                facedetectionsProfiles[it.key()] = outProfile;
                profileObj.remove(QStringLiteral("framing"));
                it.value() = profileObj;
            }
        }
        root[QStringLiteral("speaker_profiles")] = transcriptProfiles;

        if (!facedetectionsProfiles.isEmpty()) {
            QJsonObject facedetectionsRoot;
            loadFacestreamArtifact(path, &facedetectionsRoot);
            facedetectionsRoot[QStringLiteral("schema")] = QStringLiteral("jcut_facedetections_v1");
            facedetectionsRoot[QStringLiteral("speaker_profiles")] = facedetectionsProfiles;
            if (!saveFacestreamArtifact(path, facedetectionsRoot)) {
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

bool TranscriptEngine::transcriptSourceWordRanges(const QString& transcriptPath,
                                                  int transcriptPrependMs,
                                                  int transcriptPostpendMs,
                                                  int transcriptOffsetMs,
                                                  QVector<ExportRangeSegment>* rangesOut) const
{
    if (!rangesOut || transcriptPath.trimmed().isEmpty()) {
        return false;
    }

    const QFileInfo transcriptInfo(transcriptPath);
    if (!transcriptInfo.exists() || !transcriptInfo.isFile()) {
        return false;
    }

    const qint64 modifiedMs = transcriptInfo.lastModified().toMSecsSinceEpoch();
    const qint64 fileSize = transcriptInfo.size();
    const std::shared_ptr<const TranscriptRuntimeDocument> runtimeDocument =
        loadTranscriptRuntimeDocument(transcriptPath);
    if (!runtimeDocument) {
        return false;
    }
    auto cacheIt = m_transcriptSourceWordRangesCache.constFind(transcriptPath);
    if (cacheIt != m_transcriptSourceWordRangesCache.constEnd() &&
        cacheIt->valid &&
        cacheIt->modifiedMs == modifiedMs &&
        cacheIt->fileSize == fileSize &&
        cacheIt->prependMs == transcriptPrependMs &&
        cacheIt->postpendMs == transcriptPostpendMs &&
        cacheIt->offsetMs == transcriptOffsetMs) {
        *rangesOut = cacheIt->sourceWordRanges;
        return true;
    }

    QVector<ExportRangeSegment> sourceWordRanges =
        transcriptPaddedWordRanges(
            runtimeDocument->sections, transcriptPrependMs, transcriptPostpendMs, transcriptOffsetMs);

    sourceWordRanges = mergeRanges(sourceWordRanges);

    TranscriptSourceWordRangeCacheEntry entry;
    entry.modifiedMs = modifiedMs;
    entry.fileSize = fileSize;
    entry.prependMs = transcriptPrependMs;
    entry.postpendMs = transcriptPostpendMs;
    entry.offsetMs = transcriptOffsetMs;
    entry.sourceWordRanges = sourceWordRanges;
    entry.valid = true;
    m_transcriptSourceWordRangesCache.insert(transcriptPath, entry);

    *rangesOut = sourceWordRanges;
    return true;
}

QVector<ExportRangeSegment> TranscriptEngine::transcriptWordExportRanges(const QVector<ExportRangeSegment> &baseRanges,
                                                           const QVector<TimelineClip> &clips,
                                                           const QVector<RenderSyncMarker> &markers,
                                                           int transcriptPrependMs,
                                                           int transcriptPostpendMs,
                                                           int transcriptOffsetMs) const
    {
        QString cacheSignature;
        cacheSignature.reserve(256);
        cacheSignature += QStringLiteral("pre=%1|post=%2|offset=%3|")
                              .arg(transcriptPrependMs)
                              .arg(transcriptPostpendMs)
                              .arg(transcriptOffsetMs);
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

            QVector<ExportRangeSegment> mergedSourceWordRanges;
            if (!transcriptSourceWordRanges(transcriptPathForClip(clip),
                                            transcriptPrependMs,
                                            transcriptPostpendMs,
                                            transcriptOffsetMs,
                                            &mergedSourceWordRanges))
            {
                continue;
            }

            filteredClipCoverage.push_back(ExportRangeSegment{
                clip.startFrame,
                clip.startFrame + clip.durationFrames - 1,
            });

            if (mergedSourceWordRanges.isEmpty())
            {
                continue;
            }

            QVector<ExportRangeSegment> clipTimelineRanges;

            if (markers.isEmpty())
            {
                for (const ExportRangeSegment& wordRange : std::as_const(mergedSourceWordRanges))
                {
                    const int64_t localStart = qMax<int64_t>(0, wordRange.startFrame - clip.sourceInFrame);
                    const int64_t localEnd = qMin<int64_t>(
                        clip.durationFrames - 1, wordRange.endFrame - clip.sourceInFrame);
                    if (localEnd < localStart) {
                        continue;
                    }

                    const int64_t wordTimelineStart = clip.startFrame + localStart;
                    const int64_t wordTimelineEnd = clip.startFrame + localEnd;
                    for (const ExportRangeSegment& baseRange : resolvedBaseRanges)
                    {
                        const int64_t start = qMax<int64_t>(wordTimelineStart, baseRange.startFrame);
                        const int64_t end = qMin<int64_t>(wordTimelineEnd, baseRange.endFrame);
                        if (end >= start) {
                            clipTimelineRanges.push_back(ExportRangeSegment{start, end});
                        }
                    }
                }
                clipTimelineRanges = mergeRanges(clipTimelineRanges);
            }
            else
            {
                for (const ExportRangeSegment &baseRange : resolvedBaseRanges)
                {
                    const int64_t clipStart = qMax<int64_t>(clip.startFrame, baseRange.startFrame);
                    const int64_t clipEnd =
                        qMin<int64_t>(clip.startFrame + clip.durationFrames - 1, baseRange.endFrame);
                    if (clipEnd < clipStart)
                    {
                        continue;
                    }

                    int wordRangeIndex = 0;
                    for (int64_t timelineFrame = clipStart; timelineFrame <= clipEnd; ++timelineFrame)
                    {
                        const int64_t localTimelineFrame = timelineFrame - clip.startFrame;
                        if (localTimelineFrame < 0 || localTimelineFrame >= clip.durationFrames)
                        {
                            continue;
                        }
                        const int64_t adjustedLocalFrame = adjustedLocalFrameForClip(clip, localTimelineFrame, markers);
                        const int64_t sourceFrame = clip.sourceInFrame + adjustedLocalFrame;
                        while (wordRangeIndex < mergedSourceWordRanges.size() &&
                               mergedSourceWordRanges.at(wordRangeIndex).endFrame < sourceFrame) {
                            ++wordRangeIndex;
                        }
                        if (wordRangeIndex < mergedSourceWordRanges.size() &&
                            sourceFrame >= mergedSourceWordRanges.at(wordRangeIndex).startFrame &&
                            sourceFrame <= mergedSourceWordRanges.at(wordRangeIndex).endFrame)
                        {
                            appendMergedExportFrame(clipTimelineRanges, timelineFrame);
                        }
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

QVector<ExportRangeSegment> TranscriptEngine::transcriptWordExportRangesDiscrete(
    const QVector<ExportRangeSegment> &baseRanges,
    const QVector<TimelineClip> &clips,
    const QVector<RenderSyncMarker> &markers,
    int transcriptPrependMs,
    int transcriptPostpendMs,
    int neighborWordRadius,
    int transcriptOffsetMs) const
{
    const int safeNeighborWordRadius = qBound(0, neighborWordRadius, 10);
    QString cacheSignature;
    cacheSignature.reserve(256);
    cacheSignature += QStringLiteral("discrete|pre=%1|post=%2|offset=%3|radius=%4|")
                          .arg(transcriptPrependMs)
                          .arg(transcriptPostpendMs)
                          .arg(transcriptOffsetMs)
                          .arg(safeNeighborWordRadius);
    for (const ExportRangeSegment &range : baseRanges) {
        cacheSignature += QStringLiteral("base:%1-%2|").arg(range.startFrame).arg(range.endFrame);
    }
    for (const RenderSyncMarker &marker : markers) {
        cacheSignature += QStringLiteral("marker:%1:%2:%3:%4|")
                              .arg(marker.clipId)
                              .arg(marker.frame)
                              .arg(static_cast<int>(marker.action))
                              .arg(marker.count);
    }
    for (const TimelineClip &clip : clips) {
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
                              .arg(transcriptInfo.exists()
                                       ? transcriptInfo.lastModified().toMSecsSinceEpoch()
                                       : -1);
    }

    if (m_transcriptWordRangesDiscreteCacheSignature == cacheSignature) {
        return m_transcriptWordRangesDiscreteCache;
    }

    m_transcriptWordRangesDiscreteCacheSignature = cacheSignature;
    m_transcriptWordRangesDiscreteCache.clear();

    QVector<ExportRangeSegment> resolvedBaseRanges = baseRanges;
    if (resolvedBaseRanges.isEmpty()) {
        int64_t endFrame = -1;
        for (const TimelineClip &clip : clips) {
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

    QVector<ExportRangeSegment> discreteRanges;

    for (const TimelineClip &clip : clips) {
        if ((clip.mediaType != ClipMediaType::Audio && !clip.hasAudio) || clip.durationFrames <= 0) {
            continue;
        }

        const std::shared_ptr<const TranscriptRuntimeDocument> runtimeDocument =
            loadTranscriptRuntimeDocument(transcriptPathForClip(clip));
        if (!runtimeDocument) {
            continue;
        }

        QVector<ExportRangeSegment> sourceWordRanges =
            transcriptPaddedWordRanges(
                runtimeDocument->sections, transcriptPrependMs, transcriptPostpendMs, transcriptOffsetMs);

        QVector<ExportRangeSegment> normalizeWordRanges = sourceWordRanges;
        if (safeNeighborWordRadius > 0 && sourceWordRanges.size() > 1) {
            normalizeWordRanges.resize(sourceWordRanges.size());
            for (int i = 0; i < sourceWordRanges.size(); ++i) {
                const int begin = qMax(0, i - safeNeighborWordRadius);
                const int end = qMin(sourceWordRanges.size() - 1, i + safeNeighborWordRadius);
                int64_t expandedStart = sourceWordRanges.at(i).startFrame;
                int64_t expandedEnd = sourceWordRanges.at(i).endFrame;
                for (int j = begin; j <= end; ++j) {
                    expandedStart = qMin(expandedStart, sourceWordRanges.at(j).startFrame);
                    expandedEnd = qMax(expandedEnd, sourceWordRanges.at(j).endFrame);
                }
                normalizeWordRanges[i] = ExportRangeSegment{expandedStart, expandedEnd};
            }
        }

        for (const ExportRangeSegment& wordRange : std::as_const(normalizeWordRanges)) {
            QVector<ExportRangeSegment> timelineRangesForWord;
            for (const ExportRangeSegment &baseRange : resolvedBaseRanges) {
                const int64_t clipStart = qMax<int64_t>(clip.startFrame, baseRange.startFrame);
                const int64_t clipEnd =
                    qMin<int64_t>(clip.startFrame + clip.durationFrames - 1, baseRange.endFrame);
                if (clipEnd < clipStart) {
                    continue;
                }

                for (int64_t timelineFrame = clipStart; timelineFrame <= clipEnd; ++timelineFrame) {
                    const int64_t localTimelineFrame = timelineFrame - clip.startFrame;
                    if (localTimelineFrame < 0 || localTimelineFrame >= clip.durationFrames) {
                        continue;
                    }
                    const int64_t adjustedLocalFrame =
                        adjustedLocalFrameForClip(clip, localTimelineFrame, markers);
                    const int64_t sourceFrame = clip.sourceInFrame + adjustedLocalFrame;
                    if (sourceFrame >= wordRange.startFrame && sourceFrame <= wordRange.endFrame) {
                        appendMergedExportFrame(timelineRangesForWord, timelineFrame);
                    }
                }
            }
            if (!timelineRangesForWord.isEmpty()) {
                discreteRanges += timelineRangesForWord;
            }
        }
    }

    m_transcriptWordRangesDiscreteCache = discreteRanges;
    return m_transcriptWordRangesDiscreteCache;
}

void TranscriptEngine::invalidateCache()
{
    m_transcriptWordRangesCache.clear();
    m_transcriptWordRangesCacheSignature.clear();
    m_transcriptWordRangesMergedCache.clear();
    m_transcriptWordRangesDiscreteCacheSignature.clear();
    m_transcriptWordRangesDiscreteCache.clear();
    QMutexLocker locker(&g_facedetectionsDocCacheMutex);
    g_facedetectionsDocCache.clear();
}

} // namespace editor
