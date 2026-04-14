#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

namespace editor {

bool isStillImagePath(const QString& path);
QImage loadSingleImageFile(const QString& framePath);
QImage loadSequenceFrameImage(const QStringList& framePaths, int64_t frameNumber);

// Batched image loading for better performance with image sequences
QVector<QImage> loadImageSequenceBatch(const QStringList& framePaths, const QVector<int64_t>& frameNumbers);

// Batched loading with cache management
// additionalFrames: frames that are likely to be needed soon (for prefetching)
// batchCache: cache of loaded frames from previous batch
QImage loadSequenceFrameImageBatched(const QStringList& framePaths, int64_t frameNumber, 
                                     QVector<int64_t>& additionalFrames, QVector<QImage>& batchCache);

} // namespace editor
