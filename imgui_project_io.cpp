#include "imgui_project_io.h"

#include "editor_document_core_json.h"
#include "project_save_lock.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::uint32_t kProjectIoFailurePointCount =
    static_cast<std::uint32_t>(
        jcut::testing::ImGuiProjectIoFailurePoint::Count);
std::atomic<std::uint32_t> g_projectIoFailurePoints{0};

std::uint32_t projectIoFailureBit(
    jcut::testing::ImGuiProjectIoFailurePoint point)
{
    const std::uint32_t index = static_cast<std::uint32_t>(point);
    return index < kProjectIoFailurePointCount
        ? (std::uint32_t{1} << index)
        : 0;
}

void armProjectIoFailure(jcut::testing::ImGuiProjectIoFailurePoint point)
{
    const std::uint32_t bit = projectIoFailureBit(point);
    if (bit != 0) {
        g_projectIoFailurePoints.fetch_or(bit, std::memory_order_relaxed);
    }
}

bool consumeProjectIoFailure(jcut::testing::ImGuiProjectIoFailurePoint point)
{
    const std::uint32_t bit = projectIoFailureBit(point);
    return bit != 0 &&
        (g_projectIoFailurePoints.fetch_and(~bit, std::memory_order_relaxed) & bit) != 0;
}

void appendError(std::string* errorOut, const std::string& message)
{
    if (!errorOut || message.empty()) {
        return;
    }
    if (!errorOut->empty()) {
        *errorOut += "; ";
    }
    *errorOut += message;
}

std::string readFile(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

std::string trim(std::string value)
{
    const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string pathString(const fs::path& path)
{
    return path.lexically_normal().string();
}

std::string stringValue(const json& object, const char* key)
{
    if (!object.is_object()) {
        return {};
    }
    const json::const_iterator it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

std::string resolvePathForRoot(const std::string& path, const fs::path& rootDir)
{
    const std::string cleaned = trim(path);
    if (cleaned.empty()) {
        return {};
    }
    fs::path resolved(cleaned);
    if (resolved.is_relative()) {
        resolved = rootDir / resolved;
    }
    std::error_code ec;
    const fs::path canonical = fs::canonical(resolved, ec);
    if (!ec) {
        return pathString(canonical);
    }
    return pathString(fs::absolute(resolved, ec));
}

std::optional<fs::path> normalizedExistingDirPath(const std::string& path)
{
    const std::string cleaned = trim(path);
    if (cleaned.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    const fs::path candidate(cleaned);
    if (!fs::exists(candidate, ec) || !fs::is_directory(candidate, ec)) {
        return std::nullopt;
    }
    const fs::path canonical = fs::canonical(candidate, ec);
    if (!ec) {
        return canonical;
    }
    return fs::absolute(candidate, ec);
}

fs::path applicationDirPath()
{
    std::error_code ec;
#if defined(__linux__)
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !exe.empty()) {
        return exe.parent_path();
    }
#endif
    return fs::current_path();
}

fs::path configFilePath()
{
    const char* xdgConfig = std::getenv("XDG_CONFIG_HOME");
    if (xdgConfig && *xdgConfig) {
        return fs::path(xdgConfig) / "PanelTalkEditor" / "editor.config";
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return fs::path(home) / ".config" / "PanelTalkEditor" / "editor.config";
    }
    return applicationDirPath() / "editor.config";
}

fs::path legacyConfigFilePath()
{
    return applicationDirPath() / "editor.config";
}

fs::path defaultRootDirPath()
{
    fs::path appDir = applicationDirPath();
    if (const auto normalized = normalizedExistingDirPath(appDir.string())) {
        appDir = *normalized;
    }

    fs::path walker = appDir;
    for (int i = 0; i < 3; ++i) {
        const std::string name = walker.filename().string();
        const bool bundleComponent =
            name == "MacOS" || name == "Contents" ||
            (name.size() > 4 && name.substr(name.size() - 4) == ".app");
        if (!bundleComponent || !walker.has_parent_path()) {
            break;
        }
        walker = walker.parent_path();
    }
    if (const auto normalized = normalizedExistingDirPath(walker.string())) {
        appDir = *normalized;
    }

    const std::string baseName = appDir.filename().string();
    if (baseName == "build" || baseName.rfind("build-", 0) == 0) {
        if (appDir.has_parent_path()) {
            return appDir.parent_path();
        }
    }
    return appDir;
}

fs::path rootDirPath()
{
    if (const char* envRoot = std::getenv("JCUT_PROJECT_ROOT")) {
        if (const auto normalized = normalizedExistingDirPath(envRoot)) {
            return *normalized;
        }
    }

    for (const fs::path& configPath : {configFilePath(), legacyConfigFilePath()}) {
        if (const auto normalized = normalizedExistingDirPath(readFile(configPath))) {
            return *normalized;
        }
    }

    return defaultRootDirPath();
}

fs::path projectsDirPath()
{
    return rootDirPath() / "projects";
}

fs::path currentProjectMarkerPath()
{
    return projectsDirPath() / ".current_project";
}

bool writeTextAtomically(const fs::path& path, const std::string& content, std::string* errorOut)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        if (errorOut) {
            *errorOut = "failed to create parent directory for write: " + ec.message();
        }
        return false;
    }
    if (fs::exists(path, ec) && !fs::is_regular_file(path, ec)) {
        if (errorOut) {
            *errorOut = "refusing to replace non-file path: " + path.string();
        }
        return false;
    }

    static std::atomic<std::uint64_t> nextTemporaryId{0};
    const std::uint64_t temporaryId = nextTemporaryId.fetch_add(1, std::memory_order_relaxed);
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path tempPath = path.string() + ".tmp-" +
        std::to_string(timestamp) + "-" + std::to_string(temporaryId);
    {
        std::ofstream output(tempPath, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!output.is_open()) {
            if (errorOut) {
                *errorOut = "failed to open temporary file for write: " + tempPath.string();
            }
            return false;
        }
        output << content;
        if (!output.good()) {
            output.close();
            fs::remove(tempPath, ec);
            if (errorOut) {
                *errorOut = "failed to write temporary file: " + tempPath.string();
            }
            return false;
        }
    }
#if defined(_WIN32)
    if (MoveFileExW(tempPath.wstring().c_str(),
                    path.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
#else
    fs::rename(tempPath, path, ec);
    if (!ec) {
        return true;
    }
#endif
    std::error_code removeError;
    fs::remove(tempPath, removeError);
    if (errorOut) {
        *errorOut = "failed to atomically replace file " + path.string() + ": " + ec.message();
    }
    return false;
}

bool commitActiveProjectMarker(const std::string& projectId, std::string* errorOut)
{
    if (consumeProjectIoFailure(
            jcut::testing::ImGuiProjectIoFailurePoint::ActiveProjectMarkerCommit)) {
        if (errorOut) {
            *errorOut = "injected active-project marker commit failure";
        }
        return false;
    }
    return writeTextAtomically(currentProjectMarkerPath(), projectId, errorOut);
}

std::vector<std::string> availableProjectIds(std::string* errorOut = nullptr)
{
    std::vector<std::string> ids;
    std::error_code ec;
    fs::create_directories(projectsDirPath(), ec);
    if (ec) {
        if (errorOut) {
            *errorOut = "failed to create projects directory: " + ec.message();
        }
        return {};
    }
    fs::directory_iterator iterator(projectsDirPath(), ec);
    const fs::directory_iterator end;
    while (!ec && iterator != end) {
        std::error_code typeError;
        const fs::directory_entry& entry = *iterator;
        const fs::file_status status = entry.symlink_status(typeError);
        if (!typeError && fs::is_directory(status) && !fs::is_symlink(status)) {
            ids.push_back(entry.path().filename().string());
        }
        if (typeError) {
            ec = typeError;
            break;
        }
        iterator.increment(ec);
    }
    if (ec) {
        if (errorOut) {
            *errorOut = "failed to enumerate projects: " + ec.message();
        }
        return {};
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::string currentProjectId()
{
    std::error_code ec;
    fs::create_directories(projectsDirPath(), ec);
    fs::create_directories(projectsDirPath() / "default", ec);

    std::string current = trim(readFile(currentProjectMarkerPath()));
    const std::vector<std::string> ids = availableProjectIds();
    if (ids.empty()) {
        current = "default";
    } else if (current.empty() || std::find(ids.begin(), ids.end(), current) == ids.end()) {
        current = std::find(ids.begin(), ids.end(), "default") != ids.end() ? "default" : ids.front();
    }
    std::string ignoredError;
    writeTextAtomically(currentProjectMarkerPath(), current, &ignoredError);
    return current;
}

enum class JsonObjectFileStatus {
    Missing,
    Empty,
    Valid,
    Invalid,
};

JsonObjectFileStatus loadJsonObjectFile(
    const fs::path& path,
    json* rootOut,
    std::string* errorOut)
{
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        if (ec && errorOut) {
            *errorOut = "failed to inspect JSON file: " + path.string() + ": " + ec.message();
        }
        return ec ? JsonObjectFileStatus::Invalid : JsonObjectFileStatus::Missing;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        if (errorOut) {
            *errorOut = "failed to open JSON file for read: " + path.string();
        }
        return JsonObjectFileStatus::Invalid;
    }
    const std::string bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        if (errorOut) {
            *errorOut = "failed to read JSON file: " + path.string();
        }
        return JsonObjectFileStatus::Invalid;
    }
    if (bytes.empty()) {
        if (rootOut) {
            *rootOut = json::object();
        }
        return JsonObjectFileStatus::Empty;
    }
    try {
        json root = json::parse(bytes);
        if (!root.is_object()) {
            if (errorOut) {
                *errorOut = "JSON root is not an object: " + path.string();
            }
            return JsonObjectFileStatus::Invalid;
        }
        if (rootOut) {
            *rootOut = std::move(root);
        }
        return JsonObjectFileStatus::Valid;
    } catch (const std::exception& exception) {
        if (errorOut) {
            *errorOut = "failed to parse JSON file " + path.string() + ": " + exception.what();
        }
        return JsonObjectFileStatus::Invalid;
    }
}

struct ProjectHistory {
    json root = json::object();
    json entries = json::array();
    std::size_t index = 0;
    bool usesSnapshotDeltas = false;
};

constexpr std::size_t kDefaultProjectHistoryEntries = 100;
constexpr std::size_t kDefaultProjectHistoryMegabytes = 16;

std::size_t boundedHistorySetting(const json& stateRoot,
                                  const char* key,
                                  std::size_t fallback,
                                  std::size_t minimum,
                                  std::size_t maximum)
{
    const json::const_iterator it = stateRoot.find(key);
    if (it == stateRoot.end() ||
        (!it->is_number_integer() && !it->is_number_unsigned())) {
        return fallback;
    }
    try {
        if (it->is_number_unsigned()) {
            return std::clamp(it->get<std::size_t>(), minimum, maximum);
        }
        const std::int64_t value = it->get<std::int64_t>();
        if (value <= 0) {
            return minimum;
        }
        return std::clamp(static_cast<std::size_t>(value), minimum, maximum);
    } catch (const json::exception&) {
        return fallback;
    }
}

void stripHeavyClipHistoryState(json* clip)
{
    if (clip && clip->is_object()) {
        clip->erase("speakerFramingKeyframes");
    }
}

json sanitizedHistorySnapshot(json snapshot)
{
    if (!snapshot.is_object()) {
        return json::object();
    }
    snapshot.erase("__historyTranscriptDocuments");
    snapshot.erase("stateRevision");

    json timeline = snapshot.value("timeline", json::array());
    if (timeline.is_array()) {
        for (json& clip : timeline) {
            stripHeavyClipHistoryState(&clip);
        }
        snapshot["timeline"] = std::move(timeline);
    }

    json selectedClip = snapshot.value("selectedClip", json::object());
    stripHeavyClipHistoryState(&selectedClip);
    snapshot["selectedClip"] = std::move(selectedClip);
    return snapshot;
}

bool applyHistoryObjectPatch(json* base, const json& patch)
{
    if (!base || !base->is_object() || !patch.is_object()) {
        return false;
    }

    json set = json::object();
    if (const json::const_iterator setIt = patch.find("set"); setIt != patch.end()) {
        if (!setIt->is_object()) {
            return false;
        }
        set = *setIt;
    }
    for (const auto& [key, value] : set.items()) {
        const json::iterator existing = base->find(key);
        if (existing != base->end() && existing->is_object() && value.is_object() &&
            (value.contains("set") || value.contains("remove"))) {
            json nested = *existing;
            if (!applyHistoryObjectPatch(&nested, value)) {
                return false;
            }
            (*base)[key] = std::move(nested);
        } else {
            (*base)[key] = value;
        }
    }

    json remove = json::array();
    if (const json::const_iterator removeIt = patch.find("remove"); removeIt != patch.end()) {
        if (!removeIt->is_array()) {
            return false;
        }
        remove = *removeIt;
    }
    for (const json& value : remove) {
        if (!value.is_string()) {
            return false;
        }
        base->erase(value.get<std::string>());
    }
    return true;
}

json diffHistoryObject(const json& previous, const json& current)
{
    json set = json::object();
    json remove = json::array();

    for (const auto& [key, value] : current.items()) {
        const json::const_iterator previousIt = previous.find(key);
        if (previousIt != previous.end() && *previousIt == value) {
            continue;
        }
        if (previousIt != previous.end() && previousIt->is_object() && value.is_object()) {
            json nested = diffHistoryObject(*previousIt, value);
            if (!nested.at("set").empty() || !nested.at("remove").empty()) {
                set[key] = std::move(nested);
            }
        } else {
            set[key] = value;
        }
    }

    for (const auto& [key, value] : previous.items()) {
        (void)value;
        if (!current.contains(key)) {
            remove.push_back(key);
        }
    }
    return {{"set", std::move(set)}, {"remove", std::move(remove)}};
}

std::size_t normalizedHistoryIndex(const json& root, std::size_t entryCount)
{
    if (entryCount == 0) {
        return 0;
    }

    const json::const_iterator indexIt = root.find("index");
    if (indexIt == root.end() ||
        (!indexIt->is_number_integer() && !indexIt->is_number_unsigned())) {
        return entryCount - 1;
    }
    if (indexIt->is_number_unsigned()) {
        return std::min(indexIt->get<std::size_t>(), entryCount - 1);
    }

    const std::int64_t index = indexIt->get<std::int64_t>();
    if (index <= 0) {
        return 0;
    }
    const std::uint64_t unsignedIndex = static_cast<std::uint64_t>(index);
    if (unsignedIndex >= entryCount) {
        return entryCount - 1;
    }
    return static_cast<std::size_t>(unsignedIndex);
}

bool loadProjectHistory(const fs::path& path, ProjectHistory* history, std::string* errorOut)
{
    if (!history) {
        return false;
    }

    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec) {
        if (errorOut) {
            *errorOut = "failed to inspect project history: " + path.string();
        }
        return false;
    }
    if (!exists) {
        *history = ProjectHistory{};
        return true;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        if (errorOut) {
            *errorOut = "failed to open project history for read: " + path.string();
        }
        return false;
    }
    const std::string bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        if (errorOut) {
            *errorOut = "failed to read project history: " + path.string();
        }
        return false;
    }
    if (bytes.empty()) {
        *history = ProjectHistory{};
        return true;
    }

    json root;
    try {
        root = json::parse(bytes);
    } catch (const std::exception& exception) {
        if (errorOut) {
            *errorOut = "failed to parse project history: " + std::string(exception.what());
        }
        return false;
    }
    if (!root.is_object()) {
        if (errorOut) {
            *errorOut = "project history root is not an object: " + path.string();
        }
        return false;
    }

    ProjectHistory loaded;
    loaded.root = std::move(root);
    const json::const_iterator formatIt = loaded.root.find("format");
    if (formatIt != loaded.root.end() &&
        (!formatIt->is_string() ||
         (!formatIt->get<std::string>().empty() &&
          formatIt->get<std::string>() != "snapshot-delta-v1"))) {
        if (errorOut) {
            *errorOut = "unsupported project history format: " + path.string();
        }
        return false;
    }
    loaded.usesSnapshotDeltas = stringValue(loaded.root, "format") == "snapshot-delta-v1";
    if (!loaded.usesSnapshotDeltas) {
        const json::const_iterator entriesIt = loaded.root.find("entries");
        if (entriesIt != loaded.root.end()) {
            if (!entriesIt->is_array()) {
                if (errorOut) {
                    *errorOut = "project history entries are not an array: " + path.string();
                }
                return false;
            }
            loaded.entries = *entriesIt;
            for (const json& entry : loaded.entries) {
                if (!entry.is_object()) {
                    if (errorOut) {
                        *errorOut = "project history contains a non-object entry: " + path.string();
                    }
                    return false;
                }
            }
        }
    } else {
        const json::const_iterator baseIt = loaded.root.find("base");
        const json::const_iterator patchesIt = loaded.root.find("patches");
        const json base = baseIt == loaded.root.end() ? json::object() : *baseIt;
        const json patches = patchesIt == loaded.root.end() ? json::array() : *patchesIt;
        if (!base.is_object() || !patches.is_array()) {
            if (errorOut) {
                *errorOut = "project snapshot-delta history has an invalid base or patches array: " +
                    path.string();
            }
            return false;
        }

        if (!base.empty() || !patches.empty()) {
            json current = base;
            loaded.entries.push_back(current);
            for (std::size_t patchIndex = 0; patchIndex < patches.size(); ++patchIndex) {
                if (!applyHistoryObjectPatch(&current, patches.at(patchIndex))) {
                    if (errorOut) {
                        *errorOut = "project snapshot-delta history contains an invalid patch at index " +
                            std::to_string(patchIndex);
                    }
                    return false;
                }
                loaded.entries.push_back(current);
            }
        }
    }

    loaded.index = normalizedHistoryIndex(loaded.root, loaded.entries.size());
    *history = std::move(loaded);
    return true;
}

bool activeSessionRevisionMatchesProjectFiles(
    const jcut::ImGuiProjectSession& session,
    const ProjectHistory& history,
    std::string* errorOut)
{
    json stateRoot;
    std::string stateError;
    const JsonObjectFileStatus stateStatus = loadJsonObjectFile(
        session.statePath, &stateRoot, &stateError);

    json currentRoot;
    if (stateStatus == JsonObjectFileStatus::Valid && !stateRoot.empty()) {
        if (!session.legacyStateRoot.is_object() ||
            session.legacyStateRoot.empty() ||
            stateRoot != session.legacyStateRoot) {
            if (errorOut) {
                *errorOut =
                    "project changed on disk; reload before navigating history";
            }
            return false;
        }
        currentRoot = std::move(stateRoot);
    } else if (!history.entries.empty() &&
               sanitizedHistorySnapshot(
                   history.entries.at(history.index)) ==
                   sanitizedHistorySnapshot(session.legacyStateRoot)) {
        // Match the existing malformed/missing-state recovery policy: a
        // session loaded from the active history entry may repair state.json.
        currentRoot = session.legacyStateRoot;
    } else {
        if (errorOut) {
            *errorOut = stateStatus == JsonObjectFileStatus::Invalid &&
                    !stateError.empty()
                ? stateError
                : "project changed on disk; reload before navigating history";
        }
        return false;
    }

    if (!history.entries.empty() &&
        sanitizedHistorySnapshot(history.entries.at(history.index)) !=
            sanitizedHistorySnapshot(currentRoot)) {
        if (errorOut) {
            *errorOut =
                "project state and history index are inconsistent; reload before navigating history";
        }
        return false;
    }
    return true;
}

bool commitPairedProjectFiles(
    const fs::path& statePath,
    const fs::path& historyPath,
    const std::string& statePayload,
    const std::string& historyPayload,
    std::string* errorOut)
{
    std::error_code historyExistsError;
    const bool historyExisted = fs::exists(historyPath, historyExistsError);
    if (historyExistsError) {
        if (errorOut) {
            *errorOut = "failed to inspect project history before commit: " +
                historyExistsError.message();
        }
        return false;
    }
    const std::string previousHistoryPayload = historyExisted
        ? readFile(historyPath)
        : std::string{};

    // Commit history first, then restore it if state cannot commit. Each file
    // replacement is atomic; the rollback keeps their visible revisions paired
    // for all ordinary write failures.
    if (!writeTextAtomically(historyPath, historyPayload, errorOut)) {
        return false;
    }

    std::string stateWriteError;
    bool stateCommitted = false;
    if (consumeProjectIoFailure(
            jcut::testing::ImGuiProjectIoFailurePoint::
                PairedProjectStateCommit)) {
        stateWriteError = "injected paired project state commit failure";
    } else {
        stateCommitted = writeTextAtomically(
            statePath, statePayload, &stateWriteError);
    }
    if (stateCommitted) {
        return true;
    }

    std::string rollbackError;
    bool rolledBack = false;
    if (historyExisted) {
        rolledBack = writeTextAtomically(
            historyPath, previousHistoryPayload, &rollbackError);
    } else {
        std::error_code removeError;
        fs::remove(historyPath, removeError);
        std::error_code remainingError;
        const bool remains = fs::exists(historyPath, remainingError);
        rolledBack = !removeError && !remainingError && !remains;
        if (removeError) {
            rollbackError = removeError.message();
        } else if (remainingError) {
            rollbackError = remainingError.message();
        } else if (remains) {
            rollbackError = "new history file still exists";
        }
    }
    if (errorOut) {
        *errorOut = stateWriteError;
        if (!rolledBack) {
            *errorOut += "; failed to roll back history: " + rollbackError;
        }
    }
    return false;
}

json historyRootFromEntries(const ProjectHistory& history)
{
    json root = history.root.is_object() ? history.root : json::object();
    const bool hasEntries = !history.entries.empty();
    root["index"] = hasEntries
        ? static_cast<std::int64_t>(history.index)
        : std::int64_t{-1};
    if (!history.usesSnapshotDeltas) {
        root["entries"] = history.entries;
        return root;
    }

    root.erase("entries");
    root["format"] = "snapshot-delta-v1";
    if (!hasEntries) {
        root["base"] = json::object();
        root["patches"] = json::array();
        return root;
    }

    root["base"] = history.entries.front();
    json patches = json::array();
    for (std::size_t i = 1; i < history.entries.size(); ++i) {
        patches.push_back(diffHistoryObject(history.entries.at(i - 1), history.entries.at(i)));
    }
    root["patches"] = std::move(patches);
    return root;
}

void trimProjectHistory(ProjectHistory* history,
                        std::size_t maxEntries,
                        std::size_t maxBytes)
{
    if (!history || history->entries.empty()) {
        if (history) {
            history->index = 0;
        }
        return;
    }

    history->index = std::min(history->index, history->entries.size() - 1);
    auto removeEntryWithoutRemovingActiveSnapshot = [&]() {
        if (history->index > 0) {
            history->entries.erase(history->entries.begin());
            --history->index;
        } else {
            history->entries.erase(history->entries.end() - 1);
        }
    };
    while (history->entries.size() > maxEntries) {
        removeEntryWithoutRemovingActiveSnapshot();
    }
    while (history->entries.size() > 1 &&
           historyRootFromEntries(*history).dump().size() > maxBytes) {
        removeEntryWithoutRemovingActiveSnapshot();
    }
}

bool updatedProjectHistory(
    const fs::path& path,
    const json& stateRoot,
    json* historyRootOut,
    std::string* errorOut)
{
    ProjectHistory history;
    if (!loadProjectHistory(path, &history, errorOut)) {
        return false;
    }

    for (json& entry : history.entries) {
        entry = sanitizedHistorySnapshot(std::move(entry));
    }
    const json historySnapshot = sanitizedHistorySnapshot(stateRoot);
    const bool currentEntryMatches = !history.entries.empty() &&
        history.entries.at(history.index) == historySnapshot;
    if (!currentEntryMatches) {
        // Match Qt undo branching: a new edit after undo invalidates redo.
        if (!history.entries.empty() && history.index + 1 < history.entries.size()) {
            while (history.entries.size() > history.index + 1) {
                history.entries.erase(history.entries.end() - 1);
            }
        }
        history.entries.push_back(historySnapshot);
        history.index = history.entries.size() - 1;
    }
    const std::size_t maxEntries = boundedHistorySetting(
        stateRoot, "historyMaxEntries", kDefaultProjectHistoryEntries, 10, 500);
    const std::size_t maxMegabytes = boundedHistorySetting(
        stateRoot, "historyMaxMegabytes", kDefaultProjectHistoryMegabytes, 1, 256);
    trimProjectHistory(&history, maxEntries, maxMegabytes * 1024 * 1024);
    *historyRootOut = historyRootFromEntries(history);
    return true;
}

std::optional<json> loadProjectRoot(
    const fs::path& statePath,
    const fs::path& historyPath,
    std::string* errorOut)
{
    json stateRoot;
    std::string stateError;
    const JsonObjectFileStatus stateStatus =
        loadJsonObjectFile(statePath, &stateRoot, &stateError);
    if (stateRoot.is_object() && !stateRoot.empty()) {
        return stateRoot;
    }

    ProjectHistory history;
    std::string historyError;
    if (loadProjectHistory(historyPath, &history, &historyError) && !history.entries.empty()) {
        return history.entries.at(history.index);
    }

    if (stateStatus == JsonObjectFileStatus::Invalid) {
        if (errorOut) {
            *errorOut = stateError;
            if (!historyError.empty()) {
                *errorOut += "; history recovery failed: " + historyError;
            }
        }
        return std::nullopt;
    }
    if (!historyError.empty()) {
        if (errorOut) {
            *errorOut = historyError;
        }
        return std::nullopt;
    }
    return json::object();
}

bool validProjectName(const std::string& projectName,
                      std::string* cleanedNameOut,
                      std::string* errorOut)
{
    const std::string cleanedName = trim(projectName);
    if (cleanedName.empty()) {
        if (errorOut) {
            *errorOut = "project name is empty";
        }
        return false;
    }
    if (cleanedName == "." || cleanedName == ".." ||
        cleanedName.find('/') != std::string::npos ||
        cleanedName.find('\\') != std::string::npos ||
        cleanedName.find('\0') != std::string::npos) {
        if (errorOut) {
            *errorOut = "project name must not contain a path or traversal component";
        }
        return false;
    }
    if (cleanedNameOut) {
        *cleanedNameOut = cleanedName;
    }
    return true;
}

std::string sanitizedProjectIdBase(const std::string& projectName)
{
    std::string id;
    id.reserve(projectName.size());
    bool previousWasDash = false;
    for (const unsigned char byte : projectName) {
        char normalized = '-';
        if (byte >= 'A' && byte <= 'Z') {
            normalized = static_cast<char>(byte - 'A' + 'a');
        } else if ((byte >= 'a' && byte <= 'z') ||
                   (byte >= '0' && byte <= '9') ||
                   byte == '_' || byte == '-') {
            normalized = static_cast<char>(byte);
        }
        if (normalized == '-') {
            if (id.empty() || previousWasDash) {
                previousWasDash = true;
                continue;
            }
            previousWasDash = true;
            id.push_back(normalized);
        } else {
            previousWasDash = false;
            id.push_back(normalized);
        }
    }
    while (!id.empty() && id.back() == '-') {
        id.pop_back();
    }
    return id.empty() ? std::string("project") : id;
}

bool safeProjectIdComponent(const std::string& projectId)
{
    if (projectId.empty() || projectId == "." || projectId == ".." ||
        projectId.find('/') != std::string::npos ||
        projectId.find('\\') != std::string::npos ||
        projectId.find('\0') != std::string::npos) {
        return false;
    }
    const fs::path component(projectId);
    return component == component.filename() && !component.has_parent_path();
}

fs::path normalizedAbsolutePath(const fs::path& path)
{
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    return (ec ? path : absolute).lexically_normal();
}

bool createProjectsDirectory(std::string* errorOut)
{
    const fs::path root = normalizedAbsolutePath(rootDirPath());
    const fs::path projectsDir = root / "projects";
    std::error_code ec;
    const fs::file_status initialStatus = fs::symlink_status(projectsDir, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        if (errorOut) {
            *errorOut = "failed to inspect projects directory: " + ec.message();
        }
        return false;
    }
    if (!ec && fs::is_symlink(initialStatus)) {
        if (errorOut) {
            *errorOut = "projects directory must not be a symbolic link";
        }
        return false;
    }
    if (!ec && fs::exists(initialStatus) && !fs::is_directory(initialStatus)) {
        if (errorOut) {
            *errorOut = "projects path is not a directory";
        }
        return false;
    }
    ec.clear();
    fs::create_directories(projectsDir, ec);
    if (ec) {
        if (errorOut) {
            *errorOut = "failed to create projects directory: " + ec.message();
        }
        return false;
    }

    const fs::path canonicalProjects = fs::canonical(projectsDir, ec);
    if (ec || canonicalProjects != projectsDir) {
        if (errorOut) {
            *errorOut = ec
                ? "failed to canonicalize projects directory: " + ec.message()
                : "projects directory escapes the active project root";
        }
        return false;
    }
    return true;
}

bool sessionUsesManagedProjectStore(const jcut::ImGuiProjectSession& session)
{
    return !session.projectId.empty() || !session.rootDirPath.empty();
}

bool sessionBelongsToProjectStore(const jcut::ImGuiProjectSession& session,
                                  bool requireActiveProject,
                                  std::string* errorOut)
{
    if (!safeProjectIdComponent(session.projectId)) {
        if (errorOut) {
            *errorOut = "project session has an invalid project ID";
        }
        return false;
    }

    if (!createProjectsDirectory(errorOut)) {
        return false;
    }
    const fs::path root = normalizedAbsolutePath(rootDirPath());
    const fs::path projectsDir = root / "projects";
    const fs::path projectDir = projectsDir / session.projectId;
    if (normalizedAbsolutePath(session.rootDirPath) != root ||
        normalizedAbsolutePath(session.statePath) != projectDir / "state.json" ||
        normalizedAbsolutePath(session.historyPath) != projectDir / "history.json") {
        if (errorOut) {
            *errorOut = "project session paths are outside the active project store";
        }
        return false;
    }

    std::error_code ec;
    const fs::file_status projectStatus = fs::symlink_status(projectDir, ec);
    if (ec) {
        if (errorOut) {
            *errorOut = "project directory does not exist: " + projectDir.string();
        }
        return false;
    }
    if (fs::is_symlink(projectStatus)) {
        if (errorOut) {
            *errorOut = "project directory must not be a symbolic link: " +
                projectDir.string();
        }
        return false;
    }
    if (!fs::is_directory(projectStatus)) {
        if (errorOut) {
            *errorOut = "project directory does not exist: " + projectDir.string();
        }
        return false;
    }
    const fs::path canonicalProjectDir = fs::canonical(projectDir, ec);
    if (ec || canonicalProjectDir.parent_path() != projectsDir) {
        if (errorOut) {
            *errorOut = ec
                ? "failed to canonicalize project directory: " + ec.message()
                : "project directory escapes the active project store";
        }
        return false;
    }

    for (const fs::path& projectFile : {
             projectDir / "state.json", projectDir / "history.json"}) {
        ec.clear();
        const fs::file_status fileStatus = fs::symlink_status(projectFile, ec);
        if (ec == std::errc::no_such_file_or_directory) {
            continue;
        }
        if (ec) {
            if (errorOut) {
                *errorOut = "failed to inspect project file: " +
                    projectFile.string() + ": " + ec.message();
            }
            return false;
        }
        if (fs::is_symlink(fileStatus)) {
            if (errorOut) {
                *errorOut = "project files must not be symbolic links: " +
                    projectFile.string();
            }
            return false;
        }
        if (fs::exists(fileStatus) && !fs::is_regular_file(fileStatus)) {
            if (errorOut) {
                *errorOut = "project file is not a regular file: " +
                    projectFile.string();
            }
            return false;
        }
    }

    if (requireActiveProject && currentProjectId() != session.projectId) {
        if (errorOut) {
            *errorOut = "project session is not the active project";
        }
        return false;
    }
    return true;
}

bool activeProjectMarkerMatchesSessionWithoutRepair(
    const jcut::ImGuiProjectSession& session,
    std::string* errorOut)
{
    if (trim(readFile(currentProjectMarkerPath())) == session.projectId) {
        return true;
    }
    if (errorOut) {
        *errorOut = "project session is not the active project";
    }
    return false;
}

std::optional<std::string> uniqueProjectId(const std::string& baseId,
                                           std::string* errorOut)
{
    for (int suffix = 1; ; ++suffix) {
        const std::string candidate = suffix == 1
            ? baseId
            : baseId + "-" + std::to_string(suffix);
        std::error_code ec;
        const fs::file_status status =
            fs::symlink_status(projectsDirPath() / candidate, ec);
        if (ec == std::errc::no_such_file_or_directory ||
            (!ec && !fs::exists(status))) {
            return candidate;
        }
        if (ec) {
            if (errorOut) {
                *errorOut = "failed to inspect project destination: " +
                    ec.message();
            }
            return std::nullopt;
        }
    }
}

bool reserveUniqueProjectDirectory(const std::string& baseId,
                                   std::string* projectIdOut,
                                   fs::path* projectDirOut,
                                   std::string* errorOut)
{
    for (int suffix = 1; ; ++suffix) {
        const std::string projectId = suffix == 1
            ? baseId
            : baseId + "-" + std::to_string(suffix);
        const fs::path projectDir = projectsDirPath() / projectId;
        std::error_code ec;
        if (fs::create_directory(projectDir, ec)) {
            if (projectIdOut) {
                *projectIdOut = projectId;
            }
            if (projectDirOut) {
                *projectDirOut = projectDir;
            }
            return true;
        }
        if (!ec || ec == std::errc::file_exists) {
            continue;
        }
        if (errorOut) {
            *errorOut = "failed to create project directory: " + ec.message();
        }
        return false;
    }
}

bool removeReservedProjectDirectory(const fs::path& projectDir,
                                    std::string* errorOut)
{
    if (projectDir.empty() || projectDir.parent_path() != projectsDirPath()) {
        appendError(errorOut, "refusing to remove an invalid reserved project directory");
        return false;
    }
    if (consumeProjectIoFailure(
            jcut::testing::ImGuiProjectIoFailurePoint::
                ReservedProjectDirectoryCleanup)) {
        appendError(errorOut, "injected reserved-project directory cleanup failure");
        return false;
    }
    std::error_code removeError;
    fs::remove_all(projectDir, removeError);
    if (removeError) {
        appendError(
            errorOut,
            "failed to remove incomplete project directory: " +
                removeError.message());
        return false;
    }
    return true;
}

jcut::ImGuiProjectSession projectSessionFor(
    const std::string& projectId,
    const jcut::EditorDocumentCore& document,
    const json& legacyStateRoot)
{
    const fs::path root = rootDirPath();
    const fs::path projectDir = root / "projects" / projectId;
    jcut::ImGuiProjectSession session;
    session.document = document;
    session.projectId = projectId;
    session.statePath = pathString(projectDir / "state.json");
    session.historyPath = pathString(projectDir / "history.json");
    session.rootDirPath = pathString(root);
    session.mediaRootPath = resolvePathForRoot(
        stringValue(legacyStateRoot, "mediaRoot").empty()
            ? stringValue(legacyStateRoot, "explorerRoot")
            : stringValue(legacyStateRoot, "mediaRoot"),
        root);
    if (session.mediaRootPath.empty()) {
        session.mediaRootPath = session.rootDirPath;
    }
    session.legacyStateRoot = legacyStateRoot.is_object()
        ? legacyStateRoot
        : json::object();
    return session;
}

std::optional<jcut::ImGuiProjectSession> loadManagedProjectSession(
    const std::string& projectId,
    bool requireActiveProject,
    std::string* errorOut)
{
    jcut::ImGuiProjectSession session = projectSessionFor(
        projectId, jcut::EditorDocumentCore{}, json::object());
    if (!sessionBelongsToProjectStore(
            session, requireActiveProject, errorOut)) {
        return std::nullopt;
    }

    const std::optional<json> loadedRoot = loadProjectRoot(
        session.statePath, session.historyPath, errorOut);
    if (!loadedRoot.has_value()) {
        return std::nullopt;
    }
    std::string parseError;
    const std::optional<jcut::EditorDocumentCore> document =
        jcut::editorDocumentCoreFromJson(*loadedRoot, &parseError);
    if (!document.has_value()) {
        if (errorOut) {
            *errorOut = parseError.empty()
                ? "failed to parse project state"
                : parseError;
        }
        return std::nullopt;
    }
    return projectSessionFor(projectId, *document, *loadedRoot);
}

std::optional<jcut::ImGuiProjectSession> loadResultingProjectSession(
    const std::string& expectedProjectId,
    const std::string& previousProjectId,
    const fs::path& newProjectDir,
    std::string* errorOut)
{
    std::string loadError;
    std::optional<jcut::ImGuiProjectSession> result;
    if (consumeProjectIoFailure(
            jcut::testing::ImGuiProjectIoFailurePoint::
                ResultingProjectSessionLoad)) {
        loadError = "injected resulting project session load failure";
    } else {
        result = loadManagedProjectSession(expectedProjectId, true, &loadError);
    }
    if (result.has_value() && result->projectId == expectedProjectId) {
        return result;
    }

    if (errorOut) {
        *errorOut = loadError.empty()
            ? "new project did not become active"
            : "failed to load new project: " + loadError;
    }

    std::string markerRollbackError;
    const bool markerRolledBack = writeTextAtomically(
        currentProjectMarkerPath(), previousProjectId, &markerRollbackError);
    if (markerRolledBack) {
        removeReservedProjectDirectory(newProjectDir, errorOut);
    } else {
        // Atomic marker replacement leaves the new ID active on failure.
        // Retain its directory so the marker never points at deleted data.
        appendError(
            errorOut,
            "failed to restore previous project marker; retained new project: " +
                markerRollbackError);
    }
    return std::nullopt;
}

} // namespace

namespace jcut {

namespace testing {

void injectNextImGuiProjectIoFailure(ImGuiProjectIoFailurePoint point)
{
    armProjectIoFailure(point);
}

void clearImGuiProjectIoFailures()
{
    g_projectIoFailurePoints.store(0, std::memory_order_relaxed);
}

} // namespace testing

std::vector<std::string> availableImGuiProjectIds(std::string* errorOut)
{
    if (errorOut) {
        errorOut->clear();
    }
    if (!createProjectsDirectory(errorOut)) {
        return {};
    }
    projectio::ScopedProjectSaveLock lifecycleLock;
    if (!lifecycleLock.acquire(projectsDirPath(), errorOut)) {
        return {};
    }
    return availableProjectIds(errorOut);
}

std::optional<ImGuiProjectSession> activateImGuiProjectSession(
    const std::string& projectId,
    std::string* errorOut)
{
    if (errorOut) {
        errorOut->clear();
    }
    const std::string normalized = trim(projectId);
    if (!safeProjectIdComponent(normalized) ||
        !createProjectsDirectory(errorOut)) {
        if (errorOut && errorOut->empty()) {
            *errorOut = "project has an invalid project ID";
        }
        return std::nullopt;
    }

    projectio::ScopedProjectSaveLock lifecycleLock;
    if (!lifecycleLock.acquire(projectsDirPath(), errorOut)) {
        return std::nullopt;
    }
    std::string listError;
    const std::vector<std::string> ids = availableProjectIds(&listError);
    if (!listError.empty()) {
        if (errorOut) {
            *errorOut = listError;
        }
        return std::nullopt;
    }
    if (std::find(ids.begin(), ids.end(), normalized) == ids.end()) {
        if (errorOut) {
            *errorOut = "project not found: " + normalized;
        }
        return std::nullopt;
    }

    std::string loadError;
    std::optional<ImGuiProjectSession> loaded =
        loadManagedProjectSession(normalized, false, &loadError);
    if (!loaded.has_value()) {
        if (errorOut) {
            *errorOut = "project cannot be activated: " + loadError;
        }
        return std::nullopt;
    }
    if (!commitActiveProjectMarker(normalized, errorOut)) {
        return std::nullopt;
    }
    // The fully parsed session is prepared before the marker commit, so callers
    // can adopt it in the same UI action without a marker-only intermediate state.
    return loaded;
}

bool setActiveImGuiProject(const std::string& projectId, std::string* errorOut)
{
    return activateImGuiProjectSession(projectId, errorOut).has_value();
}

std::optional<ImGuiProjectSession> loadActiveImGuiProjectSession(std::string* errorOut)
{
    if (errorOut) {
        errorOut->clear();
    }
    if (!createProjectsDirectory(errorOut)) {
        return std::nullopt;
    }
    projectio::ScopedProjectSaveLock lifecycleLock;
    if (!lifecycleLock.acquire(projectsDirPath(), errorOut)) {
        return std::nullopt;
    }
    const std::string projectId = currentProjectId();
    return loadManagedProjectSession(projectId, true, errorOut);
}

bool saveImGuiProjectSessionImpl(
    const ImGuiProjectSession& session,
    const EditorDocumentCore& document,
    std::string* errorOut,
    bool lifecycleLockAlreadyHeld,
    bool requireActiveManagedSession)
{
    if (trim(session.statePath).empty() || trim(session.historyPath).empty()) {
        if (errorOut) {
            *errorOut = "project session paths are empty";
        }
        return false;
    }

    const bool managedSession = sessionUsesManagedProjectStore(session);
    projectio::ScopedProjectSaveLock lifecycleLock;
    if (managedSession) {
        if (!createProjectsDirectory(errorOut)) {
            return false;
        }
        if (!lifecycleLockAlreadyHeld &&
            !lifecycleLock.acquire(projectsDirPath(), errorOut)) {
            return false;
        }
        // Revalidate only after lifecycle serialization is held. A save that
        // waited behind rename must fail here rather than recreate the old path.
        if (!sessionBelongsToProjectStore(
                session, requireActiveManagedSession, errorOut)) {
            return false;
        }
    } else {
        std::error_code ec;
        fs::create_directories(fs::path(session.statePath).parent_path(), ec);
        fs::create_directories(fs::path(session.historyPath).parent_path(), ec);
        if (ec) {
            if (errorOut) {
                *errorOut = "failed to create project directory: " + ec.message();
            }
            return false;
        }
    }

    jcut::projectio::ScopedProjectSaveLock saveLock;
    if (!saveLock.acquire(fs::path(session.historyPath).parent_path(), errorOut)) {
        return false;
    }

    json stateRoot;
    json historyRoot;
    std::string statePayload;
    std::string historyPayload;
    try {
        json onDiskState;
        std::string onDiskStateError;
        const JsonObjectFileStatus onDiskStateStatus = loadJsonObjectFile(
            session.statePath, &onDiskState, &onDiskStateError);
        if (onDiskStateStatus == JsonObjectFileStatus::Invalid) {
            // A session recovered from history is allowed to repair malformed
            // state. Otherwise fail closed instead of overwriting unreadable data.
            ProjectHistory recoveryHistory;
            std::string recoveryError;
            if (!loadProjectHistory(session.historyPath, &recoveryHistory, &recoveryError) ||
                recoveryHistory.entries.empty() ||
                sanitizedHistorySnapshot(
                    recoveryHistory.entries.at(recoveryHistory.index)) !=
                    sanitizedHistorySnapshot(session.legacyStateRoot)) {
                if (errorOut) {
                    *errorOut = onDiskStateError;
                }
                return false;
            }
        }
        if (onDiskStateStatus == JsonObjectFileStatus::Valid && !onDiskState.empty() &&
            session.legacyStateRoot.is_object() && !session.legacyStateRoot.empty() &&
            onDiskState != session.legacyStateRoot) {
            if (errorOut) {
                *errorOut = "project changed on disk; reload before saving";
            }
            return false;
        }

        const json* preservationRoot =
            onDiskStateStatus == JsonObjectFileStatus::Valid && !onDiskState.empty()
            ? &onDiskState
            : &session.legacyStateRoot;
        stateRoot = toLegacyStateJson(document, preservationRoot);
        if (session.legacyStateOverrides.is_object() &&
            !session.legacyStateOverrides.empty()) {
            stateRoot.merge_patch(session.legacyStateOverrides);
        }
        // Validate and merge history before state.json is changed. A damaged
        // history file must never be silently replaced by a one-entry stack.
        if (!updatedProjectHistory(session.historyPath, stateRoot, &historyRoot, errorOut)) {
            return false;
        }
        statePayload = stateRoot.dump(2) + "\n";
        historyPayload = historyRoot.dump(2) + "\n";
    } catch (const std::exception& exception) {
        if (errorOut) {
            *errorOut = "failed to prepare project files for save: " + std::string(exception.what());
        }
        return false;
    }

    return commitPairedProjectFiles(
        session.statePath,
        session.historyPath,
        statePayload,
        historyPayload,
        errorOut);
}

bool saveImGuiProjectSession(
    const ImGuiProjectSession& session,
    const EditorDocumentCore& document,
    std::string* errorOut)
{
    if (errorOut) {
        errorOut->clear();
    }
    return saveImGuiProjectSessionImpl(
        session, document, errorOut, false, true);
}

bool writeImGuiProjectAutosave(
    const ImGuiProjectSession& session,
    const EditorDocumentCore& document,
    int maxBackups,
    std::string* backupPathOut,
    std::string* errorOut)
{
    if (backupPathOut) backupPathOut->clear();
    if (errorOut) errorOut->clear();
    if (trim(session.statePath).empty()) {
        if (errorOut) *errorOut = "project state path is empty";
        return false;
    }
    const fs::path projectDirectory =
        fs::path(session.statePath).parent_path();
    std::error_code error;
    fs::create_directories(projectDirectory, error);
    if (error) {
        if (errorOut) {
            *errorOut = "failed to create autosave directory: " +
                error.message();
        }
        return false;
    }

    projectio::ScopedProjectSaveLock saveLock;
    if (!saveLock.acquire(projectDirectory, errorOut)) {
        return false;
    }

    json stateRoot;
    try {
        const json* preservationRoot =
            session.legacyStateRoot.is_object()
            ? &session.legacyStateRoot
            : nullptr;
        stateRoot = toLegacyStateJson(document, preservationRoot);
        if (session.legacyStateOverrides.is_object() &&
            !session.legacyStateOverrides.empty()) {
            stateRoot.merge_patch(session.legacyStateOverrides);
        }
    } catch (const std::exception& exception) {
        if (errorOut) {
            *errorOut = "failed to prepare autosave: " +
                std::string(exception.what());
        }
        return false;
    }

    const std::time_t now = std::time(nullptr);
    std::tm localTime{};
    localtime_r(&now, &localTime);
    std::ostringstream name;
    name << "state_backup_"
         << std::put_time(&localTime, "%Y-%m-%d_%H-%M-%S")
         << ".json";
    const fs::path backupPath = projectDirectory / name.str();
    if (!writeTextAtomically(
            backupPath, stateRoot.dump(2) + "\n", errorOut)) {
        return false;
    }

    std::vector<fs::path> backups;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(projectDirectory, error)) {
        if (error) break;
        if (!entry.is_regular_file(error) || error) continue;
        const std::string fileName = entry.path().filename().string();
        if (fileName.starts_with("state_backup_") &&
            entry.path().extension() == ".json") {
            backups.push_back(entry.path());
        }
    }
    if (error) {
        if (errorOut) {
            *errorOut = "failed to enumerate autosave backups: " +
                error.message();
        }
        return false;
    }
    std::sort(backups.begin(), backups.end());
    const std::size_t keep = static_cast<std::size_t>(
        std::clamp(maxBackups, 1, 200));
    while (backups.size() > keep) {
        fs::remove(backups.front(), error);
        if (error) {
            if (errorOut) {
                *errorOut = "failed to trim autosave backup: " +
                    error.message();
            }
            return false;
        }
        backups.erase(backups.begin());
    }
    if (backupPathOut) *backupPathOut = backupPath.string();
    return true;
}

std::optional<std::vector<ImGuiProjectHistoryEntry>>
listImGuiProjectHistoryEntries(
    const ImGuiProjectSession& session,
    std::string* errorOut)
{
    if (errorOut) {
        errorOut->clear();
    }
    if (!sessionUsesManagedProjectStore(session)) {
        if (errorOut) {
            *errorOut = "history navigation requires a managed project session";
        }
        return std::nullopt;
    }
    if (!createProjectsDirectory(errorOut)) {
        return std::nullopt;
    }

    projectio::ScopedProjectSaveLock lifecycleLock;
    if (!lifecycleLock.acquire(projectsDirPath(), errorOut) ||
        !sessionBelongsToProjectStore(session, false, errorOut) ||
        !activeProjectMarkerMatchesSessionWithoutRepair(session, errorOut)) {
        return std::nullopt;
    }
    projectio::ScopedProjectSaveLock saveLock;
    if (!saveLock.acquire(
            fs::path(session.historyPath).parent_path(), errorOut)) {
        return std::nullopt;
    }

    ProjectHistory history;
    if (!loadProjectHistory(session.historyPath, &history, errorOut) ||
        !activeSessionRevisionMatchesProjectFiles(session, history, errorOut)) {
        return std::nullopt;
    }

    std::vector<ImGuiProjectHistoryEntry> entries;
    entries.reserve(history.entries.size());
    for (std::size_t index = 0; index < history.entries.size(); ++index) {
        const json snapshot = sanitizedHistorySnapshot(
            history.entries.at(index));
        std::string parseError;
        const std::optional<EditorDocumentCore> document =
            editorDocumentCoreFromJson(snapshot, &parseError);
        if (!document.has_value()) {
            if (errorOut) {
                *errorOut = "failed to parse project history entry " +
                    std::to_string(index) + ": " + parseError;
            }
            return std::nullopt;
        }
        entries.push_back({
            index,
            index == history.index,
            document->projectName,
            document->transport.currentFrame,
            document->clips.size(),
        });
    }
    return entries;
}

std::optional<ImGuiProjectSession> activateImGuiProjectHistoryEntry(
    const ImGuiProjectSession& session,
    std::size_t historyIndex,
    std::string* errorOut)
{
    if (errorOut) {
        errorOut->clear();
    }
    if (!sessionUsesManagedProjectStore(session)) {
        if (errorOut) {
            *errorOut = "history navigation requires a managed project session";
        }
        return std::nullopt;
    }
    if (!createProjectsDirectory(errorOut)) {
        return std::nullopt;
    }

    projectio::ScopedProjectSaveLock lifecycleLock;
    if (!lifecycleLock.acquire(projectsDirPath(), errorOut) ||
        !sessionBelongsToProjectStore(session, false, errorOut) ||
        !activeProjectMarkerMatchesSessionWithoutRepair(session, errorOut)) {
        return std::nullopt;
    }
    projectio::ScopedProjectSaveLock saveLock;
    if (!saveLock.acquire(
            fs::path(session.historyPath).parent_path(), errorOut)) {
        return std::nullopt;
    }

    ProjectHistory history;
    if (!loadProjectHistory(session.historyPath, &history, errorOut) ||
        !activeSessionRevisionMatchesProjectFiles(session, history, errorOut)) {
        return std::nullopt;
    }
    if (historyIndex >= history.entries.size()) {
        if (errorOut) {
            *errorOut = "project history index is out of range";
        }
        return std::nullopt;
    }

    if (historyIndex == history.index) {
        std::string parseError;
        const std::optional<EditorDocumentCore> currentDocument =
            editorDocumentCoreFromJson(session.legacyStateRoot, &parseError);
        if (!currentDocument.has_value()) {
            if (errorOut) {
                *errorOut = "failed to parse active project history entry: " +
                    parseError;
            }
            return std::nullopt;
        }
        return projectSessionFor(
            session.projectId, *currentDocument, session.legacyStateRoot);
    }

    const json stateRoot = sanitizedHistorySnapshot(
        history.entries.at(historyIndex));
    std::string parseError;
    const std::optional<EditorDocumentCore> document =
        editorDocumentCoreFromJson(stateRoot, &parseError);
    if (!document.has_value()) {
        if (errorOut) {
            *errorOut = "failed to parse project history entry " +
                std::to_string(historyIndex) + ": " + parseError;
        }
        return std::nullopt;
    }

    json historyRoot = history.root;
    historyRoot["index"] = static_cast<std::int64_t>(historyIndex);
    const std::string statePayload = stateRoot.dump(2) + "\n";
    const std::string historyPayload = historyRoot.dump(2) + "\n";
    if (!commitPairedProjectFiles(
            session.statePath,
            session.historyPath,
            statePayload,
            historyPayload,
            errorOut)) {
        return std::nullopt;
    }
    return projectSessionFor(
        session.projectId, *document, stateRoot);
}

std::optional<ImGuiProjectSession> createImGuiProjectSession(
    const std::string& projectName,
    std::string* errorOut)
{
    if (errorOut) {
        errorOut->clear();
    }
    std::string cleanedName;
    if (!validProjectName(projectName, &cleanedName, errorOut) ||
        !createProjectsDirectory(errorOut)) {
        return std::nullopt;
    }

    projectio::ScopedProjectSaveLock lifecycleLock;
    if (!lifecycleLock.acquire(projectsDirPath(), errorOut)) {
        return std::nullopt;
    }
    const std::string previousProjectId = currentProjectId();

    std::string newProjectId;
    fs::path newProjectDir;
    if (!reserveUniqueProjectDirectory(
            sanitizedProjectIdBase(cleanedName),
            &newProjectId,
            &newProjectDir,
            errorOut)) {
        return std::nullopt;
    }

    EditorDocumentCore document =
        editorDocumentCoreFromJson(json::object()).value_or(EditorDocumentCore{});
    document.projectName = cleanedName;
    ImGuiProjectSession newSession = projectSessionFor(
        newProjectId, document, json::object());
    if (!saveImGuiProjectSessionImpl(
            newSession, document, errorOut, true, false)) {
        removeReservedProjectDirectory(newProjectDir, errorOut);
        return std::nullopt;
    }
    if (!commitActiveProjectMarker(newProjectId, errorOut)) {
        removeReservedProjectDirectory(newProjectDir, errorOut);
        return std::nullopt;
    }
    return loadResultingProjectSession(
        newProjectId, previousProjectId, newProjectDir, errorOut);
}

std::optional<ImGuiProjectSession> saveImGuiProjectSessionAs(
    const ImGuiProjectSession& sourceSession,
    const EditorDocumentCore& document,
    const std::string& projectName,
    std::string* errorOut)
{
    if (errorOut) {
        errorOut->clear();
    }
    std::string cleanedName;
    if (!validProjectName(projectName, &cleanedName, errorOut) ||
        !createProjectsDirectory(errorOut)) {
        return std::nullopt;
    }

    projectio::ScopedProjectSaveLock lifecycleLock;
    if (!lifecycleLock.acquire(projectsDirPath(), errorOut) ||
        !sessionBelongsToProjectStore(sourceSession, true, errorOut)) {
        return std::nullopt;
    }
    const std::string previousProjectId = sourceSession.projectId;

    projectio::ScopedProjectSaveLock sourceSaveLock;
    const fs::path sourceProjectDir = fs::path(sourceSession.statePath).parent_path();
    if (!sourceSaveLock.acquire(sourceProjectDir, errorOut)) {
        return std::nullopt;
    }

    json sourceState;
    std::string sourceStateError;
    const JsonObjectFileStatus sourceStateStatus = loadJsonObjectFile(
        sourceSession.statePath, &sourceState, &sourceStateError);
    if (sourceStateStatus == JsonObjectFileStatus::Invalid) {
        if (errorOut) {
            *errorOut = sourceStateError;
        }
        return std::nullopt;
    }
    if (sourceStateStatus == JsonObjectFileStatus::Valid && !sourceState.empty() &&
        sourceSession.legacyStateRoot.is_object() &&
        !sourceSession.legacyStateRoot.empty() &&
        sourceState != sourceSession.legacyStateRoot) {
        if (errorOut) {
            *errorOut = "project changed on disk; reload before saving as";
        }
        return std::nullopt;
    }

    ProjectHistory sourceHistory;
    if (!loadProjectHistory(sourceSession.historyPath, &sourceHistory, errorOut)) {
        return std::nullopt;
    }
    std::error_code historyExistsError;
    const bool sourceHistoryExists = fs::exists(
        sourceSession.historyPath, historyExistsError);
    if (historyExistsError) {
        if (errorOut) {
            *errorOut = "failed to inspect source project history: " +
                historyExistsError.message();
        }
        return std::nullopt;
    }
    const std::string sourceHistoryPayload = sourceHistoryExists
        ? readFile(sourceSession.historyPath)
        : std::string{};

    std::string newProjectId;
    fs::path newProjectDir;
    if (!reserveUniqueProjectDirectory(
            sanitizedProjectIdBase(cleanedName),
            &newProjectId,
            &newProjectDir,
            errorOut)) {
        return std::nullopt;
    }
    if (sourceHistoryExists &&
        !writeTextAtomically(
            newProjectDir / "history.json", sourceHistoryPayload, errorOut)) {
        removeReservedProjectDirectory(newProjectDir, errorOut);
        return std::nullopt;
    }

    const json& preservationRoot =
        sourceStateStatus == JsonObjectFileStatus::Valid && !sourceState.empty()
        ? sourceState
        : sourceSession.legacyStateRoot;
    ImGuiProjectSession newSession = projectSessionFor(
        newProjectId, document, preservationRoot);
    newSession.legacyStateOverrides = sourceSession.legacyStateOverrides;
    if (!saveImGuiProjectSessionImpl(
            newSession, document, errorOut, true, false)) {
        removeReservedProjectDirectory(newProjectDir, errorOut);
        return std::nullopt;
    }
    if (!commitActiveProjectMarker(newProjectId, errorOut)) {
        removeReservedProjectDirectory(newProjectDir, errorOut);
        return std::nullopt;
    }
    return loadResultingProjectSession(
        newProjectId, previousProjectId, newProjectDir, errorOut);
}

std::optional<ImGuiProjectSession> renameImGuiProjectSession(
    const ImGuiProjectSession& sourceSession,
    const std::string& projectName,
    std::string* errorOut)
{
    if (errorOut) {
        errorOut->clear();
    }
    std::string cleanedName;
    if (!validProjectName(projectName, &cleanedName, errorOut) ||
        !createProjectsDirectory(errorOut)) {
        return std::nullopt;
    }

    projectio::ScopedProjectSaveLock lifecycleLock;
    if (!lifecycleLock.acquire(projectsDirPath(), errorOut) ||
        !sessionBelongsToProjectStore(sourceSession, true, errorOut)) {
        return std::nullopt;
    }

    const fs::path sourceProjectDir = projectsDirPath() / sourceSession.projectId;
    // Managed saves acquire the lifecycle lock before their per-project lock.
    // Holding it here therefore excludes every managed writer without keeping
    // a file handle inside the directory open across a Windows rename.
    std::optional<ImGuiProjectSession> currentSession =
        loadManagedProjectSession(sourceSession.projectId, true, errorOut);
    if (!currentSession.has_value()) {
        return std::nullopt;
    }

    const std::string baseProjectId = sanitizedProjectIdBase(cleanedName);
    if (baseProjectId == sourceSession.projectId) {
        return currentSession;
    }
    const std::optional<std::string> uniqueRenamedProjectId =
        uniqueProjectId(baseProjectId, errorOut);
    if (!uniqueRenamedProjectId.has_value()) {
        return std::nullopt;
    }
    const std::string& renamedProjectId = *uniqueRenamedProjectId;
    const fs::path renamedProjectDir = projectsDirPath() / renamedProjectId;
    ImGuiProjectSession renamedSession = projectSessionFor(
        renamedProjectId,
        currentSession->document,
        currentSession->legacyStateRoot);

    std::error_code renameError;
    if (consumeProjectIoFailure(
            testing::ImGuiProjectIoFailurePoint::ProjectDirectoryRename)) {
        renameError = std::make_error_code(std::errc::io_error);
    } else {
        fs::rename(sourceProjectDir, renamedProjectDir, renameError);
    }
    if (renameError) {
        if (errorOut) {
            *errorOut = "failed to rename project directory: " + renameError.message();
        }
        return std::nullopt;
    }

    std::string markerError;
    if (!commitActiveProjectMarker(renamedProjectId, &markerError)) {
        std::error_code rollbackError;
        if (consumeProjectIoFailure(
                testing::ImGuiProjectIoFailurePoint::
                    ProjectDirectoryRenameRollback)) {
            rollbackError = std::make_error_code(std::errc::io_error);
        } else {
            fs::rename(renamedProjectDir, sourceProjectDir, rollbackError);
        }
        if (!rollbackError) {
            if (errorOut) {
                *errorOut = markerError;
            }
            return std::nullopt;
        }

        // Directory rollback failed, so settle on the renamed identity when
        // its marker can be committed and return that committed session.
        std::string markerRecoveryError;
        bool markerRecovered = false;
        if (consumeProjectIoFailure(
                testing::ImGuiProjectIoFailurePoint::
                    RenamedProjectMarkerRecoveryCommit)) {
            markerRecoveryError =
                "injected renamed-project marker recovery commit failure";
        } else {
            markerRecovered = writeTextAtomically(
                currentProjectMarkerPath(), renamedProjectId, &markerRecoveryError);
        }
        if (markerRecovered) {
            return renamedSession;
        }

        // Both ways of committing the rename failed. A transient first
        // directory-rollback failure may have cleared, so make one final
        // attempt to restore the original identity before conceding an
        // inconsistent on-disk state.
        std::error_code finalRollbackError;
        fs::rename(renamedProjectDir, sourceProjectDir, finalRollbackError);
        if (!finalRollbackError) {
            if (errorOut) {
                *errorOut = markerError +
                    "; initial project rename rollback failed: " +
                    rollbackError.message() +
                    "; failed to activate committed rename: " +
                    markerRecoveryError +
                    "; restored original project directory";
            }
            return std::nullopt;
        }
        if (errorOut) {
            *errorOut = markerError +
                "; failed to roll back project rename: " +
                rollbackError.message() +
                "; failed to activate committed rename: " +
                markerRecoveryError +
                "; final project rename rollback failed: " +
                finalRollbackError.message();
        }
        return std::nullopt;
    }
    return renamedSession;
}

} // namespace jcut
