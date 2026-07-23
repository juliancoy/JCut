#pragma once

#include "editor_document_core.h"
#include "render_contract_types.h"

#include <functional>
#include <cstdint>
#include <string>

namespace jcut::standalone_render {

struct ExportRenderRequest {
    EditorDocumentCore document;
    std::string rootDirectory;
    std::int64_t imageSequenceFrameNumberOffset = 0;
    std::int64_t outputFrameLimit = 0;
};

using ExportProgressCallback = std::function<bool(const render::RenderProgressCore&)>;

render::RenderResultCore exportTimelineToFile(const ExportRenderRequest& request,
                                              const ExportProgressCallback& progressCallback = {});

} // namespace jcut::standalone_render
