#include "audio_time_stretch_cache.h"

#include "audio_source_key.h"
#include "debug_controls.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDebug>
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
constexpr quint32 kTimeStretchSidecarRawVersion = 2;
constexpr quint8 kTimeStretchSidecarRawFloat32 = 0;

QString cacheFileNameForSource(const QString& sourceIdentity,
                               const QFileInfo& info,
                               int speedKey)
{
    const QByteArray hash = QCryptographicHash::hash(
        sourceIdentity.toUtf8(),
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
    const QString mediaPath = editor::audio::pathFromSourceKey(sourcePath);
    const QFileInfo info(mediaPath);
    if (!info.exists() || !info.isFile() || speedKey <= 1000) {
        return {};
    }
    const QString sourceIdentity =
        editor::audio::makeSourceKey(info.absoluteFilePath(),
                                     editor::audio::streamIndexFromSourceKey(sourcePath));
    const QString fileName = cacheFileNameForSource(sourceIdentity, info, speedKey);
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
    const QFileInfo sourceInfo(editor::audio::pathFromSourceKey(sourcePath));
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
            (version != kTimeStretchSidecarVersion &&
             version != kTimeStretchSidecarRawVersion) ||
            storedSpeedKey != speedKey ||
            sourceSize != sourceInfo.size() ||
            sourceMtimeMs != sourceInfo.lastModified().toMSecsSinceEpoch() ||
            sampleRate <= 0 ||
            channelCount <= 0) {
            continue;
        }

        QByteArray sampleBytes;
        if (version == kTimeStretchSidecarVersion) {
            QByteArray compressedSamples;
            stream >> compressedSamples;
            if (stream.status() != QDataStream::Ok) {
                continue;
            }
            sampleBytes = qUncompress(compressedSamples);
        } else {
            quint8 encoding = 255;
            qint64 sampleByteCount = -1;
            stream >> encoding;
            stream >> sampleByteCount;
            if (stream.status() != QDataStream::Ok ||
                encoding != kTimeStretchSidecarRawFloat32 ||
                sampleByteCount <= 0 ||
                (sampleByteCount % static_cast<int>(sizeof(float))) != 0) {
                continue;
            }
            sampleBytes = file.read(sampleByteCount);
            if (sampleBytes.size() != sampleByteCount) {
                continue;
            }
        }
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
    const QFileInfo sourceInfo(editor::audio::pathFromSourceKey(sourcePath));
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
            (version != kTimeStretchSidecarVersion &&
             version != kTimeStretchSidecarRawVersion) ||
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
                                  const AudioTimeStretchCacheEntry& entry,
                                  const std::function<void(double)>& progressCallback)
{
    if (!entry.valid || !entry.fullyDecoded || entry.samples.isEmpty() || speedKey <= 1000) {
        return false;
    }
    const QFileInfo sourceInfo(editor::audio::pathFromSourceKey(sourcePath));
    const char* sampleBytes = reinterpret_cast<const char*>(entry.samples.constData());
    const qint64 sampleByteCount =
        static_cast<qint64>(entry.samples.size()) * static_cast<qint64>(sizeof(float));
    if (!sampleBytes || sampleByteCount <= 0) {
        return false;
    }
    for (const QString& sidecarPath : candidateSidecarPaths(sourcePath, speedKey)) {
        QDir().mkpath(QFileInfo(sidecarPath).absolutePath());
        QSaveFile file(sidecarPath);
        if (!file.open(QIODevice::WriteOnly)) {
            if (editor::debugCacheWarnEnabled()) {
                qDebug().noquote()
                    << QStringLiteral("[CACHE][TIMESTRETCH][WARN] sidecar-open-failed path=\"%1\" speed=%2 error=\"%3\" source=\"%4\"")
                           .arg(sidecarPath)
                           .arg(speedKey)
                           .arg(file.errorString())
                           .arg(sourcePath);
            }
            continue;
        }
        if (editor::debugCacheEnabled()) {
            qDebug().noquote()
                << QStringLiteral("[CACHE][TIMESTRETCH] sidecar-write-start path=\"%1\" speed=%2 bytes=%3 samples=%4 sample_rate=%5 channels=%6 source=\"%7\"")
                       .arg(sidecarPath)
                       .arg(speedKey)
                       .arg(sampleByteCount)
                       .arg(entry.samples.size())
                       .arg(entry.sampleRate)
                       .arg(entry.channelCount)
                       .arg(sourcePath);
        }

        QDataStream stream(&file);
        stream.setVersion(QDataStream::Qt_6_0);
        stream << kTimeStretchSidecarMagic;
        stream << kTimeStretchSidecarRawVersion;
        stream << qint64(sourceInfo.size());
        stream << qint64(sourceInfo.lastModified().toMSecsSinceEpoch());
        stream << qint32(speedKey);
        stream << qint32(entry.sampleRate);
        stream << qint32(entry.channelCount);
        stream << entry.fullyDecoded;
        stream << kTimeStretchSidecarRawFloat32;
        stream << sampleByteCount;
        if (stream.status() != QDataStream::Ok) {
            if (editor::debugCacheWarnEnabled()) {
                qDebug().noquote()
                    << QStringLiteral("[CACHE][TIMESTRETCH][WARN] sidecar-header-write-failed path=\"%1\" speed=%2 status=%3 source=\"%4\"")
                           .arg(sidecarPath)
                           .arg(speedKey)
                           .arg(static_cast<int>(stream.status()))
                           .arg(sourcePath);
            }
            continue;
        }

        constexpr qint64 kWriteChunkBytes = 8 * 1024 * 1024;
        qint64 written = 0;
        while (written < sampleByteCount) {
            const qint64 chunk = std::min(kWriteChunkBytes, sampleByteCount - written);
            const qint64 n = file.write(sampleBytes + written, chunk);
            if (n <= 0) {
                if (editor::debugCacheWarnEnabled()) {
                    qDebug().noquote()
                        << QStringLiteral("[CACHE][TIMESTRETCH][WARN] sidecar-chunk-write-failed path=\"%1\" speed=%2 written=%3/%4 error=\"%5\" source=\"%6\"")
                               .arg(sidecarPath)
                               .arg(speedKey)
                               .arg(written)
                               .arg(sampleByteCount)
                               .arg(file.errorString())
                               .arg(sourcePath);
                }
                break;
            }
            written += n;
            if (progressCallback && written < sampleByteCount) {
                progressCallback(static_cast<double>(written) /
                                 static_cast<double>(sampleByteCount));
            }
        }
        if (written == sampleByteCount && file.commit()) {
            if (editor::debugCacheEnabled()) {
                const QFileInfo committedInfo(sidecarPath);
                qDebug().noquote()
                    << QStringLiteral("[CACHE][TIMESTRETCH] sidecar-write-committed path=\"%1\" speed=%2 bytes=%3 file_size=%4 source=\"%5\"")
                           .arg(sidecarPath)
                           .arg(speedKey)
                           .arg(sampleByteCount)
                           .arg(committedInfo.size())
                           .arg(sourcePath);
            }
            if (progressCallback) {
                progressCallback(1.0);
            }
            return true;
        }
        if (written == sampleByteCount && editor::debugCacheWarnEnabled()) {
            qDebug().noquote()
                << QStringLiteral("[CACHE][TIMESTRETCH][WARN] sidecar-commit-failed path=\"%1\" speed=%2 bytes=%3 error=\"%4\" source=\"%5\"")
                       .arg(sidecarPath)
                       .arg(speedKey)
                       .arg(sampleByteCount)
                       .arg(file.errorString())
                       .arg(sourcePath);
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
