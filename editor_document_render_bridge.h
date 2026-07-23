#pragma once

#include "editor_document_core.h"
#include "render_runtime.h"

namespace jcut::render {

TimelineRenderData buildTimelineRenderData(const EditorDocumentCore& document,
                                           bool probeMedia = true);

} // namespace jcut::render
