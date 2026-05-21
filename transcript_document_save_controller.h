#pragma once

#include "transcript_document_io.h"

#include <QFutureWatcher>
#include <QJsonDocument>
#include <QString>

#include <functional>

class TranscriptDocumentSaveController
{
public:
    using SyncSaveFn = std::function<void(const QString&, const QJsonDocument&)>;

    explicit TranscriptDocumentSaveController(const QString& logPrefix = QString());

    void queueSave(const QString& path,
                   const QJsonDocument& doc,
                   bool synchronous,
                   const SyncSaveFn& syncSave);
    void clear();

private:
    void startAsyncSave();

    QString m_logPrefix;
    QFutureWatcher<TranscriptDocumentSaveResult> m_saveWatcher;
    qint64 m_saveRevision = 0;
    qint64 m_pendingSaveRevision = 0;
    QString m_pendingSavePath;
    QJsonDocument m_pendingSaveDoc;
};
