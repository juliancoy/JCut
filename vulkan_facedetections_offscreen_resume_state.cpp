#include "vulkan_facedetections_offscreen_resume_state.h"

#include "json_io_utils.h"
#include "vulkan_facedetections_offscreen_artifact_io.h"

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <limits>

namespace {

constexpr quint32 kFaceDetectionsCheckpointMagic = 0x4A465354; // JFST
constexpr quint32 kFaceDetectionsCheckpointVersion = 2;

QString checkpointMagicString(quint32 magic) {
  return QStringLiteral("0x%1").arg(magic, 8, 16, QChar('0'));
}

QString checkpointRepairTimestamp() {
  return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
}

QString uniqueSiblingPath(const QString &path, const QString &suffix) {
  const QFileInfo info(path);
  const QDir dir = info.dir();
  const QString base = info.fileName() + suffix;
  QString candidate = dir.absoluteFilePath(base);
  for (int i = 1; QFileInfo::exists(candidate); ++i) {
    candidate = dir.absoluteFilePath(QStringLiteral("%1.%2").arg(base).arg(i));
  }
  return candidate;
}

bool writeRepairReport(const QString &reportPath, const QJsonObject &report,
                       QString *error) {
  return jcut::jsonio::writeJsonFile(reportPath, report, true, error);
}

bool readCheckpointMagic(const QString &path, quint32 *magic, QString *error) {
  if (magic) {
    *magic = 0;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    if (error) {
      *error = QStringLiteral("Failed to open facedetections checkpoint: %1")
                   .arg(path);
    }
    return false;
  }
  QDataStream stream(&file);
  stream.setVersion(QDataStream::Qt_6_0);
  quint32 value = 0;
  stream >> value;
  if (stream.status() != QDataStream::Ok) {
    if (error) {
      *error = QStringLiteral("Failed to read facedetections checkpoint header: "
                              "%1")
                   .arg(path);
    }
    return false;
  }
  if (magic) {
    *magic = value;
  }
  return true;
}

} // namespace

QString faceDetectionsResumeIndexPath(const QString &faceStreamPath) {
  return faceStreamPath + QStringLiteral(".resume_index.json");
}

QString continuityTrackStateToResumeString(
    jcut::facedetections::ContinuityTrackState state) {
  switch (state) {
  case jcut::facedetections::ContinuityTrackState::Confirmed:
    return QStringLiteral("confirmed");
  case jcut::facedetections::ContinuityTrackState::Lost:
    return QStringLiteral("lost");
  case jcut::facedetections::ContinuityTrackState::Removed:
    return QStringLiteral("removed");
  case jcut::facedetections::ContinuityTrackState::Tentative:
  default:
    return QStringLiteral("tentative");
  }
}

jcut::facedetections::ContinuityTrackState
continuityTrackStateFromResumeString(const QString &value) {
  if (value == QStringLiteral("confirmed")) {
    return jcut::facedetections::ContinuityTrackState::Confirmed;
  }
  if (value == QStringLiteral("lost")) {
    return jcut::facedetections::ContinuityTrackState::Lost;
  }
  if (value == QStringLiteral("removed")) {
    return jcut::facedetections::ContinuityTrackState::Removed;
  }
  return jcut::facedetections::ContinuityTrackState::Tentative;
}

QJsonArray completedFrameRangesToJson(const QSet<int> &frames) {
  QList<int> sorted = frames.values();
  std::sort(sorted.begin(), sorted.end());
  QJsonArray ranges;
  if (sorted.isEmpty()) {
    return ranges;
  }
  int start = sorted.first();
  int previous = start;
  for (int i = 1; i < sorted.size(); ++i) {
    const int frame = sorted.at(i);
    if (frame == previous + 1) {
      previous = frame;
      continue;
    }
    ranges.append(QJsonArray{start, previous});
    start = previous = frame;
  }
  ranges.append(QJsonArray{start, previous});
  return ranges;
}

QSet<int> completedFrameRangesFromJson(const QJsonArray &ranges) {
  QSet<int> frames;
  for (const QJsonValue &value : ranges) {
    const QJsonArray range = value.toArray();
    if (range.size() < 2) {
      continue;
    }
    const int start = range.at(0).toInt(-1);
    const int end = range.at(1).toInt(-1);
    if (start < 0 || end < start) {
      continue;
    }
    for (int frame = start; frame <= end; ++frame) {
      frames.insert(frame);
    }
  }
  return frames;
}

QJsonObject continuityTrackToResumeJson(const Track &track) {
  QJsonArray recentDetections;
  if (!track.detections.isEmpty()) {
    recentDetections.append(
        jcut::facedetections::trackDetectionToJson(track.detections.last()));
  }
  return QJsonObject{
      {QStringLiteral("id"), track.id},
      {QStringLiteral("box_x"), track.box.x()},
      {QStringLiteral("box_y"), track.box.y()},
      {QStringLiteral("box_w"), track.box.width()},
      {QStringLiteral("box_h"), track.box.height()},
      {QStringLiteral("predicted_x"), track.predictedBox.x()},
      {QStringLiteral("predicted_y"), track.predictedBox.y()},
      {QStringLiteral("predicted_w"), track.predictedBox.width()},
      {QStringLiteral("predicted_h"), track.predictedBox.height()},
      {QStringLiteral("center_vx"), track.centerVelocity.x()},
      {QStringLiteral("center_vy"), track.centerVelocity.y()},
      {QStringLiteral("size_vw"), track.sizeVelocity.width()},
      {QStringLiteral("size_vh"), track.sizeVelocity.height()},
      {QStringLiteral("first_frame"), track.firstFrame},
      {QStringLiteral("last_frame"), track.lastFrame},
      {QStringLiteral("hits"), track.hits},
      {QStringLiteral("misses"), track.misses},
      {QStringLiteral("state"),
       continuityTrackStateToResumeString(track.state)},
      {QStringLiteral("recent_detections"), recentDetections}};
}

QVector<Track> continuityTracksFromResumeJson(const QJsonArray &rows) {
  QVector<Track> tracks;
  tracks.reserve(rows.size());
  for (const QJsonValue &value : rows) {
    const QJsonObject object = value.toObject();
    Track track;
    track.id = object.value(QStringLiteral("id")).toInt(-1);
    track.box = QRectF(object.value(QStringLiteral("box_x")).toDouble(),
                       object.value(QStringLiteral("box_y")).toDouble(),
                       object.value(QStringLiteral("box_w")).toDouble(),
                       object.value(QStringLiteral("box_h")).toDouble());
    track.predictedBox = QRectF(
        object.value(QStringLiteral("predicted_x")).toDouble(track.box.x()),
        object.value(QStringLiteral("predicted_y")).toDouble(track.box.y()),
        object.value(QStringLiteral("predicted_w")).toDouble(track.box.width()),
        object.value(QStringLiteral("predicted_h"))
            .toDouble(track.box.height()));
    track.centerVelocity =
        QPointF(object.value(QStringLiteral("center_vx")).toDouble(),
                object.value(QStringLiteral("center_vy")).toDouble());
    track.sizeVelocity =
        QSizeF(object.value(QStringLiteral("size_vw")).toDouble(),
               object.value(QStringLiteral("size_vh")).toDouble());
    track.firstFrame = object.value(QStringLiteral("first_frame")).toInt(-1);
    track.lastFrame = object.value(QStringLiteral("last_frame")).toInt(-1);
    track.hits = object.value(QStringLiteral("hits")).toInt(0);
    track.misses = object.value(QStringLiteral("misses")).toInt(0);
    track.state = continuityTrackStateFromResumeString(
        object.value(QStringLiteral("state")).toString());
    const QJsonArray recentDetections =
        object.value(QStringLiteral("recent_detections")).toArray();
    track.detections.reserve(recentDetections.size());
    for (const QJsonValue &detectionValue : recentDetections) {
      track.detections.push_back(jcut::facedetections::trackDetectionFromJson(
          detectionValue.toObject()));
    }
    if (track.id >= 0 && track.box.isValid() && !track.box.isEmpty()) {
      tracks.push_back(track);
    }
  }
  return tracks;
}

void applyFaceStreamFrameRecord(const FaceStreamFrameRecord &frame,
                                const QString &backend,
                                int startFrame,
                                int endFrame,
                                FaceDetectionsResumeState *state) {
  if (!state || frame.frame < 0) {
    return;
  }
  const int frameNumber = frame.frame;
  if (frameNumber < startFrame || frameNumber > endFrame ||
      state->completedFrames.contains(frameNumber)) {
    return;
  }
  state->completedFrames.insert(frameNumber);
  ++state->processed;
  const int detectionCount = frame.detections.size();
  state->totalDetections += detectionCount;
  if (frame.appVulkanFramePath)
    ++state->appVulkanFramePathFrames;
  if (frame.decoderDirectHandoff)
    ++state->decoderDirectHandoffFrames;
  if (frame.decoderVulkanUploadFallback)
    ++state->decoderVulkanUploadFallbackFrames;
  if (frame.hardwareDirectHandoff)
    ++state->hardwareDirectHandoffFrames;
  if (frame.hardwareInteropProbeSupported)
    ++state->hardwareInteropProbeSupportedFrames;
  if (frame.hardwareInteropProbeFailed)
    ++state->hardwareInteropProbeFailedFrames;
  if (frame.hardwareFrame)
    ++state->hardwareFrames;
  if (frame.cpuFrame)
    ++state->cpuFrames;
  state->renderDecodeMsTotal += frame.appRenderDecodeMs;
  state->renderCompositeMsTotal += frame.appRenderCompositeMs;
  state->renderReadbackMsTotal += frame.appRenderReadbackMs;
  state->decoderUploadMsTotal += frame.decoderVulkanUploadMs;
  state->vulkanDetectMsTotal += frame.vulkanZeroCopyDetectionMs;
  state->ncnnInputMsTotal += frame.ncnnInputMs;
  state->ncnnExtractMsTotal += frame.ncnnExtractMs;
  state->ncnnExtractLevel8MsTotal += frame.ncnnExtractLevel8Ms;
  state->ncnnExtractLevel16MsTotal += frame.ncnnExtractLevel16Ms;
  state->ncnnExtractLevel32MsTotal += frame.ncnnExtractLevel32Ms;
  state->ncnnPostMsTotal += frame.ncnnPostMs;
  state->ncnnTotalMsTotal += frame.ncnnTotalMs;

  state->frameRows.append(QJsonObject{
      {QStringLiteral("frame"), frameNumber},
      {QStringLiteral("detector"), frame.detector.isEmpty() ? backend
                                                            : frame.detector},
      {QStringLiteral("detections"), detectionCount},
      {QStringLiteral("tracks"), frame.trackDetections.size()},
      {QStringLiteral("app_vulkan_frame_path"), frame.appVulkanFramePath},
      {QStringLiteral("app_render_decode_ms"), frame.appRenderDecodeMs},
      {QStringLiteral("app_render_texture_ms"), frame.appRenderTextureMs},
      {QStringLiteral("app_render_composite_ms"), frame.appRenderCompositeMs},
      {QStringLiteral("app_render_readback_ms"), frame.appRenderReadbackMs},
      {QStringLiteral("vulkan_zero_copy_detection_ms"),
       frame.vulkanZeroCopyDetectionMs},
      {QStringLiteral("decoder_vulkan_upload_ms"),
       frame.decoderVulkanUploadMs},
      {QStringLiteral("decoder_vulkan_upload_fallback"),
       frame.decoderVulkanUploadFallback},
      {QStringLiteral("hardware_direct_handoff"),
       frame.hardwareDirectHandoff},
      {QStringLiteral("hardware_direct_attempt_reason"),
       frame.hardwareDirectAttemptReason},
      {QStringLiteral("qimage_materialized"), frame.qimageMaterialized}});
  QJsonArray detectionBoxes;
  for (const auto &detection : frame.detections) {
    detectionBoxes.append(
        jcut::facedetections::compactDetectionJson(detection, frame.frameSize));
  }
  state->rawDetectionFrames.append(QJsonObject{
      {QStringLiteral("frame"), frameNumber},
      {QStringLiteral("detector"), frame.detector.isEmpty() ? backend
                                                            : frame.detector},
      {QStringLiteral("frame_width"), frame.frameSize.width()},
      {QStringLiteral("frame_height"), frame.frameSize.height()},
      {QStringLiteral("detection_count"), detectionCount},
      {QStringLiteral("detections"), detectionBoxes}});
  QJsonArray trackDetections;
  for (const auto &trackDetection : frame.trackDetections) {
    trackDetections.append(
        jcut::facedetections::frameTrackDetectionToJson(trackDetection));
  }
  state->trackDetectionsByFrame.insert(frameNumber, trackDetections);
}

bool saveFaceDetectionsResumeIndex(
    const QString &indexPath, const QString &faceStreamPath,
    const QString &videoPath, const QString &backend, int startFrame,
    int endFrame, const FaceDetectionsResumeState &state,
    const QVector<Track> &runtimeTracks, QString *error) {
  QFileInfo partInfo(faceStreamPath);
  QJsonArray trackRows;
  for (const Track &track : runtimeTracks) {
    if (track.state != jcut::facedetections::ContinuityTrackState::Removed) {
      trackRows.append(continuityTrackToResumeJson(track));
    }
  }
  const QJsonObject root{
      {QStringLiteral("schema"),
       QStringLiteral("jcut_facedetections_resume_index_v1")},
      {QStringLiteral("video"), videoPath},
      {QStringLiteral("backend"), backend},
      {QStringLiteral("start_frame"), startFrame},
      {QStringLiteral("end_frame"), endFrame},
      {QStringLiteral("part_path"),
       QFileInfo(faceStreamPath).absoluteFilePath()},
      {QStringLiteral("part_size"),
       static_cast<qint64>(partInfo.exists() ? partInfo.size() : 0)},
      {QStringLiteral("part_mtime_ms"),
       partInfo.exists() ? partInfo.lastModified().toMSecsSinceEpoch() : 0},
      {QStringLiteral("completed_ranges"),
       completedFrameRangesToJson(state.completedFrames)},
      {QStringLiteral("processed"), state.processed},
      {QStringLiteral("total_detections"), state.totalDetections},
      {QStringLiteral("app_vulkan_frame_path_frames"),
       state.appVulkanFramePathFrames},
      {QStringLiteral("decoder_direct_handoff_frames"),
       state.decoderDirectHandoffFrames},
      {QStringLiteral("decoder_vulkan_upload_fallback_frames"),
       state.decoderVulkanUploadFallbackFrames},
      {QStringLiteral("hardware_direct_handoff_frames"),
       state.hardwareDirectHandoffFrames},
      {QStringLiteral("hardware_interop_probe_supported_frames"),
       state.hardwareInteropProbeSupportedFrames},
      {QStringLiteral("hardware_interop_probe_failed_frames"),
       state.hardwareInteropProbeFailedFrames},
      {QStringLiteral("hardware_frames"), state.hardwareFrames},
      {QStringLiteral("cpu_frames"), state.cpuFrames},
      {QStringLiteral("render_decode_ms_total"), state.renderDecodeMsTotal},
      {QStringLiteral("render_composite_ms_total"),
       state.renderCompositeMsTotal},
      {QStringLiteral("render_readback_ms_total"), state.renderReadbackMsTotal},
      {QStringLiteral("decoder_upload_ms_total"), state.decoderUploadMsTotal},
      {QStringLiteral("vulkan_detect_ms_total"), state.vulkanDetectMsTotal},
      {QStringLiteral("ncnn_input_ms_total"), state.ncnnInputMsTotal},
      {QStringLiteral("ncnn_extract_ms_total"), state.ncnnExtractMsTotal},
      {QStringLiteral("ncnn_extract_level8_ms_total"),
       state.ncnnExtractLevel8MsTotal},
      {QStringLiteral("ncnn_extract_level16_ms_total"),
       state.ncnnExtractLevel16MsTotal},
      {QStringLiteral("ncnn_extract_level32_ms_total"),
       state.ncnnExtractLevel32MsTotal},
      {QStringLiteral("ncnn_post_ms_total"), state.ncnnPostMsTotal},
      {QStringLiteral("ncnn_total_ms_total"), state.ncnnTotalMsTotal},
      {QStringLiteral("runtime_tracks"), trackRows}};
  return jcut::jsonio::writeJsonFile(indexPath, root, false, error);
}

bool loadFaceDetectionsResumeIndex(
    const QString &indexPath, const QString &faceStreamPath,
    const QString &videoPath, const QString &backend, int startFrame,
    int endFrame, FaceDetectionsResumeState *state, QString *error) {
  if (!state || !QFileInfo::exists(indexPath)) {
    return false;
  }
  QJsonObject root;
  if (!jcut::jsonio::readJsonFile(indexPath, &root, error)) {
    return false;
  }
  if (root.value(QStringLiteral("schema")).toString() !=
      QStringLiteral("jcut_facedetections_resume_index_v1")) {
    if (error)
      *error = QStringLiteral("Resume index has an unknown schema.");
    return false;
  }
  const int indexedStartFrame =
      root.value(QStringLiteral("start_frame")).toInt(-1);
  const int indexedEndFrame = root.value(QStringLiteral("end_frame")).toInt(-1);
  if (root.value(QStringLiteral("video")).toString() != videoPath ||
      root.value(QStringLiteral("backend")).toString() != backend ||
      indexedStartFrame != startFrame || indexedEndFrame < startFrame ||
      indexedEndFrame > endFrame) {
    if (error)
      *error = QStringLiteral("Resume index is for a different run.");
    return false;
  }
  const QFileInfo partInfo(faceStreamPath);
  if (!partInfo.exists() ||
      partInfo.size() < root.value(QStringLiteral("part_size")).toInteger(0)) {
    if (error)
      *error =
          QStringLiteral("Resume index is newer than facedetections.part.");
    return false;
  }
  if (partInfo.size() > 0) {
    quint32 partMagic = 0;
    QString magicError;
    if (!readCheckpointMagic(faceStreamPath, &partMagic, &magicError)) {
      if (error)
        *error = magicError;
      return false;
    }
    if (partMagic != kFaceDetectionsCheckpointMagic) {
      if (error) {
        *error = QStringLiteral(
                     "Resume index points to an invalid facedetections.part "
                     "header in %1: expected typed JFST/%2, found %3. "
                     "Delete old face-detection artifacts and regenerate.")
                     .arg(faceStreamPath)
                     .arg(checkpointMagicString(kFaceDetectionsCheckpointMagic))
                     .arg(checkpointMagicString(partMagic));
      }
      return false;
    }
  }
  state->completedFrames = completedFrameRangesFromJson(
      root.value(QStringLiteral("completed_ranges")).toArray());
  for (auto it = state->completedFrames.begin();
       it != state->completedFrames.end();) {
    if (*it < startFrame || *it > endFrame) {
      it = state->completedFrames.erase(it);
    } else {
      ++it;
    }
  }
  state->processed = root.value(QStringLiteral("processed"))
                         .toInt(state->completedFrames.size());
  state->totalDetections =
      root.value(QStringLiteral("total_detections")).toInt(0);
  state->appVulkanFramePathFrames =
      root.value(QStringLiteral("app_vulkan_frame_path_frames")).toInt(0);
  state->decoderDirectHandoffFrames =
      root.value(QStringLiteral("decoder_direct_handoff_frames")).toInt(0);
  state->decoderVulkanUploadFallbackFrames =
      root.value(QStringLiteral("decoder_vulkan_upload_fallback_frames"))
          .toInt(0);
  state->hardwareDirectHandoffFrames =
      root.value(QStringLiteral("hardware_direct_handoff_frames")).toInt(0);
  state->hardwareInteropProbeSupportedFrames =
      root.value(QStringLiteral("hardware_interop_probe_supported_frames"))
          .toInt(0);
  state->hardwareInteropProbeFailedFrames =
      root.value(QStringLiteral("hardware_interop_probe_failed_frames"))
          .toInt(0);
  state->hardwareFrames =
      root.value(QStringLiteral("hardware_frames")).toInt(0);
  state->cpuFrames = root.value(QStringLiteral("cpu_frames")).toInt(0);
  state->renderDecodeMsTotal =
      root.value(QStringLiteral("render_decode_ms_total")).toDouble(0.0);
  state->renderCompositeMsTotal =
      root.value(QStringLiteral("render_composite_ms_total")).toDouble(0.0);
  state->renderReadbackMsTotal =
      root.value(QStringLiteral("render_readback_ms_total")).toDouble(0.0);
  state->decoderUploadMsTotal =
      root.value(QStringLiteral("decoder_upload_ms_total")).toDouble(0.0);
  state->vulkanDetectMsTotal =
      root.value(QStringLiteral("vulkan_detect_ms_total")).toDouble(0.0);
  state->ncnnInputMsTotal =
      root.value(QStringLiteral("ncnn_input_ms_total")).toDouble(0.0);
  state->ncnnExtractMsTotal =
      root.value(QStringLiteral("ncnn_extract_ms_total")).toDouble(0.0);
  state->ncnnExtractLevel8MsTotal =
      root.value(QStringLiteral("ncnn_extract_level8_ms_total")).toDouble(0.0);
  state->ncnnExtractLevel16MsTotal =
      root.value(QStringLiteral("ncnn_extract_level16_ms_total")).toDouble(0.0);
  state->ncnnExtractLevel32MsTotal =
      root.value(QStringLiteral("ncnn_extract_level32_ms_total")).toDouble(0.0);
  state->ncnnPostMsTotal =
      root.value(QStringLiteral("ncnn_post_ms_total")).toDouble(0.0);
  state->ncnnTotalMsTotal =
      root.value(QStringLiteral("ncnn_total_ms_total")).toDouble(0.0);
  state->runtimeTracks = continuityTracksFromResumeJson(
      root.value(QStringLiteral("runtime_tracks")).toArray());
  state->loadedFromCompactIndex = true;
  state->fullPayloadDeferred = true;
  return true;
}

bool loadFaceDetectionsResume(const QString &path, const QString &videoPath,
                              const QString &backend, int startFrame,
                              int endFrame, FaceDetectionsResumeState *state,
                              QString *error) {
  if (!state || !QFileInfo::exists(path)) {
    return true;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    if (error)
      *error = QStringLiteral(
                   "Failed to open existing facedetections checkpoint: %1")
                   .arg(path);
    return false;
  }
  bool sawMeta = false;
  while (!file.atEnd()) {
    FaceStreamMetaRecord meta;
    FaceStreamFrameRecord frame;
    QString recordError;
    if (!readFaceStreamRecord(&file, &meta, &frame, &recordError)) {
      if (error) {
        *error = recordError.isEmpty()
                     ? QStringLiteral("Failed to read typed facedetections "
                                      "checkpoint record from %1.")
                           .arg(path)
                     : recordError;
      }
      return false;
    }
    if (!meta.video.isEmpty() || !meta.backend.isEmpty()) {
      sawMeta = true;
      if (meta.video != videoPath || meta.backend != backend) {
        if (error) {
          *error = QStringLiteral("Existing facedetections checkpoint is for a "
                                  "different video/backend.");
        }
        return false;
      }
      continue;
    }
    if (frame.frame < 0) {
      continue;
    }
    if (sawMeta && (frame.video != videoPath || frame.backend != backend)) {
      continue;
    }
    applyFaceStreamFrameRecord(frame, backend, startFrame, endFrame, state);
  }
  return true;
}

bool repairFaceDetectionsResumeCheckpoint(const QString &path,
                                          const QString &videoPath,
                                          const QString &backend,
                                          int startFrame,
                                          int endFrame,
                                          FaceDetectionsResumeState *state,
                                          QString *message) {
  if (!state || !QFileInfo::exists(path)) {
    return true;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    if (message) {
      *message = QStringLiteral(
                     "Failed to open facedetections checkpoint for repair: %1")
                     .arg(path);
    }
    return false;
  }
  FaceDetectionsResumeState repaired;
  bool sawMeta = false;
  qint64 lastGoodOffset = file.pos();
  QString recordError;
  while (!file.atEnd()) {
    const qint64 recordOffset = file.pos();
    FaceStreamMetaRecord meta;
    FaceStreamFrameRecord frame;
    recordError.clear();
    if (!readFaceStreamRecord(&file, &meta, &frame, &recordError)) {
      break;
    }
    if (!meta.video.isEmpty() || !meta.backend.isEmpty()) {
      sawMeta = true;
      if (meta.video != videoPath || meta.backend != backend) {
        if (message) {
          *message = QStringLiteral("Existing facedetections checkpoint is for a "
                                    "different video/backend.");
        }
        return false;
      }
      lastGoodOffset = file.pos();
      continue;
    }
    if (frame.frame >= 0 &&
        (!sawMeta || (frame.video == videoPath && frame.backend == backend))) {
      applyFaceStreamFrameRecord(frame, backend, startFrame, endFrame,
                                 &repaired);
    }
    lastGoodOffset = file.pos();
    Q_UNUSED(recordOffset);
  }
  const bool foundCorruption = !recordError.isEmpty();
  const qint64 originalSize = QFileInfo(path).size();
  file.close();
  if (foundCorruption && lastGoodOffset < originalSize) {
    const QString timestamp = checkpointRepairTimestamp();
    const QString backupPath =
        uniqueSiblingPath(path, QStringLiteral(".corrupt_%1.bak").arg(timestamp));
    const QString repairReportPath =
        uniqueSiblingPath(path, QStringLiteral(".repair_%1.json").arg(timestamp));
    QString backupError;
    bool copiedBackup = QFile::copy(path, backupPath);
    if (!copiedBackup) {
      backupError =
          QStringLiteral("Failed to copy corrupt checkpoint backup to %1")
              .arg(backupPath);
    }
    QString reportError;
    const QJsonObject repairReport{
        {QStringLiteral("schema"),
         QStringLiteral("jcut_facedetections_checkpoint_repair_v1")},
        {QStringLiteral("repaired_at_utc"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("checkpoint_path"), path},
        {QStringLiteral("backup_path"), copiedBackup ? backupPath : QString()},
        {QStringLiteral("backup_created"), copiedBackup},
        {QStringLiteral("backup_error"), backupError},
        {QStringLiteral("original_size_bytes"), originalSize},
        {QStringLiteral("valid_prefix_bytes"), lastGoodOffset},
        {QStringLiteral("discarded_from_byte"), lastGoodOffset},
        {QStringLiteral("discarded_bytes"), originalSize - lastGoodOffset},
        {QStringLiteral("processed_frames_recovered"), repaired.processed},
        {QStringLiteral("total_detections_recovered"), repaired.totalDetections},
        {QStringLiteral("video"), videoPath},
        {QStringLiteral("backend"), backend},
        {QStringLiteral("record_error"), recordError}};
    if (!writeRepairReport(repairReportPath, repairReport, &reportError)) {
      if (message) {
        *message = QStringLiteral(
                       "Failed to write facedetections checkpoint repair report "
                       "before truncating corrupt checkpoint: %1")
                       .arg(reportError);
      }
      return false;
    }
    if (!copiedBackup) {
      if (message) {
        *message =
            QStringLiteral("Refusing to discard corrupt facedetections checkpoint "
                           "tail because backup creation failed: %1")
                .arg(backupError);
      }
      return false;
    }
    QFile writable(path);
    if (!writable.open(QIODevice::ReadWrite) || !writable.resize(lastGoodOffset)) {
      if (message) {
        *message =
            QStringLiteral("Failed to truncate corrupt facedetections checkpoint "
                           "at byte %1: %2")
                .arg(lastGoodOffset)
                .arg(path);
      }
      return false;
    }
  }
  repaired.loadedFromCompactIndex = false;
  repaired.fullPayloadDeferred = false;
  *state = repaired;
  if (message) {
    if (foundCorruption) {
      *message = QStringLiteral(
                     "Recovered facedetections checkpoint through byte %1; "
                     "discarded corrupt tail from byte %2. %3")
                     .arg(lastGoodOffset)
                     .arg(lastGoodOffset)
                     .arg(recordError);
    } else {
      *message = QStringLiteral("Validated facedetections checkpoint payload.");
    }
  }
  return true;
}
