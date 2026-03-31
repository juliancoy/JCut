#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

namespace editor {

bool isStillImagePath(const QString& path);
QImage loadSingleImageFile(const QString& framePath);
QImage loadSequenceFrameImage(const QStringList& framePaths, int64_t frameNumber);

} // namespace editor
