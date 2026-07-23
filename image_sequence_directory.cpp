#include "image_sequence_directory.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <unordered_map>

namespace {

namespace fs = std::filesystem;

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool isImageExtension(const std::string& extension)
{
    return extension == ".png" || extension == ".jpg" ||
        extension == ".jpeg" || extension == ".webp" ||
        extension == ".tga" || extension == ".tif" ||
        extension == ".tiff" || extension == ".exr" ||
        extension == ".bmp" || extension == ".gif";
}

bool containsDigit(const std::string& value)
{
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
}

bool naturalFileNameLessCaseInsensitive(const fs::path& lhsPath,
                                        const fs::path& rhsPath)
{
    const std::string lhs = lhsPath.filename().string();
    const std::string rhs = rhsPath.filename().string();
    std::size_t lhsIndex = 0;
    std::size_t rhsIndex = 0;

    while (lhsIndex < lhs.size() && rhsIndex < rhs.size()) {
        const unsigned char lhsChar = static_cast<unsigned char>(lhs[lhsIndex]);
        const unsigned char rhsChar = static_cast<unsigned char>(rhs[rhsIndex]);
        const bool lhsDigit = std::isdigit(lhsChar) != 0;
        const bool rhsDigit = std::isdigit(rhsChar) != 0;

        if (lhsDigit && rhsDigit) {
            std::size_t lhsSignificant = lhsIndex;
            std::size_t rhsSignificant = rhsIndex;
            while (lhsSignificant < lhs.size() && lhs[lhsSignificant] == '0') {
                ++lhsSignificant;
            }
            while (rhsSignificant < rhs.size() && rhs[rhsSignificant] == '0') {
                ++rhsSignificant;
            }

            std::size_t lhsEnd = lhsSignificant;
            std::size_t rhsEnd = rhsSignificant;
            while (lhsEnd < lhs.size() &&
                   std::isdigit(static_cast<unsigned char>(lhs[lhsEnd])) != 0) {
                ++lhsEnd;
            }
            while (rhsEnd < rhs.size() &&
                   std::isdigit(static_cast<unsigned char>(rhs[rhsEnd])) != 0) {
                ++rhsEnd;
            }

            const std::size_t lhsLength = lhsEnd - lhsSignificant;
            const std::size_t rhsLength = rhsEnd - rhsSignificant;
            if (lhsLength != rhsLength) {
                return lhsLength < rhsLength;
            }
            const int digitCompare = lhs.compare(
                lhsSignificant, lhsLength, rhs, rhsSignificant, rhsLength);
            if (digitCompare != 0) {
                return digitCompare < 0;
            }

            const std::size_t lhsRunLength = lhsEnd - lhsIndex;
            const std::size_t rhsRunLength = rhsEnd - rhsIndex;
            if (lhsRunLength != rhsRunLength) {
                return lhsRunLength < rhsRunLength;
            }
            lhsIndex = lhsEnd;
            rhsIndex = rhsEnd;
            continue;
        }

        const unsigned char foldedLhs = static_cast<unsigned char>(std::tolower(lhsChar));
        const unsigned char foldedRhs = static_cast<unsigned char>(std::tolower(rhsChar));
        if (foldedLhs != foldedRhs) {
            return foldedLhs < foldedRhs;
        }
        ++lhsIndex;
        ++rhsIndex;
    }

    if (lhs.size() != rhs.size()) {
        return lhs.size() < rhs.size();
    }
    return lhs < rhs;
}

struct CacheEntry {
    fs::file_time_type modified;
    std::shared_ptr<const jcut::ImageSequenceDirectoryInfo> info;
};

jcut::ImageSequenceDirectoryInfo probeUncached(const fs::path& directory)
{
    jcut::ImageSequenceDirectoryInfo result;
    result.directory = directory;

    std::error_code error;
    if (!fs::is_directory(directory, error) || error) {
        return result;
    }

    struct ExtensionStats {
        int files = 0;
        int numberedFiles = 0;
    };
    std::map<std::string, ExtensionStats> statsByExtension;
    std::vector<fs::path> imagePaths;
    for (fs::directory_iterator iterator(directory, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        std::error_code typeError;
        if (!iterator->is_regular_file(typeError) || typeError) {
            continue;
        }
        const fs::path entryPath = iterator->path();
        const std::string extension = lowerAscii(entryPath.extension().string());
        if (!isImageExtension(extension)) {
            continue;
        }
        imagePaths.push_back(entryPath);
        ExtensionStats& stats = statsByExtension[extension];
        ++stats.files;
        if (containsDigit(entryPath.stem().string())) {
            ++stats.numberedFiles;
        }
    }
    if (error) {
        return result;
    }

    int bestCount = 0;
    for (const auto& [extension, stats] : statsByExtension) {
        if (stats.files > bestCount) {
            result.extension = extension;
            bestCount = stats.files;
        }
    }
    const auto best = statsByExtension.find(result.extension);
    if (best == statsByExtension.end() || best->second.files < 2 ||
        best->second.numberedFiles < 2 ||
        best->second.numberedFiles * 2 < best->second.files) {
        result.extension.clear();
        return result;
    }

    result.framePaths.reserve(static_cast<std::size_t>(best->second.files));
    for (const fs::path& imagePath : imagePaths) {
        if (lowerAscii(imagePath.extension().string()) == result.extension) {
            result.framePaths.push_back(imagePath);
        }
    }
    std::sort(result.framePaths.begin(), result.framePaths.end(),
              naturalFileNameLessCaseInsensitive);
    return result;
}

std::shared_ptr<const jcut::ImageSequenceDirectoryInfo> cachedProbe(
    const fs::path& path)
{
    if (path.empty()) {
        return std::make_shared<const jcut::ImageSequenceDirectoryInfo>();
    }
    std::error_code error;
    const fs::path absolutePath = fs::absolute(path, error).lexically_normal();
    if (error) {
        return std::make_shared<const jcut::ImageSequenceDirectoryInfo>();
    }
    const fs::file_time_type modified = fs::last_write_time(absolutePath, error);
    if (error) {
        return std::make_shared<const jcut::ImageSequenceDirectoryInfo>();
    }

    static std::mutex cacheMutex;
    static std::unordered_map<std::string, CacheEntry> cache;
    const std::string cacheKey = absolutePath.string();
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto cached = cache.find(cacheKey);
        if (cached != cache.end() && cached->second.modified == modified) {
            return cached->second.info;
        }
    }

    auto result = std::make_shared<const jcut::ImageSequenceDirectoryInfo>(
        probeUncached(absolutePath));
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cache[cacheKey] = {modified, result};
    }
    return result;
}

} // namespace

namespace jcut {

ImageSequenceDirectoryInfo probeImageSequenceDirectory(const fs::path& path)
{
    return *cachedProbe(path);
}

bool isImageSequenceDirectory(const fs::path& path)
{
    return cachedProbe(path)->detected();
}

} // namespace jcut
