#include "vulkan_facedetections_offscreen_artifact_io.h"

#include "json_io_utils.h"

#include <QFile>

bool writeJson(const QString &path, const QJsonObject &object) {
  return jcut::jsonio::writeJsonFile(path, object, true, nullptr);
}

bool writeBinaryJsonObject(const QString &path, const QJsonObject &object) {
  return jcut::jsonio::writeBinaryJsonObject(path, object, 0x4A435554, 1,
                                             nullptr);
}

bool appendBinaryCborRecord(QFile *file, const QJsonObject &object) {
  return jcut::jsonio::appendBinaryCborRecord(file, object, 0x4A465342, 1,
                                              nullptr);
}
