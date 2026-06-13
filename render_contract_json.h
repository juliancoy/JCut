#pragma once

#include "render_contract_types.h"

#include <nlohmann/json.hpp>

namespace jcut::render {

nlohmann::json toJson(RenderOutputMode mode);
nlohmann::json toJson(const RenderRequestCore& request);
nlohmann::json toJson(const RenderProgressCore& progress);
nlohmann::json toJson(const RenderResultCore& result);

} // namespace jcut::render
