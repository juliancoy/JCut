#include <QtTest/QtTest>

#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSignalSpy>

#include "../async_decoder.h"
#include "../debug_controls.h"
#include "../decoder_context.h"
#include "../editor_shared_media.h"
#include "../memory_budget.h"
#include "../preview_frame_selection.h"
#include "../timeline_cache.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

using namespace editor;

class TestNoProxyHardwarePlaybackContract : public QObject {
    Q_OBJECT

private slots:
    void noProxyPreviewKeepsOriginalMediaPath();
    void staleHardwareFrameIsPresentedNotBlack();
    void optionalFixtureDecodesHardwareWithoutProxy();
};

namespace {

TimelineClip makeSixtyFpsClip(const QString& path)
{
    TimelineClip clip;
    clip.id = QStringLiteral("clip-no-proxy");
    clip.label = QStringLiteral("No Proxy Hardware Clip");
    clip.filePath = path;
    clip.proxyPath = QStringLiteral("/tmp/should-not-be-used.proxy");
    clip.useProxy = false;
    clip.mediaType = ClipMediaType::Video;
    clip.sourceKind = MediaSourceKind::File;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.sourceFps = 60.0;
    clip.sourceInFrame = 0;
    clip.sourceDurationFrames = 600;
    clip.startFrame = 0;
    clip.durationFrames = 300;
    clip.playbackRate = 1.0;
    return clip;
}

FrameHandle makeHardwareFrame(int64_t frameNumber, const QString& path)
{
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return {};
    }
    frame->format = AV_PIX_FMT_NV12;
    frame->width = 16;
    frame->height = 16;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return {};
    }
    FrameHandle handle = FrameHandle::createHardwareFrame(frame, frameNumber, path, AV_PIX_FMT_NV12);
    av_frame_free(&frame);
    return handle;
}

class ScopedDecodePreference {
public:
    explicit ScopedDecodePreference(DecodePreference preference)
        : previous(debugDecodePreference())
    {
        setDebugDecodePreference(preference);
    }

    ~ScopedDecodePreference()
    {
        setDebugDecodePreference(previous);
    }

private:
    DecodePreference previous;
};

}  // namespace

void TestNoProxyHardwarePlaybackContract::noProxyPreviewKeepsOriginalMediaPath()
{
    const TimelineClip clip = makeSixtyFpsClip(QStringLiteral("/tmp/original-source.mp4"));
    QCOMPARE(playbackProxyPathForClip(clip), QString());
    QCOMPARE(interactivePreviewMediaPathForClip(clip), clip.filePath);
}

void TestNoProxyHardwarePlaybackContract::staleHardwareFrameIsPresentedNotBlack()
{
    AsyncDecoder decoder;
    MemoryBudget budget;
    TimelineCache cache(&decoder, &budget);
    const TimelineClip clip = makeSixtyFpsClip(QStringLiteral("/tmp/original-source.mp4"));
    cache.registerClip(clip);
    cache.setPlaybackState(TimelineCache::PlaybackState::Playing);
    cache.setPlayheadFrame(58);

    const FrameHandle lateHardwareFrame = makeHardwareFrame(100, clip.filePath);
    QVERIFY2(!lateHardwareFrame.isNull(), "test must synthesize a hardware payload");
    QVERIFY(lateHardwareFrame.hasHardwareFrame());
    decoder.frameReady(lateHardwareFrame);

    const int64_t requestedSourceFrame = 116;
    QVERIFY2(!cache.hasDisplayableFrameForPreview(
                 clip.id,
                 requestedSourceFrame,
                 true,
                 true,
                 true),
             "cache displayability may still classify this frame as stale for health diagnostics");

    const int64_t maxStaleFrameDelta = previewMaxPlaybackStaleFrameDelta(clip.sourceFps);
    QVERIFY(previewFrameIsTooStaleForPlayback(lateHardwareFrame,
                                              requestedSourceFrame,
                                              maxStaleFrameDelta));

    const PreviewFrameSelectionResult selection = selectPreviewFrame(
        PreviewFrameSelectionRequest{
            clip.id,
            requestedSourceFrame,
            true,
            false,
            false,
            true,
            false,
            false,
            -1,
        },
        &cache,
        nullptr,
        FrameHandle(),
        [](const FrameHandle& frame) {
            return !frame.isNull() && !frame.hasHardwareFrame() && !frame.hasGpuTexture();
        });

    QVERIFY2(!selection.frame.isNull(),
             "direct Vulkan playback must hold a late hardware frame instead of producing a missing/black frame");
    QVERIFY(selection.frame.hasHardwareFrame());
    QCOMPARE(selection.frame.frameNumber(), static_cast<int64_t>(100));
    QVERIFY(selection.selectedApproximate);
    QVERIFY(!selection.rejectedStale);
}

void TestNoProxyHardwarePlaybackContract::optionalFixtureDecodesHardwareWithoutProxy()
{
    const QString fixturePath =
        QString::fromLocal8Bit(qgetenv("JCUT_NO_PROXY_HARDWARE_VIDEO")).trimmed();
    if (fixturePath.isEmpty()) {
        QSKIP("Set JCUT_NO_PROXY_HARDWARE_VIDEO to a real H.264/H.265 source to run the hardware no-proxy playback fixture.");
    }
    QVERIFY2(QFileInfo::exists(fixturePath), qPrintable(QStringLiteral("fixture does not exist: %1").arg(fixturePath)));

    ScopedDecodePreference decodePreference(DecodePreference::HardwareZeroCopy);
    TimelineClip clip = makeSixtyFpsClip(fixturePath);
    clip.proxyPath.clear();
    QVERIFY(!clip.useProxy);
    QCOMPARE(interactivePreviewMediaPathForClip(clip), fixturePath);

    DecoderContext decoder(interactivePreviewMediaPathForClip(clip));
    QVERIFY2(decoder.initialize(), "decoder must initialize for the no-proxy source");
    QVERIFY2(decoder.info().hardwareAccelerated,
             qPrintable(QStringLiteral("fixture decoded through %1 instead of hardware").arg(decoder.info().decodePath)));

    int decoded = 0;
    int hardware = 0;
    int nulls = 0;
    qint64 totalDecodeMs = 0;
    QElapsedTimer timer;
    for (int timelineFrame = 0; timelineFrame < 30; ++timelineFrame) {
        const int64_t sourceFrame = sourceFrameForClipAtTimelinePosition(
            clip,
            static_cast<qreal>(timelineFrame),
            {});
        timer.restart();
        const FrameHandle frame = decoder.decodeFrame(sourceFrame);
        totalDecodeMs += timer.elapsed();
        if (frame.isNull()) {
            ++nulls;
            continue;
        }
        ++decoded;
        hardware += frame.hasHardwareFrame() ? 1 : 0;
        QCOMPARE(frame.frameNumber(), sourceFrame);
    }

    QVERIFY2(decoded >= 24, qPrintable(QStringLiteral("decoded only %1/30 requested no-proxy frames").arg(decoded)));
    QCOMPARE(nulls, 0);
    QVERIFY2(hardware == decoded,
             qPrintable(QStringLiteral("hardware frames %1 did not match decoded frames %2").arg(hardware).arg(decoded)));
    QVERIFY2(totalDecodeMs < 3000,
             qPrintable(QStringLiteral("headless no-proxy hardware decode was unexpectedly slow: %1 ms").arg(totalDecodeMs)));
}

QTEST_MAIN(TestNoProxyHardwarePlaybackContract)
#include "test_no_proxy_hardware_playback_contract.moc"
