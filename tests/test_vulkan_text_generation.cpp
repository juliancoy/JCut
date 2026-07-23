#include <QtTest/QtTest>

#include "vulkan_text_renderer.h"
#include "title_mesh_extrusion.h"
#include "title_3d_projection_core.h"

#include <QMatrix4x4>
#include <QVector3D>
#include <QVector4D>

class TestVulkanTextGeneration : public QObject {
    Q_OBJECT

private slots:
    void speakerLabelGeneratesGlyphAtlasAndSeparateCards();
    void speakerLabelStyleControlsAffectLayout();
    void sharedTextExtrudeModesAffectSpeakerAndTranscriptAtlases();
    void transcriptOverlayGeneratesBackgroundHighlightAndGlyphs();
    void transcriptOverlayStyleControlsAffectGlyphPasses();
    void transcriptOverlayCanDisableCurrentWordHighlight();
    void transcriptOverlayKeepsExpectedScaleWhenTitleIsEnabled();
    void transcriptOverlayCrowdedBoxUsesSingleReadableLine();
    void transcriptOverlayLayoutsRemainReadableAcrossPreviewSizes();
    void transcriptOverlaySeparatesAtlasKeyFromActiveWordLayout();
    void transcriptOverlayKeepsFirstLineStableAcrossLineCounts();
    void emptyInputsDoNotGenerateText();
    void titleContourMeshHasFrontBackSideAndBevelGeometry();
    void neutralTitleProjectionMatchesQtVulkanMvp();
};

namespace {

bool rectsContainedIn(const QVector<QRectF>& rects, const QRectF& bounds)
{
    for (const QRectF& rect : rects) {
        if (rect.isEmpty() || !bounds.contains(rect.center())) {
            return false;
        }
    }
    return true;
}

bool colorsContainApprox(const QVector<QColor>& colors,
                         int red,
                         int green,
                         int blue,
                         int alpha,
                         int tolerance = 2)
{
    for (const QColor& color : colors) {
        if (color.red() == red &&
            color.green() == green &&
            color.blue() == blue &&
            std::abs(color.alpha() - alpha) <= tolerance) {
            return true;
        }
    }
    return false;
}

render_detail::SpeakerLabelOverlaySpec speakerSpec()
{
    render_detail::SpeakerLabelOverlaySpec spec;
    spec.showName = true;
    spec.showOrganization = true;
    spec.name = QStringLiteral("Izzy Patoka");
    spec.organization = QStringLiteral("Baltimore County Council");
    spec.nameTextScale = 1.35;
    spec.organizationTextScale = 1.05;
    spec.nameVerticalPosition = 0.82;
    spec.organizationVerticalPosition = 0.91;
    return spec;
}

TimelineClip transcriptClip()
{
    TimelineClip clip;
    clip.id = QStringLiteral("clip-transcript");
    clip.transcriptOverlay.enabled = true;
    clip.transcriptOverlay.fontFamily = kDefaultFontFamily;
    clip.transcriptOverlay.fontPointSize = 58.0;
    clip.transcriptOverlay.bold = true;
    clip.transcriptOverlay.italic = false;
    clip.transcriptOverlay.showBackground = true;
    clip.transcriptOverlay.showShadow = true;
    clip.transcriptOverlay.showSpeakerTitle = true;
    clip.transcriptOverlay.textColor = QColor(QStringLiteral("#f4f8fc"));
    clip.transcriptOverlay.boxWidth = 920.0;
    clip.transcriptOverlay.boxHeight = 260.0;
    return clip;
}

TranscriptOverlayLayout transcriptLayout(int activeWord, const QString& replacement = QString())
{
    TranscriptOverlayLine line1;
    line1.words = QStringList{QStringLiteral("Meet"), QStringLiteral("the"), QStringLiteral("candidates")};
    line1.activeWord = activeWord;

    TranscriptOverlayLine line2;
    line2.words = QStringList{QStringLiteral("for"), QStringLiteral("Baltimore"), replacement.isEmpty() ? QStringLiteral("County") : replacement};
    line2.activeWord = -1;

    TranscriptOverlayLayout layout;
    layout.lines = QVector<TranscriptOverlayLine>{line1, line2};
    return layout;
}

} // namespace

void TestVulkanTextGeneration::speakerLabelGeneratesGlyphAtlasAndSeparateCards()
{
    VulkanTextRenderer renderer;
    const QSize outputSize(1080, 1920);
    const VulkanTextLayoutDebug debug =
        renderer.buildSpeakerLabelLayoutForTesting(outputSize, speakerSpec());

    QVERIFY2(debug.valid, "speaker label layout should be generated without a Vulkan device");
    QCOMPARE(debug.atlasSize, QSize(2048, 2048));
    QCOMPARE(debug.cardCount, 2);
    QVERIFY(debug.glyphAtlasEntryCount > 10);
    QVERIFY(debug.glyphDrawCount > debug.glyphAtlasEntryCount);
    QVERIFY(rectsContainedIn(debug.cards, QRectF(QPointF(0, 0), QSizeF(outputSize))));
    QVERIFY(rectsContainedIn(debug.glyphRects, QRectF(QPointF(0, 0), QSizeF(outputSize))));
}

void TestVulkanTextGeneration::speakerLabelStyleControlsAffectLayout()
{
    VulkanTextRenderer renderer;
    render_detail::SpeakerLabelOverlaySpec spec = speakerSpec();
    spec.nameColor = QColor(QStringLiteral("#ff3366"));
    spec.organizationColor = QColor(QStringLiteral("#44ccff"));
    spec.shadowColor = QColor(1, 2, 3, 77);
    spec.showShadow = true;
    const VulkanTextLayoutDebug shadowed =
        renderer.buildSpeakerLabelLayoutForTesting(QSize(1080, 1920), spec);

    QVERIFY2(shadowed.valid, "styled speaker label layout should be generated");
    QVERIFY(shadowed.glyphColors.contains(spec.nameColor));
    QVERIFY(shadowed.glyphColors.contains(spec.organizationColor));
    QVERIFY(shadowed.glyphColors.contains(spec.shadowColor));

    spec.showShadow = false;
    const VulkanTextLayoutDebug unshadowed =
        renderer.buildSpeakerLabelLayoutForTesting(QSize(1080, 1920), spec);
    QVERIFY2(unshadowed.valid, "speaker label layout should survive disabled shadows");
    QVERIFY(!unshadowed.glyphColors.contains(spec.shadowColor));
    QVERIFY(unshadowed.glyphDrawCount < shadowed.glyphDrawCount);
}

void TestVulkanTextGeneration::sharedTextExtrudeModesAffectSpeakerAndTranscriptAtlases()
{
    using Mode = TimelineClip::TitleKeyframe::TextExtrudeMode;
    VulkanTextRenderer renderer;
    render_detail::SpeakerLabelOverlaySpec speaker = speakerSpec();
    const auto flatSpeaker = renderer.buildSpeakerLabelLayoutForTesting(QSize(1080, 1920), speaker);
    speaker.textExtrudeMode = Mode::ErodedSolid;
    const auto erodedSpeaker = renderer.buildSpeakerLabelLayoutForTesting(QSize(1080, 1920), speaker);
    QVERIFY(flatSpeaker.valid && erodedSpeaker.valid);
    QVERIFY(flatSpeaker.atlasKey != erodedSpeaker.atlasKey);
    QVERIFY(flatSpeaker.layoutKey != erodedSpeaker.layoutKey);

    TimelineClip clip = transcriptClip();
    const TranscriptOverlayLayout layout = transcriptLayout(-1);
    const QRectF rect(90, 1400, 900, 300);
    const auto flatTranscript = renderer.buildTranscriptOverlayLayoutForTesting(
        QSize(1080, 1920), clip, layout, rect, QStringLiteral("Speaker"));
    clip.transcriptOverlay.textExtrudeMode = Mode::StackedCopies;
    const auto stackedTranscript = renderer.buildTranscriptOverlayLayoutForTesting(
        QSize(1080, 1920), clip, layout, rect, QStringLiteral("Speaker"));
    clip.transcriptOverlay.textExtrudeMode = Mode::ErodedSolid;
    const auto erodedTranscript = renderer.buildTranscriptOverlayLayoutForTesting(
        QSize(1080, 1920), clip, layout, rect, QStringLiteral("Speaker"));
    QVERIFY(flatTranscript.atlasKey != stackedTranscript.atlasKey);
    QVERIFY(stackedTranscript.atlasKey != erodedTranscript.atlasKey);
    QVERIFY(stackedTranscript.layoutKey != erodedTranscript.layoutKey);
}

void TestVulkanTextGeneration::transcriptOverlayGeneratesBackgroundHighlightAndGlyphs()
{
    VulkanTextRenderer renderer;
    const QSize outputSize(1080, 1920);
    const QRectF outputRect(80.0, 1320.0, 920.0, 260.0);
    const TimelineClip clip = transcriptClip();
    const VulkanTextLayoutDebug debug = renderer.buildTranscriptOverlayLayoutForTesting(
        outputSize,
        clip,
        transcriptLayout(2),
        outputRect,
        QStringLiteral("Council District 2"));

    QVERIFY2(debug.valid, "transcript layout should generate Vulkan text geometry");
    QCOMPARE(debug.atlasSize, QSize(2048, 2048));
    QCOMPARE(debug.backgroundCount, 1);
    QCOMPARE(debug.highlightCount, 1);
    QVERIFY(debug.glyphAtlasEntryCount > 12);
    QVERIFY(debug.glyphDrawCount > debug.glyphAtlasEntryCount);
    QVERIFY(rectsContainedIn(debug.backgrounds, QRectF(QPointF(0, 0), QSizeF(outputSize))));
    QVERIFY(rectsContainedIn(debug.highlights, outputRect));
    QVERIFY(rectsContainedIn(debug.glyphRects, outputRect.adjusted(-8.0, -8.0, 8.0, 8.0)));
}

void TestVulkanTextGeneration::transcriptOverlayStyleControlsAffectGlyphPasses()
{
    VulkanTextRenderer renderer;
    const QSize outputSize(1080, 1920);
    const QRectF outputRect(80.0, 1320.0, 920.0, 260.0);
    TimelineClip clip = transcriptClip();
    clip.transcriptOverlay.showSpeakerTitle = false;
    clip.transcriptOverlay.showShadow = true;
    clip.transcriptOverlay.shadowColor = QColor(7, 11, 13);
    clip.transcriptOverlay.shadowOpacity = 0.42;

    const VulkanTextLayoutDebug shadowed = renderer.buildTranscriptOverlayLayoutForTesting(
        outputSize,
        clip,
        transcriptLayout(-1),
        outputRect,
        QString());
    QVERIFY2(shadowed.valid, "shadowed transcript layout should be generated");
    QVERIFY(colorsContainApprox(shadowed.glyphColors, 7, 11, 13, 107));

    clip.transcriptOverlay.showShadow = false;
    const VulkanTextLayoutDebug unshadowed = renderer.buildTranscriptOverlayLayoutForTesting(
        outputSize,
        clip,
        transcriptLayout(-1),
        outputRect,
        QString());
    QVERIFY2(unshadowed.valid, "transcript layout should survive disabled shadows");
    QVERIFY(unshadowed.glyphDrawCount < shadowed.glyphDrawCount);

    clip.transcriptOverlay.textOutlineEnabled = true;
    clip.transcriptOverlay.textOutlineWidth = 2.0;
    clip.transcriptOverlay.textOutlineColor = QColor(3, 5, 9);
    clip.transcriptOverlay.textOutlineOpacity = 0.75;
    const VulkanTextLayoutDebug dilated = renderer.buildTranscriptOverlayLayoutForTesting(
        outputSize,
        clip,
        transcriptLayout(-1),
        outputRect,
        QString());
    QVERIFY2(dilated.valid, "dilated transcript layout should be generated");
    QVERIFY(dilated.glyphDrawCount > unshadowed.glyphDrawCount);
    QVERIFY(colorsContainApprox(dilated.glyphColors, 3, 5, 9, 191));
}

void TestVulkanTextGeneration::transcriptOverlayCanDisableCurrentWordHighlight()
{
    VulkanTextRenderer renderer;
    const QSize outputSize(1080, 1920);
    const QRectF outputRect(80.0, 1320.0, 920.0, 260.0);
    TimelineClip clip = transcriptClip();
    clip.transcriptOverlay.highlightCurrentWord = false;
    const VulkanTextLayoutDebug debug = renderer.buildTranscriptOverlayLayoutForTesting(
        outputSize,
        clip,
        transcriptLayout(2),
        outputRect,
        QStringLiteral("Council District 2"));

    QVERIFY(debug.valid);
    QCOMPARE(debug.highlightCount, 0);
    QVERIFY(debug.highlights.isEmpty());
}

void TestVulkanTextGeneration::transcriptOverlayKeepsExpectedScaleWhenTitleIsEnabled()
{
    VulkanTextRenderer renderer;
    const QSize outputSize(1080, 1920);
    const QRectF outputRect(80.0, 1320.0, 920.0, 260.0);
    TimelineClip clip = transcriptClip();
    clip.transcriptOverlay.fontPointSize = 42;
    clip.transcriptOverlay.maxLines = 6;
    clip.transcriptOverlay.boxHeight = outputRect.height();
    clip.transcriptOverlay.showSpeakerTitle = true;

    TranscriptOverlayLayout layout;
    for (int i = 0; i < transcriptOverlayEffectiveLinesForBox(clip); ++i) {
        TranscriptOverlayLine line;
        line.words = QStringList{QStringLiteral("steady"), QStringLiteral("caption"), QStringLiteral("scale")};
        line.activeWord = i == 0 ? 1 : -1;
        layout.lines.push_back(line);
    }

    const VulkanTextLayoutDebug debug = renderer.buildTranscriptOverlayLayoutForTesting(
        outputSize,
        clip,
        layout,
        outputRect,
        QStringLiteral("Council District 2"));

    QVERIFY(debug.valid);
    QVERIFY(!debug.glyphRects.isEmpty());
    qreal tallestGlyph = 0.0;
    for (const QRectF& rect : debug.glyphRects) {
        tallestGlyph = qMax(tallestGlyph, rect.height());
    }
    QVERIFY2(tallestGlyph > clip.transcriptOverlay.fontPointSize * 0.45,
             "title reservation should prevent whole-caption shrink below expected subtitle scale");
}

void TestVulkanTextGeneration::transcriptOverlayCrowdedBoxUsesSingleReadableLine()
{
    VulkanTextRenderer renderer;
    const QSize outputSize(1080, 1920);
    const QRectF outputRect(260.0, 1220.0, 560.0, 120.0);
    TimelineClip clip = transcriptClip();
    clip.transcriptOverlay.fontPointSize = 42;
    clip.transcriptOverlay.maxLines = 3;
    clip.transcriptOverlay.boxWidth = outputRect.width();
    clip.transcriptOverlay.boxHeight = outputRect.height();
    clip.transcriptOverlay.showSpeakerTitle = false;
    clip.transcriptOverlay.showShadow = true;

    QCOMPARE(transcriptOverlayEffectiveLinesForBox(clip), 1);

    TranscriptOverlayLine line;
    line.words = QStringList{QStringLiteral("this"), QStringLiteral("must"), QStringLiteral("stay"), QStringLiteral("readable")};
    line.activeWord = 2;
    TranscriptOverlayLayout layout;
    layout.lines = QVector<TranscriptOverlayLine>{line};

    const VulkanTextLayoutDebug debug = renderer.buildTranscriptOverlayLayoutForTesting(
        outputSize,
        clip,
        layout,
        outputRect,
        QString());

    QVERIFY(debug.valid);
    QVERIFY(rectsContainedIn(debug.glyphRects, outputRect));
    QVERIFY(rectsContainedIn(debug.highlights, outputRect));
    QCOMPARE(debug.glyphRects.size(), debug.glyphColors.size());
    QVERIFY(!debug.highlights.isEmpty());
    for (int i = 0; i < debug.glyphRects.size(); ++i) {
        const QColor color = debug.glyphColors.at(i);
        const bool isShadow = color.red() == 0 && color.green() == 0 && color.blue() == 0 && color.alpha() >= 180;
        QVERIFY2(!isShadow || !debug.highlights.constFirst().contains(debug.glyphRects.at(i).center()),
                 "highlighted active words must not receive black shadow glyphs");
    }
}

void TestVulkanTextGeneration::transcriptOverlayLayoutsRemainReadableAcrossPreviewSizes()
{
    struct Case {
        QSize outputSize;
        QRectF outputRect;
        qreal fontPointSize;
    };
    const QVector<Case> cases{
        {QSize(512, 512), QRectF(50.0, 320.0, 412.0, 132.0), 36.0},
        {QSize(608, 1080), QRectF(64.0, 760.0, 480.0, 170.0), 42.0},
        {QSize(1080, 1920), QRectF(80.0, 1320.0, 920.0, 260.0), 58.0},
        {QSize(1920, 1080), QRectF(430.0, 720.0, 1060.0, 220.0), 52.0}
    };

    VulkanTextRenderer renderer;
    for (const Case& item : cases) {
        TimelineClip clip = transcriptClip();
        clip.transcriptOverlay.fontPointSize = item.fontPointSize;
        clip.transcriptOverlay.boxWidth = item.outputRect.width();
        clip.transcriptOverlay.boxHeight = item.outputRect.height();

        const VulkanTextLayoutDebug debug = renderer.buildTranscriptOverlayLayoutForTesting(
            item.outputSize,
            clip,
            transcriptLayout(2),
            item.outputRect,
            QStringLiteral("Council District 2"));

        QVERIFY2(debug.valid,
                 qPrintable(QStringLiteral("GPU text layout should be valid for %1x%2")
                                .arg(item.outputSize.width())
                                .arg(item.outputSize.height())));
        QVERIFY2(rectsContainedIn(debug.backgrounds, QRectF(QPointF(0, 0), QSizeF(item.outputSize))),
                 "subtitle background must stay inside the preview surface");
        QVERIFY2(rectsContainedIn(debug.highlights, item.outputRect),
                 "active-word highlight must stay inside the subtitle box");
        QVERIFY2(rectsContainedIn(debug.glyphRects, item.outputRect.adjusted(-8.0, -8.0, 8.0, 8.0)),
                 "subtitle glyphs must stay inside the subtitle box");

        qreal tallestGlyph = 0.0;
        qreal widestGlyph = 0.0;
        for (const QRectF& rect : debug.glyphRects) {
            tallestGlyph = qMax(tallestGlyph, rect.height());
            widestGlyph = qMax(widestGlyph, rect.width());
        }
        QVERIFY2(tallestGlyph >= item.fontPointSize * 0.42,
                 qPrintable(QStringLiteral("GPU text glyphs too small for %1x%2: tallest=%3 font=%4")
                                .arg(item.outputSize.width())
                                .arg(item.outputSize.height())
                                .arg(tallestGlyph)
                                .arg(item.fontPointSize)));
        QVERIFY2(widestGlyph > tallestGlyph * 0.15,
                 "glyph dimensions must look like real glyph rectangles, not collapsed quads");
    }
}

void TestVulkanTextGeneration::transcriptOverlaySeparatesAtlasKeyFromActiveWordLayout()
{
    VulkanTextRenderer renderer;
    const QSize outputSize(1080, 1920);
    const QRectF outputRect(80.0, 1320.0, 920.0, 260.0);
    const TimelineClip clip = transcriptClip();

    const VulkanTextLayoutDebug activeFirst = renderer.buildTranscriptOverlayLayoutForTesting(
        outputSize, clip, transcriptLayout(0), outputRect, QStringLiteral("Council District 2"));
    const VulkanTextLayoutDebug activeThird = renderer.buildTranscriptOverlayLayoutForTesting(
        outputSize, clip, transcriptLayout(2), outputRect, QStringLiteral("Council District 2"));
    const VulkanTextLayoutDebug changedText = renderer.buildTranscriptOverlayLayoutForTesting(
        outputSize, clip, transcriptLayout(2, QStringLiteral("Council")), outputRect, QStringLiteral("Council District 2"));

    QVERIFY(activeFirst.valid);
    QVERIFY(activeThird.valid);
    QVERIFY(changedText.valid);
    QVERIFY2(activeFirst.layoutKey != activeThird.layoutKey,
             "active word changes must still invalidate transcript layout geometry");
    QVERIFY2(activeFirst.atlasKey == activeThird.atlasKey,
             "active word changes must reuse the transcript glyph atlas");
    QVERIFY2(activeThird.atlasKey != changedText.atlasKey,
             "word text changes must invalidate the transcript glyph atlas");
}

void TestVulkanTextGeneration::transcriptOverlayKeepsFirstLineStableAcrossLineCounts()
{
    VulkanTextRenderer renderer;
    const QSize outputSize(1080, 1920);
    const QRectF outputRect(90.0, 830.0, 900.0, 262.0);
    TimelineClip clip = transcriptClip();
    clip.transcriptOverlay.showShadow = false;
    clip.transcriptOverlay.showSpeakerTitle = false;
    clip.transcriptOverlay.fontPointSize = 42;
    clip.transcriptOverlay.boxWidth = outputRect.width();
    clip.transcriptOverlay.boxHeight = outputRect.height();

    TranscriptOverlayLine line;
    line.words = QStringList{QStringLiteral("steady"), QStringLiteral("first"), QStringLiteral("line")};
    line.activeWord = 1;

    TranscriptOverlayLine secondLine;
    secondLine.words = QStringList{QStringLiteral("second"), QStringLiteral("line")};
    secondLine.activeWord = -1;

    TranscriptOverlayLayout oneLine;
    oneLine.lines = QVector<TranscriptOverlayLine>{line};
    TranscriptOverlayLayout twoLines;
    twoLines.lines = QVector<TranscriptOverlayLine>{line, secondLine};

    const VulkanTextLayoutDebug oneLineDebug = renderer.buildTranscriptOverlayLayoutForTesting(
        outputSize, clip, oneLine, outputRect, QString());
    const VulkanTextLayoutDebug twoLineDebug = renderer.buildTranscriptOverlayLayoutForTesting(
        outputSize, clip, twoLines, outputRect, QString());

    QVERIFY(oneLineDebug.valid);
    QVERIFY(twoLineDebug.valid);
    QVERIFY(!oneLineDebug.glyphRects.isEmpty());
    QVERIFY(!twoLineDebug.glyphRects.isEmpty());

    auto minGlyphTop = [](const QVector<QRectF>& rects) {
        qreal top = std::numeric_limits<qreal>::max();
        for (const QRectF& rect : rects) {
            top = qMin(top, rect.top());
        }
        return top;
    };

    QVERIFY2(std::abs(minGlyphTop(oneLineDebug.glyphRects) -
                      minGlyphTop(twoLineDebug.glyphRects)) < 1.0,
             "the first transcript line should not bounce vertically when a second line appears");
}

void TestVulkanTextGeneration::emptyInputsDoNotGenerateText()
{
    VulkanTextRenderer renderer;
    render_detail::SpeakerLabelOverlaySpec emptySpeaker;
    emptySpeaker.showName = true;
    emptySpeaker.showOrganization = true;
    QVERIFY(!renderer.buildSpeakerLabelLayoutForTesting(QSize(1080, 1920), emptySpeaker).valid);

    QVERIFY(!renderer.buildTranscriptOverlayLayoutForTesting(
        QSize(1080, 1920), transcriptClip(), TranscriptOverlayLayout{}, QRectF(0, 0, 900, 220), QString()).valid);
}

void TestVulkanTextGeneration::titleContourMeshHasFrontBackSideAndBevelGeometry()
{
    TitleMeshExtrusionOptions options;
    options.fontFamily = kDefaultFontFamily;
    options.bold = true;
    options.pixelHeight = 96;
    options.depth = 0.35;
    options.bevelScale = 0.8;
    QString error;
    const QVector<TitleMeshVertex> mesh = buildExtrudedTitleMesh(QStringLiteral("O8"), options, &error);
    QVERIFY2(!mesh.isEmpty(), qPrintable(error));
    QCOMPARE(mesh.size() % 3, 0);
    bool front = false, back = false, side = false, bevel = false;
    for (const TitleMeshVertex& vertex : mesh) {
        front = front || vertex.normal.z() > 0.99f;
        back = back || vertex.normal.z() < -0.99f;
        side = side || std::abs(vertex.normal.z()) < 0.05f;
        bevel = bevel || (std::abs(vertex.normal.z()) > 0.1f && std::abs(vertex.normal.z()) < 0.99f);
    }
    QVERIFY(front);
    QVERIFY(back);
    QVERIFY(side);
    QVERIFY(bevel);

    const QVector<TitleMeshVertex> multiline =
        buildExtrudedTitleMesh(QStringLiteral("A\nA"), options, &error);
    QVERIFY2(!multiline.isEmpty(), qPrintable(error));

    const QVector<TitleMeshVertex> upright =
        buildExtrudedTitleMesh(QStringLiteral("L"), options, &error);
    QVERIFY2(!upright.isEmpty(), qPrintable(error));
    qreal frontY = 0.0;
    int frontCount = 0;
    for (const TitleMeshVertex& vertex : upright) {
        if (vertex.normal.z() > 0.99f) {
            frontY += vertex.position.y();
            ++frontCount;
        }
    }
    QVERIFY(frontCount > 0);
    QVERIFY2(frontY / frontCount > 0.0,
             "screen-space mesh orientation must keep the foot of L below its stem");
}

void TestVulkanTextGeneration::neutralTitleProjectionMatchesQtVulkanMvp()
{
    constexpr int width = 640;
    constexpr int height = 360;
    constexpr float cameraDistance = 5.2f;
    constexpr float fovDegrees = 43.0f;
    const QRectF rect(95.0, 74.0, 218.0, 82.0);
    const QPointF titleCenter(331.0, 193.0);
    const qreal aspect =
        static_cast<qreal>(width) / static_cast<qreal>(height);
    const qreal halfViewHeight =
        std::tan((fovDegrees * M_PI / 180.0) * 0.5) *
        cameraDistance;
    const qreal halfViewWidth = halfViewHeight * aspect;
    const auto screenToWorld = [&](const QPointF& point) {
        return QVector3D(
            static_cast<float>(
                ((2.0 * point.x() / width) - 1.0) *
                halfViewWidth),
            static_cast<float>(
                ((2.0 * point.y() / height) - 1.0) *
                halfViewHeight),
            0.0f);
    };
    const QVector3D centerWorld =
        screenToWorld(titleCenter);
    const QPointF localCenter =
        rect.center() - titleCenter;
    const QVector3D localWorld(
        static_cast<float>(
            (localCenter.x() / width) * 2.0 *
            halfViewWidth),
        static_cast<float>(
            (localCenter.y() / height) * 2.0 *
            halfViewHeight),
        0.0f);

    jcut::Title3DProjectionCore projection;
    projection.enabled = true;
    projection.yawDegrees = 37.0;
    projection.pitchDegrees = -19.0;
    projection.rollDegrees = 11.0;
    projection.depth = 0.85;
    projection.scale = 1.27;

    QMatrix4x4 qtProjection;
    qtProjection.perspective(
        fovDegrees,
        static_cast<float>(aspect),
        0.1f,
        32.0f);
    QMatrix4x4 view;
    view.lookAt(
        QVector3D(0.0f, 0.0f, cameraDistance),
        QVector3D(0.0f, 0.0f, 0.0f),
        QVector3D(0.0f, 1.0f, 0.0f));
    QMatrix4x4 model;
    model.translate(centerWorld);
    model.rotate(
        static_cast<float>(projection.yawDegrees),
        0.0f,
        1.0f,
        0.0f);
    model.rotate(
        static_cast<float>(projection.pitchDegrees),
        1.0f,
        0.0f,
        0.0f);
    model.rotate(
        static_cast<float>(projection.rollDegrees),
        0.0f,
        0.0f,
        1.0f);
    model.translate(
        localWorld.x(),
        localWorld.y(),
        static_cast<float>(projection.depth));
    model.scale(
        static_cast<float>(
            (rect.width() / width) *
            halfViewWidth *
            projection.scale),
        static_cast<float>(
            (rect.height() / height) *
            halfViewHeight *
            projection.scale),
        1.0f);
    const QMatrix4x4 mvp =
        qtProjection * view * model;

    for (const auto& corner : {
             std::pair{-1.0, -1.0},
             std::pair{1.0, -1.0},
             std::pair{-1.0, 1.0},
             std::pair{1.0, 1.0}}) {
        const QVector4D clip =
            mvp * QVector4D(
                      static_cast<float>(corner.first),
                      static_cast<float>(corner.second),
                      0.0f,
                      1.0f);
        QVERIFY(std::abs(clip.w()) > 0.0001f);
        const double expectedX =
            (clip.x() / clip.w() + 1.0) *
            width * 0.5;
        const double expectedY =
            (clip.y() / clip.w() + 1.0) *
            height * 0.5;
        const double sourceX =
            corner.first < 0.0 ? rect.left() : rect.right();
        const double sourceY =
            corner.second < 0.0 ? rect.top() : rect.bottom();
        const jcut::TitleProjectedPointCore actual =
            jcut::projectTitle3DPointCore(
                sourceX,
                sourceY,
                rect.center().x(),
                rect.center().y(),
                titleCenter.x(),
                titleCenter.y(),
                width,
                height,
                projection);
        QVERIFY(actual.valid);
        QVERIFY2(
            std::abs(actual.x - expectedX) < 0.001,
            "neutral title X projection diverged from Qt's Vulkan MVP");
        QVERIFY2(
            std::abs(actual.y - expectedY) < 0.001,
            "neutral title Y projection diverged from Qt's Vulkan MVP");
    }

    projection.enabled = false;
    const auto identity = jcut::projectTitle3DPointCore(
        17.25,
        28.5,
        rect.center().x(),
        rect.center().y(),
        titleCenter.x(),
        titleCenter.y(),
        width,
        height,
        projection);
    QVERIFY(identity.valid);
    QCOMPARE(identity.x, 17.25);
    QCOMPARE(identity.y, 28.5);
}

QTEST_MAIN(TestVulkanTextGeneration)
#include "test_vulkan_text_generation.moc"
