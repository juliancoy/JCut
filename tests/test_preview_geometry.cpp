#include <QtTest/QtTest>

#include <cmath>

#include "../editor_shared.h"

namespace {

TimelineClip makeOverlayClip() {
    TimelineClip clip;
    clip.id = QStringLiteral("clip-geom");
    clip.filePath = QStringLiteral("/tmp/nonexistent.wav");
    clip.mediaType = ClipMediaType::Audio;
    clip.hasAudio = true;
    clip.transcriptOverlay.boxWidth = 420.0;
    clip.transcriptOverlay.boxHeight = 180.0;
    clip.transcriptOverlay.fontPointSize = 42;
    clip.transcriptOverlay.translationX = 0.0;
    clip.transcriptOverlay.translationY = 0.0;
    clip.transcriptOverlay.useManualPlacement = true;
    return clip;
}

QRectF mapOutputRectToTargetRect(const QRectF& outputRect,
                                 const QSize& outputSize,
                                 const QRect& targetRect) {
    const QPointF scale = previewCanvasScaleForTargetRect(targetRect, outputSize);
    const qreal outputWidth = qMax<qreal>(1.0, static_cast<qreal>(outputSize.width()));
    const qreal outputHeight = qMax<qreal>(1.0, static_cast<qreal>(outputSize.height()));
    const QSizeF size(qMax<qreal>(40.0, outputRect.width() * scale.x()),
                      qMax<qreal>(20.0, outputRect.height() * scale.y()));
    const QPointF outputTranslation(outputRect.center().x() - (outputWidth / 2.0),
                                    outputRect.center().y() - (outputHeight / 2.0));
    const QPointF center(targetRect.center().x() + (outputTranslation.x() * scale.x()),
                         targetRect.center().y() + (outputTranslation.y() * scale.y()));
    return QRectF(center.x() - (size.width() / 2.0),
                  center.y() - (size.height() / 2.0),
                  size.width(),
                  size.height());
}

} // namespace

class TestPreviewGeometry : public QObject {
    Q_OBJECT

private slots:
    void testPreviewCanvasBaseRectKeepsOutputAspect();
    void testScaledCanvasRectScalesAroundCenter();
    void testPreviewCanvasScaleTracksZoomedTargetRect();
    void testTranscriptOverlayRectScalesProportionallyWithZoom();
    void testTranscriptOverlayRectMapsToTargetCenterAtZeroTranslation();
    void testBaseCanvasScaleIsZoomInvariantForTextMetrics();
};

void TestPreviewGeometry::testPreviewCanvasBaseRectKeepsOutputAspect() {
    const QRect baseRect =
        previewCanvasBaseRectForWidget(QRect(0, 0, 1400, 900), QSize(1080, 1920), 36);
    QVERIFY(baseRect.isValid());
    QVERIFY(baseRect.width() > 0);
    QVERIFY(baseRect.height() > 0);

    const qreal baseAspect = static_cast<qreal>(baseRect.width()) / static_cast<qreal>(baseRect.height());
    const qreal outputAspect = 1080.0 / 1920.0;
    QVERIFY(std::abs(baseAspect - outputAspect) < 0.01);
}

void TestPreviewGeometry::testScaledCanvasRectScalesAroundCenter() {
    const QRect baseRect =
        previewCanvasBaseRectForWidget(QRect(0, 0, 1280, 720), QSize(1080, 1920), 36);
    const QRect scaledRect = scaledPreviewCanvasRect(baseRect, 1.75);

    QVERIFY(std::abs(scaledRect.center().x() - baseRect.center().x()) <= 1);
    QVERIFY(std::abs(scaledRect.center().y() - baseRect.center().y()) <= 1);
    const qreal expectedW = baseRect.width() * 1.75;
    const qreal expectedH = baseRect.height() * 1.75;
    QVERIFY(std::abs(static_cast<qreal>(scaledRect.width()) - expectedW) <= 1.0);
    QVERIFY(std::abs(static_cast<qreal>(scaledRect.height()) - expectedH) <= 1.0);
}

void TestPreviewGeometry::testPreviewCanvasScaleTracksZoomedTargetRect() {
    const QSize outputSize(1080, 1920);
    const QRect baseRect =
        previewCanvasBaseRectForWidget(QRect(0, 0, 1280, 720), outputSize, 36);
    const QRect zoom1Rect = scaledPreviewCanvasRect(baseRect, 1.0);
    const QRect zoom2Rect = scaledPreviewCanvasRect(baseRect, 2.0);
    const QPointF scale1 = previewCanvasScaleForTargetRect(zoom1Rect, outputSize);
    const QPointF scale2 = previewCanvasScaleForTargetRect(zoom2Rect, outputSize);

    QVERIFY(scale1.x() > 0.0);
    QVERIFY(scale1.y() > 0.0);
    QVERIFY(std::abs((scale2.x() / scale1.x()) - 2.0) < 0.02);
    QVERIFY(std::abs((scale2.y() / scale1.y()) - 2.0) < 0.02);
}

void TestPreviewGeometry::testTranscriptOverlayRectScalesProportionallyWithZoom() {
    const TimelineClip clip = makeOverlayClip();
    const QSize outputSize(1080, 1920);
    const QRect outputRect =
        previewCanvasBaseRectForWidget(QRect(0, 0, 1280, 720), outputSize, 36);
    const QRectF overlayInOutput =
        transcriptOverlayRectInOutputSpace(clip, outputSize, QString(), {}, 0);

    const QRect target1 = scaledPreviewCanvasRect(outputRect, 1.0);
    const QRectF mapped1 = mapOutputRectToTargetRect(overlayInOutput, outputSize, target1);
    const QRect target2 = scaledPreviewCanvasRect(outputRect, 1.6);
    const QRectF mapped2 = mapOutputRectToTargetRect(overlayInOutput, outputSize, target2);

    QVERIFY(std::abs((mapped2.width() / mapped1.width()) - 1.6) < 0.02);
    QVERIFY(std::abs((mapped2.height() / mapped1.height()) - 1.6) < 0.02);
}

void TestPreviewGeometry::testTranscriptOverlayRectMapsToTargetCenterAtZeroTranslation() {
    TimelineClip clip = makeOverlayClip();
    clip.transcriptOverlay.translationX = 0.0;
    clip.transcriptOverlay.translationY = 0.0;
    clip.transcriptOverlay.useManualPlacement = true;

    const QSize outputSize(1080, 1920);
    const QRect baseRect =
        previewCanvasBaseRectForWidget(QRect(0, 0, 1280, 720), outputSize, 36);
    const QRect targetRect = scaledPreviewCanvasRect(baseRect, 1.3);
    const QRectF overlayInOutput =
        transcriptOverlayRectInOutputSpace(clip, outputSize, QString(), {}, 0);
    const QRectF mapped = mapOutputRectToTargetRect(overlayInOutput, outputSize, targetRect);

    QVERIFY(std::abs(mapped.center().x() - targetRect.center().x()) < 1.0);
    QVERIFY(std::abs(mapped.center().y() - targetRect.center().y()) < 1.0);
}

void TestPreviewGeometry::testBaseCanvasScaleIsZoomInvariantForTextMetrics() {
    const QSize outputSize(1080, 1920);
    const QRect baseRect =
        previewCanvasBaseRectForWidget(QRect(0, 0, 1280, 720), outputSize, 36);
    const QRect zoom1Rect = scaledPreviewCanvasRect(baseRect, 1.0);
    const QRect zoom24Rect = scaledPreviewCanvasRect(baseRect, 2.4);

    const QPointF baseScale = previewCanvasScaleForTargetRect(baseRect, outputSize);
    const QPointF zoomedScale = previewCanvasScaleForTargetRect(zoom24Rect, outputSize);

    QVERIFY(std::abs(baseScale.x() - previewCanvasScaleForTargetRect(zoom1Rect, outputSize).x()) < 0.0001);
    QVERIFY(std::abs(baseScale.y() - previewCanvasScaleForTargetRect(zoom1Rect, outputSize).y()) < 0.0001);
    QVERIFY(zoomedScale.x() > baseScale.x());
    QVERIFY(zoomedScale.y() > baseScale.y());
}

QTEST_MAIN(TestPreviewGeometry)
#include "test_preview_geometry.moc"
