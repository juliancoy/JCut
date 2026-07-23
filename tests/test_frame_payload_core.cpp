#include "../frame_payload_core.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

int failures = 0;

void expect(bool condition, const std::string& message)
{
    if (condition) {
        return;
    }
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

bool nearlyEqual(double left, double right)
{
    return std::abs(left - right) < 0.000001;
}

AVFrame* makeFrame(int pixelFormat, int width, int height)
{
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return nullptr;
    }
    frame->format = pixelFormat;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
    }
    return frame;
}

void testOpaquePayloadOwnershipAndAccounting()
{
    int cpuPayloadDestructions = 0;
    int gpuTextureToken = 0;
    {
        jcut::core::FramePayloadCore payload;
        payload.setIdentity(42, "media/clip.mov", 123456);
        payload.setSize({10, 20});
        payload.setCpuPayload(
            std::shared_ptr<const void>(
                new int(7),
                [&cpuPayloadDestructions](const void* value) {
                    delete static_cast<const int*>(value);
                    ++cpuPayloadDestructions;
                }),
            800);
        payload.setOpaqueGpuTexture(
            &gpuTextureToken,
            {10, 20},
            2,
            true);

        expect(payload.frameNumber() == 42, "frame identity is retained");
        expect(payload.sourcePath() == "media/clip.mov", "source identity is retained");
        expect(payload.decodeTimestampMs() == 123456, "decode timestamp is retained");
        expect(payload.hasCpuPayload(), "opaque CPU payload is retained");
        expect(payload.cpuMemoryUsage() == 800, "CPU byte count is exact");
        expect(payload.gpuTextureMemoryUsage() == 400,
               "GPU texture bytes use explicit neutral footprint");
        expect(payload.gpuMemoryUsage() == 400,
               "texture-only GPU residency is exact");
        expect(payload.memoryUsage() == 1200,
               "legacy total counts CPU and texture bytes");
        expect(payload.adapterReportsGpuTextureOwnership(),
               "adapter ownership metadata is retained without deleting the opaque texture");

        jcut::core::FramePayloadCore moved(std::move(payload));
        expect(moved.hasCpuPayload(), "move preserves CPU payload ownership");
        expect(moved.hasOpaqueGpuTexture(), "move preserves opaque GPU identity");
        expect(!payload.hasCpuPayload(), "moved-from payload releases CPU ownership");
        expect(!payload.hasOpaqueGpuTexture(), "moved-from payload clears GPU identity");
    }
    expect(cpuPayloadDestructions == 1, "opaque CPU payload is destroyed exactly once");
}

void testHardwareCloneCropAndNv12Residency()
{
    AVFrame* source = makeFrame(AV_PIX_FMT_NV12, 192, 112);
    expect(source != nullptr, "NV12 source frame allocation succeeds");
    if (!source) {
        return;
    }
    source->crop_left = 8;
    source->crop_right = 12;
    source->crop_top = 4;
    source->crop_bottom = 4;

    jcut::core::FramePayloadCore payload;
    expect(payload.cloneHardwareFrame(source, AV_PIX_FMT_NV12),
           "hardware frame reference is cloned");
    const AVFrame* cloned = payload.hardwareFrame();
    expect(cloned && cloned != source, "clone owns a distinct AVFrame header");
    av_frame_free(&source);

    expect(payload.hasHardwareFrame(), "clone survives producer release");
    expect(payload.size() == jcut::core::SizeI{192, 112},
           "hardware dimensions are retained");
    const jcut::core::RectF crop = payload.validTextureRectNormalized();
    expect(nearlyEqual(crop.x, 8.0 / 192.0), "normalized crop left is retained");
    expect(nearlyEqual(crop.y, 4.0 / 112.0), "normalized crop top is retained");
    expect(nearlyEqual(crop.width, 172.0 / 192.0), "normalized crop width is retained");
    expect(nearlyEqual(crop.height, 104.0 / 112.0), "normalized crop height is retained");

    constexpr std::size_t expectedNv12Bytes =
        static_cast<std::size_t>(192 * 112 * 3 / 2) * 4;
    expect(payload.hardwareFrameMemoryUsage() == expectedNv12Bytes,
           "NV12 residency keeps the conservative four-times estimate");
    expect(payload.gpuMemoryUsage() == expectedNv12Bytes,
           "hardware residency is charged to the GPU budget");
    expect(payload.memoryUsage() == 0,
           "legacy total excludes hardware residency just as FrameHandle did");
}

void testHighBitDepthAndFallbackResidency()
{
    AVFrame* p010Source = makeFrame(AV_PIX_FMT_P010, 16, 8);
    expect(p010Source != nullptr, "P010 source frame allocation succeeds");
    if (p010Source) {
        jcut::core::FramePayloadCore p010;
        expect(p010.cloneHardwareFrame(p010Source, AV_PIX_FMT_P010),
               "P010 hardware frame is cloned");
        expect(p010.hardwareFrameMemoryUsage() ==
                   static_cast<std::size_t>(16 * 8 * 3 * 4),
               "P010 uses the established three-byte conservative payload");
        av_frame_free(&p010Source);
    }

    AVFrame* rgbaSource = makeFrame(AV_PIX_FMT_RGBA, 16, 8);
    expect(rgbaSource != nullptr, "RGBA source frame allocation succeeds");
    if (rgbaSource) {
        jcut::core::FramePayloadCore fallback;
        expect(fallback.cloneHardwareFrame(rgbaSource, AV_PIX_FMT_RGBA),
               "fallback-format hardware frame is cloned");
        expect(fallback.hardwareFrameMemoryUsage() ==
                   static_cast<std::size_t>(16 * 8 * 4 * 4),
               "unknown hardware layouts keep the four-byte fallback estimate");
        av_frame_free(&rgbaSource);
    }
}

} // namespace

int main()
{
    testOpaquePayloadOwnershipAndAccounting();
    testHardwareCloneCropAndNv12Residency();
    testHighBitDepthAndFallbackResidency();
    if (failures != 0) {
        std::cerr << failures << " frame payload assertion(s) failed\n";
        return 1;
    }
    std::cout << "frame payload core assertions passed\n";
    return 0;
}
