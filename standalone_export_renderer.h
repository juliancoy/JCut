#pragma once

#include "editor_document_core.h"
#include "render_contract_types.h"

#include <functional>
#include <string>

namespace jcut::standalone_render {

struct ExportRenderRequest {
    EditorDocumentCore document;
    std::string rootDirectory;
};

using ExportProgressCallback = std::function<bool(const render::RenderProgressCore&)>;

render::RenderResultCore exportTimelineToFile(const ExportRenderRequest& request,
                                              const ExportProgressCallback& progressCallback = {});

} // namespace jcut::standalone_render
