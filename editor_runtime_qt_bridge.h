#pragma once

#include "editor_document_core.h"

#include <QString>
#include <QVector>

struct TimelineClip;
struct TimelineTrack;

namespace jcut {

EditorDocumentCore buildEditorDocumentCore(const QString& projectName,
                                           const QVector<TimelineClip>& clips,
                                           const QVector<TimelineTrack>& tracks);

} // namespace jcut
