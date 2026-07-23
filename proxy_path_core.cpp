#include "proxy_path_core.h"

#include "image_sequence_directory.h"

#include <filesystem>

namespace jcut {

namespace fs = std::filesystem;

std::vector<std::string> proxyCandidatePaths(const std::string& sourcePath)
{
    const fs::path source(sourcePath);
    const fs::path parent = source.parent_path();
    const std::string stem = source.stem().string();
    return {
        (parent / (stem + ".proxy")).string(),
        (parent / (stem + ".proxy.mp4")).string(),
        (parent / (stem + ".proxy.mov")).string(),
    };
}

bool proxyPathIsUsable(const std::string& path)
{
    if (path.empty()) return false;
    std::error_code error;
    const fs::path candidate(path);
    if (fs::is_regular_file(candidate, error) && !error) return true;
    error.clear();
    return fs::is_directory(candidate, error) && !error &&
        isImageSequenceDirectory(candidate);
}

std::string discoverExistingProxyPath(const std::string& sourcePath)
{
    for (const std::string& candidate : proxyCandidatePaths(sourcePath)) {
        if (proxyPathIsUsable(candidate)) return candidate;
    }
    return {};
}

} // namespace jcut
