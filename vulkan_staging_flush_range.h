#pragma once

#include <cstdint>
#include <optional>

namespace render_detail {

struct VulkanStagingFlushRange {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

inline std::optional<VulkanStagingFlushRange> alignedVulkanStagingFlushRange(
    std::uint64_t writeOffset,
    std::uint64_t writeSize,
    std::uint64_t allocationSize,
    std::uint64_t nonCoherentAtomSize)
{
    if (writeSize == 0 ||
        nonCoherentAtomSize == 0 ||
        writeOffset > allocationSize ||
        writeSize > allocationSize - writeOffset) {
        return std::nullopt;
    }

    const std::uint64_t alignedOffset =
        writeOffset - (writeOffset % nonCoherentAtomSize);
    const std::uint64_t writeEnd = writeOffset + writeSize;
    std::uint64_t alignedEnd = writeEnd;
    const std::uint64_t endRemainder = writeEnd % nonCoherentAtomSize;
    if (endRemainder != 0) {
        const std::uint64_t padding = nonCoherentAtomSize - endRemainder;
        alignedEnd = padding <= allocationSize - writeEnd
            ? writeEnd + padding
            : allocationSize;
    }

    return VulkanStagingFlushRange{
        alignedOffset,
        alignedEnd - alignedOffset,
    };
}

}  // namespace render_detail
