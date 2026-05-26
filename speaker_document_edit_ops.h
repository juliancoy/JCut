#pragma once

#include "transcript_document_session.h"

#include <QString>
#include <QVector>

struct SpeakerDocumentEditResult
{
    bool ok = false;
    bool changed = false;
};

struct SpeakerFieldValueUpdate
{
    QString speakerId;
    QString value;
};

namespace speaker_document_edit_ops {

SpeakerDocumentEditResult applyProfileCellEdit(TranscriptDocumentSession& session,
                                               const QString& speakerId,
                                               int column,
                                               const QString& valueText);
SpeakerDocumentEditResult setTrackingEnabled(TranscriptDocumentSession& session,
                                             const QString& speakerId,
                                             bool enabled);
SpeakerDocumentEditResult deleteAutoTrackPointstream(TranscriptDocumentSession& session,
                                                     const QString& speakerId);
SpeakerDocumentEditResult setSpeakerSkipped(TranscriptDocumentSession& session,
                                            const QString& speakerId,
                                            bool skipped);
SpeakerDocumentEditResult applyProfileStringFieldUpdates(TranscriptDocumentSession& session,
                                                         const QString& fieldKey,
                                                         const QVector<SpeakerFieldValueUpdate>& updates);
SpeakerDocumentEditResult cycleFramingMode(TranscriptDocumentSession& session,
                                           const QString& speakerId);

} // namespace speaker_document_edit_ops
