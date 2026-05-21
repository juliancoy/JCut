#include "transcript_document_io.h"

#include "editor_shared.h"

#include <QCoreApplication>

bool shouldUseSynchronousTranscriptIo()
{
    if (qEnvironmentVariableIntValue("JCUT_SYNC_TRANSCRIPT_IO") == 1) {
        return true;
    }
    const QString appName = QCoreApplication::applicationName().trimmed().toLower();
    return appName.startsWith(QStringLiteral("test_")) || appName.contains(QStringLiteral("qtest"));
}

TranscriptDocumentSaveResult saveTranscriptDocumentResult(
    const QString& path,
    const QJsonDocument& doc,
    qint64 revision,
    const QString& invalidPayloadError)
{
    TranscriptDocumentSaveResult result;
    result.transcriptPath = path;
    result.revision = revision;
    if (path.trimmed().isEmpty() || !doc.isObject()) {
        result.error = invalidPayloadError;
        return result;
    }
    editor::TranscriptEngine engine;
    result.ok = engine.saveTranscriptJson(path, doc);
    if (!result.ok) {
        result.error = QStringLiteral("saveTranscriptJson returned false.");
    }
    return result;
}

TranscriptDocumentLoadResult loadTranscriptDocumentResultWithEngine(
    const QString& clipFilePath,
    const QString& transcriptPath,
    const QString& defaultError)
{
    TranscriptDocumentLoadResult result;
    result.clipFilePath = clipFilePath;
    result.transcriptPath = transcriptPath;
    editor::TranscriptEngine engine;
    QString error;
    result.ok = engine.loadTranscriptJson(transcriptPath, &result.document, &error);
    if (!result.ok) {
        result.error = error.isEmpty() ? defaultError : error;
    }
    return result;
}

TranscriptDocumentLoadResult loadTranscriptDocumentResultCached(
    const QString& clipFilePath,
    const QString& transcriptPath,
    const QString& defaultError)
{
    TranscriptDocumentLoadResult result;
    result.clipFilePath = clipFilePath;
    result.transcriptPath = transcriptPath;
    result.ok = loadTranscriptJsonCached(transcriptPath, &result.document);
    if (!result.ok) {
        result.error = defaultError;
    }
    return result;
}
