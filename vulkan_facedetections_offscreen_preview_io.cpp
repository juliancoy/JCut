#include "vulkan_facedetections_offscreen_preview_io.h"

#include "facedetections_generation.h"

#include <QDataStream>
#include <QLocalSocket>

#include <cstring>

QImage buildPreview(QImage image, const QVector<Track> &tracks,
                    const QVector<Detection> &detections,
                    const QRectF &roiRect) {
  QVector<QRect> boxes;
  if (!tracks.isEmpty()) {
    boxes.reserve(tracks.size());
    for (const Track &track : tracks) {
      if (track.id < 0 || !track.box.isValid() || track.box.isEmpty()) {
        continue;
      }
      boxes.push_back(track.box.toAlignedRect());
    }
  } else {
    boxes.reserve(detections.size());
    for (const Detection &detection : detections) {
      boxes.push_back(detection.box.toAlignedRect());
    }
  }
  return jcut::facedetections::buildScanPreview(image, boxes, boxes.size(),
                                                roiRect);
}

bool sendPreviewFrame(QLocalSocket *socket, const QImage &image) {
  if (!socket || socket->state() != QLocalSocket::ConnectedState ||
      image.isNull()) {
    return false;
  }
  const QImage argb =
      image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  QByteArray payload;
  payload.resize(argb.sizeInBytes());
  std::memcpy(payload.data(), argb.constBits(),
              static_cast<size_t>(payload.size()));
  QDataStream stream(socket);
  stream.setVersion(QDataStream::Qt_6_0);
  stream << quint32(argb.width()) << quint32(argb.height())
         << quint32(payload.size());
  const qint64 written = socket->write(payload);
  socket->flush();
  socket->waitForBytesWritten(100);
  return written == payload.size();
}
