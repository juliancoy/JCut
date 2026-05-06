#pragma once

#include "editor_shared.h"
#include "frame_handle.h"
#include "render.h"
#include "render_internal.h"

#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QRect>
#include <QString>

#include <memory>

namespace jcut::boxstream {

struct VulkanFrameStats {
    qint64 decodeMs = 0;
    qint64 textureMs = 0;
    qint64 compositeMs = 0;
    qint64 readbackMs = 0;
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

bool renderFrameToVulkan(VulkanFrameProvider* provider,
                         const TimelineClip& sourceClip,
                         const QString& mediaPath,
                         int64_t timelineFrame,
                         int64_t sourceFrame,
                         const QSize& outputSize,
                         render_detail::OffscreenVulkanFrame* frame,
                         VulkanFrameStats* stats = nullptr,
                         QString* errorMessage = nullptr);

QImage readLastRenderedVulkanFrameImage(VulkanFrameProvider* provider,
                                        VulkanFrameStats* stats = nullptr,
                                        QString* errorMessage = nullptr);

QImage buildScanPreview(const QImage& source, const QVector<QRect>& detections, int activeTracks);

QJsonArray buildContinuityStreams(const QJsonArray& tracks,
                                  const QJsonObject& transcriptRoot,
                                  const QString& detectorMode,
                                  bool onlyDialogue);

QJsonObject buildContinuityRoot(const QString& runId,
                                bool onlyDialogue,
                                int64_t scanStart,
                                int64_t scanEnd,
                                const QJsonArray& streams);

bool saveContinuityArtifact(const QString& transcriptPath,
                            const QString& clipId,
                            const QJsonObject& continuityRoot,
                            QJsonObject* artifactRootOut = nullptr);

} // namespace jcut::boxstream
