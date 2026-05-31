#pragma once

#include "editor_shared_core.h"

#include <QString>

namespace editor {

QString transcriptRuntimeSidecarPath(const QString& transcriptPath);
bool loadTranscriptRuntimeSidecar(const QString& transcriptPath,
                                  qint64 mtimeMs,
                                  qint64 fileSize,
                                  TranscriptRuntimeDocument* documentOut);
void writeTranscriptRuntimeSidecar(const QString& transcriptPath,
                                   const TranscriptRuntimeDocument& document);

} // namespace editor
