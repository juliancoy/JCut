#pragma once

#include "vulkan_facedetections_offscreen_detection_filters.h"

#include <QImage>
#include <QRectF>
#include <QString>
#include <QVector>

class QLocalSocket;

struct LivePreviewSample {
  int frameNumber = -1;
  QVector<jcut::facedetections::ContinuityTrack> tracks;
  QVector<jcut::facedetections::Detection> detections;
  QString titlePrefix;
};

QImage buildPreview(QImage image, const QVector<Track> &tracks,
                    const QVector<Detection> &detections,
                    const QRectF &roiRect);
bool sendPreviewFrame(QLocalSocket *socket, const QImage &image);
