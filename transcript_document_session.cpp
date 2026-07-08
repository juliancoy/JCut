#include "transcript_document_session.h"

TranscriptDocumentSession::TranscriptDocumentSession(const QString& logPrefix)
    : m_saveController(logPrefix)
{
}

bool TranscriptDocumentSession::matches(const QString& clipFilePath, const QString& transcriptPath) const
{
    return m_clipFilePath == clipFilePath && m_transcriptPath == transcriptPath;
}

QJsonObject TranscriptDocumentSession::rootObject() const
{
    return m_document.object();
}

void TranscriptDocumentSession::assign(const QString& clipFilePath,
                                       const QString& transcriptPath,
                                       const QJsonDocument& document)
{
    m_clipFilePath = clipFilePath;
    m_transcriptPath = transcriptPath;
    m_document = document;
}

void TranscriptDocumentSession::clear()
{
    m_transcriptPath.clear();
    m_clipFilePath.clear();
    m_document = QJsonDocument();
    m_saveController.clear();
}

bool TranscriptDocumentSession::mutateRoot(const std::function<bool(QJsonObject&)>& mutator)
{
    if (!m_document.isObject() || !mutator) {
        return false;
    }

    QJsonObject root = m_document.object();
    if (!mutator(root)) {
        return false;
    }

    m_document.setObject(root);
    return true;
}

void TranscriptDocumentSession::queueSave(bool synchronous, const SyncSaveFn& syncSave)
{
    m_saveController.queueSave(m_transcriptPath, m_document, synchronous, syncSave);
}

LoadedTranscriptDocumentStateRef TranscriptDocumentSession::stateRef()
{
    return LoadedTranscriptDocumentStateRef{
        .transcriptPath = &m_transcriptPath,
        .clipFilePath = &m_clipFilePath,
        .document = &m_document,
    };
}

LoadedTranscriptDocumentStateRef TranscriptDocumentSession::stateRef() const
{
    return LoadedTranscriptDocumentStateRef{
        .transcriptPath = const_cast<QString*>(&m_transcriptPath),
        .clipFilePath = const_cast<QString*>(&m_clipFilePath),
        .document = const_cast<QJsonDocument*>(&m_document),
    };
}
