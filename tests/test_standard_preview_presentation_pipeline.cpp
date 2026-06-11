#include <QtTest/QtTest>

#include <QDateTime>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>

#include "../async_decoder.h"
#include "../memory_budget.h"
#include "../preview_frame_selection.h"
#include "../timeline_cache.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

using namespace editor;

namespace {

TimelineClip makeClip(const QString& id, const QString& path)
{
    TimelineClip clip;
    clip.id = id;
    clip.filePath = path;
    clip.mediaType = ClipMediaType::Video;
    clip.sourceKind = MediaSourceKind::File;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.sourceFps = 60.0;
    clip.startFrame = 0;
    clip.durationFrames = 600;
    clip.sourceInFrame = 0;
    clip.sourceDurationFrames = 600;
    clip.playbackRate = 1.0;
    return clip;
}

FrameHandle makeCpuFrame(int64_t frameNumber, const QString& path)
{
    QImage image(16, 16, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::black);
    return FrameHandle::createCpuFrame(image, frameNumber, path);
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

bool rejectCpuOnlyPayload(const FrameHandle& frame)
{
    return !frame.isNull() && !frame.hasHardwareFrame() && !frame.hasGpuTexture();
}

}  // namespace

class TestStandardPreviewPresentationPipeline : public QObject {
    Q_OBJECT

private slots:
    void visibleRequestDecisionKeepsCurrentRequestsMoving();
    void pendingVisibleRequestDeduplicatesCallbacks();
    void strictPayloadRequirementRejectsCpuFrames();
    void selectionUsesBoundedPriorHardwareFrameDuringPlayback();
    void staleApproximateSelectionFallsBackToHeldFrame();
    void stalePlaybackFramePredicateBoundsApproximatePresentation();
};

void TestStandardPreviewPresentationPipeline::visibleRequestDecisionKeepsCurrentRequestsMoving()
{
    PreviewVisibleRequestInputs inputs;
    inputs.exactCached = true;
    PreviewVisibleRequestDecision decision = evaluatePreviewVisibleRequest(inputs);
    QVERIFY(!decision.dispatch);
    QCOMPARE(decision.blockReason, QStringLiteral("exact_frame_already_cached"));

    inputs = {};
    inputs.pending = true;
    decision = evaluatePreviewVisibleRequest(inputs);
    QVERIFY(!decision.dispatch);
    QCOMPARE(decision.blockReason, QStringLiteral("visible_request_already_pending"));

    inputs = {};
    inputs.pending = true;
    inputs.forceRetry = true;
    decision = evaluatePreviewVisibleRequest(inputs);
    QVERIFY(decision.dispatch);
    QCOMPARE(decision.decision, QStringLiteral("dispatch"));

    inputs = {};
    inputs.displayableCached = true;
    inputs.pendingBacklog = 8;
    inputs.backlogLimit = 4;
    decision = evaluatePreviewVisibleRequest(inputs);
    QVERIFY2(decision.dispatch,
             "an approximate/displayable frame must not suppress the exact current visible request");
    QCOMPARE(decision.decision, QStringLiteral("dispatch_current_over_backlog"));
}

void TestStandardPreviewPresentationPipeline::pendingVisibleRequestDeduplicatesCallbacks()
{
    MemoryBudget budget;
    TimelineCache cache(nullptr, &budget);
    const TimelineClip clip = makeClip(QStringLiteral("clip-pending"), QStringLiteral("/tmp/pending.mp4"));
    cache.registerClip(clip);

    bool firstCallbackCalled = false;
    bool secondCallbackCalled = false;
    cache.requestFrame(clip.id, 24, [&firstCallbackCalled](FrameHandle) {
        firstCallbackCalled = true;
    });
    cache.requestFrame(clip.id, 24, [&secondCallbackCalled](FrameHandle) {
        secondCallbackCalled = true;
    });

    QVERIFY(!firstCallbackCalled);
    QVERIFY(!secondCallbackCalled);
    QCOMPARE(cache.pendingVisibleRequestCount(), 1);
    QVERIFY(cache.isVisibleRequestPending(clip.id, 24));

    const QJsonArray pending = cache.pendingVisibleDebugSnapshot(QDateTime::currentMSecsSinceEpoch(), 4);
    QCOMPARE(pending.size(), 1);
    const QJsonObject entry = pending.at(0).toObject();
    QCOMPARE(entry.value(QStringLiteral("clip_id")).toString(), clip.id);
    QCOMPARE(entry.value(QStringLiteral("frame_number")).toInteger(), static_cast<qint64>(24));
    QCOMPARE(entry.value(QStringLiteral("callback_count")).toInt(), 2);
}

void TestStandardPreviewPresentationPipeline::strictPayloadRequirementRejectsCpuFrames()
{
    AsyncDecoder decoder;
    MemoryBudget budget;
    TimelineCache cache(&decoder, &budget);
    const TimelineClip clip = makeClip(QStringLiteral("clip-cpu"), QStringLiteral("/tmp/cpu.mp4"));
    cache.registerClip(clip);

    const FrameHandle cpuFrame = makeCpuFrame(42, clip.filePath);
    QVERIFY(cpuFrame.hasCpuImage());
    decoder.frameReady(cpuFrame);

    QVERIFY(cache.hasExactFrameForPreview(clip.id, 42, false, true, false));
    QVERIFY(!cache.hasExactFrameForPreview(clip.id, 42, false, true, true));

    const PreviewFrameSelectionResult selection = selectPreviewFrame(
        PreviewFrameSelectionRequest{
            clip.id,
            42,
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
        rejectCpuOnlyPayload);

    QVERIFY(selection.frame.isNull());
    QVERIFY(selection.rejectedStale);
    QCOMPARE(selection.selection, QStringLiteral("stale"));
}

void TestStandardPreviewPresentationPipeline::selectionUsesBoundedPriorHardwareFrameDuringPlayback()
{
    AsyncDecoder decoder;
    MemoryBudget budget;
    TimelineCache cache(&decoder, &budget);
    cache.setPlaybackState(TimelineCache::PlaybackState::Playing);
    const TimelineClip clip = makeClip(QStringLiteral("clip-hardware"), QStringLiteral("/tmp/hardware.mp4"));
    cache.registerClip(clip);

    const FrameHandle hardwareFrame = makeHardwareFrame(100, clip.filePath);
    QVERIFY2(!hardwareFrame.isNull(), "test must synthesize a hardware frame");
    QVERIFY(hardwareFrame.hasHardwareFrame());
    decoder.frameReady(hardwareFrame);

    const PreviewFrameSelectionResult selection = selectPreviewFrame(
        PreviewFrameSelectionRequest{
            clip.id,
            104,
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
        rejectCpuOnlyPayload);

    QVERIFY(!selection.frame.isNull());
    QVERIFY(selection.frame.hasHardwareFrame());
    QCOMPARE(selection.frame.frameNumber(), static_cast<int64_t>(100));
    QVERIFY(selection.selectedApproximate);
    QCOMPARE(selection.selection, QStringLiteral("latest"));
}

void TestStandardPreviewPresentationPipeline::staleApproximateSelectionFallsBackToHeldFrame()
{
    AsyncDecoder decoder;
    MemoryBudget budget;
    TimelineCache cache(&decoder, &budget);
    cache.setPlaybackState(TimelineCache::PlaybackState::Playing);
    const TimelineClip clip = makeClip(QStringLiteral("clip-held"), QStringLiteral("/tmp/held.mp4"));
    cache.registerClip(clip);

    const FrameHandle staleCachedFrame = makeHardwareFrame(100, clip.filePath);
    const FrameHandle heldFrame = makeHardwareFrame(108, clip.filePath);
    QVERIFY(!staleCachedFrame.isNull());
    QVERIFY(!heldFrame.isNull());
    decoder.frameReady(staleCachedFrame);

    const PreviewFrameSelectionResult selection = selectPreviewFrame(
        PreviewFrameSelectionRequest{
            clip.id,
            110,
            true,
            false,
            false,
            true,
            false,
            false,
            8,
        },
        &cache,
        nullptr,
        heldFrame,
        [](const FrameHandle& frame) {
            return !frame.isNull() && frame.frameNumber() < 106;
        });

    QVERIFY(!selection.frame.isNull());
    QCOMPARE(selection.frame.frameNumber(), static_cast<int64_t>(108));
    QVERIFY(selection.selectedHeld);
    QVERIFY(selection.rejectedStale);
    QCOMPARE(selection.selection, QStringLiteral("held"));
}

void TestStandardPreviewPresentationPipeline::stalePlaybackFramePredicateBoundsApproximatePresentation()
{
    const QString path = QStringLiteral("/tmp/stale.mp4");
    const FrameHandle nearFrame = makeHardwareFrame(100, path);
    const FrameHandle staleFrame = makeHardwareFrame(100, path);
    QVERIFY(!nearFrame.isNull());
    QVERIFY(!staleFrame.isNull());

    const int64_t maxDelta = previewMaxPlaybackStaleFrameDelta(60.0);
    QCOMPARE(maxDelta, static_cast<int64_t>(5));
    QVERIFY(!previewFrameIsTooStaleForPlayback(nearFrame, 100 + maxDelta, maxDelta));
    QVERIFY(previewFrameIsTooStaleForPlayback(staleFrame, 101 + maxDelta, maxDelta));
}

QTEST_MAIN(TestStandardPreviewPresentationPipeline)
#include "test_standard_preview_presentation_pipeline.moc"
