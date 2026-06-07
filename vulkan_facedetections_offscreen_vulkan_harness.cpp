#include "vulkan_facedetections_offscreen_vulkan_harness.h"

#include "detector_settings.h"
#include "render_internal.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRectF>

#include <algorithm>
QString findRes10NcnnModelFile(const QString &explicitPath,
                               const QString &fileName) {
  if (!explicitPath.isEmpty()) {
    return explicitPath;
  }
  const QStringList roots{QDir::currentPath(),
                          QCoreApplication::applicationDirPath(),
                          QDir(QCoreApplication::applicationDirPath())
                              .absoluteFilePath(QStringLiteral(".."))};
  const QStringList rels{
      QStringLiteral("assets/models/%1").arg(fileName),
      QStringLiteral("testbench_assets/models/%1").arg(fileName),
      QStringLiteral("models/%1").arg(fileName)};
  for (const QString &root : roots) {
    for (const QString &rel : rels) {
      const QString candidate = QDir(root).absoluteFilePath(rel);
      if (QFileInfo::exists(candidate)) {
        return candidate;
      }
    }
  }
  return QDir::current().absoluteFilePath(
      QStringLiteral("assets/models/%1").arg(fileName));
}

QString scrfdModelFileName(const QString &variantId, const QString &suffix) {
  jcut::facedetections::ScrfdModelVariantDefinition definition;
  if (jcut::facedetections::scrfdModelVariantById(variantId, &definition)) {
    return definition.modelStem + suffix;
  }
  return QStringLiteral("scrfd_500m-opt2") + suffix;
}

uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits,
                        VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties memProps{};
  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
  for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) &&
        (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }
  return UINT32_MAX;
}

QVector<Detection> readDetections(VulkanHarnessContext &vk, int imageWidth,
                                  int imageHeight) {
  QVector<Detection> out;
  void *mapped = nullptr;
  if (vkMapMemory(vk.device, vk.detectionMemory, 0, vk.detectionSize, 0,
                  &mapped) != VK_SUCCESS) {
    return out;
  }
  const auto *bytes = static_cast<const unsigned char *>(mapped);
  const uint32_t count =
      qMin<uint32_t>(*reinterpret_cast<const uint32_t *>(bytes),
                     static_cast<uint32_t>(vk.maxDetections));
  const float *det = reinterpret_cast<const float *>(bytes + 16);
  out.reserve(static_cast<int>(count));
  for (uint32_t i = 0; i < count; ++i) {
    const float x = det[i * 8 + 0];
    const float y = det[i * 8 + 1];
    const float w = det[i * 8 + 2];
    const float h = det[i * 8 + 3];
    const float c = det[i * 8 + 4];
    QRectF box(x * imageWidth, y * imageHeight, w * imageWidth,
               h * imageHeight);
    box = box.intersected(QRectF(0, 0, imageWidth, imageHeight));
    if (box.width() >= 8.0 && box.height() >= 8.0) {
      out.push_back({box, c});
    }
  }
  vkUnmapMemory(vk.device, vk.detectionMemory);
  std::sort(out.begin(), out.end(), [](const Detection &a, const Detection &b) {
    return a.confidence > b.confidence;
  });
  return out;
}

QVector<Detection> detectVulkanFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *detector,
    VulkanHarnessContext *vk, const render_detail::OffscreenVulkanFrame &frame,
    int maxDetections, float threshold, double *vulkanMs, QString *error) {
  QVector<Detection> out;
  if (!detector || !vk || !frame.valid) {
    if (error)
      *error = QStringLiteral("Invalid Vulkan detector frame.");
    return out;
  }
  if (!vk->attachExternalDevice({frame.physicalDevice, frame.device,
                                 frame.queue, frame.queueFamilyIndex},
                                error)) {
    return out;
  }
  if (!detector->isInitialized() &&
      !detector->initialize(vk->detectorContext(), error)) {
    return out;
  }
  if (!vk->ensureDetectorBuffers(detector->tensorSpec().byteSize(),
                                 maxDetections, error)) {
    return out;
  }

  QElapsedTimer timer;
  timer.start();
  const jcut::vulkan_detector::VulkanExternalImage source{
      frame.imageView, frame.imageLayout, frame.size, false};
  const jcut::vulkan_detector::VulkanTensorBuffer tensor{
      vk->tensorBuffer, detector->tensorSpec().byteSize()};
  if (!detector->preprocessToTensor(source, tensor, error)) {
    return out;
  }
  const jcut::vulkan_detector::VulkanTensorBuffer detectionBuffer{
      vk->detectionBuffer, vk->detectionSize};
  if (!detector->inferFromTensor(tensor, detectionBuffer, maxDetections,
                                 threshold, error)) {
    return out;
  }
  out = readDetections(*vk, frame.size.width(), frame.size.height());
  if (vulkanMs) {
    *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
  }
  return out;
}

QVector<Detection> detectRes10VulkanFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector *detector,
    VulkanHarnessContext *vk, const render_detail::OffscreenVulkanFrame &frame,
    const QString &paramPath, const QString &binPath, float threshold,
    bool smallFaceFallback, bool suppressNcnnInfo, double *vulkanMs,
    QString *error) {
  QVector<Detection> out;
  if (!preprocessor || !detector || !vk || !frame.valid) {
    if (error)
      *error = QStringLiteral("Invalid Vulkan Res10 detector frame.");
    return out;
  }
  if (!vk->attachExternalDevice({frame.physicalDevice, frame.device,
                                 frame.queue, frame.queueFamilyIndex},
                                error)) {
    return out;
  }
  if (!preprocessor->isInitialized() &&
      !preprocessor->initialize(vk->detectorContext(), error)) {
    return out;
  }
  if (!detector->isInitialized()) {
    ScopedStderrSilencer silence(suppressNcnnInfo);
    if (!detector->initialize(vk->detectorContext(), paramPath, binPath,
                              error)) {
      return out;
    }
  }
  if (!vk->ensureDetectorBuffers(preprocessor->tensorSpec().byteSize(), 1,
                                 error)) {
    return out;
  }

  QElapsedTimer timer;
  timer.start();
  const jcut::vulkan_detector::VulkanTensorBuffer tensor{
      vk->tensorBuffer, preprocessor->tensorSpec().byteSize()};
  auto runRoi = [&](const QRectF &roi, QVector<Detection> *dst) -> bool {
    const QRectF bounded = roi.intersected(QRectF(0.0, 0.0, 1.0, 1.0));
    if (bounded.width() <= 0.01 || bounded.height() <= 0.01) {
      return true;
    }
    const jcut::vulkan_detector::VulkanExternalImage source{
        frame.imageView,
        frame.imageLayout,
        frame.size,
        false,
        static_cast<float>(bounded.x()),
        static_cast<float>(bounded.y()),
        static_cast<float>(bounded.width()),
        static_cast<float>(bounded.height())};
    if (!preprocessor->preprocessToTensor(source, tensor, error)) {
      return false;
    }
    const int roiWidth = qMax(1, qRound(frame.size.width() * bounded.width()));
    const int roiHeight =
        qMax(1, qRound(frame.size.height() * bounded.height()));
    const QVector<jcut::vulkan_detector::Res10Detection> raw =
        detector->inferFromTensor(tensor, roiWidth, roiHeight, threshold,
                                  error);
    if (!preprocessor->finishPendingPreprocess(error)) {
      return false;
    }
    dst->reserve(dst->size() + raw.size());
    for (const auto &det : raw) {
      QRectF box(bounded.x() * frame.size.width() + det.box.x(),
                 bounded.y() * frame.size.height() + det.box.y(),
                 det.box.width(), det.box.height());
      dst->push_back({box, det.confidence});
    }
    return true;
  };
  if (!runRoi(QRectF(0.0, 0.0, 1.0, 1.0), &out)) {
    return out;
  }
  if (out.isEmpty() && smallFaceFallback) {
    static const QRectF rois[] = {
        QRectF(0.0, 0.0, 0.50, 1.0),   QRectF(0.25, 0.0, 0.50, 1.0),
        QRectF(0.50, 0.0, 0.50, 1.0),  QRectF(0.0, 0.0, 0.50, 0.65),
        QRectF(0.25, 0.0, 0.50, 0.65), QRectF(0.50, 0.0, 0.50, 0.65)};
    for (const QRectF &roi : rois) {
      if (!runRoi(roi, &out)) {
        return out;
      }
    }
  }
  if (vulkanMs) {
    *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
  }
  return out;
}

QVector<Detection> detectRes10FromDecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const editor::FrameHandle &frame, const QString &paramPath,
    const QString &binPath, float threshold, bool allowCpuUploadFallback,
    double *uploadMs, double *vulkanMs, bool *hardwareDirectUsed,
    QString *error) {
  QVector<Detection> out;
  if (!preprocessor || !detector || !vk || !handoff || frame.isNull()) {
    if (error)
      *error =
          QStringLiteral("Invalid decoder frame for Vulkan detector handoff.");
    return out;
  }
  if (!handoff->uploadFrame(frame, allowCpuUploadFallback, uploadMs, error)) {
    return out;
  }
  if (hardwareDirectUsed) {
    *hardwareDirectUsed =
        handoff->lastMode() ==
        jcut::vulkan_detector::FrameHandoffMode::HardwareDirect;
  }
  const jcut::vulkan_detector::VulkanExternalImage source =
      handoff->externalImage();
  if (source.imageView == VK_NULL_HANDLE || !source.size.isValid()) {
    if (error)
      *error = QStringLiteral("Frame handoff produced invalid external image.");
    return out;
  }
  render_detail::OffscreenVulkanFrame detectorFrame;
  detectorFrame.physicalDevice = vk->physicalDevice;
  detectorFrame.device = vk->device;
  detectorFrame.queue = vk->queue;
  detectorFrame.queueFamilyIndex = vk->queueFamilyIndex;
  detectorFrame.imageView = source.imageView;
  detectorFrame.imageLayout = source.imageLayout;
  detectorFrame.size = source.size;
  detectorFrame.queueSupportsCompute = true;
  detectorFrame.valid = true;
  out = detectRes10VulkanFrame(preprocessor, detector, vk, detectorFrame,
                               paramPath, binPath, threshold, false, false,
                               vulkanMs, error);
  if (!handoff->finishPendingUpload(nullptr, error)) {
    out.clear();
    return out;
  }
  return out;
}

QVector<Detection> detectScrfdVulkanFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector *detector,
    VulkanHarnessContext *vk, const render_detail::OffscreenVulkanFrame &frame,
    const QString &paramPath, const QString &binPath, float threshold,
    int targetSize, bool tiledPass, bool suppressNcnnInfo, double *vulkanMs,
    QString *error) {
  QVector<Detection> out;
  if (!preprocessor || !detector || !vk || !frame.valid) {
    if (error)
      *error = QStringLiteral("Invalid Vulkan SCRFD detector frame.");
    return out;
  }
  if (!vk->attachExternalDevice({frame.physicalDevice, frame.device,
                                 frame.queue, frame.queueFamilyIndex},
                                error)) {
    return out;
  }
  if (!preprocessor->isInitialized() &&
      !preprocessor->initialize(vk->detectorContext(), error)) {
    return out;
  }
  if (!detector->isInitialized()) {
    ScopedStderrSilencer silence(suppressNcnnInfo);
    if (!detector->initialize(vk->detectorContext(), paramPath, binPath,
                              error)) {
      return out;
    }
  }

  const int sourceWidth = qMax(1, frame.size.width());
  const int sourceHeight = qMax(1, frame.size.height());
  targetSize = qMax(320, targetSize);
  float scale = 1.0f;
  int resizedW = sourceWidth;
  int resizedH = sourceHeight;
  if (resizedW > resizedH) {
    scale = static_cast<float>(targetSize) / static_cast<float>(resizedW);
    resizedW = targetSize;
    resizedH = qMax(1, qRound(static_cast<float>(resizedH) * scale));
  } else {
    scale = static_cast<float>(targetSize) / static_cast<float>(resizedH);
    resizedH = targetSize;
    resizedW = qMax(1, qRound(static_cast<float>(resizedW) * scale));
  }
  const VkDeviceSize tensorBytes =
      static_cast<VkDeviceSize>(((resizedW + 31) / 32) * 32) *
      static_cast<VkDeviceSize>(((resizedH + 31) / 32) * 32) *
      static_cast<VkDeviceSize>(3 * sizeof(float));
  if (!vk->ensureDetectorBuffers(tensorBytes, 1, error)) {
    return out;
  }

  QElapsedTimer timer;
  timer.start();
  const jcut::vulkan_detector::VulkanTensorBuffer tensor{vk->tensorBuffer,
                                                         vk->tensorSize};
  auto appendPass = [&](const QRectF &roiNorm) -> bool {
    const QRectF bounded = roiNorm.intersected(QRectF(0.0, 0.0, 1.0, 1.0));
    if (bounded.width() <= 0.05 || bounded.height() <= 0.05) {
      return true;
    }
    const jcut::vulkan_detector::VulkanExternalImage source{
        frame.imageView,
        frame.imageLayout,
        frame.size,
        false,
        static_cast<float>(bounded.x()),
        static_cast<float>(bounded.y()),
        static_cast<float>(bounded.width()),
        static_cast<float>(bounded.height())};
    jcut::vulkan_detector::ScrfdTensorLayout layout;
    if (!preprocessor->preprocessScrfdToTensor(source, tensor, targetSize,
                                               &layout, error)) {
      return false;
    }
    const int roiWidth =
        qMax(1, qRound(static_cast<double>(sourceWidth) * bounded.width()));
    const int roiHeight =
        qMax(1, qRound(static_cast<double>(sourceHeight) * bounded.height()));
    const QVector<jcut::vulkan_detector::ScrfdDetection> raw =
        detector->inferFromTensor(tensor, layout, roiWidth, roiHeight,
                                  threshold, error);
    if (!preprocessor->finishPendingPreprocess(error)) {
      return false;
    }
    out.reserve(out.size() + raw.size());
    for (const auto &det : raw) {
      QRectF box(bounded.x() * sourceWidth + det.box.x(),
                 bounded.y() * sourceHeight + det.box.y(), det.box.width(),
                 det.box.height());
      out.push_back({box, det.confidence});
    }
    return true;
  };
  if (!appendPass(QRectF(0.0, 0.0, 1.0, 1.0))) {
    return out;
  }
  if (tiledPass) {
    constexpr qreal kTileSize = 0.60;
    constexpr qreal kTileStep = 0.40;
    static const QRectF tileRois[] = {
        QRectF(0.00, 0.00, kTileSize, kTileSize),
        QRectF(kTileStep, 0.00, kTileSize, kTileSize),
        QRectF(0.00, kTileStep, kTileSize, kTileSize),
        QRectF(kTileStep, kTileStep, kTileSize, kTileSize)};
    for (const QRectF &roi : tileRois) {
      if (!appendPass(roi)) {
        return out;
      }
    }
  }
  if (vulkanMs) {
    *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
  }
  return out;
}

QVector<Detection> detectScrfdFromDecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const editor::FrameHandle &frame, const QString &paramPath,
    const QString &binPath, float threshold, int targetSize, bool tiledPass,
    bool suppressNcnnInfo, bool allowCpuUploadFallback, double *uploadMs,
    double *vulkanMs, bool *hardwareDirectUsed, QString *error) {
  QVector<Detection> out;
  if (!preprocessor || !detector || !vk || !handoff || frame.isNull()) {
    if (error)
      *error =
          QStringLiteral("Invalid decoder frame for Vulkan SCRFD handoff.");
    return out;
  }
  if (!handoff->uploadFrame(frame, allowCpuUploadFallback, uploadMs, error)) {
    return out;
  }
  if (hardwareDirectUsed) {
    *hardwareDirectUsed =
        handoff->lastMode() ==
        jcut::vulkan_detector::FrameHandoffMode::HardwareDirect;
  }
  const jcut::vulkan_detector::VulkanExternalImage source =
      handoff->externalImage();
  if (source.imageView == VK_NULL_HANDLE || !source.size.isValid()) {
    if (error)
      *error = QStringLiteral("Frame handoff produced invalid external image.");
    return out;
  }
  render_detail::OffscreenVulkanFrame detectorFrame;
  detectorFrame.physicalDevice = vk->physicalDevice;
  detectorFrame.device = vk->device;
  detectorFrame.queue = vk->queue;
  detectorFrame.queueFamilyIndex = vk->queueFamilyIndex;
  detectorFrame.imageView = source.imageView;
  detectorFrame.imageLayout = source.imageLayout;
  detectorFrame.size = source.size;
  detectorFrame.queueSupportsCompute = true;
  detectorFrame.valid = true;
  out = detectScrfdVulkanFrame(preprocessor, detector, vk, detectorFrame,
                               paramPath, binPath, threshold, targetSize,
                               tiledPass, suppressNcnnInfo, vulkanMs, error);
  if (!handoff->finishPendingUpload(nullptr, error)) {
    out.clear();
    return out;
  }
  return out;
}

VkDeviceSize scrfdTensorBytesForSourceSize(const QSize &sourceSize,
                                           int targetSize) {
  const int sourceWidth = qMax(1, sourceSize.width());
  const int sourceHeight = qMax(1, sourceSize.height());
  targetSize = qMax(320, targetSize);
  int resizedW = sourceWidth;
  int resizedH = sourceHeight;
  if (resizedW > resizedH) {
    const float scale =
        static_cast<float>(targetSize) / static_cast<float>(resizedW);
    resizedW = targetSize;
    resizedH = qMax(1, qRound(static_cast<float>(resizedH) * scale));
  } else {
    const float scale =
        static_cast<float>(targetSize) / static_cast<float>(resizedH);
    resizedH = targetSize;
    resizedW = qMax(1, qRound(static_cast<float>(resizedW) * scale));
  }
  return static_cast<VkDeviceSize>(((resizedW + 31) / 32) * 32) *
         static_cast<VkDeviceSize>(((resizedH + 31) / 32) * 32) *
         static_cast<VkDeviceSize>(3 * sizeof(float));
}

bool prepareRes10DecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const editor::FrameHandle &frame, const QString &paramPath,
    const QString &binPath, bool suppressNcnnInfo, bool allowCpuUploadFallback,
    double *uploadMs, bool *hardwareDirectUsed, QSize *detectionFrameSize,
    QString *error) {
  if (!preprocessor || !detector || !vk || !handoff || frame.isNull()) {
    if (error)
      *error =
          QStringLiteral("Invalid decoder frame for Vulkan Res10 preparation.");
    return false;
  }
  if (!handoff->uploadFrame(frame, allowCpuUploadFallback, uploadMs, error)) {
    return false;
  }
  if (hardwareDirectUsed) {
    *hardwareDirectUsed =
        handoff->lastMode() ==
        jcut::vulkan_detector::FrameHandoffMode::HardwareDirect;
  }
  const jcut::vulkan_detector::VulkanExternalImage source =
      handoff->externalImage();
  if (source.imageView == VK_NULL_HANDLE || !source.size.isValid()) {
    if (error)
      *error = QStringLiteral("Frame handoff produced invalid external image.");
    return false;
  }
  if (!preprocessor->isInitialized() &&
      !preprocessor->initialize(vk->detectorContext(), error)) {
    return false;
  }
  if (!detector->isInitialized()) {
    ScopedStderrSilencer silence(suppressNcnnInfo);
    if (!detector->initialize(vk->detectorContext(), paramPath, binPath,
                              error)) {
      return false;
    }
  }
  if (!vk->ensureDetectorBuffers(preprocessor->tensorSpec().byteSize(), 1,
                                 error)) {
    return false;
  }
  const jcut::vulkan_detector::VulkanTensorBuffer tensor{
      vk->tensorBuffer, preprocessor->tensorSpec().byteSize()};
  if (!preprocessor->preprocessToTensor(source, tensor, error)) {
    return false;
  }
  if (detectionFrameSize) {
    *detectionFrameSize = source.size;
  }
  return true;
}

QVector<Detection> finalizePreparedRes10DecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const QSize &detectionFrameSize, float threshold, double *vulkanMs,
    QString *error) {
  QVector<Detection> out;
  if (!preprocessor || !detector || !vk || !handoff ||
      !detectionFrameSize.isValid()) {
    if (error)
      *error = QStringLiteral("Invalid prepared Vulkan Res10 decoder frame.");
    return out;
  }
  QElapsedTimer timer;
  timer.start();
  const jcut::vulkan_detector::VulkanTensorBuffer tensor{
      vk->tensorBuffer, preprocessor->tensorSpec().byteSize()};
  const QVector<jcut::vulkan_detector::Res10Detection> raw =
      detector->inferFromTensor(tensor, detectionFrameSize.width(),
                                detectionFrameSize.height(), threshold, error);
  if (!preprocessor->finishPendingPreprocess(error) ||
      !handoff->finishPendingUpload(nullptr, error)) {
    out.clear();
    return out;
  }
  out.reserve(raw.size());
  for (const auto &det : raw) {
    out.push_back({det.box, det.confidence});
  }
  if (vulkanMs) {
    *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
  }
  return out;
}

bool prepareScrfdDecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const editor::FrameHandle &frame, const QString &paramPath,
    const QString &binPath, int targetSize, bool suppressNcnnInfo,
    bool allowCpuUploadFallback,
    jcut::vulkan_detector::ScrfdTensorLayout *layout, double *uploadMs,
    bool *hardwareDirectUsed, QSize *detectionFrameSize, QString *error) {
  if (!preprocessor || !detector || !vk || !handoff || frame.isNull() ||
      !layout) {
    if (error)
      *error =
          QStringLiteral("Invalid decoder frame for Vulkan SCRFD preparation.");
    return false;
  }
  if (!handoff->uploadFrame(frame, allowCpuUploadFallback, uploadMs, error)) {
    return false;
  }
  if (hardwareDirectUsed) {
    *hardwareDirectUsed =
        handoff->lastMode() ==
        jcut::vulkan_detector::FrameHandoffMode::HardwareDirect;
  }
  const jcut::vulkan_detector::VulkanExternalImage source =
      handoff->externalImage();
  if (source.imageView == VK_NULL_HANDLE || !source.size.isValid()) {
    if (error)
      *error = QStringLiteral("Frame handoff produced invalid external image.");
    return false;
  }
  if (!preprocessor->isInitialized() &&
      !preprocessor->initialize(vk->detectorContext(), error)) {
    return false;
  }
  if (!detector->isInitialized()) {
    ScopedStderrSilencer silence(suppressNcnnInfo);
    if (!detector->initialize(vk->detectorContext(), paramPath, binPath,
                              error)) {
      return false;
    }
  }
  const VkDeviceSize tensorBytes =
      scrfdTensorBytesForSourceSize(source.size, targetSize);
  if (!vk->ensureDetectorBuffers(tensorBytes, 1, error)) {
    return false;
  }
  const jcut::vulkan_detector::VulkanTensorBuffer tensor{vk->tensorBuffer,
                                                         vk->tensorSize};
  if (!preprocessor->preprocessScrfdToTensor(source, tensor, targetSize, layout,
                                             error)) {
    return false;
  }
  if (detectionFrameSize) {
    *detectionFrameSize = source.size;
  }
  return true;
}

QVector<Detection> finalizePreparedScrfdDecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const jcut::vulkan_detector::ScrfdTensorLayout &layout,
    const QSize &detectionFrameSize, float threshold, double *vulkanMs,
    QString *error) {
  QVector<Detection> out;
  if (!preprocessor || !detector || !vk || !handoff ||
      !detectionFrameSize.isValid()) {
    if (error)
      *error = QStringLiteral("Invalid prepared Vulkan SCRFD decoder frame.");
    return out;
  }
  QElapsedTimer timer;
  timer.start();
  const jcut::vulkan_detector::VulkanTensorBuffer tensor{vk->tensorBuffer,
                                                         vk->tensorSize};
  const QVector<jcut::vulkan_detector::ScrfdDetection> raw =
      detector->inferFromTensor(tensor, layout, detectionFrameSize.width(),
                                detectionFrameSize.height(), threshold, error);
  if (!preprocessor->finishPendingPreprocess(error) ||
      !handoff->finishPendingUpload(nullptr, error)) {
    out.clear();
    return out;
  }
  out.reserve(raw.size());
  for (const auto &det : raw) {
    out.push_back({det.box, det.confidence});
  }
  if (vulkanMs) {
    *vulkanMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
  }
  return out;
}
