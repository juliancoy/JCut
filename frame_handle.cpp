#include "frame_handle.h"

#include <chrono>
#include <memory>

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

FrameHandle FrameHandle::createCpuFrame(const QImage& image, int64_t frameNum, const QString& path) {
    FrameHandle handle;
    handle.d = new FrameData();
    handle.d->payload.setIdentity(frameNum, path.toStdString(), currentTimestampMs());
    handle.d->payload.setSize({image.width(), image.height()});
    if (!image.isNull()) {
        handle.d->payload.setCpuPayload(
            std::make_shared<QImage>(image),
            static_cast<std::size_t>(image.sizeInBytes()));
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
    handle.d->payload.setIdentity(frameNum, path.toStdString(), currentTimestampMs());
    return handle;
}

bool FrameHandle::operator==(const FrameHandle& other) const {
    if (d.constData() == other.d.constData()) return true;
    if (!d || !other.d) return false;
    return d->payload.sourcePath() == other.d->payload.sourcePath() &&
           d->payload.frameNumber() == other.d->payload.frameNumber();
}

QImage FrameHandle::cpuImage() const {
    if (!d || !d->payload.hasCpuPayload()) {
        return QImage();
    }
    return *static_cast<const QImage*>(d->payload.cpuPayload());
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
