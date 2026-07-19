#pragma once

#include <QString>

class QMimeData;

namespace editor {

// The caller owns the returned payload. Returns nullptr for an unavailable path.
QMimeData* createMediaDragMimeData(const QString& absolutePath);

} // namespace editor
