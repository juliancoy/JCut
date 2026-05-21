#pragma once

#include "transcript_document_edit_service.h"
#include "transcript_document_save_controller.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <functional>

class TranscriptDocumentSession
{
public:
    using SyncSaveFn = TranscriptDocumentSaveController::SyncSaveFn;

    explicit TranscriptDocumentSession(const QString& logPrefix = QString());

    const QString& transcriptPath() const { return m_transcriptPath; }
    const QString& clipFilePath() const { return m_clipFilePath; }
    const QJsonDocument& document() const { return m_document; }

    bool hasObjectDocument() const { return m_document.isObject(); }
    bool matches(const QString& clipFilePath, const QString& transcriptPath) const;
    QJsonObject rootObject() const;

    void assign(const QString& clipFilePath,
                const QString& transcriptPath,
                const QJsonDocument& document);
    void clear();
    bool mutateRoot(const std::function<bool(QJsonObject&)>& mutator);
    void queueSave(bool synchronous, const SyncSaveFn& syncSave);

private:
    LoadedTranscriptDocumentStateRef stateRef();
    LoadedTranscriptDocumentStateRef stateRef() const;

    QString m_transcriptPath;
    QString m_clipFilePath;
    QJsonDocument m_document;
    TranscriptDocumentSaveController m_saveController;
};
