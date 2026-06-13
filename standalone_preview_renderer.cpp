#include "standalone_preview_renderer.h"
#include "standalone_timeline_renderer.h"

namespace jcut::standalone_render {

PreviewRenderResult renderPreviewFrame(const PreviewRenderRequest& request)
{
    const TimelineRenderResult timelineResult = renderTimelineFrame({
        request.document,
        request.outputSize,
        static_cast<double>(request.timelineFrame),
        request.rootDirectory});

    PreviewRenderResult result;
    result.success = timelineResult.success;
    result.message = timelineResult.message;
    result.image = timelineResult.image;
    result.sourcePath = timelineResult.sourcePath;
    return result;
}

} // namespace jcut::standalone_render
