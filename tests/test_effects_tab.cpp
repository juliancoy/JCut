#include <QtTest/QtTest>

#include "../background_fill_effect.h"
#include "../editor_effect_presets.h"
#include "../effects_tab.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDial>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QWidget>

namespace {

struct EffectAnimationWidgets {
    QLabel path;
    QComboBox edgeFillEffect;
    QSpinBox edgeFillPixels;
    QDoubleSpinBox edgeFillPower;
    QDoubleSpinBox edgeFillOpacity;
    QDoubleSpinBox edgeFillBrightness;
    QDoubleSpinBox edgeFillSaturation;
    QComboBox presetCategory;
    QComboBox preset;
    QPushButton previousPreset{QStringLiteral("-")};
    QPushButton nextPreset{QStringLiteral("+")};
    QCheckBox enabled;
    QCheckBox transcriptAware;
    QPushButton keyOn{QStringLiteral("Key On")};
    QPushButton keyOff{QStringLiteral("Key Off")};
    QPushButton keyParameters{QStringLiteral("Key Parameters")};
    QPushButton removeKey{QStringLiteral("Remove Key")};
    QLabel keySummary;
    QTableWidget keyTable;
    QLabel presetSpecificHelp;
    QSpinBox rows;
    QDoubleSpinBox speed;
    QDoubleSpinBox scale;
    QCheckBox alternate;
    QComboBox tilingPattern;
    QDoubleSpinBox tilingSpacing;
    QCheckBox tilingWrap;
    QWidget maskBoundingBoxSection;
    QCheckBox tilingMaskBounds;
    QDoubleSpinBox tilingMaskIslandSigma;
    QCheckBox maskBoundingBoxPreview;
    QWidget directionalEchoControls;
    QDial directionalEchoDirection;
    QLabel directionalEchoDirectionValue;
    QDial directionalEchoSpread;
    QLabel directionalEchoSpreadValue;
    QDial directionalEchoHue;
    QLabel directionalEchoHueValue;
    QLabel directionalEchoSummary;
    QWidget stepRepeatFillControls;
    QDial stepRepeatFillGuideScale;
    QLabel stepRepeatFillGuideScaleValue;
    QDial stepRepeatFillLumaMatch;
    QLabel stepRepeatFillLumaMatchValue;
    QDial stepRepeatFillHueMatch;
    QLabel stepRepeatFillHueMatchValue;
    QLabel stepRepeatFillSummary;
    QComboBox modulationMode;
    QComboBox modulationTarget;
    QDoubleSpinBox modulationAmount;
    QDoubleSpinBox modulationRate;
    QDoubleSpinBox modulationPhase;

    EffectAnimationWidgets()
    {
        edgeFillEffect.addItem(
            QStringLiteral("None"),
            backgroundFillEffectToString(BackgroundFillEffect::None));
        edgeFillEffect.addItem(
            QStringLiteral("Progressive Edge Stretch"),
            backgroundFillEffectToString(BackgroundFillEffect::ProgressiveEdgeStretch));
        edgeFillEffect.addItem(
            QStringLiteral("Tile"),
            backgroundFillEffectToString(BackgroundFillEffect::Tile));
        edgeFillPixels.setRange(1, 512);
        edgeFillPower.setRange(0.25, 8.0);
        edgeFillOpacity.setRange(0.0, 100.0);
        edgeFillBrightness.setRange(-100.0, 100.0);
        edgeFillSaturation.setRange(0.0, 300.0);
        QStringList groups;
        for (const EffectPresetUiOption& option : effectPresetUiOptions()) {
            if (!groups.contains(option.group)) {
                groups.push_back(option.group);
                presetCategory.addItem(option.group, option.group);
            }
        }
        preset.addItem(
            QStringLiteral("None"),
            static_cast<int>(ClipEffectPreset::None));
        preset.addItem(
            QStringLiteral("Neon Glow"),
            static_cast<int>(ClipEffectPreset::NeonGlow));
        preset.addItem(
            QStringLiteral("Source Tile"),
            static_cast<int>(ClipEffectPreset::SourceTile));
        modulationMode.addItem(QStringLiteral("None"), QStringLiteral("none"));
        modulationMode.addItem(QStringLiteral("LFO"), QStringLiteral("lfo"));
        modulationMode.addItem(QStringLiteral("Steady increase"),
                               QStringLiteral("steady_increase"));
        modulationTarget.addItem(QStringLiteral("Copies / radius"),
                                 QStringLiteral("rows"));
        modulationTarget.addItem(QStringLiteral("Speed"),
                                 QStringLiteral("speed"));
        modulationTarget.addItem(QStringLiteral("Amount / strength"),
                                 QStringLiteral("scale"));
        modulationTarget.addItem(QStringLiteral("Spacing"),
                                 QStringLiteral("spacing"));
        modulationAmount.setRange(-512.0, 512.0);
        modulationRate.setRange(0.0, 20.0);
        modulationPhase.setRange(-360.0, 360.0);
        rows.setRange(1, 96);
        speed.setRange(-8.0, 8.0);
        scale.setRange(0.1, 8.0);
        tilingSpacing.setRange(0.1, 8.0);
        tilingMaskIslandSigma.setRange(0.0, 100.0);
        tilingMaskIslandSigma.setSuffix(QStringLiteral("%"));
        directionalEchoDirection.setRange(0, 359);
        directionalEchoDirection.setWrapping(true);
        directionalEchoSpread.setRange(0, 100);
        directionalEchoHue.setRange(0, 100);
        stepRepeatFillGuideScale.setRange(0, 100);
        stepRepeatFillLumaMatch.setRange(0, 100);
        stepRepeatFillHueMatch.setRange(0, 100);
        tilingPattern.addItem(QStringLiteral("Grid"), static_cast<int>(ClipTilingPattern::Grid));
        tilingPattern.addItem(QStringLiteral("Encircle"), static_cast<int>(ClipTilingPattern::Encircle));
    }

    EffectsTab::Widgets dependencies()
    {
        EffectsTab::Widgets widgets;
        widgets.effectsPathLabel = &path;
        widgets.edgeFillEffectCombo = &edgeFillEffect;
        widgets.edgeFillPixelsSpin = &edgeFillPixels;
        widgets.edgeFillPowerSpin = &edgeFillPower;
        widgets.edgeFillOpacitySpin = &edgeFillOpacity;
        widgets.edgeFillBrightnessSpin = &edgeFillBrightness;
        widgets.edgeFillSaturationSpin = &edgeFillSaturation;
        widgets.effectPresetCategoryCombo = &presetCategory;
        widgets.effectPresetCombo = &preset;
        widgets.effectPresetPreviousButton = &previousPreset;
        widgets.effectPresetNextButton = &nextPreset;
        widgets.effectEnabledCheck = &enabled;
        widgets.effectSpeechSyncCheck = &transcriptAware;
        widgets.effectKeyframeOnButton = &keyOn;
        widgets.effectKeyframeOffButton = &keyOff;
        widgets.effectParameterKeyframeButton = &keyParameters;
        widgets.effectKeyframeRemoveButton = &removeKey;
        widgets.effectKeyframesLabel = &keySummary;
        widgets.effectKeyframeTable = &keyTable;
        widgets.effectPresetSpecificHelpLabel = &presetSpecificHelp;
        widgets.effectRowsSpin = &rows;
        widgets.effectSpeedSpin = &speed;
        widgets.effectScaleSpin = &scale;
        widgets.effectAlternateDirectionCheck = &alternate;
        widgets.tilingPatternCombo = &tilingPattern;
        widgets.tilingSpacingSpin = &tilingSpacing;
        widgets.tilingWrapCheck = &tilingWrap;
        widgets.maskBoundingBoxSection = &maskBoundingBoxSection;
        widgets.tilingUseMaskBoundsCheck = &tilingMaskBounds;
        widgets.tilingMaskIslandSigmaSpin = &tilingMaskIslandSigma;
        widgets.maskBoundingBoxPreviewCheck = &maskBoundingBoxPreview;
        widgets.directionalEchoControlsWidget = &directionalEchoControls;
        widgets.directionalEchoDirectionDial = &directionalEchoDirection;
        widgets.directionalEchoDirectionValueLabel = &directionalEchoDirectionValue;
        widgets.directionalEchoSpreadDial = &directionalEchoSpread;
        widgets.directionalEchoSpreadValueLabel = &directionalEchoSpreadValue;
        widgets.directionalEchoHueDial = &directionalEchoHue;
        widgets.directionalEchoHueValueLabel = &directionalEchoHueValue;
        widgets.directionalEchoSummaryLabel = &directionalEchoSummary;
        widgets.stepRepeatFillControlsWidget = &stepRepeatFillControls;
        widgets.stepRepeatFillGuideScaleDial = &stepRepeatFillGuideScale;
        widgets.stepRepeatFillGuideScaleValueLabel = &stepRepeatFillGuideScaleValue;
        widgets.stepRepeatFillLumaMatchDial = &stepRepeatFillLumaMatch;
        widgets.stepRepeatFillLumaMatchValueLabel = &stepRepeatFillLumaMatchValue;
        widgets.stepRepeatFillHueMatchDial = &stepRepeatFillHueMatch;
        widgets.stepRepeatFillHueMatchValueLabel = &stepRepeatFillHueMatchValue;
        widgets.stepRepeatFillSummaryLabel = &stepRepeatFillSummary;
        widgets.effectModulationModeCombo = &modulationMode;
        widgets.effectModulationTargetCombo = &modulationTarget;
        widgets.effectModulationAmountSpin = &modulationAmount;
        widgets.effectModulationRateSpin = &modulationRate;
        widgets.effectModulationPhaseSpin = &modulationPhase;
        return widgets;
    }
};

TimelineClip makeClip()
{
    TimelineClip clip;
    clip.id = QStringLiteral("clip");
    clip.clipRole = ClipRole::Media;
    clip.mediaType = ClipMediaType::Video;
    clip.startFrame = 100;
    clip.durationFrames = 100;
    clip.effectPreset = ClipEffectPreset::NeonGlow;
    return clip;
}

} // namespace

class TestEffectsTab final : public QObject
{
    Q_OBJECT

private slots:
    void effectEnableButtonsEditTheSelectedClipAtThePlayhead();
    void effectParameterButtonEditsTheSelectedClipAtThePlayhead();
    void dynamicControlsPersistIndependentPerClipParameters();
    void directionalFrameEchoUsesCustomKnobsForKeyframes();
    void stepRepeatFillUsesCustomKnobsForKeyframes();
    void sourceMosaicVariantUsesCustomKnobsForKeyframes();
    void maskDomainEffectsExposeBoundingBoxOutsidePixelPercentage();
    void edgeFillRowsStayHiddenUntilBackgroundFillUsesThem();
    void effectPresetSelectorUsesCategoriesAndLinearNavigation();
    void mirrorGeometryControlsExposeSpecificParameters();
    void presetSpecificGuidanceTracksSelectedEffect();
    void effectKeyframeTableShowsAndRemovesSelectedRows();
    void effectKeyframeTableShiftSelectDeletesRanges();
    void effectKeyframeTableSelectionDoesNotSeekOrRebuild();
    void effectKeyframeTableEditsCellsAndDeletesWithKeyboard();
};

void TestEffectsTab::effectEnableButtonsEditTheSelectedClipAtThePlayhead()
{
    TimelineClip clip = makeClip();
    int64_t playhead = 125;
    int historyPushes = 0;
    EffectAnimationWidgets controls;

    EffectsTab::Dependencies deps;
    deps.getSelectedClip = [&clip]() { return &clip; };
    deps.updateClipById =
        [&clip](const QString& id,
                const std::function<void(TimelineClip&)>& update) {
            if (id != clip.id) return false;
            update(clip);
            return true;
        };
    deps.currentTimelineFrame = [&playhead]() { return playhead; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.getClipFilePath = [](const TimelineClip&) { return QString(); };
    deps.pushHistorySnapshot = [&historyPushes]() { ++historyPushes; };

    EffectsTab tab(controls.dependencies(), deps);
    tab.wire();
    for (QWidget* widget : {
             static_cast<QWidget*>(&controls.edgeFillPixels),
             static_cast<QWidget*>(&controls.edgeFillPower),
             static_cast<QWidget*>(&controls.edgeFillOpacity),
             static_cast<QWidget*>(&controls.edgeFillBrightness),
             static_cast<QWidget*>(&controls.edgeFillSaturation)}) {
        widget->show();
    }
    tab.refresh();

    QTest::mouseClick(&controls.keyOff, Qt::LeftButton);
    QCOMPARE(clip.effectEnabledKeyframes.size(), 1);
    QCOMPARE(clip.effectEnabledKeyframes.front().frame, int64_t{25});
    QVERIFY(!clip.effectEnabledKeyframes.front().enabled);
    QVERIFY(controls.keySummary.text().contains(QStringLiteral("25:Off")));
    QVERIFY(controls.removeKey.isEnabled());

    QTest::mouseClick(&controls.keyOn, Qt::LeftButton);
    QCOMPARE(clip.effectEnabledKeyframes.size(), 1);
    QVERIFY(clip.effectEnabledKeyframes.front().enabled);
    QVERIFY(controls.keySummary.text().contains(QStringLiteral("25:On")));

    QTest::mouseClick(&controls.removeKey, Qt::LeftButton);
    QVERIFY(clip.effectEnabledKeyframes.isEmpty());
    QVERIFY(!controls.removeKey.isEnabled());
    QCOMPARE(historyPushes, 3);
}

void TestEffectsTab::effectParameterButtonEditsTheSelectedClipAtThePlayhead()
{
    TimelineClip clip = makeClip();
    clip.effectPreset = ClipEffectPreset::SourceTile;
    int64_t playhead = 140;
    int historyPushes = 0;
    EffectAnimationWidgets controls;

    EffectsTab::Dependencies deps;
    deps.getSelectedClip = [&clip]() { return &clip; };
    deps.updateClipById =
        [&clip](const QString& id,
                const std::function<void(TimelineClip&)>& update) {
            if (id != clip.id) return false;
            update(clip);
            return true;
        };
    deps.currentTimelineFrame = [&playhead]() { return playhead; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.getClipFilePath = [](const TimelineClip&) { return QString(); };
    deps.pushHistorySnapshot = [&historyPushes]() { ++historyPushes; };

    EffectsTab tab(controls.dependencies(), deps);
    tab.wire();
    tab.refresh();

    controls.rows.setValue(12);
    controls.speed.setValue(2.5);
    controls.scale.setValue(1.75);
    controls.alternate.setChecked(false);
    controls.tilingPattern.setCurrentIndex(
        controls.tilingPattern.findData(static_cast<int>(ClipTilingPattern::Encircle)));
    controls.tilingSpacing.setValue(2.25);
    controls.tilingWrap.setChecked(false);
    controls.tilingMaskBounds.setChecked(true);
    controls.tilingMaskIslandSigma.setValue(2.5);

    QTest::mouseClick(&controls.keyParameters, Qt::LeftButton);
    QCOMPARE(clip.effectParameterKeyframes.size(), 1);
    const TimelineClip::EffectParameterKeyframe keyframe =
        clip.effectParameterKeyframes.constFirst();
    QCOMPARE(keyframe.frame, int64_t{40});
    QCOMPARE(keyframe.effectRows, 12);
    QCOMPARE(keyframe.effectSpeed, 2.5);
    QCOMPARE(keyframe.effectScale, 1.75);
    QVERIFY(!keyframe.effectAlternateDirection);
    QCOMPARE(keyframe.tilingPattern, ClipTilingPattern::Encircle);
    QCOMPARE(keyframe.tilingSpacing, 2.25);
    QVERIFY(!keyframe.tilingWrap);
    QVERIFY(keyframe.tilingUseMaskBounds);
    QCOMPARE(keyframe.tilingMaskIslandSigma, 2.5);
    QVERIFY(controls.keySummary.text().contains(QStringLiteral("40:Params")));
    QVERIFY(controls.removeKey.isEnabled());

    QTest::mouseClick(&controls.removeKey, Qt::LeftButton);
    QVERIFY(clip.effectParameterKeyframes.isEmpty());
    QVERIFY(!controls.removeKey.isEnabled());
    QCOMPARE(historyPushes, 2);
}

void TestEffectsTab::dynamicControlsPersistIndependentPerClipParameters()
{
    TimelineClip clip = makeClip();
    clip.effectPreset = ClipEffectPreset::SourceTile;
    EffectAnimationWidgets controls;

    EffectsTab::Dependencies deps;
    deps.getSelectedClip = [&clip]() { return &clip; };
    deps.updateClipById =
        [&clip](const QString& id,
                const std::function<void(TimelineClip&)>& update) {
            if (id != clip.id) return false;
            update(clip);
            return true;
        };
    deps.currentTimelineFrame = []() { return int64_t{100}; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.getClipFilePath = [](const TimelineClip&) { return QString(); };

    EffectsTab tab(controls.dependencies(), deps);
    tab.wire();
    tab.refresh();

    controls.modulationMode.setCurrentIndex(
        controls.modulationMode.findData(QStringLiteral("lfo")));
    controls.modulationTarget.setCurrentIndex(
        controls.modulationTarget.findData(QStringLiteral("speed")));
    controls.modulationAmount.setValue(2.5);
    controls.modulationRate.setValue(0.75);
    controls.modulationPhase.setValue(90.0);

    QCOMPARE(clip.effectParameterKeyframes.size(), 1);
    QCOMPARE(clip.effectParameterKeyframes.constFirst().frame, int64_t{0});
    QCOMPARE(clip.effectParameterKeyframes.constFirst().effectModulationMode,
             QStringLiteral("lfo"));
    QCOMPARE(clip.effectParameterKeyframes.constFirst().effectModulationTarget,
             QStringLiteral("speed"));
    QCOMPARE(clip.effectParameterKeyframes.constFirst().effectModulationAmount,
             2.5);
    QCOMPARE(clip.effectParameterKeyframes.constFirst().effectModulationRate,
             0.75);
    QCOMPARE(clip.effectParameterKeyframes.constFirst().effectModulationPhaseDegrees,
             90.0);
    QVERIFY(controls.modulationTarget.isEnabled());
    QVERIFY(controls.modulationRate.isEnabled());

    controls.modulationMode.setCurrentIndex(
        controls.modulationMode.findData(QStringLiteral("steady_increase")));
    QCOMPARE(clip.effectParameterKeyframes.size(), 1);
    QCOMPARE(clip.effectParameterKeyframes.constFirst().effectModulationMode,
             QStringLiteral("steady_increase"));
    QCOMPARE(controls.transcriptAware.text(),
             QStringLiteral("Transcript-aware steady increase"));
    QVERIFY(controls.transcriptAware.isEnabled());
    controls.transcriptAware.setChecked(false);
    QVERIFY(!clip.effectParameterKeyframes.constFirst().effectSkipAwareTiming);
    controls.transcriptAware.setChecked(true);
    QVERIFY(clip.effectParameterKeyframes.constFirst().effectSkipAwareTiming);
    QVERIFY(!controls.modulationRate.isEnabled());
    QVERIFY(!controls.modulationPhase.isEnabled());

    controls.tilingMaskBounds.setChecked(true);
    QCOMPARE(clip.effectParameterKeyframes.size(), 1);
    QVERIFY(clip.effectParameterKeyframes.constFirst().tilingUseMaskBounds);
    controls.tilingMaskIslandSigma.setValue(3.25);
    QCOMPARE(clip.effectParameterKeyframes.size(), 1);
    QCOMPARE(clip.effectParameterKeyframes.constFirst().tilingMaskIslandSigma,
	             3.25);
}

void TestEffectsTab::directionalFrameEchoUsesCustomKnobsForKeyframes()
{
    TimelineClip clip = makeClip();
    clip.effectPreset = ClipEffectPreset::DirectionalFrameEcho;
    clip.effectRows = 5;
    clip.effectSpeed = 0.0;
    clip.effectScale = 2.0;
    clip.tilingSpacing = 1.0;
    int64_t playhead = 145;
    EffectAnimationWidgets controls;

    EffectsTab::Dependencies deps;
    deps.getSelectedClip = [&clip]() { return &clip; };
    deps.updateClipById =
        [&clip](const QString& id,
                const std::function<void(TimelineClip&)>& update) {
            if (id != clip.id) return false;
            update(clip);
            return true;
        };
    deps.currentTimelineFrame = [&playhead]() { return playhead; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.getClipFilePath = [](const TimelineClip&) { return QString(); };

    EffectsTab tab(controls.dependencies(), deps);
    tab.wire();
    controls.directionalEchoControls.show();
    controls.speed.show();
    controls.scale.show();
    controls.tilingSpacing.show();
    tab.refresh();

    QVERIFY(controls.directionalEchoControls.isVisible());
    QVERIFY(!controls.speed.isVisible());
    QVERIFY(!controls.scale.isVisible());
    QVERIFY(!controls.tilingSpacing.isVisible());

    controls.rows.setValue(7);
    controls.directionalEchoDirection.setValue(90);
    controls.directionalEchoSpread.setValue(50);
    controls.directionalEchoHue.setValue(25);

    QCOMPARE(clip.effectParameterKeyframes.size(), 1);
    const TimelineClip::EffectParameterKeyframe keyframe =
        clip.effectParameterKeyframes.constFirst();
    QCOMPARE(keyframe.frame, int64_t{45});
    QCOMPARE(keyframe.effectPreset, ClipEffectPreset::DirectionalFrameEcho);
    QCOMPARE(keyframe.effectRows, 7);
    QCOMPARE(keyframe.effectSpeed, 2.0);
    QVERIFY(keyframe.tilingSpacing > 4.0);
    QVERIFY(keyframe.tilingSpacing < 4.1);
    QCOMPARE(keyframe.effectScale, 2.0);
    QVERIFY(controls.directionalEchoDirectionValue.text().contains(QStringLiteral("90")));
    QVERIFY(controls.directionalEchoSpreadValue.text().contains(QStringLiteral("50")));
    QVERIFY(controls.directionalEchoHueValue.text().contains(QStringLiteral("25")));
    QVERIFY(controls.directionalEchoSummary.text().contains(QStringLiteral("7 instanced")));
    QVERIFY(controls.directionalEchoSummary.text().contains(QStringLiteral("90")));
    QVERIFY(controls.directionalEchoSummary.text().contains(QStringLiteral("Key Parameters")));
}

void TestEffectsTab::stepRepeatFillUsesCustomKnobsForKeyframes()
{
    TimelineClip clip = makeClip();
    clip.effectPreset = ClipEffectPreset::StepRepeatFill;
    clip.effectRows = 8;
    clip.effectSpeed = 6.0;
    clip.effectScale = 4.0;
    clip.tilingSpacing = 4.0;
    int64_t playhead = 150;
    EffectAnimationWidgets controls;

    EffectsTab::Dependencies deps;
    deps.getSelectedClip = [&clip]() { return &clip; };
    deps.updateClipById =
        [&clip](const QString& id,
                const std::function<void(TimelineClip&)>& update) {
            if (id != clip.id) return false;
            update(clip);
            return true;
        };
    deps.currentTimelineFrame = [&playhead]() { return playhead; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.getClipFilePath = [](const TimelineClip&) { return QString(); };

    EffectsTab tab(controls.dependencies(), deps);
    tab.wire();
    controls.stepRepeatFillControls.show();
    controls.speed.show();
    controls.scale.show();
    controls.tilingSpacing.show();
    tab.refresh();

    QVERIFY(controls.stepRepeatFillControls.isVisible());
    QVERIFY(!controls.speed.isVisible());
    QVERIFY(!controls.scale.isVisible());
    QVERIFY(!controls.tilingSpacing.isVisible());

    controls.rows.setValue(12);
    controls.stepRepeatFillGuideScale.setValue(60);
    controls.stepRepeatFillLumaMatch.setValue(80);
    controls.stepRepeatFillHueMatch.setValue(35);

    QCOMPARE(clip.effectParameterKeyframes.size(), 1);
    const TimelineClip::EffectParameterKeyframe keyframe =
        clip.effectParameterKeyframes.constFirst();
    QCOMPARE(keyframe.frame, int64_t{50});
    QCOMPARE(keyframe.effectPreset, ClipEffectPreset::StepRepeatFill);
    QCOMPARE(keyframe.effectRows, 12);
    QVERIFY(keyframe.tilingSpacing > 4.9);
    QVERIFY(keyframe.tilingSpacing < 5.1);
    QVERIFY(qAbs(keyframe.effectSpeed - 6.4) < 0.0001);
    QVERIFY(qAbs(keyframe.effectScale - 2.8) < 0.0001);
    QVERIFY(controls.stepRepeatFillGuideScaleValue.text().contains(QStringLiteral("60")));
    QVERIFY(controls.stepRepeatFillLumaMatchValue.text().contains(QStringLiteral("80")));
    QVERIFY(controls.stepRepeatFillHueMatchValue.text().contains(QStringLiteral("35")));
    QVERIFY(controls.stepRepeatFillSummary.text().contains(QStringLiteral("12 cells")));
    QVERIFY(controls.stepRepeatFillSummary.text().contains(QStringLiteral("Key Parameters")));
}

void TestEffectsTab::sourceMosaicVariantUsesCustomKnobsForKeyframes()
{
    TimelineClip clip = makeClip();
    clip.effectPreset = ClipEffectPreset::SourceMosaicFlow;
    clip.effectRows = 10;
    clip.effectSpeed = 4.0;
    clip.effectScale = 4.0;
    clip.tilingSpacing = 4.0;
    int64_t playhead = 160;
    EffectAnimationWidgets controls;

    EffectsTab::Dependencies deps;
    deps.getSelectedClip = [&clip]() { return &clip; };
    deps.updateClipById =
        [&clip](const QString& id,
                const std::function<void(TimelineClip&)>& update) {
            if (id != clip.id) return false;
            update(clip);
            return true;
        };
    deps.currentTimelineFrame = [&playhead]() { return playhead; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.getClipFilePath = [](const TimelineClip&) { return QString(); };

    EffectsTab tab(controls.dependencies(), deps);
    tab.wire();
    controls.stepRepeatFillControls.show();
    controls.speed.show();
    controls.scale.show();
    controls.tilingSpacing.show();
    tab.refresh();

    QVERIFY(controls.stepRepeatFillControls.isVisible());
    QVERIFY(!controls.speed.isVisible());
    QVERIFY(!controls.scale.isVisible());
    QVERIFY(!controls.tilingSpacing.isVisible());

    controls.rows.setValue(18);
    controls.stepRepeatFillGuideScale.setValue(40);
    controls.stepRepeatFillLumaMatch.setValue(65);
    controls.stepRepeatFillHueMatch.setValue(90);

    QCOMPARE(clip.effectParameterKeyframes.size(), 1);
    const TimelineClip::EffectParameterKeyframe keyframe =
        clip.effectParameterKeyframes.constFirst();
    QCOMPARE(keyframe.frame, int64_t{60});
    QCOMPARE(keyframe.effectPreset, ClipEffectPreset::SourceMosaicFlow);
    QCOMPARE(keyframe.effectRows, 18);
    QVERIFY(qAbs(keyframe.tilingSpacing - 3.5) < 0.0001);
    QVERIFY(qAbs(keyframe.effectSpeed - 5.2) < 0.0001);
    QVERIFY(qAbs(keyframe.effectScale - 7.2) < 0.0001);
    QVERIFY(controls.stepRepeatFillSummary.text().contains(QStringLiteral("18 cells")));
}

void TestEffectsTab::maskDomainEffectsExposeBoundingBoxOutsidePixelPercentage()
{
    TimelineClip clip = makeClip();
    clip.clipRole = ClipRole::MaskMatte;
    clip.effectPreset = ClipEffectPreset::RecursiveZoomTunnel;
    clip.effectRows = 10;
    clip.effectSpeed = 0.75;
    clip.effectScale = 0.30;
    clip.tilingSpacing = 1.0;
    int64_t playhead = 180;
    EffectAnimationWidgets controls;

    EffectsTab::Dependencies deps;
    deps.getSelectedClip = [&clip]() { return &clip; };
    deps.updateClipById =
        [&clip](const QString& id,
                const std::function<void(TimelineClip&)>& update) {
            if (id != clip.id) return false;
            update(clip);
            return true;
        };
    deps.currentTimelineFrame = [&playhead]() { return playhead; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.getClipFilePath = [](const TimelineClip&) { return QString(); };

    EffectsTab tab(controls.dependencies(), deps);
    tab.wire();
    controls.maskBoundingBoxSection.show();
    controls.tilingMaskBounds.show();
    controls.tilingMaskIslandSigma.show();
    controls.maskBoundingBoxPreview.show();
    tab.refresh();

    QVERIFY(controls.maskBoundingBoxSection.isVisible());
    QVERIFY(controls.tilingMaskBounds.isVisible());
    QVERIFY(controls.tilingMaskIslandSigma.isVisible());
    QVERIFY(controls.maskBoundingBoxPreview.isVisible());
    QCOMPARE(controls.tilingMaskIslandSigma.suffix(), QStringLiteral("%"));
    QVERIFY(controls.tilingMaskBounds.isEnabled());
    QVERIFY(!controls.tilingMaskIslandSigma.isEnabled());
    QVERIFY(controls.maskBoundingBoxPreview.isEnabled());

    controls.tilingMaskBounds.setChecked(true);
    controls.tilingMaskIslandSigma.setValue(4.5);
    controls.maskBoundingBoxPreview.setChecked(true);

    QCOMPARE(clip.effectParameterKeyframes.size(), 1);
    const TimelineClip::EffectParameterKeyframe keyframe =
        clip.effectParameterKeyframes.constFirst();
    QCOMPARE(keyframe.effectPreset, ClipEffectPreset::RecursiveZoomTunnel);
    QVERIFY(keyframe.tilingUseMaskBounds);
    QCOMPARE(keyframe.tilingMaskIslandSigma, 4.5);
    QVERIFY(clip.tilingUseMaskBounds);
    QCOMPARE(clip.tilingMaskIslandSigma, 4.5);
    QVERIFY(clip.maskBoundingBoxPreview);

    clip.effectPreset = ClipEffectPreset::SourceMosaicHex;
    clip.effectParameterKeyframes.clear();
    tab.refresh();
    QVERIFY(controls.maskBoundingBoxSection.isVisible());
    QVERIFY(controls.tilingMaskBounds.isVisible());
    QVERIFY(controls.tilingMaskIslandSigma.isVisible());
    QVERIFY(controls.maskBoundingBoxPreview.isVisible());

    clip.effectPreset = ClipEffectPreset::None;
    clip.effectParameterKeyframes.clear();
    tab.refresh();
    QVERIFY(controls.maskBoundingBoxSection.isVisible());
    QVERIFY(controls.tilingMaskBounds.isVisible());
    QVERIFY(controls.tilingMaskIslandSigma.isVisible());
    QVERIFY(controls.maskBoundingBoxPreview.isVisible());
}

void TestEffectsTab::edgeFillRowsStayHiddenUntilBackgroundFillUsesThem()
{
    TimelineClip clip = makeClip();
    EffectAnimationWidgets controls;

    EffectsTab::Dependencies deps;
    deps.getSelectedClip = [&clip]() { return &clip; };
    deps.updateClipById =
        [&clip](const QString& id,
                const std::function<void(TimelineClip&)>& update) {
            if (id != clip.id) return false;
            update(clip);
            return true;
        };
    deps.currentTimelineFrame = []() { return int64_t{100}; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.getClipFilePath = [](const TimelineClip&) { return QString(); };

    EffectsTab tab(controls.dependencies(), deps);
    tab.wire();
    tab.refresh();

    QVERIFY(controls.edgeFillPixels.isHidden());
    QVERIFY(controls.edgeFillPower.isHidden());
    QVERIFY(controls.edgeFillOpacity.isHidden());

    controls.edgeFillEffect.setCurrentIndex(
        controls.edgeFillEffect.findData(
            backgroundFillEffectToString(BackgroundFillEffect::ProgressiveEdgeStretch)));
    QVERIFY(!controls.edgeFillPixels.isHidden());
    QVERIFY(!controls.edgeFillPower.isHidden());
    QVERIFY(!controls.edgeFillOpacity.isHidden());

    controls.edgeFillEffect.setCurrentIndex(
        controls.edgeFillEffect.findData(
            backgroundFillEffectToString(BackgroundFillEffect::Tile)));
    QVERIFY(controls.edgeFillPixels.isHidden());
    QVERIFY(controls.edgeFillPower.isHidden());
    QVERIFY(!controls.edgeFillOpacity.isHidden());
}

void TestEffectsTab::effectPresetSelectorUsesCategoriesAndLinearNavigation()
{
    TimelineClip clip = makeClip();
    clip.effectPreset = ClipEffectPreset::NeonGlow;
    int64_t playhead = 100;
    EffectAnimationWidgets controls;

    EffectsTab::Dependencies deps;
    deps.getSelectedClip = [&clip]() { return &clip; };
    deps.updateClipById =
        [&clip](const QString& id,
                const std::function<void(TimelineClip&)>& update) {
            if (id != clip.id) return false;
            update(clip);
            return true;
        };
    deps.currentTimelineFrame = [&playhead]() { return playhead; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.getClipFilePath = [](const TimelineClip&) { return QString(); };

    EffectsTab tab(controls.dependencies(), deps);
    tab.wire();
    tab.refresh();

    QCOMPARE(controls.presetCategory.currentData().toString(),
             QStringLiteral("Glitch & Stylize"));
    QCOMPARE(static_cast<ClipEffectPreset>(controls.preset.currentData().toInt()),
             ClipEffectPreset::NeonGlow);

    QTest::mouseClick(&controls.nextPreset, Qt::LeftButton);
    QCOMPARE(clip.effectParameterKeyframes.size(), 1);
    QCOMPARE(clip.effectParameterKeyframes.constFirst().effectPreset,
             ClipEffectPreset::SpeakerMaskDilation);
    QCOMPARE(controls.presetCategory.currentData().toString(),
             QStringLiteral("Speaker Mask"));
    QCOMPARE(static_cast<ClipEffectPreset>(controls.preset.currentData().toInt()),
             ClipEffectPreset::SpeakerMaskDilation);

    controls.presetCategory.setCurrentIndex(
        controls.presetCategory.findData(QStringLiteral("Time & Repeat")));
    QCOMPARE(clip.effectParameterKeyframes.size(), 1);
    QCOMPARE(clip.effectParameterKeyframes.constFirst().effectPreset,
             ClipEffectPreset::TemporalEcho);
    QCOMPARE(controls.presetCategory.currentData().toString(),
             QStringLiteral("Time & Repeat"));
    QCOMPARE(static_cast<ClipEffectPreset>(controls.preset.currentData().toInt()),
             ClipEffectPreset::TemporalEcho);

    QTest::mouseClick(&controls.previousPreset, Qt::LeftButton);
    QCOMPARE(clip.effectParameterKeyframes.constFirst().effectPreset,
             ClipEffectPreset::GlassRefraction);
    QCOMPARE(controls.presetCategory.currentData().toString(),
             QStringLiteral("Warp & Distort"));
}

void TestEffectsTab::mirrorGeometryControlsExposeSpecificParameters()
{
    TimelineClip clip = makeClip();
    clip.effectPreset = ClipEffectPreset::MirrorRing;
    clip.effectRows = 18;
    clip.effectScale = 1.75;
    clip.effectSpeed = -0.5;
    clip.tilingSpacing = 2.25;

    QWidget root;
    QFormLayout form(&root);
    QLabel path;
    QComboBox preset;
    QSpinBox rows;
    QDoubleSpinBox speed;
    QDoubleSpinBox scale;
    QDoubleSpinBox geometry;
    QCheckBox alternate;
    QComboBox tilingPattern;
    QCheckBox tilingWrap;
    speed.setRange(-8.0, 8.0);
    scale.setRange(0.1, 8.0);
    geometry.setRange(0.1, 8.0);
    preset.addItem(QStringLiteral("Mirror Ring"),
                   static_cast<int>(ClipEffectPreset::MirrorRing));
    form.addRow(QStringLiteral("Preset"), &preset);
    form.addRow(QStringLiteral("Copies"), &rows);
    form.addRow(QStringLiteral("Speed"), &speed);
    form.addRow(QStringLiteral("Scale"), &scale);
    form.addRow(QStringLiteral("Alternate"), &alternate);
    form.addRow(QStringLiteral("Pattern"), &tilingPattern);
    form.addRow(QStringLiteral("Spacing"), &geometry);
    form.addRow(QStringLiteral("Wrap"), &tilingWrap);

    EffectsTab::Widgets widgets;
    widgets.effectsPathLabel = &path;
    widgets.effectPresetCombo = &preset;
    widgets.effectRowsSpin = &rows;
    widgets.effectSpeedSpin = &speed;
    widgets.effectScaleSpin = &scale;
    widgets.effectAlternateDirectionCheck = &alternate;
    widgets.tilingPatternCombo = &tilingPattern;
    widgets.tilingSpacingSpin = &geometry;
    widgets.tilingWrapCheck = &tilingWrap;

    EffectsTab::Dependencies deps;
    deps.getSelectedClip = [&clip]() { return &clip; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.getClipFilePath = [](const TimelineClip&) { return QString(); };

    EffectsTab tab(widgets, deps);
    tab.refresh();

    QCOMPARE(rows.value(), 18);
    QCOMPARE(scale.value(), 1.75);
    QCOMPARE(speed.value(), -0.5);
    QCOMPARE(geometry.value(), 2.25);
    QCOMPARE(qobject_cast<QLabel*>(form.labelForField(&rows))->text(),
             QStringLiteral("Mirror sectors"));
    QCOMPARE(qobject_cast<QLabel*>(form.labelForField(&scale))->text(),
             QStringLiteral("Source grain size"));
    QCOMPARE(qobject_cast<QLabel*>(form.labelForField(&speed))->text(),
             QStringLiteral("Rotation speed"));
    QCOMPARE(qobject_cast<QLabel*>(form.labelForField(&geometry))->text(),
             QStringLiteral("Geometry amount"));
    QVERIFY(!rows.isHidden());
    QVERIFY(!scale.isHidden());
    QVERIFY(!speed.isHidden());

    clip.effectPreset = ClipEffectPreset::HexagonalPrism;
    tab.refresh();
    QVERIFY(!rows.isHidden());
    QVERIFY(scale.isHidden());
    QCOMPARE(qobject_cast<QLabel*>(form.labelForField(&rows))->text(),
             QStringLiteral("Cells across"));
    QVERIFY(!geometry.isHidden());
    QVERIFY(tilingPattern.isHidden());
    QVERIFY(tilingWrap.isHidden());
    QVERIFY(alternate.isHidden());
}

void TestEffectsTab::presetSpecificGuidanceTracksSelectedEffect()
{
    TimelineClip clip = makeClip();
    EffectAnimationWidgets controls;

    EffectsTab::Dependencies deps;
    deps.getSelectedClip = [&clip]() { return &clip; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.getClipFilePath = [](const TimelineClip&) { return QString(); };

    EffectsTab tab(controls.dependencies(), deps);
    tab.refresh();
    QVERIFY(controls.presetSpecificHelp.text().contains(QStringLiteral("Glow controls")));

    clip.effectPreset = ClipEffectPreset::SourceTile;
    tab.refresh();
    QVERIFY(controls.presetSpecificHelp.text().contains(QStringLiteral("Pattern/repetition controls")));

    clip.effectPreset = ClipEffectPreset::TemporalEcho;
    tab.refresh();
    QVERIFY(controls.presetSpecificHelp.text().contains(QStringLiteral("Echo frames")));
}

void TestEffectsTab::effectKeyframeTableShowsAndRemovesSelectedRows()
{
    TimelineClip clip = makeClip();
    clip.effectEnabledKeyframes = {
        TimelineClip::BoolKeyframe{10, false},
        TimelineClip::BoolKeyframe{40, true},
    };
    TimelineClip::EffectParameterKeyframe params;
    params.frame = 25;
    params.effectRows = 12;
    params.effectSpeed = 2.5;
    params.effectScale = 1.75;
    params.tilingPattern = ClipTilingPattern::Encircle;
    clip.effectParameterKeyframes = {params};

    int64_t playhead = 100;
    int historyPushes = 0;
    EffectAnimationWidgets controls;

    EffectsTab::Dependencies deps;
    deps.getSelectedClip = [&clip]() { return &clip; };
    deps.updateClipById =
        [&clip](const QString& id,
                const std::function<void(TimelineClip&)>& update) {
            if (id != clip.id) return false;
            update(clip);
            return true;
        };
    deps.currentTimelineFrame = [&playhead]() { return playhead; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.getClipFilePath = [](const TimelineClip&) { return QString(); };
    deps.pushHistorySnapshot = [&historyPushes]() { ++historyPushes; };

    EffectsTab tab(controls.dependencies(), deps);
    tab.wire();
    tab.refresh();

    QCOMPARE(controls.keyTable.rowCount(), 3);
    QCOMPARE(controls.keyTable.item(0, 0)->text(), QStringLiteral("10"));
    QCOMPARE(controls.keyTable.item(0, 1)->text(), QStringLiteral("Enabled"));
    QCOMPARE(controls.keyTable.item(0, 3)->text(), QStringLiteral("Off"));
    QCOMPARE(controls.keyTable.item(1, 0)->text(), QStringLiteral("25"));
    QCOMPARE(controls.keyTable.item(1, 1)->text(), QStringLiteral("Parameters"));
    QCOMPARE(controls.keyTable.item(1, 4)->text(), QStringLiteral("12"));
    QCOMPARE(controls.keyTable.item(1, 7)->text(), QStringLiteral("Encircle"));
    QVERIFY(controls.removeKey.isEnabled());

    controls.keyTable.selectRow(1);
    QTest::mouseClick(&controls.removeKey, Qt::LeftButton);
    QCOMPARE(clip.effectEnabledKeyframes.size(), 2);
    QVERIFY(clip.effectParameterKeyframes.isEmpty());
    QCOMPARE(controls.keyTable.rowCount(), 2);
    QCOMPARE(historyPushes, 1);
}

void TestEffectsTab::effectKeyframeTableShiftSelectDeletesRanges()
{
    TimelineClip clip = makeClip();
    clip.effectEnabledKeyframes = {
        TimelineClip::BoolKeyframe{10, false},
        TimelineClip::BoolKeyframe{40, true},
    };
    TimelineClip::EffectParameterKeyframe firstParams;
    firstParams.frame = 20;
    firstParams.effectRows = 8;
    TimelineClip::EffectParameterKeyframe secondParams = firstParams;
    secondParams.frame = 30;
    secondParams.effectRows = 12;
    clip.effectParameterKeyframes = {firstParams, secondParams};

    int64_t playhead = 100;
    int historyPushes = 0;
    int seekCalls = 0;
    EffectAnimationWidgets controls;
    EffectsTab* tab = nullptr;

    EffectsTab::Dependencies deps;
    deps.getSelectedClip = [&clip]() { return &clip; };
    deps.updateClipById =
        [&clip](const QString& id,
                const std::function<void(TimelineClip&)>& update) {
            if (id != clip.id) return false;
            update(clip);
            return true;
        };
    deps.currentTimelineFrame = [&playhead]() { return playhead; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.getClipFilePath = [](const TimelineClip&) { return QString(); };
    deps.pushHistorySnapshot = [&historyPushes]() { ++historyPushes; };
    deps.seekToTimelineFrame = [&seekCalls, &tab](int64_t) {
        ++seekCalls;
        if (tab) {
            tab->refresh();
        }
    };

    EffectsTab effectsTab(controls.dependencies(), deps);
    tab = &effectsTab;
    effectsTab.wire();
    effectsTab.refresh();

    QCOMPARE(controls.keyTable.selectionBehavior(),
             QAbstractItemView::SelectRows);
    QCOMPARE(controls.keyTable.selectionMode(),
             QAbstractItemView::ExtendedSelection);
    QCOMPARE(controls.keyTable.rowCount(), 4);

    QTest::mouseClick(
        controls.keyTable.viewport(),
        Qt::LeftButton,
        Qt::NoModifier,
        controls.keyTable.visualItemRect(controls.keyTable.item(1, 0)).center());
    QCOMPARE(seekCalls, 0);
    QTest::mouseClick(
        controls.keyTable.viewport(),
        Qt::LeftButton,
        Qt::ShiftModifier,
        controls.keyTable.visualItemRect(controls.keyTable.item(3, 0)).center());
    QCOMPARE(seekCalls, 0);
    QTest::keyClick(&controls.keyTable, Qt::Key_Delete);

    QCOMPARE(clip.effectEnabledKeyframes.size(), 1);
    QCOMPARE(clip.effectEnabledKeyframes.constFirst().frame, int64_t{10});
    QVERIFY(clip.effectParameterKeyframes.isEmpty());
    QCOMPARE(controls.keyTable.rowCount(), 1);
    QCOMPARE(historyPushes, 1);
}

void TestEffectsTab::effectKeyframeTableSelectionDoesNotSeekOrRebuild()
{
    TimelineClip clip = makeClip();
    clip.startFrame = 240;
    clip.durationFrames = 90;
    clip.effectEnabledKeyframes = {
        TimelineClip::BoolKeyframe{12, false},
    };
    TimelineClip::EffectParameterKeyframe params;
    params.frame = 35;
    params.effectRows = 12;
    params.effectSpeed = 2.5;
    params.effectScale = 1.75;
    clip.effectParameterKeyframes = {params};

    int64_t playhead = 240;
    int seekCalls = 0;
    int refreshesFromSeek = 0;
    EffectAnimationWidgets controls;
    EffectsTab* tab = nullptr;

    EffectsTab::Dependencies deps;
    deps.getSelectedClip = [&clip]() { return &clip; };
    deps.currentTimelineFrame = [&playhead]() { return playhead; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.getClipFilePath = [](const TimelineClip&) { return QString(); };
    deps.seekToTimelineFrame = [&seekCalls, &refreshesFromSeek, &tab](int64_t) {
        ++seekCalls;
        ++refreshesFromSeek;
        if (tab) {
            tab->refresh();
        }
    };

    EffectsTab effectsTab(controls.dependencies(), deps);
    tab = &effectsTab;
    effectsTab.wire();
    effectsTab.refresh();

    QCOMPARE(controls.keyTable.rowCount(), 2);
    QCOMPARE(controls.keyTable.item(1, 0)->text(), QStringLiteral("35"));
    QTest::mouseClick(
        controls.keyTable.viewport(),
        Qt::LeftButton,
        Qt::NoModifier,
        controls.keyTable.visualItemRect(controls.keyTable.item(1, 0)).center());
    QCOMPARE(seekCalls, 0);
    QCOMPARE(refreshesFromSeek, 0);
    QCOMPARE(controls.keyTable.rowCount(), 2);
    QVERIFY(controls.keyTable.selectionModel()->isRowSelected(1, QModelIndex()));
}

void TestEffectsTab::effectKeyframeTableEditsCellsAndDeletesWithKeyboard()
{
    TimelineClip clip = makeClip();
    TimelineClip::EffectParameterKeyframe params;
    params.frame = 25;
    params.effectPreset = ClipEffectPreset::NeonGlow;
    params.effectPresetKeyframed = true;
    params.effectRows = 12;
    params.effectSpeed = 2.5;
    params.effectScale = 1.75;
    clip.effectParameterKeyframes = {params};

    int64_t playhead = 100;
    int historyPushes = 0;
    EffectAnimationWidgets controls;

    EffectsTab::Dependencies deps;
    deps.getSelectedClip = [&clip]() { return &clip; };
    deps.updateClipById =
        [&clip](const QString& id,
                const std::function<void(TimelineClip&)>& update) {
            if (id != clip.id) return false;
            update(clip);
            return true;
        };
    deps.currentTimelineFrame = [&playhead]() { return playhead; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.getClipFilePath = [](const TimelineClip&) { return QString(); };
    deps.pushHistorySnapshot = [&historyPushes]() { ++historyPushes; };

    EffectsTab tab(controls.dependencies(), deps);
    tab.wire();
    tab.refresh();

    QVERIFY(controls.keyTable.item(0, 0)->flags() & Qt::ItemIsEditable);
    QVERIFY(controls.keyTable.item(0, 2)->flags() & Qt::ItemIsEditable);
    QVERIFY(controls.keyTable.item(0, 4)->flags() & Qt::ItemIsEditable);
    QVERIFY(!(controls.keyTable.item(0, 1)->flags() & Qt::ItemIsEditable));

    controls.keyTable.item(0, 0)->setText(QStringLiteral("30"));
    QCOMPARE(clip.effectParameterKeyframes.constFirst().frame, int64_t{30});

    controls.keyTable.item(0, 2)->setText(QStringLiteral("Source image tiling"));
    QCOMPARE(clip.effectParameterKeyframes.constFirst().effectPreset,
             ClipEffectPreset::SourceTile);

    controls.keyTable.item(0, 4)->setText(QStringLiteral("18"));
    QCOMPARE(clip.effectParameterKeyframes.constFirst().effectRows, 18);

    controls.keyTable.item(0, 5)->setText(QStringLiteral("-1.25"));
    QCOMPARE(clip.effectParameterKeyframes.constFirst().effectSpeed, -1.25);

    controls.keyTable.item(0, 7)->setText(QStringLiteral("Encircle"));
    QCOMPARE(clip.effectParameterKeyframes.constFirst().tilingPattern,
             ClipTilingPattern::Encircle);

    controls.keyTable.selectRow(0);
    QTest::keyClick(&controls.keyTable, Qt::Key_Delete);
    QVERIFY(clip.effectParameterKeyframes.isEmpty());
    QVERIFY(historyPushes >= 6);
}

QTEST_MAIN(TestEffectsTab)
#include "test_effects_tab.moc"
