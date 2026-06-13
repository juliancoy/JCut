#pragma once

#include "editor_shared.h"
#include "render.h"
#include "render_contract_types.h"

#include <functional>
#include <vector>

namespace jcut::render {

struct TimelineRenderData {
    std::vector<TimelineClip> clips;
    std::vector<TimelineTrack> tracks;
    std::vector<RenderSyncMarker> renderSyncMarkers;
    std::vector<ExportRangeSegment> exportRanges;
};

struct PreviewFrameResultCore {
    bool success = false;
    bool usedGpu = false;
    std::string requestedRenderBackend;
    std::string effectiveRenderBackend;
    std::string message;
    core::ImageBuffer image;
};

using RenderProgressCoreCallback = std::function<bool(const RenderProgressCore&)>;

RenderRequest toQtRenderRequest(const RenderRequestCore& request,
                                const TimelineRenderData& timelineData);

RenderResultCore renderTimelineToFileCore(const RenderRequestCore& request,
                                          const TimelineRenderData& timelineData,
                                          const RenderProgressCoreCallback& progressCallback = {});

PreviewFrameResultCore renderPreviewFrameCore(const RenderRequestCore& request,
                                              const TimelineRenderData& timelineData,
                                              std::int64_t timelineFrame);

} // namespace jcut::render
