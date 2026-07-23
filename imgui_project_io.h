#pragma once

#include "editor_document_core.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jcut {

struct ImGuiProjectSession {
    EditorDocumentCore document;
    std::string projectId;
    std::string statePath;
    std::string historyPath;
    std::string rootDirPath;
    std::string mediaRootPath;
    nlohmann::json legacyStateRoot;
    // Pending ImGui-owned legacy fields are applied only after optimistic
    // concurrency validates legacyStateRoot against state.json.
    nlohmann::json legacyStateOverrides;
};

struct ImGuiProjectHistoryEntry {
    std::size_t index = 0;
    bool isActive = false;
    std::string projectName;
    std::int64_t currentFrame = 0;
    std::size_t clipCount = 0;
};

std::optional<ImGuiProjectSession> loadActiveImGuiProjectSession(
    std::string* errorOut = nullptr);

std::vector<std::string> availableImGuiProjectIds(std::string* errorOut = nullptr);

std::optional<ImGuiProjectSession> activateImGuiProjectSession(
    const std::string& projectId,
    std::string* errorOut = nullptr);

bool setActiveImGuiProject(const std::string& projectId,
                           std::string* errorOut = nullptr);

bool saveImGuiProjectSession(
    const ImGuiProjectSession& session,
    const EditorDocumentCore& document,
    std::string* errorOut = nullptr);

// Reconstructs lightweight summaries without changing state.json or history.json.
std::optional<std::vector<ImGuiProjectHistoryEntry>>
listImGuiProjectHistoryEntries(
    const ImGuiProjectSession& session,
    std::string* errorOut = nullptr);

// Atomically selects an existing disk-history snapshot for the active managed
// project. Navigation changes the active index but preserves all redo entries.
std::optional<ImGuiProjectSession> activateImGuiProjectHistoryEntry(
    const ImGuiProjectSession& session,
    std::size_t historyIndex,
    std::string* errorOut = nullptr);

std::optional<ImGuiProjectSession> createImGuiProjectSession(
    const std::string& projectName,
    std::string* errorOut = nullptr);

std::optional<ImGuiProjectSession> saveImGuiProjectSessionAs(
    const ImGuiProjectSession& sourceSession,
    const EditorDocumentCore& document,
    const std::string& projectName,
    std::string* errorOut = nullptr);

std::optional<ImGuiProjectSession> renameImGuiProjectSession(
    const ImGuiProjectSession& sourceSession,
    const std::string& projectName,
    std::string* errorOut = nullptr);

namespace testing {

enum class ImGuiProjectIoFailurePoint {
    ActiveProjectMarkerCommit,
    ResultingProjectSessionLoad,
    ProjectDirectoryRename,
    ProjectDirectoryRenameRollback,
    RenamedProjectMarkerRecoveryCommit,
    ReservedProjectDirectoryCleanup,
    PairedProjectStateCommit,
    Count,
};

// One-shot failure points used by project-store transaction tests. Multiple
// distinct points may be armed together; each point clears when consumed.
void injectNextImGuiProjectIoFailure(ImGuiProjectIoFailurePoint point);
void clearImGuiProjectIoFailures();

} // namespace testing

} // namespace jcut
