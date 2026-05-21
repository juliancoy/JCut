#include "transcript_document_edit_service.h"

bool mutateLoadedTranscriptRoot(const LoadedTranscriptDocumentStateRef& state,
                                const std::function<bool(QJsonObject&)>& mutator)
{
    if (!state.document || !state.document->isObject() || !mutator) {
        return false;
    }

    QJsonObject root = state.document->object();
    if (!mutator(root)) {
        return false;
    }

    state.document->setObject(root);
    return true;
}

void assignLoadedTranscriptState(const LoadedTranscriptDocumentStateRef& state,
                                 const QString& clipFilePath,
                                 const QString& transcriptPath,
                                 const QJsonDocument& document)
{
    if (state.clipFilePath) {
        *state.clipFilePath = clipFilePath;
    }
    if (state.transcriptPath) {
        *state.transcriptPath = transcriptPath;
    }
    if (state.document) {
        *state.document = document;
    }
}

void clearLoadedTranscriptState(const LoadedTranscriptDocumentStateRef& state)
{
    if (state.transcriptPath) {
        state.transcriptPath->clear();
    }
    if (state.clipFilePath) {
        state.clipFilePath->clear();
    }
    if (state.document) {
        *state.document = QJsonDocument();
    }
}
