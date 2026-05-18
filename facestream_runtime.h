#pragma once

#include "editor_shared.h"
#include "frame_handle.h"
#include "preview_interaction_state.h"
#include "render.h"
#include "render_internal.h"

#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QRect>
#include <QString>

#include <memory>

namespace jcut::facestream {

TimelineClip buildFacestreamRenderClip(const TimelineClip& sourceClip,
                                       const QString& mediaPath,
                                       int64_t timelineFrame,
                                       int64_t sourceFrame);

struct VulkanFrameStats {
    qint64 decodeMs = 0;
    qint64 textureMs = 0;
    qint64 compositeMs = 0;
    qint64 readbackMs = 0;
};

struct VulkanRenderResult {
    render_detail::OffscreenRenderFrame frame;

    bool hasCpuImage() const { return frame.hasCpuImage(); }
    bool hasVulkanFrame() const { return frame.hasVulkanFrame(); }
};

struct VulkanFrameProvider {
    std::unique_ptr<render_detail::OffscreenVulkanRenderer> renderer;
    QHash<QString, editor::DecoderContext*> decoders;
    QHash<render_detail::RenderAsyncFrameKey, editor::FrameHandle> asyncFrameCache;
    QSize outputSize;
    bool initialized = false;
    bool failed = false;
    QString failureReason;

    ~VulkanFrameProvider();
    bool ensureInitialized(const QSize& size);
};

QImage renderFrameWithVulkan(VulkanFrameProvider* provider,
                             const TimelineClip& sourceClip,
                             const QString& mediaPath,
                             int64_t timelineFrame,
                             int64_t sourceFrame,
                             const QSize& outputSize,
                             VulkanFrameStats* stats = nullptr);

bool renderFrameWithVulkanResult(VulkanFrameProvider* provider,
                                 const TimelineClip& sourceClip,
                                 const QString& mediaPath,
                                 int64_t timelineFrame,
                                 int64_t sourceFrame,
                                 const QSize& outputSize,
                                 VulkanRenderResult* result,
                                 bool readbackToCpuImage = false,
                                 VulkanFrameStats* stats = nullptr,
                                 QString* errorMessage = nullptr);

bool renderFrameToVulkan(VulkanFrameProvider* provider,
                         const TimelineClip& sourceClip,
                         const QString& mediaPath,
                         int64_t timelineFrame,
                         int64_t sourceFrame,
                         const QSize& outputSize,
                         render_detail::OffscreenVulkanFrame* frame,
                         VulkanFrameStats* stats = nullptr,
                         QString* errorMessage = nullptr);

bool renderFrameToVulkanWithPreviewImage(VulkanFrameProvider* provider,
                                         const TimelineClip& sourceClip,
                                         const QString& mediaPath,
                                         int64_t timelineFrame,
                                         int64_t sourceFrame,
                                         const QSize& outputSize,
                                         render_detail::OffscreenVulkanFrame* frame,
                                         QImage* previewImageOut,
                                         VulkanFrameStats* stats = nullptr,
                                         QString* errorMessage = nullptr);

QImage readLastRenderedVulkanFrameImage(VulkanFrameProvider* provider,
                                        VulkanFrameStats* stats = nullptr,
                                        QString* errorMessage = nullptr);

VulkanPreviewClipFrameStatus buildPreviewClipFrameStatus(const QString& clipId,
                                                         const editor::FrameHandle& frameHandle,
                                                         int64_t requestedFrame,
                                                         const QSize& fallbackFrameSize);
VulkanPreviewClipFrameStatus buildPreviewClipFrameStatus(const QString& clipId,
                                                         const render_detail::OffscreenVulkanFrame& frame,
                                                         int64_t requestedFrame);

QVector<VulkanPreviewFacestreamOverlay> buildDetectionPreviewOverlays(
    const QString& clipId,
    int64_t sourceFrame,
    const QSize& frameSize,
    const QVector<QRectF>& detectionBoxes,
    const QVector<float>& confidences,
    const QRectF& roiRect = QRectF(),
    const QString& source = QStringLiteral("facestream"));

void updateSingleClipPreviewInteractionState(PreviewInteractionState* state,
                                             const TimelineClip& sourceClip,
                                             int64_t frameNumber,
                                             const VulkanPreviewClipFrameStatus& status,
                                             const QVector<VulkanPreviewFacestreamOverlay>& overlays);

QImage buildScanPreview(const QImage& source,
                        const QVector<QRect>& detections,
                        int detectionCount,
                        const QRectF& roiRect = QRectF());

QJsonArray buildContinuityStreams(const QJsonArray& tracks,
                                  const QJsonObject& transcriptRoot,
                                  const QString& detectorMode,
                                  bool onlyDialogue);

QJsonArray continuityStreamsForRoot(const QJsonObject& continuityRoot,
                                    const QJsonObject& transcriptRoot = QJsonObject{});

bool continuityRootHasTracks(const QJsonObject& continuityRoot,
                             const QJsonObject& transcriptRoot = QJsonObject{});
bool continuityRootHasStoredPayload(const QJsonObject& continuityRoot);

QJsonObject buildProcessedContinuityRoot(const QString& clipId,
                                         const QJsonObject& rawContinuityRoot,
                                         const QJsonObject& transcriptRoot,
                                         const QString& rawArtifactPath = QString{});

bool saveProcessedContinuityArtifact(const QString& transcriptPath,
                                     const QString& clipId,
                                     const QJsonObject& rawContinuityRoot,
                                     const QJsonObject& transcriptRoot,
                                     QJsonObject* artifactRootOut = nullptr);

QJsonObject buildContinuityRoot(const QString& runId,
                                bool onlyDialogue,
                                int64_t scanStart,
                                int64_t scanEnd,
                                const QJsonArray& streams = QJsonArray{},
                                const QJsonArray& rawTracks = QJsonArray{},
                                const QJsonArray& rawFrames = QJsonArray{},
                                const QString& detectorMode = QString{});

bool readBinaryJsonObject(const QString& path,
                          QJsonObject* objectOut,
                          QString* errorOut = nullptr);

bool saveContinuityArtifact(const QString& transcriptPath,
                            const QString& clipId,
                            const QJsonObject& continuityRoot,
                            QJsonObject* artifactRootOut = nullptr);

} // namespace jcut::facestream
