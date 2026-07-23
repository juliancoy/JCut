#include "frame_payload_core.h"

#include <algorithm>
#include <limits>
#include <utility>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

std::size_t saturatedMultiply(std::size_t left, std::size_t right)
{
    if (left == 0 || right == 0) {
        return 0;
    }
    if (left > std::numeric_limits<std::size_t>::max() / right) {
        return std::numeric_limits<std::size_t>::max();
    }
    return left * right;
}

std::size_t saturatedAdd(std::size_t left, std::size_t right)
{
    if (left > std::numeric_limits<std::size_t>::max() - right) {
        return std::numeric_limits<std::size_t>::max();
    }
    return left + right;
}

} // namespace

namespace jcut::core {

FramePayloadCore::FramePayloadCore() = default;

FramePayloadCore::~FramePayloadCore()
{
    resetHardwareFrame();
}

FramePayloadCore::FramePayloadCore(FramePayloadCore&& other) noexcept
    : m_cpuPayload(std::move(other.m_cpuPayload)),
      m_cpuByteCount(other.m_cpuByteCount),
      m_gpuTexture(other.m_gpuTexture),
      m_gpuTextureByteCount(other.m_gpuTextureByteCount),
      m_adapterReportsGpuTextureOwnership(other.m_adapterReportsGpuTextureOwnership),
      m_hardwareFrame(other.m_hardwareFrame),
      m_hardwarePixelFormat(other.m_hardwarePixelFormat),
      m_hardwareSoftwarePixelFormat(other.m_hardwareSoftwarePixelFormat),
      m_frameNumber(other.m_frameNumber),
      m_sourcePath(std::move(other.m_sourcePath)),
      m_size(other.m_size),
      m_validTextureRectNormalized(other.m_validTextureRectNormalized),
      m_decodeTimestampMs(other.m_decodeTimestampMs)
{
    other.m_cpuByteCount = 0;
    other.m_gpuTexture = nullptr;
    other.m_gpuTextureByteCount = 0;
    other.m_adapterReportsGpuTextureOwnership = false;
    other.m_hardwareFrame = nullptr;
    other.m_hardwarePixelFormat = -1;
    other.m_hardwareSoftwarePixelFormat = -1;
    other.m_frameNumber = -1;
    other.m_size = {};
    other.m_validTextureRectNormalized = {0.0, 0.0, 1.0, 1.0};
    other.m_decodeTimestampMs = 0;
}

FramePayloadCore& FramePayloadCore::operator=(FramePayloadCore&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    resetHardwareFrame();
    m_cpuPayload = std::move(other.m_cpuPayload);
    m_cpuByteCount = other.m_cpuByteCount;
    m_gpuTexture = other.m_gpuTexture;
    m_gpuTextureByteCount = other.m_gpuTextureByteCount;
    m_adapterReportsGpuTextureOwnership = other.m_adapterReportsGpuTextureOwnership;
    m_hardwareFrame = other.m_hardwareFrame;
    m_hardwarePixelFormat = other.m_hardwarePixelFormat;
    m_hardwareSoftwarePixelFormat = other.m_hardwareSoftwarePixelFormat;
    m_frameNumber = other.m_frameNumber;
    m_sourcePath = std::move(other.m_sourcePath);
    m_size = other.m_size;
    m_validTextureRectNormalized = other.m_validTextureRectNormalized;
    m_decodeTimestampMs = other.m_decodeTimestampMs;

    other.m_cpuByteCount = 0;
    other.m_gpuTexture = nullptr;
    other.m_gpuTextureByteCount = 0;
    other.m_adapterReportsGpuTextureOwnership = false;
    other.m_hardwareFrame = nullptr;
    other.m_hardwarePixelFormat = -1;
    other.m_hardwareSoftwarePixelFormat = -1;
    other.m_frameNumber = -1;
    other.m_size = {};
    other.m_validTextureRectNormalized = {0.0, 0.0, 1.0, 1.0};
    other.m_decodeTimestampMs = 0;
    return *this;
}

void FramePayloadCore::setIdentity(std::int64_t frameNumber,
                                   std::string sourcePath,
                                   std::int64_t decodeTimestampMs)
{
    m_frameNumber = frameNumber;
    m_sourcePath = std::move(sourcePath);
    m_decodeTimestampMs = decodeTimestampMs;
}

void FramePayloadCore::setSize(SizeI size)
{
    m_size = size;
}

void FramePayloadCore::setCpuPayload(OpaqueCpuPayload payload,
                                     std::size_t byteCount)
{
    m_cpuPayload = std::move(payload);
    m_cpuByteCount = m_cpuPayload ? byteCount : 0;
}

bool FramePayloadCore::cloneHardwareFrame(const AVFrame* frame,
                                          int softwarePixelFormat)
{
    if (!frame) {
        return false;
    }

    AVFrame* clonedFrame = av_frame_clone(frame);
    if (!clonedFrame) {
        return false;
    }

    resetHardwareFrame();
    m_hardwareFrame = clonedFrame;
    m_hardwarePixelFormat = frame->format;
    m_hardwareSoftwarePixelFormat = softwarePixelFormat;
    m_size = {frame->width, frame->height};

    const int width = std::max(1, frame->width);
    const int height = std::max(1, frame->height);
    const std::int64_t croppedWidth =
        static_cast<std::int64_t>(frame->width) -
        static_cast<std::int64_t>(frame->crop_left) -
        static_cast<std::int64_t>(frame->crop_right);
    const std::int64_t croppedHeight =
        static_cast<std::int64_t>(frame->height) -
        static_cast<std::int64_t>(frame->crop_top) -
        static_cast<std::int64_t>(frame->crop_bottom);
    m_validTextureRectNormalized = {
        static_cast<double>(frame->crop_left) / static_cast<double>(width),
        static_cast<double>(frame->crop_top) / static_cast<double>(height),
        static_cast<double>(std::max<std::int64_t>(1, croppedWidth)) /
            static_cast<double>(width),
        static_cast<double>(std::max<std::int64_t>(1, croppedHeight)) /
            static_cast<double>(height)};
    return true;
}

void FramePayloadCore::setOpaqueGpuTexture(void* texture,
                                           SizeI size,
                                           int bytesPerPixel,
                                           bool adapterReportsOwnership)
{
    m_gpuTexture = texture;
    m_adapterReportsGpuTextureOwnership =
        texture && adapterReportsOwnership;
    if (!texture || !size.valid() || bytesPerPixel <= 0) {
        m_gpuTextureByteCount = 0;
        return;
    }

    m_size = size;
    const std::size_t pixelCount = saturatedMultiply(
        static_cast<std::size_t>(size.width),
        static_cast<std::size_t>(size.height));
    m_gpuTextureByteCount = saturatedMultiply(
        pixelCount, static_cast<std::size_t>(bytesPerPixel));
}

std::int64_t FramePayloadCore::frameNumber() const
{
    return m_frameNumber;
}

const std::string& FramePayloadCore::sourcePath() const
{
    return m_sourcePath;
}

std::int64_t FramePayloadCore::decodeTimestampMs() const
{
    return m_decodeTimestampMs;
}

SizeI FramePayloadCore::size() const
{
    return m_size;
}

RectF FramePayloadCore::validTextureRectNormalized() const
{
    return m_validTextureRectNormalized;
}

bool FramePayloadCore::hasCpuPayload() const
{
    return static_cast<bool>(m_cpuPayload);
}

const void* FramePayloadCore::cpuPayload() const
{
    return m_cpuPayload.get();
}

bool FramePayloadCore::hasOpaqueGpuTexture() const
{
    return m_gpuTexture != nullptr;
}

void* FramePayloadCore::opaqueGpuTexture() const
{
    return m_gpuTexture;
}

bool FramePayloadCore::adapterReportsGpuTextureOwnership() const
{
    return m_adapterReportsGpuTextureOwnership;
}

bool FramePayloadCore::hasHardwareFrame() const
{
    return m_hardwareFrame != nullptr;
}

const AVFrame* FramePayloadCore::hardwareFrame() const
{
    return m_hardwareFrame;
}

int FramePayloadCore::hardwarePixelFormat() const
{
    return m_hardwarePixelFormat;
}

int FramePayloadCore::hardwareSoftwarePixelFormat() const
{
    return m_hardwareSoftwarePixelFormat;
}

std::size_t FramePayloadCore::memoryUsage() const
{
    return saturatedAdd(cpuMemoryUsage(), gpuTextureMemoryUsage());
}

std::size_t FramePayloadCore::cpuMemoryUsage() const
{
    return m_cpuPayload ? m_cpuByteCount : 0;
}

std::size_t FramePayloadCore::gpuMemoryUsage() const
{
    return saturatedAdd(gpuTextureMemoryUsage(), hardwareFrameMemoryUsage());
}

std::size_t FramePayloadCore::gpuTextureMemoryUsage() const
{
    return m_gpuTexture ? m_gpuTextureByteCount : 0;
}

std::size_t FramePayloadCore::hardwareFrameMemoryUsage() const
{
    if (!m_hardwareFrame || !m_size.valid()) {
        return 0;
    }

    const std::size_t width = static_cast<std::size_t>(m_size.width);
    const std::size_t height = static_cast<std::size_t>(m_size.height);
    const std::size_t pixelCount = saturatedMultiply(width, height);

    std::size_t payloadBytes = 0;
    switch (m_hardwareSoftwarePixelFormat) {
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_YUV420P:
        payloadBytes = saturatedMultiply(pixelCount, 3) / 2;
        break;
    case AV_PIX_FMT_P010:
    case AV_PIX_FMT_P016:
        payloadBytes = saturatedMultiply(pixelCount, 3);
        break;
    default:
        payloadBytes = saturatedMultiply(pixelCount, 4);
        break;
    }

    // Hardware decode frames retain driver-side allocations beyond the
    // visible image. Keep the legacy conservative multiplier so cache
    // pressure and eviction behavior do not change during extraction.
    constexpr std::size_t kHardwareFrameResidencyMultiplier = 4;
    return saturatedMultiply(payloadBytes,
                             kHardwareFrameResidencyMultiplier);
}

void FramePayloadCore::resetHardwareFrame()
{
    if (m_hardwareFrame) {
        av_frame_free(&m_hardwareFrame);
    }
}

} // namespace jcut::core
