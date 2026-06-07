#include "vulkan_facedetections_offscreen_resume_state.h"

#include "json_io_utils.h"

#include <QDataStream>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

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
    recentDetections.append(track.detections.last());
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
    track.detections =
        object.value(QStringLiteral("recent_detections")).toArray();
    if (track.id >= 0 && track.box.isValid() && !track.box.isEmpty()) {
      tracks.push_back(track);
    }
  }
  return tracks;
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
  QDataStream stream(&file);
  stream.setVersion(QDataStream::Qt_6_0);
  while (!stream.atEnd()) {
    quint32 magic = 0;
    quint32 version = 0;
    quint32 compressedSize = 0;
    stream >> magic;
    stream >> version;
    stream >> compressedSize;
    if (stream.status() != QDataStream::Ok) {
      break;
    }
    if (magic != 0x4A465342 || version != 1) {
      if (error) {
        *error =
            QStringLiteral("Invalid facedetections checkpoint record header.");
      }
      return false;
    }
    QByteArray compressed;
    compressed.resize(static_cast<int>(compressedSize));
    if (compressedSize > 0) {
      const int bytesRead = stream.readRawData(
          compressed.data(), static_cast<int>(compressedSize));
      if (bytesRead != static_cast<int>(compressedSize)) {
        if (error) {
          *error =
              QStringLiteral("Truncated facedetections checkpoint record.");
        }
        return false;
      }
    }
    QJsonObject object;
    if (!jcut::jsonio::parseCborRecordPayload(qUncompress(compressed), &object,
                                              nullptr)) {
      continue;
    }
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("meta")) {
      sawMeta = true;
      if (object.value(QStringLiteral("video")).toString() != videoPath ||
          object.value(QStringLiteral("backend")).toString() != backend) {
        if (error) {
          *error = QStringLiteral("Existing facedetections checkpoint is for a "
                                  "different video/backend.");
        }
        return false;
      }
      continue;
    }
    if (type != QStringLiteral("frame")) {
      continue;
    }
    if (sawMeta && (object.value(QStringLiteral("video")).toString(videoPath) !=
                        videoPath ||
                    object.value(QStringLiteral("backend")).toString(backend) !=
                        backend)) {
      continue;
    }
    const int frameNumber = object.value(QStringLiteral("frame")).toInt(-1);
    if (frameNumber < startFrame || frameNumber > endFrame ||
        state->completedFrames.contains(frameNumber)) {
      continue;
    }
    state->completedFrames.insert(frameNumber);
    ++state->processed;
    const int detectionCount =
        object.value(QStringLiteral("detections")).toInt(0);
    state->totalDetections += detectionCount;
    if (object.value(QStringLiteral("app_vulkan_frame_path")).toBool(false))
      ++state->appVulkanFramePathFrames;
    if (object.value(QStringLiteral("decoder_direct_handoff")).toBool(false))
      ++state->decoderDirectHandoffFrames;
    if (object.value(QStringLiteral("decoder_vulkan_upload_fallback"))
            .toBool(false))
      ++state->decoderVulkanUploadFallbackFrames;
    if (object.value(QStringLiteral("hardware_direct_handoff")).toBool(false))
      ++state->hardwareDirectHandoffFrames;
    if (object.value(QStringLiteral("hardware_interop_probe_supported"))
            .toBool(false))
      ++state->hardwareInteropProbeSupportedFrames;
    if (object.value(QStringLiteral("hardware_interop_probe_failed"))
            .toBool(false))
      ++state->hardwareInteropProbeFailedFrames;
    if (object.value(QStringLiteral("hardware_frame")).toBool(false))
      ++state->hardwareFrames;
    if (object.value(QStringLiteral("cpu_frame")).toBool(false))
      ++state->cpuFrames;
    state->renderDecodeMsTotal +=
        object.value(QStringLiteral("app_render_decode_ms")).toDouble(0.0);
    state->renderCompositeMsTotal +=
        object.value(QStringLiteral("app_render_composite_ms")).toDouble(0.0);
    state->renderReadbackMsTotal +=
        object.value(QStringLiteral("app_render_readback_ms")).toDouble(0.0);
    state->decoderUploadMsTotal +=
        object.value(QStringLiteral("decoder_vulkan_upload_ms")).toDouble(0.0);
    state->vulkanDetectMsTotal +=
        object.value(QStringLiteral("vulkan_zero_copy_detection_ms"))
            .toDouble(0.0);

    state->frameRows.append(QJsonObject{
        {QStringLiteral("frame"), frameNumber},
        {QStringLiteral("detector"),
         object.value(QStringLiteral("detector")).toString(backend)},
        {QStringLiteral("detections"), detectionCount},
        {QStringLiteral("tracks"),
         object.value(QStringLiteral("tracks")).toInt(0)},
        {QStringLiteral("app_vulkan_frame_path"),
         object.value(QStringLiteral("app_vulkan_frame_path")).toBool(false)},
        {QStringLiteral("app_render_decode_ms"),
         object.value(QStringLiteral("app_render_decode_ms")).toDouble(0.0)},
        {QStringLiteral("app_render_texture_ms"),
         object.value(QStringLiteral("app_render_texture_ms")).toDouble(0.0)},
        {QStringLiteral("app_render_composite_ms"),
         object.value(QStringLiteral("app_render_composite_ms")).toDouble(0.0)},
        {QStringLiteral("app_render_readback_ms"),
         object.value(QStringLiteral("app_render_readback_ms")).toDouble(0.0)},
        {QStringLiteral("vulkan_zero_copy_detection_ms"),
         object.value(QStringLiteral("vulkan_zero_copy_detection_ms"))
             .toDouble(0.0)},
        {QStringLiteral("decoder_vulkan_upload_ms"),
         object.value(QStringLiteral("decoder_vulkan_upload_ms"))
             .toDouble(0.0)},
        {QStringLiteral("decoder_vulkan_upload_fallback"),
         object.value(QStringLiteral("decoder_vulkan_upload_fallback"))
             .toBool(false)},
        {QStringLiteral("hardware_direct_handoff"),
         object.value(QStringLiteral("hardware_direct_handoff")).toBool(false)},
        {QStringLiteral("hardware_direct_attempt_reason"),
         object.value(QStringLiteral("hardware_direct_attempt_reason"))
             .toString()},
        {QStringLiteral("qimage_materialized"),
         object.value(QStringLiteral("qimage_materialized")).toBool(false)}});
    const QJsonArray detectionBoxes =
        object.value(QStringLiteral("detection_boxes")).toArray();
    const QJsonObject firstDetection = detectionBoxes.isEmpty()
                                           ? QJsonObject{}
                                           : detectionBoxes.at(0).toObject();
    state->rawDetectionFrames.append(QJsonObject{
        {QStringLiteral("frame"), frameNumber},
        {QStringLiteral("detector"),
         object.value(QStringLiteral("detector")).toString(backend)},
        {QStringLiteral("frame_width"),
         firstDetection.value(QStringLiteral("frame_width")).toInt()},
        {QStringLiteral("frame_height"),
         firstDetection.value(QStringLiteral("frame_height")).toInt()},
        {QStringLiteral("detection_count"), detectionCount},
        {QStringLiteral("detections"), detectionBoxes}});
    state->trackDetectionsByFrame.insert(
        frameNumber,
        object.value(QStringLiteral("track_detections")).toArray());
  }
  return true;
}
