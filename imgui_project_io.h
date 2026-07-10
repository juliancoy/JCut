#pragma once

#include "editor_document_core.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace jcut {

struct ImGuiProjectSession {
    EditorDocumentCore document;
    std::string projectId;
    std::string statePath;
    std::string historyPath;
    std::string rootDirPath;
    std::string mediaRootPath;
    nlohmann::json legacyStateRoot;
};

std::optional<ImGuiProjectSession> loadActiveImGuiProjectSession(
    std::string* errorOut = nullptr);

bool saveImGuiProjectSession(
    const ImGuiProjectSession& session,
    const EditorDocumentCore& document,
    std::string* errorOut = nullptr);

} // namespace jcut
