#include "frame_handle.h"

#include <chrono>
#include <cstring>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
}

namespace {

std::int64_t currentTimestampMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

namespace editor {

// ============================================================================
// FrameData Implementation
// ============================================================================

FrameData::~FrameData() = default;

size_t FrameData::memoryUsage() const {
    return payload.memoryUsage();
}

// ============================================================================
// FrameHandle Implementation
// ============================================================================

FrameHandle::FrameHandle() = default;

FrameHandle FrameHandle::createCpuFrame(
    const QImage& image,
    int64_t frameNum,
    const QString& path,
    int64_t sourcePresentationTimestamp) {
    FrameHandle handle;
    handle.d = new FrameData();
    handle.d->payload.setIdentity(
        frameNum,
        path.toStdString(),
        currentTimestampMs(),
        sourcePresentationTimestamp);
    handle.d->payload.setSize({image.width(), image.height()});
    if (!image.isNull()) {
        const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
        auto buffer = std::make_shared<jcut::core::ImageBuffer>();
        buffer->size = {rgba.width(), rgba.height()};
        buffer->strideBytes = rgba.width() * 4;
        buffer->bytes.resize(
            static_cast<std::size_t>(buffer->strideBytes) *
            static_cast<std::size_t>(rgba.height()));
        for (int y = 0; y < rgba.height(); ++y) {
            std::memcpy(
                buffer->bytes.data() +
                    static_cast<std::size_t>(y) * buffer->strideBytes,
                rgba.constScanLine(y),
                static_cast<std::size_t>(buffer->strideBytes));
        }
        handle.d->payload.setCpuPayload(
            buffer,
            buffer->bytes.size());
    }
    return handle;
}

FrameHandle FrameHandle::createHardwareFrame(const AVFrame* frame,
                                             int64_t frameNum,
                                             const QString& path,
                                             int swPixelFormat) {
    FrameHandle handle;
    if (!frame) {
        return handle;
    }

    handle.d = new FrameData();
    if (!handle.d->payload.cloneHardwareFrame(frame, swPixelFormat)) {
        return FrameHandle();
    }
    handle.d->payload.setIdentity(
        frameNum,
        path.toStdString(),
        currentTimestampMs(),
        frame->best_effort_timestamp);
    return handle;
}

bool FrameHandle::operator==(const FrameHandle& other) const {
    if (d.constData() == other.d.constData()) return true;
    if (!d || !other.d) return false;
    return d->payload.sourcePath() == other.d->payload.sourcePath() &&
           d->payload.frameNumber() == other.d->payload.frameNumber() &&
           d->payload.sourcePresentationTimestamp() ==
               other.d->payload.sourcePresentationTimestamp();
}

QImage FrameHandle::cpuImage() const {
    const auto buffer = cpuImageBuffer();
    if (!buffer || buffer->empty()) {
        return QImage();
    }
    auto* retained =
        new std::shared_ptr<const jcut::core::ImageBuffer>(buffer);
    return QImage(
        buffer->bytes.data(),
        buffer->size.width,
        buffer->size.height,
        buffer->strideBytes,
        QImage::Format_RGBA8888,
        [](void* value) {
            delete static_cast<
                std::shared_ptr<const jcut::core::ImageBuffer>*>(value);
        },
        retained);
}

std::shared_ptr<const jcut::core::ImageBuffer>
FrameHandle::cpuImageBuffer() const {
    if (!d || !d->payload.hasCpuPayload()) {
        return {};
    }
    return std::static_pointer_cast<const jcut::core::ImageBuffer>(
        d->payload.cpuPayloadShared());
}

size_t FrameHandle::cpuMemoryUsage() const {
    if (!d) {
        return 0;
    }
    return d->payload.cpuMemoryUsage();
}

size_t FrameHandle::gpuMemoryUsage() const {
    if (!d) {
        return 0;
    }

    return d->payload.gpuMemoryUsage();
}

} // namespace editor
