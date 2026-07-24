#include "transcript_runtime_cache.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace editor {
namespace {

constexpr quint32 kTranscriptRuntimeMagic = 0x4a435452; // JCTR
constexpr quint32 kTranscriptRuntimeVersion = 3;
constexpr qsizetype kMaxRuntimeSections = 2'000'000;
constexpr qsizetype kMaxRuntimeWords = 5'000'000;
constexpr qsizetype kMaxRuntimeSentenceRuns = 2'000'000;

} // namespace

QString transcriptRuntimeSidecarPath(const QString& transcriptPath)
{
    const QFileInfo info(transcriptPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral("_transcript_runtime.bin"));
}

bool loadTranscriptRuntimeSidecar(const QString& transcriptPath,
                                  qint64 mtimeMs,
                                  qint64 fileSize,
                                  TranscriptRuntimeDocument* documentOut)
{
    if (!documentOut) {
        return false;
    }
    QFile file(transcriptRuntimeSidecarPath(transcriptPath));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);

    quint32 magic = 0;
    quint32 version = 0;
    QString sourcePath;
    qint64 storedMtimeMs = -1;
    qint64 storedFileSize = -1;
    stream >> magic >> version >> sourcePath >> storedMtimeMs >> storedFileSize;
    if (stream.status() != QDataStream::Ok ||
        magic != kTranscriptRuntimeMagic ||
        version != kTranscriptRuntimeVersion ||
        QFileInfo(sourcePath).absoluteFilePath() != QFileInfo(transcriptPath).absoluteFilePath() ||
        storedMtimeMs != mtimeMs ||
        storedFileSize != fileSize) {
        return false;
    }

    TranscriptRuntimeDocument document;
    document.mtimeMs = mtimeMs;
    document.fileSize = fileSize;

    qsizetype sectionCount = 0;
    stream >> sectionCount;
    if (stream.status() != QDataStream::Ok || sectionCount < 0 || sectionCount > kMaxRuntimeSections) {
        return false;
    }
    document.sections.reserve(sectionCount);
    qsizetype totalWords = 0;
    for (qsizetype sectionIndex = 0; sectionIndex < sectionCount; ++sectionIndex) {
        TranscriptSection section;
        qsizetype wordCount = 0;
        qint64 sectionStartFrame = 0;
        qint64 sectionEndFrame = 0;
        stream >> sectionStartFrame >> sectionEndFrame >> section.text >> wordCount;
        if (stream.status() != QDataStream::Ok ||
            wordCount < 0 ||
            totalWords + wordCount > kMaxRuntimeWords) {
            return false;
        }
        section.startFrame = sectionStartFrame;
        section.endFrame = sectionEndFrame;
        section.words.reserve(wordCount);
        for (qsizetype wordIndex = 0; wordIndex < wordCount; ++wordIndex) {
            TranscriptWord word;
            qint64 wordStartFrame = 0;
            qint64 wordEndFrame = 0;
            stream >> wordStartFrame >> wordEndFrame >> word.speaker >> word.text >> word.skipped;
            if (stream.status() != QDataStream::Ok) {
                return false;
            }
            word.startFrame = wordStartFrame;
            word.endFrame = wordEndFrame;
            section.words.push_back(std::move(word));
        }
        totalWords += wordCount;
        document.sections.push_back(std::move(section));
    }

    qsizetype speakerCount = 0;
    stream >> speakerCount;
    if (stream.status() != QDataStream::Ok || speakerCount < 0 || speakerCount > kMaxRuntimeSections) {
        return false;
    }
    for (qsizetype speakerIndex = 0; speakerIndex < speakerCount; ++speakerIndex) {
        QString speakerId;
        qsizetype runCount = 0;
        stream >> speakerId >> runCount;
        if (stream.status() != QDataStream::Ok ||
            speakerId.trimmed().isEmpty() ||
            runCount < 0 ||
            runCount > kMaxRuntimeSentenceRuns) {
            return false;
        }
        QVector<TranscriptSentenceRun> runs;
        runs.reserve(runCount);
        for (qsizetype runIndex = 0; runIndex < runCount; ++runIndex) {
            TranscriptSentenceRun run;
            qint64 runStartFrame = 0;
            qint64 runEndFrame = 0;
            stream >> runStartFrame >> runEndFrame >> run.text;
            if (stream.status() != QDataStream::Ok) {
                return false;
            }
            run.startFrame = runStartFrame;
            run.endFrame = runEndFrame;
            runs.push_back(std::move(run));
        }
        document.sentenceRunsBySpeaker.insert(speakerId, std::move(runs));
    }

    if (stream.status() != QDataStream::Ok) {
        return false;
    }
    *documentOut = std::move(document);
    return true;
}

void writeTranscriptRuntimeSidecar(const QString& transcriptPath,
                                   const TranscriptRuntimeDocument& document)
{
    QSaveFile file(transcriptRuntimeSidecarPath(transcriptPath));
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << kTranscriptRuntimeMagic
           << kTranscriptRuntimeVersion
           << QFileInfo(transcriptPath).absoluteFilePath()
           << document.mtimeMs
           << document.fileSize;

    stream << static_cast<qsizetype>(document.sections.size());
    for (const TranscriptSection& section : document.sections) {
        stream << static_cast<qint64>(section.startFrame)
               << static_cast<qint64>(section.endFrame)
               << section.text
               << static_cast<qsizetype>(section.words.size());
        for (const TranscriptWord& word : section.words) {
            stream << static_cast<qint64>(word.startFrame)
                   << static_cast<qint64>(word.endFrame)
                   << word.speaker
                   << word.text
                   << word.skipped;
        }
    }

    stream << static_cast<qsizetype>(document.sentenceRunsBySpeaker.size());
    for (auto it = document.sentenceRunsBySpeaker.constBegin();
         it != document.sentenceRunsBySpeaker.constEnd();
         ++it) {
        stream << it.key() << static_cast<qsizetype>(it.value().size());
        for (const TranscriptSentenceRun& run : it.value()) {
            stream << static_cast<qint64>(run.startFrame)
                   << static_cast<qint64>(run.endFrame)
                   << run.text;
        }
    }

    if (stream.status() == QDataStream::Ok) {
        file.commit();
    }
}

} // namespace editor
