#pragma once

#include "core/geometry.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct AVFrame;

namespace jcut::core {

// Qt-free storage and accounting for a decoded frame. UI/render adapters may
// attach framework-specific CPU or GPU objects as opaque payloads while this
// class owns the FFmpeg hardware-frame reference and the neutral metadata.
class FramePayloadCore {
public:
    using OpaqueCpuPayload = std::shared_ptr<const void>;

    FramePayloadCore();
    ~FramePayloadCore();

    FramePayloadCore(const FramePayloadCore&) = delete;
    FramePayloadCore& operator=(const FramePayloadCore&) = delete;
    FramePayloadCore(FramePayloadCore&& other) noexcept;
    FramePayloadCore& operator=(FramePayloadCore&& other) noexcept;

    void setIdentity(std::int64_t frameNumber,
                     std::string sourcePath,
                     std::int64_t decodeTimestampMs);
    void setSize(SizeI size);
    void setCpuPayload(OpaqueCpuPayload payload, std::size_t byteCount);

    // Clones the AVFrame reference so the payload remains valid after the
    // producer releases its frame. Size, pixel formats, and normalized crop
    // metadata are copied from the source frame.
    [[nodiscard]] bool cloneHardwareFrame(const AVFrame* frame,
                                          int softwarePixelFormat);

    // GPU objects remain owned by their render-thread adapter, matching the
    // legacy FrameHandle contract. Supplying the footprint explicitly avoids
    // dereferencing a private QRhi type in this core.
    void setOpaqueGpuTexture(void* texture,
                             SizeI size,
                             int bytesPerPixel,
                             bool adapterReportsOwnership);

    [[nodiscard]] std::int64_t frameNumber() const;
    [[nodiscard]] const std::string& sourcePath() const;
    [[nodiscard]] std::int64_t decodeTimestampMs() const;
    [[nodiscard]] SizeI size() const;
    [[nodiscard]] RectF validTextureRectNormalized() const;

    [[nodiscard]] bool hasCpuPayload() const;
    [[nodiscard]] const void* cpuPayload() const;
    [[nodiscard]] bool hasOpaqueGpuTexture() const;
    [[nodiscard]] void* opaqueGpuTexture() const;
    [[nodiscard]] bool adapterReportsGpuTextureOwnership() const;
    [[nodiscard]] bool hasHardwareFrame() const;
    [[nodiscard]] const AVFrame* hardwareFrame() const;
    [[nodiscard]] int hardwarePixelFormat() const;
    [[nodiscard]] int hardwareSoftwarePixelFormat() const;

    // Preserve FrameHandle's established split: memoryUsage() counts CPU and
    // texture storage, while gpuMemoryUsage() also includes the conservative
    // retained hardware-frame residency estimate.
    [[nodiscard]] std::size_t memoryUsage() const;
    [[nodiscard]] std::size_t cpuMemoryUsage() const;
    [[nodiscard]] std::size_t gpuMemoryUsage() const;
    [[nodiscard]] std::size_t gpuTextureMemoryUsage() const;
    [[nodiscard]] std::size_t hardwareFrameMemoryUsage() const;

private:
    void resetHardwareFrame();

    OpaqueCpuPayload m_cpuPayload;
    std::size_t m_cpuByteCount = 0;
    void* m_gpuTexture = nullptr;
    std::size_t m_gpuTextureByteCount = 0;
    bool m_adapterReportsGpuTextureOwnership = false;
    AVFrame* m_hardwareFrame = nullptr;
    int m_hardwarePixelFormat = -1;
    int m_hardwareSoftwarePixelFormat = -1;
    std::int64_t m_frameNumber = -1;
    std::string m_sourcePath;
    SizeI m_size;
    RectF m_validTextureRectNormalized{0.0, 0.0, 1.0, 1.0};
    std::int64_t m_decodeTimestampMs = 0;
};

} // namespace jcut::core
