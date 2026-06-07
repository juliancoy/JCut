#pragma once

#include "editor_shared.h"
#include "frame_handle.h"
#include "vulkan_detector_frame_handoff.h"
#include "vulkan_facedetections_offscreen_detection_filters.h"
#include "vulkan_res10_ncnn_face_detector.h"
#include "vulkan_scrfd_ncnn_face_detector.h"
#include "vulkan_zero_copy_face_detector.h"

#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <future>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits,
                        VkMemoryPropertyFlags properties);

class ScopedStderrSilencer {
public:
  explicit ScopedStderrSilencer(bool enabled);
  ~ScopedStderrSilencer();

  ScopedStderrSilencer(const ScopedStderrSilencer &) = delete;
  ScopedStderrSilencer &operator=(const ScopedStderrSilencer &) = delete;

private:
  int m_savedFd = -1;
};

class VulkanHarnessContext {
public:
  ~VulkanHarnessContext();

  bool initialize(QString *error);
  bool attachExternalDevice(
      const jcut::vulkan_detector::VulkanDeviceContext &context,
      QString *error);
  void release();
  bool ensureDetectorBuffers(VkDeviceSize tensorBytesIn, int maxDetectionsIn,
                             QString *error);
  bool ensureResources(const QSize &size, VkDeviceSize tensorBytes,
                       int maxDetections, QString *error);
  bool uploadCpuImage(const QImage &imageIn, double *uploadMs, QString *error);
  jcut::vulkan_detector::VulkanDeviceContext detectorContext() const;
  bool createBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags properties, VkBuffer *buffer,
                    VkDeviceMemory *memory, VkDeviceSize *storedSize,
                    QString *error);
  void destroyBuffer(VkBuffer &buffer, VkDeviceMemory &memory);
  void transition(VkImageLayout oldLayout, VkImageLayout newLayout);

  VkInstance instance = VK_NULL_HANDLE;
  bool ownsInstanceAndDevice = true;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queueFamilyIndex = UINT32_MAX;
  VkCommandPool commandPool = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory imageMemory = VK_NULL_HANDLE;
  VkImageView imageView = VK_NULL_HANDLE;
  VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  QSize imageSize;
  VkBuffer stagingBuffer = VK_NULL_HANDLE;
  VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
  VkDeviceSize stagingSize = 0;
  VkBuffer tensorBuffer = VK_NULL_HANDLE;
  VkDeviceMemory tensorMemory = VK_NULL_HANDLE;
  VkDeviceSize tensorSize = 0;
  VkDeviceSize tensorBytes = 0;
  VkBuffer detectionBuffer = VK_NULL_HANDLE;
  VkDeviceMemory detectionMemory = VK_NULL_HANDLE;
  VkDeviceSize detectionSize = 0;
  int maxDetections = 256;
};

QString findRes10NcnnModelFile(const QString &explicitPath,
                               const QString &fileName);
QString scrfdModelFileName(const QString &variantId, const QString &suffix);

QVector<Detection> readDetections(VulkanHarnessContext &vk, int imageWidth,
                                  int imageHeight);
QVector<Detection> detectVulkanFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *detector,
    VulkanHarnessContext *vk, const render_detail::OffscreenVulkanFrame &frame,
    int maxDetections, float threshold, double *vulkanMs, QString *error);
QVector<Detection> detectRes10VulkanFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector *detector,
    VulkanHarnessContext *vk, const render_detail::OffscreenVulkanFrame &frame,
    const QString &paramPath, const QString &binPath, float threshold,
    bool smallFaceFallback, bool suppressNcnnInfo, double *vulkanMs,
    QString *error);
QVector<Detection> detectRes10FromDecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const editor::FrameHandle &frame, const QString &paramPath,
    const QString &binPath, float threshold, bool allowCpuUploadFallback,
    double *uploadMs, double *vulkanMs, bool *hardwareDirectUsed,
    QString *error);
QVector<Detection> detectScrfdVulkanFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector *detector,
    VulkanHarnessContext *vk, const render_detail::OffscreenVulkanFrame &frame,
    const QString &paramPath, const QString &binPath, float threshold,
    int targetSize, bool tiledPass, bool suppressNcnnInfo, double *vulkanMs,
    QString *error);
QVector<Detection> detectScrfdFromDecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const editor::FrameHandle &frame, const QString &paramPath,
    const QString &binPath, float threshold, int targetSize, bool tiledPass,
    bool suppressNcnnInfo, bool allowCpuUploadFallback, double *uploadMs,
    double *vulkanMs, bool *hardwareDirectUsed, QString *error);

struct PreparedDecoderDetectionResult {
  QVector<Detection> detections;
  jcut::vulkan_detector::NcnnInferenceStats ncnnStats;
  jcut::vulkan_detector::HardwareInteropProbeResult handoffProbe;
  QString error;
  QString hardwareDirectAttemptReason;
  double vulkanDetectMs = 0.0;
  bool ok = false;
};

struct PreparedDecoderDetectionSlot {
  VulkanHarnessContext context;
  jcut::vulkan_detector::VulkanDetectorFrameHandoff handoff;
  jcut::vulkan_detector::VulkanZeroCopyFaceDetector preprocessor;
  jcut::vulkan_detector::VulkanRes10NcnnFaceDetector res10Detector;
  jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector scrfdDetector;
  editor::FrameHandle decodedFrame;
  jcut::vulkan_detector::ScrfdTensorLayout scrfdLayout;
  int frameNumber = -1;
  int frameOffset = -1;
  QSize detectionFrameSize;
  double decoderUploadMs = 0.0;
  int workerIndex = 0;
  bool hardwareDirectHandoff = false;
  bool decoderVulkanUploadFallback = false;
  bool active = false;
  bool detectionRunning = false;
  std::future<PreparedDecoderDetectionResult> detectionFuture;
};

constexpr int kMaxDecoderDirectPipelineSlots = 10;

struct DecoderDetectorWorker {
  bool busy = false;
};

VkDeviceSize scrfdTensorBytesForSourceSize(const QSize &sourceSize,
                                           int targetSize);
bool prepareRes10DecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const editor::FrameHandle &frame, const QString &paramPath,
    const QString &binPath, bool suppressNcnnInfo, bool allowCpuUploadFallback,
    double *uploadMs, bool *hardwareDirectUsed, QSize *detectionFrameSize,
    QString *error);
QVector<Detection> finalizePreparedRes10DecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanRes10NcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const QSize &detectionFrameSize, float threshold, double *vulkanMs,
    QString *error);
bool prepareScrfdDecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const editor::FrameHandle &frame, const QString &paramPath,
    const QString &binPath, int targetSize, bool suppressNcnnInfo,
    bool allowCpuUploadFallback,
    jcut::vulkan_detector::ScrfdTensorLayout *layout, double *uploadMs,
    bool *hardwareDirectUsed, QSize *detectionFrameSize, QString *error);
QVector<Detection> finalizePreparedScrfdDecoderFrame(
    jcut::vulkan_detector::VulkanZeroCopyFaceDetector *preprocessor,
    jcut::vulkan_detector::VulkanScrfdNcnnFaceDetector *detector,
    VulkanHarnessContext *vk,
    jcut::vulkan_detector::VulkanDetectorFrameHandoff *handoff,
    const jcut::vulkan_detector::ScrfdTensorLayout &layout,
    const QSize &detectionFrameSize, float threshold, double *vulkanMs,
    QString *error);
