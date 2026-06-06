#include "audio_time_stretch_cache.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr quint32 kTimeStretchSidecarMagic = 0x4A435453;
constexpr quint32 kTimeStretchSidecarVersion = 1;

QString cacheFileNameForSource(const QFileInfo& info, int speedKey)
{
    const QByteArray hash = QCryptographicHash::hash(
        info.absoluteFilePath().toUtf8(),
        QCryptographicHash::Sha256).toHex().left(16);
    const QString baseName = info.completeBaseName().isEmpty()
        ? QStringLiteral("audio")
        : info.completeBaseName();
    return QStringLiteral("%1_%2_ts%3.jcutsnd")
        .arg(baseName, QString::fromLatin1(hash), QString::number(speedKey));
}

QString writableUserCacheDir()
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (root.trimmed().isEmpty()) {
        root = QDir::temp().filePath(QStringLiteral("JCut"));
    }
    return QDir(root).filePath(QStringLiteral("audio_time_stretch"));
}

QStringList candidateSidecarPaths(const QString& sourcePath, int speedKey)
{
    const QFileInfo info(sourcePath);
    if (!info.exists() || !info.isFile() || speedKey <= 1000) {
        return {};
    }
    const QString fileName = cacheFileNameForSource(info, speedKey);
    return {
        info.dir().filePath(QStringLiteral(".jcut_audio_cache/%1").arg(fileName)),
        QDir(writableUserCacheDir()).filePath(fileName)
    };
}
} // namespace

QString audioTimeStretchSidecarPathForSource(const QString& sourcePath, int speedKey)
{
    const QStringList paths = candidateSidecarPaths(sourcePath, speedKey);
    return paths.isEmpty() ? QString() : paths.first();
}

bool readAudioTimeStretchSidecar(const QString& sourcePath,
                                 int speedKey,
                                 AudioTimeStretchCacheEntry* entryOut)
{
    if (entryOut) {
        *entryOut = AudioTimeStretchCacheEntry{};
    }
    const QFileInfo sourceInfo(sourcePath);
    for (const QString& sidecarPath : candidateSidecarPaths(sourcePath, speedKey)) {
        QFile file(sidecarPath);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }

        QDataStream stream(&file);
        stream.setVersion(QDataStream::Qt_6_0);
        quint32 magic = 0;
        quint32 version = 0;
        qint64 sourceSize = -1;
        qint64 sourceMtimeMs = -1;
        qint32 storedSpeedKey = 0;
        qint32 sampleRate = 0;
        qint32 channelCount = 0;
        bool fullyDecoded = false;
        QByteArray compressedSamples;
        stream >> magic;
        stream >> version;
        stream >> sourceSize;
        stream >> sourceMtimeMs;
        stream >> storedSpeedKey;
        stream >> sampleRate;
        stream >> channelCount;
        stream >> fullyDecoded;
        stream >> compressedSamples;
        if (stream.status() != QDataStream::Ok ||
            magic != kTimeStretchSidecarMagic ||
            version != kTimeStretchSidecarVersion ||
            storedSpeedKey != speedKey ||
            sourceSize != sourceInfo.size() ||
            sourceMtimeMs != sourceInfo.lastModified().toMSecsSinceEpoch() ||
            sampleRate <= 0 ||
            channelCount <= 0) {
            continue;
        }

        const QByteArray sampleBytes = qUncompress(compressedSamples);
        if (sampleBytes.isEmpty() || (sampleBytes.size() % static_cast<int>(sizeof(float))) != 0) {
            continue;
        }
        AudioTimeStretchCacheEntry entry;
        entry.samples.resize(sampleBytes.size() / static_cast<int>(sizeof(float)));
        std::memcpy(entry.samples.data(), sampleBytes.constData(), sampleBytes.size());
        entry.sampleRate = sampleRate;
        entry.channelCount = channelCount;
        entry.valid = !entry.samples.isEmpty();
        entry.fullyDecoded = fullyDecoded;
        if (entryOut) {
            *entryOut = std::move(entry);
        }
        return true;
    }
    return false;
}

bool readAudioTimeStretchSidecarMetadata(const QString& sourcePath,
                                         int speedKey,
                                         AudioTimeStretchSidecarMetadata* metadataOut)
{
    if (metadataOut) {
        *metadataOut = AudioTimeStretchSidecarMetadata{};
    }
    const QFileInfo sourceInfo(sourcePath);
    for (const QString& sidecarPath : candidateSidecarPaths(sourcePath, speedKey)) {
        QFile file(sidecarPath);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }

        QDataStream stream(&file);
        stream.setVersion(QDataStream::Qt_6_0);
        quint32 magic = 0;
        quint32 version = 0;
        qint64 sourceSize = -1;
        qint64 sourceMtimeMs = -1;
        qint32 storedSpeedKey = 0;
        qint32 sampleRate = 0;
        qint32 channelCount = 0;
        bool fullyDecoded = false;
        stream >> magic;
        stream >> version;
        stream >> sourceSize;
        stream >> sourceMtimeMs;
        stream >> storedSpeedKey;
        stream >> sampleRate;
        stream >> channelCount;
        stream >> fullyDecoded;
        if (stream.status() != QDataStream::Ok ||
            magic != kTimeStretchSidecarMagic ||
            version != kTimeStretchSidecarVersion ||
            storedSpeedKey != speedKey ||
            sourceSize != sourceInfo.size() ||
            sourceMtimeMs != sourceInfo.lastModified().toMSecsSinceEpoch() ||
            sampleRate <= 0 ||
            channelCount <= 0) {
            continue;
        }

        if (metadataOut) {
            metadataOut->sampleRate = sampleRate;
            metadataOut->channelCount = channelCount;
            metadataOut->fullyDecoded = fullyDecoded;
            metadataOut->valid = true;
        }
        return true;
    }
    return false;
}

bool writeAudioTimeStretchSidecar(const QString& sourcePath,
                                  int speedKey,
                                  const AudioTimeStretchCacheEntry& entry)
{
    if (!entry.valid || !entry.fullyDecoded || entry.samples.isEmpty() || speedKey <= 1000) {
        return false;
    }
    const QFileInfo sourceInfo(sourcePath);
    const QByteArray sampleBytes(
        reinterpret_cast<const char*>(entry.samples.constData()),
        entry.samples.size() * static_cast<int>(sizeof(float)));
    const QByteArray compressedSamples = qCompress(sampleBytes, 6);
    if (compressedSamples.isEmpty()) {
        return false;
    }
    for (const QString& sidecarPath : candidateSidecarPaths(sourcePath, speedKey)) {
        QDir().mkpath(QFileInfo(sidecarPath).absolutePath());
        QSaveFile file(sidecarPath);
        if (!file.open(QIODevice::WriteOnly)) {
            continue;
        }

        QDataStream stream(&file);
        stream.setVersion(QDataStream::Qt_6_0);
        stream << kTimeStretchSidecarMagic;
        stream << kTimeStretchSidecarVersion;
        stream << qint64(sourceInfo.size());
        stream << qint64(sourceInfo.lastModified().toMSecsSinceEpoch());
        stream << qint32(speedKey);
        stream << qint32(entry.sampleRate);
        stream << qint32(entry.channelCount);
        stream << entry.fullyDecoded;
        stream << compressedSamples;
        if (stream.status() == QDataStream::Ok && file.commit()) {
            return true;
        }
    }
    return false;
}

int64_t audioTimeStretchCacheSampleForSourceSample(int64_t sourceSample, double playbackRate)
{
    const double speed = std::clamp(playbackRate, 0.1, 3.0);
    if (std::abs(speed - 1.0) < 0.0001) {
        return std::max<int64_t>(0, sourceSample);
    }
    return std::max<int64_t>(
        0,
        static_cast<int64_t>(std::floor(
            static_cast<long double>(sourceSample) / static_cast<long double>(speed))));
}

int64_t audioTimeStretchCacheEndSampleForSourceEndSample(int64_t sourceEndSample, double playbackRate)
{
    const double speed = std::clamp(playbackRate, 0.1, 3.0);
    if (std::abs(speed - 1.0) < 0.0001) {
        return std::max<int64_t>(0, sourceEndSample);
    }
    return std::max<int64_t>(
        0,
        static_cast<int64_t>(std::ceil(
            static_cast<long double>(sourceEndSample) / static_cast<long double>(speed))));
}

int64_t audioTimeStretchSourceSamplesCoveredByCacheSamples(int64_t cacheSamples, double playbackRate)
{
    const double speed = std::clamp(playbackRate, 0.1, 3.0);
    if (std::abs(speed - 1.0) < 0.0001) {
        return std::max<int64_t>(0, cacheSamples);
    }
    return std::max<int64_t>(
        0,
        static_cast<int64_t>(std::floor(
            static_cast<long double>(cacheSamples) * static_cast<long double>(speed))));
}

bool audioTimeStretchSegmentCoversSourceRange(int64_t segmentCacheStartSample,
                                              int64_t segmentCacheFrameCount,
                                              int64_t sourceStartSample,
                                              int64_t sourceEndSampleExclusive,
                                              double playbackRate)
{
    if (segmentCacheFrameCount <= 0 || sourceEndSampleExclusive <= sourceStartSample) {
        return false;
    }
    const int64_t cacheStart =
        audioTimeStretchCacheSampleForSourceSample(sourceStartSample, playbackRate);
    const int64_t cacheEnd =
        audioTimeStretchCacheEndSampleForSourceEndSample(sourceEndSampleExclusive, playbackRate);
    const int64_t segmentEnd = segmentCacheStartSample + segmentCacheFrameCount;
    return cacheStart >= segmentCacheStartSample && cacheEnd <= segmentEnd;
}
