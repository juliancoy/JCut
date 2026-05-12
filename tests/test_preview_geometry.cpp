#include <QtTest/QtTest>

#include <cmath>

#include "../editor_shared.h"
#include "../preview_view_transform.h"

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
    void testViewTransformAllowsBoundedZoomedOutMouseAnchor();
    void testViewTransformAnchoredZoomPreservesMouseOutputPoint();
    void testViewTransformRoundTripsOutputAndScreenPoints();
    void testAnchoredResizeTranslationKeepsLeftEdgeFixed();
    void testAnchoredResizeTranslationKeepsTopLeftCornerFixed();
    void testClipGeometryMapsNormalizedPointToBounds();
    void testFittedClipRectUsesStableSourceSizeBeforePayloadSize();
    void testWheelZoomHelperPreservesAnchorPoint();
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

void TestPreviewGeometry::testViewTransformAllowsBoundedZoomedOutMouseAnchor() {
    const QRectF widgetRect(0, 0, 1280, 720);
    const QSize outputSize(1080, 1920);
    const PreviewViewTransform transform(widgetRect, outputSize, 36.0, 0.5, QPointF(-500.0, 250.0));

    QVERIFY(transform.clampedPan().x() < 0.0);
    QVERIFY(transform.clampedPan().y() > 0.0);
    QVERIFY(transform.targetRect().left() >= transform.baseRect().left() - 0.0001);
    QVERIFY(transform.targetRect().right() <= transform.baseRect().right() + 0.0001);
    QVERIFY(transform.targetRect().top() >= transform.baseRect().top() - 0.0001);
    QVERIFY(transform.targetRect().bottom() <= transform.baseRect().bottom() + 0.0001);
}

void TestPreviewGeometry::testViewTransformAnchoredZoomPreservesMouseOutputPoint() {
    const QRectF widgetRect(0, 0, 1280, 720);
    const QSize outputSize(1080, 1920);
    const PreviewViewTransform oldTransform(widgetRect, outputSize, 36.0, 1.2, QPointF(40.0, -20.0));
    const QPointF mousePoint(700.0, 250.0);
    const QPointF outputUnderMouse = oldTransform.screenToOutput(mousePoint);
    const qreal nextZoom = 1.8;
    const QPointF nextPan = PreviewViewTransform::panForAnchoredZoom(
        oldTransform.baseRect(),
        oldTransform.targetRect(),
        mousePoint,
        nextZoom);
    const PreviewViewTransform nextTransform(widgetRect, outputSize, 36.0, nextZoom, nextPan);
    const QPointF nextScreenPoint = nextTransform.outputToScreen(outputUnderMouse);

    QVERIFY(std::abs(nextScreenPoint.x() - mousePoint.x()) < 0.0001);
    QVERIFY(std::abs(nextScreenPoint.y() - mousePoint.y()) < 0.0001);
}

void TestPreviewGeometry::testViewTransformRoundTripsOutputAndScreenPoints() {
    const QRectF widgetRect(0, 0, 1280, 720);
    const QSize outputSize(1080, 1920);
    const PreviewViewTransform transform(widgetRect, outputSize, 36.0, 2.0, QPointF(80.0, -40.0));
    const QPointF outputPoint(270.0, 1440.0);
    const QPointF screenPoint = transform.outputToScreen(outputPoint);
    const QPointF roundTrip = transform.screenToOutput(screenPoint);

    QVERIFY(std::abs(roundTrip.x() - outputPoint.x()) < 0.0001);
    QVERIFY(std::abs(roundTrip.y() - outputPoint.y()) < 0.0001);
}

void TestPreviewGeometry::testAnchoredResizeTranslationKeepsLeftEdgeFixed() {
    const QRectF originBounds(200.0, 120.0, 320.0, 180.0);
    const QPointF originTranslation(40.0, -20.0);
    const QPointF originScale(1.0, 1.0);
    const QPointF nextScale(1.5, 1.0);
    const QPointF previewScale(0.2, 0.2);

    const QPointF nextTranslation = PreviewViewTransform::translationForAnchoredResize(
        originTranslation,
        originScale,
        nextScale,
        originBounds,
        PreviewResizeAnchor::Left,
        previewScale);

    const qreal scaleXFactor = nextScale.x() / originScale.x();
    const qreal centerShiftX = (nextTranslation.x() - originTranslation.x()) * previewScale.x();
    const qreal newCenterX = originBounds.center().x() + centerShiftX;
    const qreal oldLeft = originBounds.left();
    const qreal newLeft = newCenterX - (originBounds.width() * scaleXFactor * 0.5);

    QVERIFY(std::abs(newLeft - oldLeft) < 0.0001);
    QVERIFY(std::abs(nextTranslation.y() - originTranslation.y()) < 0.0001);
}

void TestPreviewGeometry::testAnchoredResizeTranslationKeepsTopLeftCornerFixed() {
    const QRectF originBounds(200.0, 120.0, 320.0, 180.0);
    const QPointF originTranslation(40.0, -20.0);
    const QPointF originScale(1.0, 1.0);
    const QPointF nextScale(1.25, 1.5);
    const QPointF previewScale(0.2, 0.2);

    const QPointF nextTranslation = PreviewViewTransform::translationForAnchoredResize(
        originTranslation,
        originScale,
        nextScale,
        originBounds,
        PreviewResizeAnchor::TopLeft,
        previewScale);

    const qreal scaleXFactor = nextScale.x() / originScale.x();
    const qreal scaleYFactor = nextScale.y() / originScale.y();
    const QPointF centerShift((nextTranslation.x() - originTranslation.x()) * previewScale.x(),
                              (nextTranslation.y() - originTranslation.y()) * previewScale.y());
    const QPointF newCenter = originBounds.center() + centerShift;
    const QPointF oldTopLeft = originBounds.topLeft();
    const QPointF newTopLeft(newCenter.x() - (originBounds.width() * scaleXFactor * 0.5),
                             newCenter.y() - (originBounds.height() * scaleYFactor * 0.5));

    QVERIFY(std::abs(newTopLeft.x() - oldTopLeft.x()) < 0.0001);
    QVERIFY(std::abs(newTopLeft.y() - oldTopLeft.y()) < 0.0001);
}

void TestPreviewGeometry::testClipGeometryMapsNormalizedPointToBounds() {
    const QRectF fitted(100.0, 50.0, 320.0, 180.0);
    const QPointF previewScale(0.2, 0.2);
    const PreviewClipGeometry geometry = PreviewViewTransform::clipGeometry(
        fitted,
        previewScale,
        QPointF(50.0, -25.0),
        0.0,
        QPointF(1.5, 0.5));

    const QPointF localCenter =
        PreviewViewTransform::localPointForNormalizedPoint(QPointF(0.5, 0.5), geometry.localRect);
    const QPointF mappedCenter = geometry.clipToScreen.map(localCenter);

    QVERIFY(std::abs(mappedCenter.x() - (fitted.center().x() + 10.0)) < 0.0001);
    QVERIFY(std::abs(mappedCenter.y() - (fitted.center().y() - 5.0)) < 0.0001);
    QVERIFY(std::abs(geometry.bounds.width() - (fitted.width() * 1.5)) < 0.0001);
    QVERIFY(std::abs(geometry.bounds.height() - (fitted.height() * 0.5)) < 0.0001);
}

void TestPreviewGeometry::testFittedClipRectUsesStableSourceSizeBeforePayloadSize() {
    const QRectF targetRect(10.0, 20.0, 400.0, 800.0);
    const QSize outputSize(1080, 1920);
    const QSize sourceSize(1920, 1080);
    const QSize payloadSize(1080, 1920);

    const QRectF fromSource = PreviewViewTransform::fittedClipRect(
        sourceSize,
        payloadSize,
        targetRect,
        outputSize);
    const QRectF fromPayloadOnly = PreviewViewTransform::fittedClipRect(
        QSize(),
        payloadSize,
        targetRect,
        outputSize);

    QVERIFY(fromSource.isValid());
    QVERIFY(fromPayloadOnly.isValid());
    QVERIFY(fromSource.width() > fromSource.height());
    QVERIFY(fromPayloadOnly.height() > fromPayloadOnly.width());
    QVERIFY(std::abs((fromSource.width() / fromSource.height()) - (16.0 / 9.0)) < 0.001);
}

void TestPreviewGeometry::testWheelZoomHelperPreservesAnchorPoint() {
    const QRectF widgetRect(0, 0, 1280, 720);
    const QSize outputSize(1080, 1920);
    const PreviewViewTransform oldTransform(widgetRect, outputSize, 36.0, 1.0, QPointF());
    const QPointF anchor(700.0, 250.0);
    const QPointF outputUnderAnchor = oldTransform.screenToOutput(anchor);

    const PreviewZoomResult zoom = PreviewViewTransform::zoomForWheel(
        widgetRect,
        outputSize,
        36.0,
        1.0,
        QPointF(),
        anchor,
        120);
    const PreviewViewTransform nextTransform(widgetRect, outputSize, 36.0, zoom.zoom, zoom.panOffset);
    const QPointF nextAnchor = nextTransform.outputToScreen(outputUnderAnchor);

    QVERIFY(zoom.changed);
    QVERIFY(zoom.zoom > 1.0);
    QVERIFY(std::abs(nextAnchor.x() - anchor.x()) < 0.0001);
    QVERIFY(std::abs(nextAnchor.y() - anchor.y()) < 0.0001);
}

QTEST_MAIN(TestPreviewGeometry)
#include "test_preview_geometry.moc"
