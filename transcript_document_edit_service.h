#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <functional>

struct LoadedTranscriptDocumentStateRef {
    QString* transcriptPath = nullptr;
    QString* clipFilePath = nullptr;
    QJsonDocument* document = nullptr;
};

bool mutateLoadedTranscriptRoot(const LoadedTranscriptDocumentStateRef& state,
                                const std::function<bool(QJsonObject&)>& mutator);
void assignLoadedTranscriptState(const LoadedTranscriptDocumentStateRef& state,
                                 const QString& clipFilePath,
                                 const QString& transcriptPath,
                                 const QJsonDocument& document);
void clearLoadedTranscriptState(const LoadedTranscriptDocumentStateRef& state);
