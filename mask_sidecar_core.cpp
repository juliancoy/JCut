#include "mask_sidecar_core.h"

#include "mask_frame_map_core.h"

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <memory>
#include <regex>
#include <set>
#include <sstream>

namespace jcut::masks {
namespace {

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string displayName(std::string name, const std::string& mediaStem)
{
    const std::string prefix = mediaStem + "_";
    if (!mediaStem.empty() && lower(name).starts_with(lower(prefix))) {
        name.erase(0, prefix.size());
    }
    for (const std::string prefixValue : {"sam3_", "sam2_", "birefnet_", "ai_"}) {
        if (lower(name).starts_with(prefixValue)) {
            name.erase(0, prefixValue.size());
            break;
        }
    }
    for (const std::string suffix : {
             "_binary_masks", "_alpha_masks", "_masks", "_mask",
             "_mattes", "_matte"}) {
        if (lower(name).ends_with(suffix)) {
            name.resize(name.size() - suffix.size());
            break;
        }
    }
    std::replace(name.begin(), name.end(), '_', ' ');
    const auto first = name.find_first_not_of(' ');
    const auto last = name.find_last_not_of(' ');
    return first == std::string::npos ? "Generated mask"
        : name.substr(first, last - first + 1);
}

bool looksLikeGeneratedMaskDirectory(
    const std::filesystem::path& candidate, const std::string& mediaStem)
{
    const std::string name = candidate.filename().string();
    const std::string prefix = lower(mediaStem + "_");
    if (!lower(name).starts_with(prefix)) return false;
    const std::string suffix = lower(name.substr(prefix.size()));
    return suffix.find("mask") != std::string::npos ||
        suffix.find("matte") != std::string::npos ||
        suffix.find("segment") != std::string::npos ||
        suffix.find("alpha") != std::string::npos ||
        std::filesystem::exists(candidate / "jcut_alpha.json") ||
        std::filesystem::exists(candidate / "jcut_mask.json");
}

} // namespace

std::string stableMaskSidecarIdCore(const std::filesystem::path& directory)
{
    std::error_code error;
    std::filesystem::path identity = std::filesystem::canonical(directory, error);
    if (error || identity.empty()) identity = directory.lexically_normal();
    const std::string text = identity.string();
    struct ShaDeleter { void operator()(AVSHA* value) const { av_free(value); } };
    std::unique_ptr<AVSHA, ShaDeleter> sha(av_sha_alloc());
    if (!sha || av_sha_init(sha.get(), 256) < 0) return {};
    av_sha_update(sha.get(), reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    std::array<std::uint8_t, 32> digest{};
    av_sha_final(sha.get(), digest.data());
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < 8; ++index) {
        output << std::setw(2) << static_cast<int>(digest[index]);
    }
    return output.str();
}

MaskSidecarCore inspectMaskSidecarCore(
    const std::filesystem::path& directory,
    const std::filesystem::path& sourceMediaPath)
{
    MaskSidecarCore sidecar;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) return sidecar;
    sidecar.directory = std::filesystem::absolute(directory, error).lexically_normal();
    if (error) sidecar.directory = directory.lexically_normal();
    sidecar.id = stableMaskSidecarIdCore(sidecar.directory);
    sidecar.displayName = displayName(
        sidecar.directory.filename().string(), sourceMediaPath.stem().string());
    const std::regex framePattern(R"(^frame_([0-9]+)\.png$)", std::regex::icase);
    for (const auto& entry : std::filesystem::directory_iterator(sidecar.directory, error)) {
        if (error) break;
        if (!entry.is_regular_file(error) || error) continue;
        std::smatch match;
        const std::string name = entry.path().filename().string();
        if (!std::regex_match(name, match, framePattern)) continue;
        const std::int64_t frame = std::stoll(match[1].str());
        sidecar.firstFrame = sidecar.firstFrame < 0
            ? frame : std::min(sidecar.firstFrame, frame);
        sidecar.lastFrame = std::max(sidecar.lastFrame, frame);
        ++sidecar.frameCount;
    }
    if (sidecar.frameCount <= 0) return {};
    const MaskFrameMapCore map = loadMaskFrameMapCore(
        sidecar.directory, sourceMediaPath);
    sidecar.decodeOrdinalFrames = map.ordinalSidecar;
    sidecar.ready = !map.ordinalSidecar || map.renderReady;
    sidecar.readinessIssue = sidecar.ready ? std::string{} : map.error;
    return sidecar;
}

std::vector<MaskSidecarCore> discoverMaskSidecarsCore(
    const std::filesystem::path& sourceMediaPath,
    const std::filesystem::path& preferredDirectory)
{
    std::vector<MaskSidecarCore> result;
    std::set<std::string> ids;
    const auto append = [&](const std::filesystem::path& path) {
        MaskSidecarCore sidecar = inspectMaskSidecarCore(path, sourceMediaPath);
        if (sidecar.valid() && ids.insert(sidecar.id).second) {
            result.push_back(std::move(sidecar));
        }
    };
    if (!preferredDirectory.empty()) append(preferredDirectory);
    std::error_code error;
    const std::filesystem::path parent = sourceMediaPath.parent_path();
    if (std::filesystem::is_directory(parent, error) && !error) {
        for (const auto& entry : std::filesystem::directory_iterator(parent, error)) {
            if (error) break;
            if (entry.is_directory(error) && !error &&
                looksLikeGeneratedMaskDirectory(entry.path(), sourceMediaPath.stem().string())) {
                append(entry.path());
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.displayName == right.displayName
            ? left.id < right.id : left.displayName < right.displayName;
    });
    return result;
}

} // namespace jcut::masks
