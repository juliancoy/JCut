#include "transcript_cut_session_core.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <utility>

namespace jcut {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string trimAscii(std::string value)
{
    const auto isSpace = [](unsigned char character) {
        return character == ' ' || character == '\t' || character == '\n' ||
            character == '\r' || character == '\f' || character == '\v';
    };
    const auto first = std::find_if_not(value.begin(), value.end(), isSpace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
    return first < last ? std::string(first, last) : std::string{};
}

fs::path absoluteNormalizedPath(const std::string& value,
                                const std::string& rootValue = {})
{
    const std::string trimmed = trimAscii(value);
    if (trimmed.empty()) {
        return {};
    }
    fs::path path(trimmed);
    std::error_code ec;
    if (path.is_relative()) {
        const fs::path root(trimAscii(rootValue));
        if (!root.empty() && fs::is_directory(root, ec) && !ec) {
            path = root / path;
        }
        ec.clear();
        const fs::path absolute = fs::absolute(path, ec);
        if (!ec) {
            path = absolute;
        }
    }
    if (ec) {
        return path.lexically_normal();
    }
    return path.lexically_normal();
}

bool regularFileExists(const fs::path& path)
{
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return fs::is_regular_file(path, ec) && !ec;
}

std::string readFileBytes(const fs::path& path, std::string* errorOut)
{
    if (errorOut) {
        errorOut->clear();
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (errorOut) {
            *errorOut = "Could not open transcript: " + path.string();
        }
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

std::optional<TranscriptDocumentCore> loadDocument(const fs::path& path,
                                                   std::string* errorOut)
{
    std::string readError;
    const std::string bytes = readFileBytes(path, &readError);
    if (!readError.empty()) {
        if (errorOut) {
            *errorOut = std::move(readError);
        }
        return std::nullopt;
    }
    return TranscriptDocumentCore::fromJsonBytes(bytes, errorOut);
}

std::string customCutLabel(const fs::path& path)
{
    std::string error;
    const std::string bytes = readFileBytes(path, &error);
    if (!error.empty()) {
        return {};
    }
    try {
        const json root = json::parse(bytes);
        const auto label = root.find("cut_label");
        return label != root.end() && label->is_string()
            ? trimAscii(label->get<std::string>())
            : std::string{};
    } catch (const json::exception&) {
        return {};
    }
}

std::optional<int> versionNumber(const std::string& fileName,
                                 const std::string& prefix)
{
    constexpr std::string_view suffix = ".json";
    if (fileName.size() <= prefix.size() + suffix.size() ||
        fileName.compare(0, prefix.size(), prefix) != 0 ||
        fileName.compare(fileName.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return std::nullopt;
    }
    const std::string_view number(
        fileName.data() + prefix.size(),
        fileName.size() - prefix.size() - suffix.size());
    int parsed = 0;
    const auto result = std::from_chars(number.data(), number.data() + number.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != number.data() + number.size() || parsed < 0) {
        return std::nullopt;
    }
    return parsed;
}

TranscriptFileStamp fileStamp(const fs::path& path)
{
    TranscriptFileStamp stamp;
    std::error_code ec;
    stamp.exists = fs::is_regular_file(path, ec) && !ec;
    if (!stamp.exists) {
        return stamp;
    }
    stamp.size = fs::file_size(path, ec);
    if (ec) {
        stamp.size = 0;
        ec.clear();
    }
    const fs::file_time_type writeTime = fs::last_write_time(path, ec);
    if (!ec) {
        stamp.writeTimeTicks = static_cast<std::int64_t>(
            writeTime.time_since_epoch().count());
    }
    return stamp;
}

bool samePath(const std::string& lhs, const std::string& rhs)
{
    return !lhs.empty() && !rhs.empty() &&
        absoluteNormalizedPath(lhs) == absoluteNormalizedPath(rhs);
}

std::string selectActivePath(const TranscriptCutCatalog& catalog,
                             const std::string& requestedPath,
                             const std::string& sourceRootPath)
{
    const fs::path normalizedRequest = absoluteNormalizedPath(
        requestedPath, sourceRootPath);
    if (!normalizedRequest.empty()) {
        const auto requested = std::find_if(
            catalog.cuts.begin(), catalog.cuts.end(),
            [&](const TranscriptCutEntry& entry) {
                return absoluteNormalizedPath(entry.path) == normalizedRequest;
            });
        if (requested != catalog.cuts.end()) {
            return requested->path;
        }
    }
    if (regularFileExists(catalog.editablePath)) {
        return catalog.editablePath;
    }
    if (regularFileExists(catalog.originalPath)) {
        return catalog.originalPath;
    }
    return catalog.cuts.empty() ? std::string{} : catalog.cuts.front().path;
}

} // namespace

TranscriptSourceIdentity resolveTranscriptSourceIdentity(
    const TranscriptSourceSpec& source)
{
    const std::string audioPath = trimAscii(source.audioSourcePath);
    const bool useAudioPath = !audioPath.empty() &&
        (trimAscii(source.audioSourceStatus) == "ok" ||
         trimAscii(source.audioSourceMode) == "explicit_file" ||
         trimAscii(source.audioSourceMode) == "embedded");
    const fs::path resolved = absoluteNormalizedPath(
        useAudioPath ? audioPath : source.sourcePath,
        source.sourceRootPath);

    TranscriptSourceIdentity identity;
    if (resolved.empty()) {
        return identity;
    }
    identity.sourcePath = resolved.string();
    identity.audioStreamIndex = source.audioStreamIndex;
    identity.canonicalKey = identity.sourcePath;
    if (identity.audioStreamIndex >= 0) {
        identity.canonicalKey += "::audio_stream=" +
            std::to_string(identity.audioStreamIndex);
    }
    identity.fileStem = resolved.stem().string();
    if (identity.audioStreamIndex >= 0) {
        identity.fileStem += "_audio_stream_" +
            std::to_string(identity.audioStreamIndex);
    }
    return identity;
}

TranscriptCutCatalog discoverTranscriptCutCatalog(
    const TranscriptSourceSpec& source,
    bool ensureEditable)
{
    TranscriptCutCatalog catalog;
    catalog.source = resolveTranscriptSourceIdentity(source);
    if (!catalog.source.valid()) {
        catalog.warning = "Selected clip has no transcript source path.";
        return catalog;
    }

    const fs::path sourcePath(catalog.source.sourcePath);
    const fs::path directory = sourcePath.parent_path();
    catalog.originalPath = (directory / (catalog.source.fileStem + ".json")).string();
    catalog.editablePath = (directory /
        (catalog.source.fileStem + "_editable.json")).string();

    if (ensureEditable && !regularFileExists(catalog.editablePath) &&
        regularFileExists(catalog.originalPath)) {
        std::error_code ec;
        fs::copy_file(catalog.originalPath, catalog.editablePath,
                      fs::copy_options::none, ec);
        if (ec && !regularFileExists(catalog.editablePath)) {
            catalog.warning = "Could not create editable transcript: " + ec.message();
        }
    }

    catalog.workingPath = regularFileExists(catalog.editablePath)
        ? catalog.editablePath
        : catalog.originalPath;
    if (regularFileExists(catalog.originalPath)) {
        catalog.cuts.push_back({
            catalog.originalPath,
            "Original (Immutable)",
            TranscriptCutKind::Original,
            0});
    }
    if (regularFileExists(catalog.editablePath)) {
        std::string label = customCutLabel(catalog.editablePath);
        catalog.cuts.push_back({
            catalog.editablePath,
            label.empty() ? "Cut 1" : std::move(label),
            TranscriptCutKind::Editable,
            1});
    }

    std::vector<TranscriptCutEntry> versions;
    const std::string prefix = catalog.source.fileStem + "_editable_v";
    std::error_code ec;
    if (fs::is_directory(directory, ec) && !ec) {
        fs::directory_iterator iterator(directory, ec);
        const fs::directory_iterator end;
        while (!ec && iterator != end) {
            const fs::directory_entry& entry = *iterator;
            std::error_code fileError;
            if (entry.is_regular_file(fileError) && !fileError) {
                const std::string name = entry.path().filename().string();
                const std::optional<int> version = versionNumber(name, prefix);
                if (version.has_value()) {
                    std::string label = customCutLabel(entry.path());
                    versions.push_back({
                        entry.path().lexically_normal().string(),
                        label.empty()
                            ? "Cut " + std::to_string(*version)
                            : std::move(label),
                        TranscriptCutKind::Version,
                        *version});
                }
            }
            iterator.increment(ec);
        }
    }
    std::sort(versions.begin(), versions.end(),
              [](const TranscriptCutEntry& lhs, const TranscriptCutEntry& rhs) {
                  if (lhs.version != rhs.version) {
                      return lhs.version < rhs.version;
                  }
                  return lhs.path < rhs.path;
              });
    catalog.cuts.insert(catalog.cuts.end(), versions.begin(), versions.end());
    return catalog;
}

TranscriptFileStamp inspectTranscriptFile(const std::string& path)
{
    return fileStamp(absoluteNormalizedPath(path));
}

std::int64_t transcriptCatalogDirectoryWriteTime(
    const TranscriptSourceIdentity& source)
{
    if (!source.valid()) {
        return 0;
    }
    std::error_code ec;
    const fs::file_time_type writeTime = fs::last_write_time(
        fs::path(source.sourcePath).parent_path(), ec);
    return ec ? 0 : static_cast<std::int64_t>(
        writeTime.time_since_epoch().count());
}

TranscriptCutSession loadTranscriptCutSession(
    const TranscriptSourceSpec& source,
    const TranscriptCutSessionOptions& options)
{
    TranscriptCutSession session;
    session.catalog = discoverTranscriptCutCatalog(source, options.ensureEditable);
    const fs::path requestedPath = absoluteNormalizedPath(
        options.requestedActivePath, source.sourceRootPath);
    if (regularFileExists(requestedPath)) {
        const bool alreadyCataloged = std::any_of(
            session.catalog.cuts.begin(), session.catalog.cuts.end(),
            [&](const TranscriptCutEntry& cut) {
                return absoluteNormalizedPath(cut.path) == requestedPath;
            });
        if (!alreadyCataloged) {
            std::string label = customCutLabel(requestedPath);
            session.catalog.cuts.push_back({
                requestedPath.string(),
                label.empty() ? requestedPath.filename().string() : std::move(label),
                TranscriptCutKind::Custom,
                0});
        }
    }
    session.warning = session.catalog.warning;
    session.catalogDirectoryWriteTimeTicks =
        transcriptCatalogDirectoryWriteTime(session.catalog.source);
    session.cutStamps.reserve(session.catalog.cuts.size());
    for (const TranscriptCutEntry& cut : session.catalog.cuts) {
        session.cutStamps.push_back(inspectTranscriptFile(cut.path));
    }
    session.activePath = selectActivePath(
        session.catalog, options.requestedActivePath, source.sourceRootPath);
    if (session.activePath.empty()) {
        session.error = "No transcript file was found for the selected clip.";
        return session;
    }

    session.activeCutMutable = !samePath(
        session.activePath, session.catalog.originalPath);
    session.activeStamp = fileStamp(session.activePath);
    session.originalStamp = fileStamp(session.catalog.originalPath);
    session.activeDocument = loadDocument(session.activePath, &session.error);
    if (!session.activeDocument.has_value()) {
        if (session.error.empty()) {
            session.error = "Invalid transcript JSON file.";
        }
        return session;
    }

    TranscriptRowBuildOptions rowOptions;
    rowOptions.timing = options.timing;
    rowOptions.adjustOverlaps = true;
    rowOptions.insertGaps = true;
    const TranscriptDocumentCore* original = nullptr;
    if (options.includeOutsideActiveCut && session.activeCutMutable &&
        regularFileExists(session.catalog.originalPath)) {
        std::string originalError;
        session.originalDocument = loadDocument(
            session.catalog.originalPath, &originalError);
        if (session.originalDocument.has_value()) {
            rowOptions.includeOutsideActiveCut = true;
            original = &*session.originalDocument;
        } else if (!originalError.empty()) {
            if (!session.warning.empty()) {
                session.warning += " ";
            }
            session.warning += "Outside-cut rows unavailable: " + originalError;
        }
    }
    session.rows = session.activeDocument->rows(rowOptions, original);
    return session;
}

} // namespace jcut
