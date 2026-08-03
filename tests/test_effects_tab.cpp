#include <QtTest/QtTest>

#include "../effects_tab.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>

namespace {

struct EffectAnimationWidgets {
    QLabel path;
    QComboBox preset;
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
    QComboBox modulationMode;
    QComboBox modulationTarget;
    QDoubleSpinBox modulationAmount;
    QDoubleSpinBox modulationRate;
    QDoubleSpinBox modulationPhase;

    EffectAnimationWidgets()
    {
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
        tilingPattern.addItem(QStringLiteral("Grid"), static_cast<int>(ClipTilingPattern::Grid));
        tilingPattern.addItem(QStringLiteral("Encircle"), static_cast<int>(ClipTilingPattern::Encircle));
    }

    EffectsTab::Widgets dependencies()
    {
        EffectsTab::Widgets widgets;
        widgets.effectsPathLabel = &path;
        widgets.effectPresetCombo = &preset;
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
    void mirrorGeometryControlsExposeSpecificParameters();
    void presetSpecificGuidanceTracksSelectedEffect();
    void effectKeyframeTableShowsAndRemovesSelectedRows();
    void effectKeyframeTableClickSeeksToClipRelativeFrame();
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

void TestEffectsTab::effectKeyframeTableClickSeeksToClipRelativeFrame()
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
    int64_t soughtFrame = -1;
    EffectAnimationWidgets controls;

    EffectsTab::Dependencies deps;
    deps.getSelectedClip = [&clip]() { return &clip; };
    deps.currentTimelineFrame = [&playhead]() { return playhead; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.getClipFilePath = [](const TimelineClip&) { return QString(); };
    deps.seekToTimelineFrame = [&soughtFrame](int64_t frame) {
        soughtFrame = frame;
    };

    EffectsTab tab(controls.dependencies(), deps);
    tab.wire();
    tab.refresh();

    QCOMPARE(controls.keyTable.rowCount(), 2);
    QCOMPARE(controls.keyTable.item(1, 0)->text(), QStringLiteral("35"));
    QTest::mouseClick(
        controls.keyTable.viewport(),
        Qt::LeftButton,
        Qt::NoModifier,
        controls.keyTable.visualItemRect(controls.keyTable.item(1, 0)).center());
    QCOMPARE(soughtFrame, int64_t{275});
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
