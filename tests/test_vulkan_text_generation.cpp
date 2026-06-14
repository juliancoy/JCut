#include <QtTest/QtTest>

#include "vulkan_text_renderer.h"

class TestVulkanTextGeneration : public QObject {
    Q_OBJECT

private slots:
    void speakerLabelGeneratesGlyphAtlasAndSeparateCards();
    void speakerLabelStyleControlsAffectLayout();
    void transcriptOverlayGeneratesBackgroundHighlightAndGlyphs();
    void transcriptOverlayKeepsExpectedScaleWhenTitleIsEnabled();
    void transcriptOverlayCrowdedBoxUsesSingleReadableLine();
    void transcriptOverlayLayoutsRemainReadableAcrossPreviewSizes();
    void transcriptOverlayKeyTracksActiveWordAndText();
    void emptyInputsDoNotGenerateText();
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

void TestVulkanTextGeneration::transcriptOverlayGeneratesBackgroundHighlightAndGlyphs()
{
    VulkanTextRenderer renderer;
    const QSize outputSize(1080, 1920);
    const QRectF outputRect(80.0, 1320.0, 920.0, 260.0);
    const VulkanTextLayoutDebug debug = renderer.buildTranscriptOverlayLayoutForTesting(
        outputSize,
        transcriptClip(),
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

void TestVulkanTextGeneration::transcriptOverlayKeyTracksActiveWordAndText()
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
    QVERIFY2(activeFirst.atlasKey != activeThird.atlasKey,
             "active word changes must invalidate transcript atlas/layout key");
    QVERIFY2(activeThird.atlasKey != changedText.atlasKey,
             "word text changes must invalidate transcript atlas/layout key");
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

QTEST_MAIN(TestVulkanTextGeneration)
#include "test_vulkan_text_generation.moc"
