#include "vulkan_facedetections_offscreen_artifact_io.h"

#include "json_io_utils.h"

#include <QDataStream>
#include <QFile>
#include <QRectF>

bool writeJson(const QString &path, const QJsonObject &object) {
  return jcut::jsonio::writeJsonFile(path, object, true, nullptr);
}

bool writeBinaryJsonObject(const QString &path, const QJsonObject &object) {
  return jcut::jsonio::writeBinaryJsonObject(path, object, 0x4A435554, 1,
                                             nullptr);
}

namespace {
constexpr quint32 kFaceStreamRecordMagic = 0x4A465354; // JFST
constexpr quint32 kFaceStreamRecordVersion = 2;
constexpr quint8 kFaceStreamMetaRecord = 1;
constexpr quint8 kFaceStreamFrameRecord = 2;

void writeDetection(QDataStream &stream,
                    const jcut::facedetections::Detection &detection) {
  stream << detection.box.x() << detection.box.y() << detection.box.width()
         << detection.box.height() << detection.confidence;
}

void readDetection(QDataStream &stream,
                   jcut::facedetections::Detection *detection) {
  double x = 0.0;
  double y = 0.0;
  double w = 0.0;
  double h = 0.0;
  float confidence = 0.0f;
  stream >> x >> y >> w >> h >> confidence;
  if (detection) {
    detection->box = QRectF(x, y, w, h);
    detection->confidence = confidence;
  }
}

void writeTrackDetection(
    QDataStream &stream,
    const jcut::facedetections::FrameTrackDetection &track) {
  stream << track.trackId << track.detection.frame << track.detection.x
         << track.detection.y << track.detection.box << track.detection.score
         << track.detection.frameWidth << track.detection.frameHeight
         << track.trackBox.x() << track.trackBox.y() << track.trackBox.width()
         << track.trackBox.height()
         << static_cast<qint32>(track.trackState) << track.firstFrame
         << track.lastFrame << track.hits << track.misses;
}

void readTrackDetection(QDataStream &stream,
                        jcut::facedetections::FrameTrackDetection *track) {
  qint32 state = 0;
  double trackX = 0.0;
  double trackY = 0.0;
  double trackW = 0.0;
  double trackH = 0.0;
  jcut::facedetections::FrameTrackDetection out;
  stream >> out.trackId >> out.detection.frame >> out.detection.x
         >> out.detection.y >> out.detection.box >> out.detection.score
         >> out.detection.frameWidth >> out.detection.frameHeight >> trackX
         >> trackY >> trackW >> trackH >> state >> out.firstFrame
         >> out.lastFrame >> out.hits >> out.misses;
  out.trackBox = QRectF(trackX, trackY, trackW, trackH);
  out.trackState =
      static_cast<jcut::facedetections::ContinuityTrackState>(state);
  if (track) {
    *track = out;
  }
}

bool appendFaceStreamPayload(QFile *file, quint8 recordType,
                             const QByteArray &payload) {
  if (!file) {
    return false;
  }
  QDataStream stream(file);
  stream.setVersion(QDataStream::Qt_6_0);
  stream << kFaceStreamRecordMagic << kFaceStreamRecordVersion << recordType
         << quint32(payload.size());
  if (stream.status() != QDataStream::Ok) {
    return false;
  }
  return payload.isEmpty() ||
         file->write(payload) == static_cast<qint64>(payload.size());
}

} // namespace

bool appendFaceStreamMetaRecord(QFile *file,
                                const FaceStreamMetaRecord &record) {
  QByteArray payload;
  QDataStream stream(&payload, QIODevice::WriteOnly);
  stream.setVersion(QDataStream::Qt_6_0);
  stream << record.video << record.backend << record.startFrame
         << record.endFrame << record.stride << record.createdUtcMs;
  return stream.status() == QDataStream::Ok &&
         appendFaceStreamPayload(file, kFaceStreamMetaRecord, payload);
}

bool appendFaceStreamFrameRecord(QFile *file,
                                 const FaceStreamFrameRecord &record) {
  QByteArray payload;
  QDataStream stream(&payload, QIODevice::WriteOnly);
  stream.setVersion(QDataStream::Qt_6_0);
  stream << record.video << record.backend << record.detector << record.frame
         << record.frameSize.width() << record.frameSize.height()
         << quint32(record.detections.size());
  for (const auto &detection : record.detections) {
    writeDetection(stream, detection);
  }
  stream << quint32(record.trackDetections.size());
  for (const auto &trackDetection : record.trackDetections) {
    writeTrackDetection(stream, trackDetection);
  }
  stream << record.appVulkanFramePath << record.decoderDirectHandoff
         << record.decoderVulkanUploadFallback << record.hardwareDirectHandoff
         << record.hardwareDirectAttemptReason
         << record.hardwareInteropProbeSupported
         << record.hardwareInteropProbeFailed << record.hardwareInteropProbePath
         << record.hardwareInteropProbeReason << record.hardwareFrame
         << record.cpuFrame << record.qimageMaterialized
         << record.appRenderDecodeMs << record.appRenderTextureMs
         << record.appRenderCompositeMs << record.appRenderReadbackMs
         << record.vulkanZeroCopyDetectionMs << record.decoderVulkanUploadMs
         << record.ncnnInputMs << record.ncnnExtractMs
         << record.ncnnExtractLevel8Ms << record.ncnnExtractLevel16Ms
         << record.ncnnExtractLevel32Ms << record.ncnnPostMs
         << record.ncnnTotalMs;
  return stream.status() == QDataStream::Ok &&
         appendFaceStreamPayload(file, kFaceStreamFrameRecord, payload);
}

bool readFaceStreamRecord(QFile *file, FaceStreamMetaRecord *metaOut,
                          FaceStreamFrameRecord *frameOut, QString *error) {
  if (metaOut) {
    *metaOut = FaceStreamMetaRecord{};
  }
  if (frameOut) {
    *frameOut = FaceStreamFrameRecord{};
  }
  if (!file || file->atEnd()) {
    return false;
  }
  const qint64 recordOffset = file->pos();
  QDataStream stream(file);
  stream.setVersion(QDataStream::Qt_6_0);
  quint32 magic = 0;
  quint32 version = 0;
  quint8 recordType = 0;
  quint32 payloadSize = 0;
  stream >> magic >> version >> recordType >> payloadSize;
  if (stream.status() != QDataStream::Ok) {
    if (error) {
      *error = QStringLiteral("Failed to read facedetections checkpoint record "
                              "header at byte %1.")
                   .arg(recordOffset);
    }
    return false;
  }
  if (magic != kFaceStreamRecordMagic || version != kFaceStreamRecordVersion) {
    if (error) {
      *error = QStringLiteral("Invalid typed facedetections checkpoint record "
                              "at byte %1: expected JFST/%2 version %3.")
                   .arg(recordOffset)
                   .arg(kFaceStreamRecordMagic)
                   .arg(kFaceStreamRecordVersion);
    }
    return false;
  }
  QByteArray payload;
  payload.resize(static_cast<int>(payloadSize));
  if (payloadSize > 0 &&
      file->read(payload.data(), payload.size()) != payload.size()) {
    if (error) {
      *error = QStringLiteral("Truncated facedetections checkpoint record at "
                              "byte %1.")
                   .arg(recordOffset);
    }
    return false;
  }
  QDataStream payloadStream(payload);
  payloadStream.setVersion(QDataStream::Qt_6_0);
  if (recordType == kFaceStreamMetaRecord) {
    FaceStreamMetaRecord meta;
    payloadStream >> meta.video >> meta.backend >> meta.startFrame
        >> meta.endFrame >> meta.stride >> meta.createdUtcMs;
    if (payloadStream.status() != QDataStream::Ok) {
      if (error) {
        *error = QStringLiteral("Invalid facedetections meta checkpoint record "
                                "at byte %1.")
                     .arg(recordOffset);
      }
      return false;
    }
    if (metaOut) {
      *metaOut = meta;
    }
    return true;
  }
  if (recordType == kFaceStreamFrameRecord) {
    FaceStreamFrameRecord frame;
    int frameWidth = 0;
    int frameHeight = 0;
    quint32 detectionCount = 0;
    payloadStream >> frame.video >> frame.backend >> frame.detector
        >> frame.frame >> frameWidth >> frameHeight >> detectionCount;
    frame.frameSize = QSize(frameWidth, frameHeight);
    frame.detections.reserve(static_cast<int>(detectionCount));
    for (quint32 i = 0; i < detectionCount; ++i) {
      jcut::facedetections::Detection detection;
      readDetection(payloadStream, &detection);
      frame.detections.push_back(detection);
    }
    quint32 trackDetectionCount = 0;
    payloadStream >> trackDetectionCount;
    frame.trackDetections.reserve(static_cast<int>(trackDetectionCount));
    for (quint32 i = 0; i < trackDetectionCount; ++i) {
      jcut::facedetections::FrameTrackDetection trackDetection;
      readTrackDetection(payloadStream, &trackDetection);
      frame.trackDetections.push_back(trackDetection);
    }
    payloadStream >> frame.appVulkanFramePath >> frame.decoderDirectHandoff
        >> frame.decoderVulkanUploadFallback >> frame.hardwareDirectHandoff
        >> frame.hardwareDirectAttemptReason
        >> frame.hardwareInteropProbeSupported
        >> frame.hardwareInteropProbeFailed >> frame.hardwareInteropProbePath
        >> frame.hardwareInteropProbeReason >> frame.hardwareFrame
        >> frame.cpuFrame >> frame.qimageMaterialized
        >> frame.appRenderDecodeMs >> frame.appRenderTextureMs
        >> frame.appRenderCompositeMs >> frame.appRenderReadbackMs
        >> frame.vulkanZeroCopyDetectionMs >> frame.decoderVulkanUploadMs
        >> frame.ncnnInputMs >> frame.ncnnExtractMs
        >> frame.ncnnExtractLevel8Ms >> frame.ncnnExtractLevel16Ms
        >> frame.ncnnExtractLevel32Ms >> frame.ncnnPostMs
        >> frame.ncnnTotalMs;
    if (payloadStream.status() != QDataStream::Ok) {
      if (error) {
        *error = QStringLiteral("Invalid facedetections frame checkpoint record "
                                "at byte %1.")
                     .arg(recordOffset);
      }
      return false;
    }
    if (frameOut) {
      *frameOut = frame;
    }
    return true;
  }
  if (error) {
    *error = QStringLiteral("Unknown facedetections checkpoint record type %1 "
                            "at byte %2.")
                 .arg(recordType)
                 .arg(recordOffset);
  }
  return false;
}
