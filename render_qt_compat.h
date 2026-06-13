#pragma once

#include "render.h"
#include "render_contract_types.h"

namespace jcut::render {

RenderRequestCore toCoreRenderRequest(const ::RenderRequest& request);
RenderProgressCore toCoreRenderProgress(const ::RenderProgress& progress);
RenderResultCore toCoreRenderResult(const ::RenderResult& result);

} // namespace jcut::render
