#include "frame_handle.h"

#include <QtGui/private/qrhi_p.h>

#include <chrono>

namespace {

std::int64_t currentTimestampMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int bytesPerPixel(QRhiTexture::Format format)
{
    switch (format) {
    case QRhiTexture::R8:
        return 1;
    case QRhiTexture::RG8:
        return 2;
    case QRhiTexture::RGBA8:
    case QRhiTexture::BGRA8:
    default:
        return 4;
    }
}

} // namespace

namespace editor {

FrameHandle FrameHandle::createGpuFrame(QRhiTexture* texture,
                                        int64_t frameNum,
                                        const QString& path)
{
    FrameHandle handle;
    if (!texture) {
        return handle;
    }

    handle.d = new FrameData();
    const QSize pixelSize = texture->pixelSize();
    handle.d->payload.setIdentity(
        frameNum, path.toStdString(), currentTimestampMs());
    handle.d->payload.setOpaqueGpuTexture(
        texture,
        {pixelSize.width(), pixelSize.height()},
        bytesPerPixel(texture->format()),
        true);
    return handle;
}

void FrameHandle::uploadToGpu(QRhi* rhi)
{
    if (!d || !hasCpuImage() || !rhi || hasGpuTexture()) {
        return;
    }

    QImage uploadImage = cpuImage();
    if (uploadImage.format() != QImage::Format_RGBA8888 &&
        uploadImage.format() != QImage::Format_ARGB32 &&
        uploadImage.format() != QImage::Format_RGB32) {
        uploadImage = uploadImage.convertToFormat(QImage::Format_RGBA8888);
    }

    QRhiTexture* texture = rhi->newTexture(
        QRhiTexture::RGBA8, uploadImage.size(), 1);
    if (!texture->create()) {
        delete texture;
        return;
    }

    QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
    batch->uploadTexture(texture, uploadImage);
    d->payload.setOpaqueGpuTexture(
        texture,
        {uploadImage.width(), uploadImage.height()},
        4,
        true);
}

} // namespace editor
