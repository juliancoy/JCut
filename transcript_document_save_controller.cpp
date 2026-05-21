#include "transcript_document_save_controller.h"

#include <QDebug>
#include <QtConcurrent/QtConcurrentRun>

TranscriptDocumentSaveController::TranscriptDocumentSaveController(const QString& logPrefix)
    : m_logPrefix(logPrefix.trimmed())
{
    QObject::connect(&m_saveWatcher, &QFutureWatcher<TranscriptDocumentSaveResult>::finished, [&]() {
        const TranscriptDocumentSaveResult result = m_saveWatcher.result();
        if (!result.ok) {
            const QString prefix = m_logPrefix.isEmpty() ? QStringLiteral("transcript") : m_logPrefix;
            qWarning().noquote()
                << QStringLiteral("[%1] async save failed for %2: %3")
                       .arg(prefix,
                            result.transcriptPath,
                            result.error.isEmpty() ? QStringLiteral("unknown error") : result.error);
        }
        if (m_pendingSaveRevision > result.revision &&
            !m_pendingSavePath.trimmed().isEmpty() &&
            m_pendingSaveDoc.isObject()) {
            startAsyncSave();
        }
    });
}

void TranscriptDocumentSaveController::queueSave(const QString& path,
                                                 const QJsonDocument& doc,
                                                 bool synchronous,
                                                 const SyncSaveFn& syncSave)
{
    if (path.trimmed().isEmpty() || !doc.isObject()) {
        return;
    }

    ++m_saveRevision;
    m_pendingSaveRevision = m_saveRevision;
    m_pendingSavePath = path;
    m_pendingSaveDoc = doc;

    if (synchronous) {
        if (syncSave) {
            syncSave(m_pendingSavePath, m_pendingSaveDoc);
        }
        return;
    }
    if (m_saveWatcher.isRunning()) {
        return;
    }
    startAsyncSave();
}

void TranscriptDocumentSaveController::clear()
{
    m_pendingSavePath.clear();
    m_pendingSaveDoc = QJsonDocument();
    m_pendingSaveRevision = 0;
}

void TranscriptDocumentSaveController::startAsyncSave()
{
    const QString path = m_pendingSavePath;
    const QJsonDocument doc = m_pendingSaveDoc;
    const qint64 revision = m_pendingSaveRevision;
    m_saveWatcher.setFuture(QtConcurrent::run([path, doc, revision]() {
        return saveTranscriptDocumentResult(path, doc, revision);
    }));
}
