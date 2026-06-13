#pragma once

#include "editor_document_core.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace jcut {

nlohmann::json toJson(const EditorDocumentCore& document);

nlohmann::json toLegacyStateJson(
    const EditorDocumentCore& document,
    const nlohmann::json* baseRoot = nullptr);

std::optional<EditorDocumentCore> editorDocumentCoreFromJson(
    const nlohmann::json& root,
    std::string* errorOut = nullptr);

std::optional<EditorDocumentCore> editorDocumentCoreFromJsonBytes(
    const std::string& bytes,
    std::string* errorOut = nullptr);

std::optional<EditorDocumentCore> loadEditorDocumentCoreFromFile(
    const std::string& path,
    std::string* errorOut = nullptr);

bool saveEditorDocumentCoreToFile(
    const EditorDocumentCore& document,
    const std::string& path,
    std::string* errorOut = nullptr);

bool saveLegacyStateDocumentToFile(
    const EditorDocumentCore& document,
    const std::string& path,
    std::string* errorOut = nullptr,
    const nlohmann::json* baseRoot = nullptr);

} // namespace jcut
