#include "media_drag_payload.h"

#include <QFileInfo>
#include <QMimeData>
#include <QUrl>

namespace editor {

QMimeData* createMediaDragMimeData(const QString& absolutePath)
{
    const QFileInfo info(absolutePath);
    if (!info.exists()) {
        return nullptr;
    }

    auto* mimeData = new QMimeData;
    mimeData->setUrls({QUrl::fromLocalFile(info.absoluteFilePath())});
    return mimeData;
}

} // namespace editor
