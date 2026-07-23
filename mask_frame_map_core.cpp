#include "mask_frame_map_core.h"

#include <nlohmann/json.hpp>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <unordered_map>

namespace jcut::masks {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kIdentityHashBytes = 1024 * 1024;
constexpr std::size_t kSourceHashChunkBytes = 4 * 1024 * 1024;
constexpr std::string_view kSourceIdentitySchema =
    "jcut_source_content_identity_v1";
constexpr std::size_t kMaxCachedSidecars = 16;

struct FileVersion {
    bool exists = false;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    std::uint32_t type = 0;
    std::int64_t modifiedNanoseconds = 0;
    std::int64_t changedNanoseconds = 0;

    bool operator==(const FileVersion& other) const
    {
        return exists == other.exists &&
            device == other.device &&
            inode == other.inode &&
            size == other.size &&
            type == other.type &&
            modifiedNanoseconds == other.modifiedNanoseconds &&
            changedNanoseconds == other.changedNanoseconds;
    }
};

FileVersion fileVersion(const std::filesystem::path& path)
{
    FileVersion result;
    if (path.empty()) return result;
    struct stat value {};
    if (::stat(path.c_str(), &value) != 0) return result;
    result.exists = true;
    result.device = static_cast<std::uint64_t>(value.st_dev);
    result.inode = static_cast<std::uint64_t>(value.st_ino);
    result.size = value.st_size >= 0 ? static_cast<std::uint64_t>(value.st_size) : 0;
    result.type = static_cast<std::uint32_t>(value.st_mode & S_IFMT);
#if defined(__APPLE__)
    result.modifiedNanoseconds =
        static_cast<std::int64_t>(value.st_mtimespec.tv_sec) * 1000000000LL +
        value.st_mtimespec.tv_nsec;
    result.changedNanoseconds =
        static_cast<std::int64_t>(value.st_ctimespec.tv_sec) * 1000000000LL +
        value.st_ctimespec.tv_nsec;
#else
    result.modifiedNanoseconds =
        static_cast<std::int64_t>(value.st_mtim.tv_sec) * 1000000000LL +
        value.st_mtim.tv_nsec;
    result.changedNanoseconds =
        static_cast<std::int64_t>(value.st_ctim.tv_sec) * 1000000000LL +
        value.st_ctim.tv_nsec;
#endif
    return result;
}

struct CacheFingerprint {
    FileVersion directory;
    FileVersion map;
    FileVersion metadata;
    FileVersion maskCompletion;
    FileVersion alphaCompletion;
    FileVersion source;

    bool stableFilesMatch(const CacheFingerprint& other) const
    {
        return map == other.map &&
            metadata == other.metadata &&
            maskCompletion == other.maskCompletion &&
            alphaCompletion == other.alphaCompletion &&
            source == other.source;
    }
};

CacheFingerprint cacheFingerprint(const std::filesystem::path& directory,
                                  const std::filesystem::path& sourceMediaPath)
{
    return CacheFingerprint{
        fileVersion(directory),
        fileVersion(directory / "jcut_frame_map.tsv"),
        fileVersion(directory / "jcut_frame_map.json"),
        fileVersion(directory / "jcut_mask.json"),
        fileVersion(directory / "jcut_alpha.json"),
        fileVersion(sourceMediaPath),
    };
}

struct CacheKey {
    std::string directory;
    std::string source;

    bool operator==(const CacheKey& other) const
    {
        return directory == other.directory && source == other.source;
    }
};

struct CacheKeyHash {
    std::size_t operator()(const CacheKey& key) const
    {
        const std::size_t directoryHash = std::hash<std::string>{}(key.directory);
        const std::size_t sourceHash = std::hash<std::string>{}(key.source);
        return directoryHash ^
            (sourceHash + 0x9e3779b97f4a7c15ULL + (directoryHash << 6) +
             (directoryHash >> 2));
    }
};

std::string normalizedCachePath(const std::filesystem::path& path)
{
    if (path.empty()) return {};
    std::error_code error;
    const std::filesystem::path absolute = path.is_absolute()
        ? path
        : std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal().string();
}

CacheKey cacheKey(const std::filesystem::path& directory,
                  const std::filesystem::path& sourceMediaPath)
{
    return CacheKey{
        normalizedCachePath(directory),
        normalizedCachePath(sourceMediaPath),
    };
}

struct CacheEntry {
    CacheFingerprint fingerprint;
    std::shared_ptr<const MaskFrameMapCore> map;
    bool directoryVersionRelevant = false;
    bool loading = false;
    std::uint64_t lastAccess = 0;
};

struct CacheState {
    std::mutex mutex;
    std::condition_variable changed;
    std::unordered_map<CacheKey, std::shared_ptr<CacheEntry>, CacheKeyHash> entries;
    std::uint64_t accessCounter = 0;
    std::uint64_t hitCount = 0;
    std::uint64_t missCount = 0;
    std::uint64_t validationCount = 0;
};

CacheState& cacheState()
{
    static CacheState state;
    return state;
}

bool cacheEntryMatches(const CacheEntry& entry,
                       const CacheFingerprint& fingerprint)
{
    return entry.map &&
        entry.fingerprint.stableFilesMatch(fingerprint) &&
        (!entry.directoryVersionRelevant ||
         entry.fingerprint.directory == fingerprint.directory);
}

void trimCacheLocked(CacheState& state, const CacheEntry* protectedEntry)
{
    while (state.entries.size() > kMaxCachedSidecars) {
        auto oldest = state.entries.end();
        for (auto candidate = state.entries.begin();
             candidate != state.entries.end();
             ++candidate) {
            const CacheEntry& entry = *candidate->second;
            if (&entry == protectedEntry || entry.loading) continue;
            if (oldest == state.entries.end() ||
                entry.lastAccess < oldest->second->lastAccess) {
                oldest = candidate;
            }
        }
        if (oldest == state.entries.end()) return;
        state.entries.erase(oldest);
    }
}

struct LoadedMaskFrameMap {
    std::shared_ptr<MaskFrameMapCore> map =
        std::make_shared<MaskFrameMapCore>();
    bool directoryVersionRelevant = false;
};

struct AvShaDeleter {
    void operator()(AVSHA* value) const { av_free(value); }
};

class Sha256 {
public:
    Sha256()
        : m_context(av_sha_alloc())
    {
        if (m_context) av_sha_init(m_context.get(), 256);
    }

    bool valid() const { return static_cast<bool>(m_context); }
    void add(const void* data, std::size_t size)
    {
        if (m_context && data && size > 0) {
            av_sha_update(m_context.get(),
                static_cast<const std::uint8_t*>(data), size);
        }
    }
    void add(std::string_view value) { add(value.data(), value.size()); }
    std::string finish()
    {
        if (!m_context) return {};
        std::array<std::uint8_t, 32> digest{};
        av_sha_final(m_context.get(), digest.data());
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const std::uint8_t byte : digest) output << std::setw(2) << static_cast<int>(byte);
        m_context.reset();
        return output.str();
    }

private:
    std::unique_ptr<AVSHA, AvShaDeleter> m_context;
};

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool containsInsensitive(const std::string& value, std::string_view needle)
{
    return lower(value).find(lower(std::string(needle))) != std::string::npos;
}

Json readJson(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) return Json::object();
    try {
        Json value;
        input >> value;
        return value.is_object() ? value : Json::object();
    } catch (...) {
        return Json::object();
    }
}

std::string stringValue(const Json& object, std::string_view key)
{
    const auto found = object.find(std::string(key));
    return found != object.end() && found->is_string()
        ? found->get<std::string>() : std::string{};
}

std::int64_t integerValue(const Json& object, std::string_view key,
                          std::int64_t fallback = -1)
{
    const auto found = object.find(std::string(key));
    if (found == object.end()) return fallback;
    if (found->is_number_integer()) return found->get<std::int64_t>();
    if (found->is_number_unsigned()) {
        const auto value = found->get<std::uint64_t>();
        return value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
            ? static_cast<std::int64_t>(value) : fallback;
    }
    return fallback;
}

bool booleanValue(const Json& object, std::string_view key, bool fallback = false)
{
    const auto found = object.find(std::string(key));
    return found != object.end() && found->is_boolean()
        ? found->get<bool>() : fallback;
}

bool isSha256(const std::string& value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(),
        [](unsigned char character) { return std::isxdigit(character) != 0; });
}

std::string sha256File(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    Sha256 digest;
    if (!input || !digest.valid()) return {};
    std::array<char, 64 * 1024> chunk{};
    while (input) {
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto count = input.gcount();
        if (count > 0) digest.add(chunk.data(), static_cast<std::size_t>(count));
    }
    return input.eof() ? digest.finish() : std::string{};
}

Json statFields(const std::filesystem::path& path)
{
    struct stat value {};
    if (::stat(path.c_str(), &value) != 0 || !S_ISREG(value.st_mode) || value.st_size < 0) {
        return Json::object();
    }
#if defined(__APPLE__)
    const std::int64_t modifiedNanoseconds =
        static_cast<std::int64_t>(value.st_mtimespec.tv_sec) * 1000000000LL + value.st_mtimespec.tv_nsec;
    const std::int64_t changedNanoseconds =
        static_cast<std::int64_t>(value.st_ctimespec.tv_sec) * 1000000000LL + value.st_ctimespec.tv_nsec;
#else
    const std::int64_t modifiedNanoseconds =
        static_cast<std::int64_t>(value.st_mtim.tv_sec) * 1000000000LL + value.st_mtim.tv_nsec;
    const std::int64_t changedNanoseconds =
        static_cast<std::int64_t>(value.st_ctim.tv_sec) * 1000000000LL + value.st_ctim.tv_nsec;
#endif
    return Json{{"size", static_cast<std::int64_t>(value.st_size)},
                {"mtime_ns", std::to_string(modifiedNanoseconds)},
                {"ctime_ns", std::to_string(changedNanoseconds)},
                {"device", std::to_string(static_cast<std::uint64_t>(value.st_dev))},
                {"inode", std::to_string(static_cast<std::uint64_t>(value.st_ino))}};
}

bool statFieldsMatch(const Json& left, const Json& right)
{
    if (integerValue(left, "size") < 0 ||
        integerValue(left, "size") != integerValue(right, "size")) return false;
    for (const char* key : {"mtime_ns", "ctime_ns", "device", "inode"}) {
        const std::string leftValue = stringValue(left, key);
        if (leftValue.empty() || leftValue != stringValue(right, key)) return false;
    }
    return true;
}

std::string portableContentHash(const Json& identity)
{
    if (stringValue(identity, "identity_schema") != kSourceIdentitySchema) return {};
    const std::string hash = lower(stringValue(identity, "content_sha256"));
    return isSha256(hash) ? hash : std::string{};
}

bool versionTokensMatch(const Json& left, const Json& right)
{
    const std::string leftEdge = lower(stringValue(left, "head_tail_sha256"));
    const std::string rightEdge = lower(stringValue(right, "head_tail_sha256"));
    const std::string leftMiddle = lower(stringValue(left, "middle_sha256"));
    const std::string rightMiddle = lower(stringValue(right, "middle_sha256"));
    return statFieldsMatch(left, right) && isSha256(leftEdge) && leftEdge == rightEdge &&
        isSha256(leftMiddle) && leftMiddle == rightMiddle;
}

bool identityObjectsMatch(const Json& left, const Json& right)
{
    for (const Json* identity : {&left, &right}) {
        const std::string schema = stringValue(*identity, "identity_schema");
        if (!schema.empty() && schema != kSourceIdentitySchema) return false;
    }
    const std::string leftHash = portableContentHash(left);
    const std::string rightHash = portableContentHash(right);
    if (!leftHash.empty() && !rightHash.empty()) {
        return integerValue(left, "size") >= 0 &&
            integerValue(left, "size") == integerValue(right, "size") &&
            leftHash == rightHash;
    }
    if ((!stringValue(left, "content_sha256").empty() && leftHash.empty()) ||
        (!stringValue(right, "content_sha256").empty() && rightHash.empty())) return false;
    return versionTokensMatch(left, right);
}

Json sourceVersionToken(const std::filesystem::path& sourcePath)
{
    const Json before = statFields(sourcePath);
    const std::int64_t size = integerValue(before, "size");
    if (size < 0) return Json::object();
    std::ifstream input(sourcePath, std::ios::binary);
    Sha256 edges;
    Sha256 middle;
    if (!input || !edges.valid() || !middle.valid()) return Json::object();
    const std::string sizeText = std::to_string(size);
    const char separator = '\0';
    edges.add(sizeText); edges.add(&separator, 1);
    middle.add(sizeText); middle.add(&separator, 1);
    std::vector<char> buffer(kIdentityHashBytes);
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    edges.add(buffer.data(), static_cast<std::size_t>(input.gcount()));
    if (size > static_cast<std::int64_t>(kIdentityHashBytes)) {
        input.clear();
        input.seekg(std::max<std::int64_t>(0, size - static_cast<std::int64_t>(kIdentityHashBytes)));
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        edges.add(buffer.data(), static_cast<std::size_t>(input.gcount()));
    }
    input.clear();
    input.seekg(std::max<std::int64_t>(0,
        (size - static_cast<std::int64_t>(kIdentityHashBytes)) / 2));
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    middle.add(buffer.data(), static_cast<std::size_t>(input.gcount()));
    const Json after = statFields(sourcePath);
    if (!statFieldsMatch(before, after)) return Json::object();
    Json result = before;
    result["head_tail_sha256"] = edges.finish();
    result["middle_sha256"] = middle.finish();
    return result;
}

bool metadataIdentityMatchesSource(const Json& metadata,
                                   const std::filesystem::path& sourcePath)
{
    const Json expected = metadata.value("source_identity", Json::object());
    if (!expected.is_object() || integerValue(expected, "size") < 0) return false;
    if (sourcePath.empty()) {
        return !portableContentHash(expected).empty() || versionTokensMatch(expected, expected);
    }
    Json actual = sourceVersionToken(sourcePath);
    if (actual.empty()) return false;
    const std::string expectedHash = portableContentHash(expected);
    if (!stringValue(expected, "content_sha256").empty() && expectedHash.empty()) return false;
    if (!expectedHash.empty()) {
        actual["identity_schema"] = std::string(kSourceIdentitySchema);
        actual["content_sha256"] = versionTokensMatch(expected, actual)
            ? expectedHash : sha256File(sourcePath);
    }
    return identityObjectsMatch(expected, actual);
}

Json generatorMetadata(const std::filesystem::path& directory)
{
    Json value = readJson(directory / "jcut_mask.json");
    return value.empty() ? readJson(directory / "jcut_alpha.json") : value;
}

bool sidecarUsesOrdinals(const std::filesystem::path& directory)
{
    const Json generator = generatorMetadata(directory);
    const Json frameMap = readJson(directory / "jcut_frame_map.json");
    if (std::filesystem::is_regular_file(directory / "jcut_frame_map.tsv") ||
        stringValue(frameMap, "schema") == "jcut_frame_index_map_v2") return true;
    const std::string domain = lower(stringValue(generator, "frame_domain"));
    if (!domain.empty()) return domain == "ordinal" || domain.ends_with("_ordinal");
    const std::string material = directory.filename().string() + " " +
        stringValue(generator, "source_type");
    return containsInsensitive(material, "sam3") ||
        containsInsensitive(material, "sam2") ||
        containsInsensitive(material, "birefnet");
}

bool exactFrameCoverage(const std::filesystem::path& directory, std::int64_t lastMaskFrame)
{
    if (lastMaskFrame < 0 || lastMaskFrame >= std::numeric_limits<int>::max()) return false;
    std::vector<bool> seen(static_cast<std::size_t>(lastMaskFrame + 1), false);
    std::int64_t count = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) return false;
        const std::string name = entry.path().filename().string();
        if (!name.starts_with("frame_") || !name.ends_with(".png")) continue;
        if (entry.is_symlink(error) || !entry.is_regular_file(error) || entry.file_size(error) == 0) return false;
        if (name.size() != 16) return false;
        const std::string digits = name.substr(6, 6);
        if (!std::all_of(digits.begin(), digits.end(), ::isdigit)) return false;
        const std::int64_t frame = std::stoll(digits) - 1;
        if (frame < 0 || frame > lastMaskFrame || seen[static_cast<std::size_t>(frame)]) return false;
        seen[static_cast<std::size_t>(frame)] = true;
        ++count;
    }
    return !error && count == lastMaskFrame + 1;
}

bool completionMatches(const std::filesystem::path& directory,
                       const Json& mapMetadata,
                       std::int64_t lastMaskFrame)
{
    const Json generator = generatorMetadata(directory);
    const std::string sourceType = stringValue(generator, "source_type");
    const bool alpha = containsInsensitive(directory.filename().string(), "birefnet") ||
        containsInsensitive(sourceType, "birefnet");
    const Json completion = readJson(directory / (alpha ? "jcut_alpha.json" : "jcut_mask.json"));
    if (completion.empty()) return false;
    const std::string expectedSchema = alpha ? "jcut_alpha_sidecar_v1" : "jcut_mask_sidecar_v1";
    if (stringValue(completion, "schema") != expectedSchema ||
        !booleanValue(completion, "complete") ||
        stringValue(completion, "frame_domain") != "decode_ordinal" ||
        stringValue(completion, "frame_index_map") != "jcut_frame_map.tsv" ||
        stringValue(completion, "frame_index_metadata") != "jcut_frame_map.json" ||
        integerValue(completion, "expected_frame_count") != lastMaskFrame + 1 ||
        lower(stringValue(completion, "frame_map_sha256")) !=
            lower(stringValue(mapMetadata, "map_sha256")) ||
        stringValue(completion, "frame_map_sha256").empty()) return false;
    return identityObjectsMatch(
        completion.value("source_identity", Json::object()),
        mapMetadata.value("source_identity", Json::object()));
}

} // namespace

LoadedMaskFrameMap loadMaskFrameMapCoreUncached(
    const std::filesystem::path& directory,
    const std::filesystem::path& sourceMediaPath)
{
    LoadedMaskFrameMap loaded;
    MaskFrameMapCore& result = *loaded.map;
    result.ordinalSidecar = sidecarUsesOrdinals(directory);
    const std::filesystem::path mapPath = directory / "jcut_frame_map.tsv";
    if (!std::filesystem::is_regular_file(mapPath)) {
        result.renderReady = !result.ordinalSidecar;
        if (result.ordinalSidecar) result.error = "ordinal mask sidecar has no frame map";
        return loaded;
    }
    std::ifstream input(mapPath);
    std::int64_t previousSource = -1;
    std::int64_t previousMask = -1;
    std::string line;
    while (std::getline(input, line)) {
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] == '#') continue;
        std::istringstream row(line);
        std::int64_t source = -1;
        std::int64_t mask = -1;
        std::string extra;
        if (!(row >> source >> mask) || source < 0 || mask < 0 ||
            mask != result.mappedFrameCount ||
            (result.mappedFrameCount > 0 &&
             (source < previousSource || mask < previousMask))) {
            result.sorted.clear();
            result.error = "invalid mask frame map";
            return loaded;
        }
        result.sorted.emplace_back(source, mask);
        if (result.firstSourceFrame < 0) result.firstSourceFrame = source;
        result.lastSourceFrame = source;
        result.lastMaskFrame = mask;
        previousSource = source;
        previousMask = mask;
        ++result.mappedFrameCount;
    }
    if (result.sorted.empty()) {
        result.error = "empty mask frame map";
        return loaded;
    }

    const Json metadata = readJson(directory / "jcut_frame_map.json");
    const std::string mapHash = sha256File(mapPath);
    result.metadataVerified =
        stringValue(metadata, "schema") == "jcut_frame_index_map_v2" &&
        stringValue(metadata, "status") == "ready" &&
        stringValue(metadata, "frame_domain") == "source_timestamp_to_generated_ordinal" &&
        stringValue(metadata, "map_file") == "jcut_frame_map.tsv" &&
        metadata.contains("output_fps") && metadata["output_fps"].is_null() &&
        integerValue(metadata, "mapped_frame_count") == result.mappedFrameCount &&
        integerValue(metadata, "min_source_frame") == result.firstSourceFrame &&
        integerValue(metadata, "max_source_frame") == result.lastSourceFrame &&
        integerValue(metadata, "max_mask_frame") == result.lastMaskFrame &&
        integerValue(metadata, "expected_output_frame_count") == result.lastMaskFrame + 1 &&
        lower(stringValue(metadata, "map_sha256")) == lower(mapHash) &&
        metadataIdentityMatchesSource(metadata, sourceMediaPath);
    const bool completionVerified =
        result.metadataVerified &&
        completionMatches(directory, metadata, result.lastMaskFrame);
    loaded.directoryVersionRelevant =
        result.ordinalSidecar && completionVerified;
    result.renderReady = !result.ordinalSidecar ||
        (completionVerified &&
         exactFrameCoverage(directory, result.lastMaskFrame));
    if (!result.renderReady) result.error = "mask sidecar validation failed";
    return loaded;
}

std::shared_ptr<const MaskFrameMapCore> cachedMaskFrameMapCore(
    const std::filesystem::path& directory,
    const std::filesystem::path& sourceMediaPath)
{
    CacheState& state = cacheState();
    const CacheKey key = cacheKey(directory, sourceMediaPath);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const CacheFingerprint before =
            cacheFingerprint(directory, sourceMediaPath);
        std::shared_ptr<CacheEntry> entry;
        {
            std::unique_lock lock(state.mutex);
            auto [found, inserted] =
                state.entries.emplace(key, std::make_shared<CacheEntry>());
            entry = found->second;
            if (!inserted && entry->loading) {
                state.changed.wait(lock, [&entry]() { return !entry->loading; });
                --attempt;
                continue;
            }
            if (cacheEntryMatches(*entry, before)) {
                ++state.hitCount;
                entry->lastAccess = ++state.accessCounter;
                return entry->map;
            }
            ++state.missCount;
            entry->loading = true;
            entry->lastAccess = ++state.accessCounter;
        }

        LoadedMaskFrameMap loaded;
        try {
            loaded = loadMaskFrameMapCoreUncached(directory, sourceMediaPath);
        } catch (...) {
            std::lock_guard lock(state.mutex);
            entry->loading = false;
            state.changed.notify_all();
            throw;
        }
        const CacheFingerprint after =
            cacheFingerprint(directory, sourceMediaPath);
        const bool stableDuringValidation =
            before.stableFilesMatch(after) &&
            (!loaded.directoryVersionRelevant ||
             before.directory == after.directory);
        {
            std::lock_guard lock(state.mutex);
            ++state.validationCount;
            if (stableDuringValidation) {
                entry->fingerprint = after;
                entry->map = loaded.map;
                entry->directoryVersionRelevant =
                    loaded.directoryVersionRelevant;
            } else {
                entry->map.reset();
                entry->directoryVersionRelevant = false;
            }
            entry->loading = false;
            trimCacheLocked(state, entry.get());
            state.changed.notify_all();
        }
        if (stableDuringValidation) return loaded.map;
    }

    auto unstable = std::make_shared<MaskFrameMapCore>();
    unstable->ordinalSidecar = sidecarUsesOrdinals(directory);
    unstable->error = "mask sidecar changed during validation";
    return unstable;
}

MaskFrameMapCore loadMaskFrameMapCore(
    const std::filesystem::path& directory,
    const std::filesystem::path& sourceMediaPath)
{
    return *cachedMaskFrameMapCore(directory, sourceMediaPath);
}

std::optional<std::int64_t> mappedMaskFrameForSourceFrameCore(
    const std::filesystem::path& directory,
    const std::filesystem::path& sourceMediaPath,
    std::int64_t sourceFrame)
{
    sourceFrame = std::max<std::int64_t>(0, sourceFrame);
    const std::shared_ptr<const MaskFrameMapCore> cached =
        cachedMaskFrameMapCore(directory, sourceMediaPath);
    const MaskFrameMapCore& map = *cached;
    if (map.sorted.empty()) return map.ordinalSidecar ? std::nullopt
                                                      : std::optional<std::int64_t>(sourceFrame);
    if (map.ordinalSidecar && (!map.metadataVerified || !map.renderReady ||
        sourceFrame < map.firstSourceFrame || sourceFrame > map.lastSourceFrame)) return std::nullopt;
    const auto lowerBound = std::lower_bound(map.sorted.begin(), map.sorted.end(), sourceFrame,
        [](const auto& entry, std::int64_t value) { return entry.first < value; });
    if (lowerBound != map.sorted.end() && lowerBound->first == sourceFrame) return lowerBound->second;
    if (map.ordinalSidecar) return std::nullopt;
    if (lowerBound == map.sorted.begin()) return lowerBound->second;
    if (lowerBound == map.sorted.end()) return map.sorted.back().second;
    const auto previous = lowerBound - 1;
    return std::abs(previous->first - sourceFrame) <= std::abs(lowerBound->first - sourceFrame)
        ? previous->second : lowerBound->second;
}

std::optional<std::filesystem::path> maskFramePathForSourceFrameCore(
    const std::filesystem::path& directory,
    const std::filesystem::path& sourceMediaPath,
    std::int64_t sourceFrame)
{
    const auto frame = mappedMaskFrameForSourceFrameCore(directory, sourceMediaPath, sourceFrame);
    if (!frame) return std::nullopt;
    std::ostringstream name;
    name << "frame_" << std::setw(6) << std::setfill('0') << (*frame + 1) << ".png";
    const std::filesystem::path path = directory / name.str();
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) &&
            !error &&
            std::filesystem::file_size(path, error) > 0 &&
            !error
        ? std::optional(path)
        : std::nullopt;
}

MaskFrameMapCoreCacheStats maskFrameMapCoreCacheStats()
{
    CacheState& state = cacheState();
    std::lock_guard lock(state.mutex);
    return MaskFrameMapCoreCacheStats{
        state.hitCount,
        state.missCount,
        state.validationCount,
        static_cast<std::uint64_t>(state.entries.size()),
    };
}

void clearMaskFrameMapCoreCache()
{
    CacheState& state = cacheState();
    std::lock_guard lock(state.mutex);
    state.entries.clear();
    state.accessCounter = 0;
    state.hitCount = 0;
    state.missCount = 0;
    state.validationCount = 0;
    state.changed.notify_all();
}

} // namespace jcut::masks
