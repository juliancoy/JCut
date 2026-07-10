#include "imgui_project_io.h"

#include "editor_document_core_json.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

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
    const fs::path tempPath = path.string() + ".tmp";
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
            if (errorOut) {
                *errorOut = "failed to write temporary file: " + tempPath.string();
            }
            return false;
        }
    }
    fs::rename(tempPath, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tempPath, path, ec);
    }
    if (ec) {
        if (errorOut) {
            *errorOut = "failed to replace file: " + path.string();
        }
        return false;
    }
    return true;
}

std::vector<std::string> availableProjectIds()
{
    std::vector<std::string> ids;
    std::error_code ec;
    fs::create_directories(projectsDirPath(), ec);
    for (const fs::directory_entry& entry : fs::directory_iterator(projectsDirPath(), ec)) {
        if (entry.is_directory()) {
            ids.push_back(entry.path().filename().string());
        }
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

json loadJsonFile(const fs::path& path)
{
    const std::string bytes = readFile(path);
    if (bytes.empty()) {
        return json::object();
    }
    try {
        return json::parse(bytes);
    } catch (...) {
        return json::object();
    }
}

json loadProjectRoot(const fs::path& statePath, const fs::path& historyPath)
{
    const json stateRoot = loadJsonFile(statePath);
    if (stateRoot.is_object() && !stateRoot.empty()) {
        return stateRoot;
    }

    const json historyRoot = loadJsonFile(historyPath);
    if (!historyRoot.is_object()) {
        return json::object();
    }
    const json entries = historyRoot.value("entries", json::array());
    if (!entries.is_array() || entries.empty()) {
        return json::object();
    }
    int index = historyRoot.value("index", static_cast<int>(entries.size()) - 1);
    index = std::clamp(index, 0, static_cast<int>(entries.size()) - 1);
    return entries.at(static_cast<std::size_t>(index));
}

} // namespace

namespace jcut {

std::optional<ImGuiProjectSession> loadActiveImGuiProjectSession(std::string* errorOut)
{
    const fs::path rootDir = rootDirPath();
    const std::string projectId = currentProjectId();
    const fs::path projectDir = rootDir / "projects" / projectId;
    const fs::path statePath = projectDir / "state.json";
    const fs::path historyPath = projectDir / "history.json";

    const json root = loadProjectRoot(statePath, historyPath);
    std::string parseError;
    const std::optional<EditorDocumentCore> document = editorDocumentCoreFromJson(root, &parseError);
    if (!document.has_value()) {
        if (errorOut) {
            *errorOut = parseError.empty() ? "failed to parse active project state" : parseError;
        }
        return std::nullopt;
    }

    ImGuiProjectSession session;
    session.document = *document;
    session.projectId = projectId;
    session.statePath = pathString(statePath);
    session.historyPath = pathString(historyPath);
    session.rootDirPath = pathString(rootDir);
    session.mediaRootPath = resolvePathForRoot(
        stringValue(root, "mediaRoot").empty()
            ? stringValue(root, "explorerRoot")
            : stringValue(root, "mediaRoot"),
        rootDir);
    if (session.mediaRootPath.empty()) {
        session.mediaRootPath = session.rootDirPath;
    }
    session.legacyStateRoot = root.is_object() ? root : json::object();
    return session;
}

bool saveImGuiProjectSession(
    const ImGuiProjectSession& session,
    const EditorDocumentCore& document,
    std::string* errorOut)
{
    if (trim(session.statePath).empty() || trim(session.historyPath).empty()) {
        if (errorOut) {
            *errorOut = "project session paths are empty";
        }
        return false;
    }

    std::error_code ec;
    fs::create_directories(fs::path(session.statePath).parent_path(), ec);
    fs::create_directories(fs::path(session.historyPath).parent_path(), ec);

    const json stateRoot = toLegacyStateJson(document, &session.legacyStateRoot);
    if (!writeTextAtomically(session.statePath, stateRoot.dump(2) + "\n", errorOut)) {
        return false;
    }

    const json historyRoot = {
        {"index", 0},
        {"entries", json::array({stateRoot})}
    };
    return writeTextAtomically(session.historyPath, historyRoot.dump(2) + "\n", errorOut);
}

} // namespace jcut
