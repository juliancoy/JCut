#pragma once

#include "transcript_engine.h"

#include <QJsonDocument>
#include <QString>

struct TranscriptDocumentLoadResult
{
    QString clipFilePath;
    QString transcriptPath;
    QJsonDocument document;
    QString error;
    bool ok = false;
};

struct TranscriptDocumentSaveResult
{
    QString transcriptPath;
    QString error;
    qint64 revision = 0;
    bool ok = false;
};

bool shouldUseSynchronousTranscriptIo();

TranscriptDocumentSaveResult saveTranscriptDocumentResult(
    const QString& path,
    const QJsonDocument& doc,
    qint64 revision,
    const QString& invalidPayloadError = QStringLiteral("Invalid transcript save payload."));

TranscriptDocumentLoadResult loadTranscriptDocumentResultWithEngine(
    const QString& clipFilePath,
    const QString& transcriptPath,
    const QString& defaultError);

TranscriptDocumentLoadResult loadTranscriptDocumentResultCached(
    const QString& clipFilePath,
    const QString& transcriptPath,
    const QString& defaultError);
