#include <QtTest/QtTest>

#include "../effects_tab.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>

namespace {

struct EffectAnimationWidgets {
    QLabel path;
    QComboBox preset;
    QCheckBox enabled;
    QCheckBox transcriptAware;
    QPushButton keyOn{QStringLiteral("Key On")};
    QPushButton keyOff{QStringLiteral("Key Off")};
    QPushButton removeKey{QStringLiteral("Remove Key")};
    QLabel keySummary;
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
        widgets.effectKeyframeRemoveButton = &removeKey;
        widgets.effectKeyframesLabel = &keySummary;
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
    void dynamicControlsPersistIndependentPerClipParameters();
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

    QCOMPARE(clip.effectModulationMode, QStringLiteral("lfo"));
    QCOMPARE(clip.effectModulationTarget, QStringLiteral("speed"));
    QCOMPARE(clip.effectModulationAmount, 2.5);
    QCOMPARE(clip.effectModulationRate, 0.75);
    QCOMPARE(clip.effectModulationPhaseDegrees, 90.0);
    QVERIFY(controls.modulationTarget.isEnabled());
    QVERIFY(controls.modulationRate.isEnabled());

    controls.modulationMode.setCurrentIndex(
        controls.modulationMode.findData(QStringLiteral("steady_increase")));
    QCOMPARE(clip.effectModulationMode, QStringLiteral("steady_increase"));
    QCOMPARE(controls.transcriptAware.text(),
             QStringLiteral("Transcript-aware steady increase"));
    QVERIFY(controls.transcriptAware.isEnabled());
    controls.transcriptAware.setChecked(false);
    QVERIFY(!clip.effectSkipAwareTiming);
    controls.transcriptAware.setChecked(true);
    QVERIFY(clip.effectSkipAwareTiming);
    QVERIFY(!controls.modulationRate.isEnabled());
    QVERIFY(!controls.modulationPhase.isEnabled());
}

QTEST_MAIN(TestEffectsTab)
#include "test_effects_tab.moc"
