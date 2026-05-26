#pragma once

#include <QImage>
#include <QJsonObject>

QImage renderTrackAvatarImage(const QImage& image,
                              const QJsonObject& keyframeObj,
                              int avatarSize);
