#include "vulkan_preview_compositor.h"

#include "render_internal.h"

#include <algorithm>
#include <memory>

namespace vulkan_preview {
namespace {

struct SharedCompositorCache {
    std::unique_ptr<render_detail::OffscreenRenderer> renderer;
    QSize size;
    QHash<QString, editor::DecoderContext*> decoders;
    QHash<render_detail::RenderAsyncFrameKey, editor::FrameHandle> asyncFrameCache;
};

SharedCompositorCache& cache()
{
    static SharedCompositorCache s_cache;
    return s_cache;
}

bool ensureRenderer(const QSize& outputSize, QString* errorMessage)
{
    SharedCompositorCache& c = cache();
    const QSize normalized(qMax(16, outputSize.width()), qMax(16, outputSize.height()));
    if (c.renderer && c.size == normalized) {
        return true;
    }
    c.renderer = std::make_unique<render_detail::OffscreenVulkanRenderer>();
    c.decoders.clear();
    c.asyncFrameCache.clear();
    c.size = normalized;
    if (!c.renderer->initialize(normalized, errorMessage)) {
        c.renderer.reset();
        return false;
    }
    return true;
}

QVector<TimelineClip> orderedVisualClips(const QVector<TimelineClip>& clips, const QVector<TimelineTrack>& tracks)
{
    QVector<TimelineClip> ordered;
    ordered.reserve(clips.size());
    for (const TimelineClip& clip : clips) {
        if (clipVisualPlaybackEnabled(clip, tracks)) {
            ordered.push_back(clip);
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const TimelineClip& a, const TimelineClip& b) {
        if (a.trackIndex == b.trackIndex) {
            return clipTimelineStartSamples(a) < clipTimelineStartSamples(b);
        }
        return a.trackIndex > b.trackIndex;
    });
    return ordered;
}

} // namespace

bool composeFrame(VulkanRendererState* state, int64_t timelineFrame, QImage* outFrame, QString* errorMessage)
{
    if (!state || !outFrame) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Invalid Vulkan preview state.");
        }
        return false;
    }
    const QSize outputSize = state->outputSize.isValid() ? state->outputSize : QSize(1080, 1920);
    if (!ensureRenderer(outputSize, errorMessage)) {
        return false;
    }

    RenderRequest request;
    request.outputPath = QStringLiteral("preview://vulkan-shared");
    request.outputFormat = QStringLiteral("preview");
    request.outputSize = outputSize;
    request.bypassGrading = state->bypassGrading;
    request.correctionsEnabled = state->correctionsEnabled;
    request.clips = state->clips;
    request.tracks = state->tracks;
    request.renderSyncMarkers = state->renderSyncMarkers;
    request.exportStartFrame = timelineFrame;
    request.exportEndFrame = timelineFrame;

    QVector<TimelineClip> ordered = orderedVisualClips(request.clips, request.tracks);
    SharedCompositorCache& c = cache();
    qint64 decodeMs = 0;
    qint64 textureMs = 0;
    qint64 compositeMs = 0;
    qint64 readbackMs = 0;
    *outFrame = c.renderer->renderFrame(request,
                                        timelineFrame,
                                        c.decoders,
                                        nullptr,
                                        &c.asyncFrameCache,
                                        ordered,
                                        nullptr,
                                        &decodeMs,
                                        &textureMs,
                                        &compositeMs,
                                        &readbackMs,
                                        nullptr,
                                        nullptr);
    if (outFrame->isNull()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Shared Vulkan compositor returned null frame.");
        }
        return false;
    }
    return true;
}

} // namespace vulkan_preview
