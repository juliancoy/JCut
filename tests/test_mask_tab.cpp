#include <QtTest/QtTest>

#include "../mask_tab.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

namespace {

TimelineClip makeSourceClip()
{
    TimelineClip source;
    source.id = QStringLiteral("source");
    source.label = QStringLiteral("Source");
    source.filePath = QStringLiteral("/missing/source.mp4");
    source.clipRole = ClipRole::Media;
    source.mediaType = ClipMediaType::Video;
    source.durationFrames = 100;
    return source;
}

TimelineClip makeMaskChild(const TimelineClip& source, const QString& directory)
{
    TimelineClip child = source;
    child.id = QStringLiteral("source-mask-child");
    child.label = QStringLiteral("Source Mask");
    child.clipRole = ClipRole::MaskMatte;
    child.linkedSourceClipId = source.id;
    child.maskFramesDir = directory;
    child.generatedFromMaskId = QStringLiteral("persisted-sidecar-id");
    child.maskEnabled = true;
    child.locked = true;
    return child;
}

struct MaskWidgets {
    QLabel label;
    QCheckBox enabled;
    QLineEdit directory;
    QComboBox sidecars;
    QPushButton browse;
    QPushButton prompt;
    QSpinBox zLevel;
    QDoubleSpinBox feather;

    MaskTab::Widgets dependencies()
    {
        MaskTab::Widgets widgets;
        widgets.clipLabel = &label;
        widgets.enabledCheck = &enabled;
        widgets.framesDirEdit = &directory;
        widgets.sidecarCombo = &sidecars;
        widgets.browseButton = &browse;
        widgets.newPromptButton = &prompt;
        widgets.zLevelSpin = &zLevel;
        widgets.featherSpin = &feather;
        return widgets;
    }
};

} // namespace

class TestMaskTab final : public QObject
{
    Q_OBJECT

private slots:
    void inactiveRefreshNeverMaterializesOrSelects();
    void activeRefreshMaterializesAndSelectsChildReentrantly();
    void failedMaterializationLeavesOnlyDiscoveryControlsEnabled();
    void unavailableChildPreservesEnabledIntentAndReportsAvailability();
};

void TestMaskTab::inactiveRefreshNeverMaterializesOrSelects()
{
    TimelineClip selected = makeSourceClip();
    selected.maskFramesDir = QStringLiteral("/missing/source_alpha");
    int materializeCalls = 0;
    int selectCalls = 0;
    MaskWidgets controls;

    MaskTab::Dependencies deps;
    deps.getSelectedClip = [&selected]() { return &selected; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.materializeMaskMatteForSidecar =
        [&materializeCalls](const QString&, const QString&) {
            ++materializeCalls;
            return QStringLiteral("unexpected-child");
        };
    deps.selectClipById = [&selectCalls](const QString&) { ++selectCalls; };
    deps.isMaskInspectorActive = []() { return false; };

    MaskTab tab(controls.dependencies(), deps);
    tab.refresh();

    QCOMPARE(materializeCalls, 0);
    QCOMPARE(selectCalls, 0);
    QVERIFY(controls.browse.isEnabled());
    QVERIFY(controls.directory.isEnabled());
    QVERIFY(!controls.enabled.isEnabled());
    QVERIFY(!controls.zLevel.isEnabled());
    QVERIFY(!controls.feather.isEnabled());
}

void TestMaskTab::activeRefreshMaterializesAndSelectsChildReentrantly()
{
    const QString sidecarDirectory = QStringLiteral("/missing/source_alpha");
    const TimelineClip source = makeSourceClip();
    const TimelineClip child = makeMaskChild(source, sidecarDirectory);
    TimelineClip selected = source;
    selected.maskFramesDir = sidecarDirectory;
    int materializeCalls = 0;
    int selectCalls = 0;
    MaskWidgets controls;
    MaskTab* tabPointer = nullptr;

    MaskTab::Dependencies deps;
    deps.getSelectedClip = [&selected]() { return &selected; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.findMaskMatteChildForSidecar =
        [](const QString&, const QString&) { return QString(); };
    deps.materializeMaskMatteForSidecar =
        [&](const QString&, const QString&) {
            ++materializeCalls;
            selected = child;
            tabPointer->refresh();
            return child.id;
        };
    deps.selectClipById = [&](const QString& clipId) {
        ++selectCalls;
        QCOMPARE(clipId, child.id);
        selected = child;
    };
    deps.isMaskInspectorActive = []() { return true; };

    MaskTab tab(controls.dependencies(), deps);
    tabPointer = &tab;
    tab.refresh();

    QCOMPARE(materializeCalls, 1);
    QCOMPARE(selectCalls, 1);
    QCOMPARE(selected.id, child.id);
    QVERIFY(controls.enabled.isEnabled());
    QVERIFY(controls.zLevel.isEnabled());
    QVERIFY(controls.feather.isEnabled());
    QCOMPARE(controls.directory.text(), sidecarDirectory);
    QCOMPARE(controls.sidecars.currentData().toString(), sidecarDirectory);
}

void TestMaskTab::failedMaterializationLeavesOnlyDiscoveryControlsEnabled()
{
    TimelineClip selected = makeSourceClip();
    selected.maskFramesDir = QStringLiteral("/missing/source_alpha");
    int materializeCalls = 0;
    MaskWidgets controls;

    MaskTab::Dependencies deps;
    deps.getSelectedClip = [&selected]() { return &selected; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.materializeMaskMatteForSidecar =
        [&materializeCalls](const QString&, const QString&) {
            ++materializeCalls;
            return QString();
        };
    deps.isMaskInspectorActive = []() { return true; };

    MaskTab tab(controls.dependencies(), deps);
    tab.refresh();

    QCOMPARE(materializeCalls, 1);
    QVERIFY(controls.browse.isEnabled());
    QVERIFY(controls.prompt.isEnabled());
    QVERIFY(controls.directory.isEnabled());
    QVERIFY(controls.sidecars.isEnabled());
    QVERIFY(!controls.enabled.isEnabled());
    QVERIFY(!controls.zLevel.isEnabled());
    QVERIFY(!controls.feather.isEnabled());
}

void TestMaskTab::unavailableChildPreservesEnabledIntentAndReportsAvailability()
{
    const TimelineClip source = makeSourceClip();
    TimelineClip selected = makeMaskChild(
        source, QStringLiteral("/missing/source_birefnet_alpha_masks"));
    selected.maskSidecarAvailable = false;
    selected.maskSidecarAvailabilityIssue = QStringLiteral("Generation incomplete");
    int materializeCalls = 0;
    int selectCalls = 0;
    MaskWidgets controls;

    MaskTab::Dependencies deps;
    deps.getSelectedClip = [&selected]() { return &selected; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.materializeMaskMatteForSidecar =
        [&materializeCalls](const QString&, const QString&) {
            ++materializeCalls;
            return QString();
        };
    deps.selectClipById = [&selectCalls](const QString&) { ++selectCalls; };
    deps.isMaskInspectorActive = []() { return true; };

    MaskTab tab(controls.dependencies(), deps);
    tab.refresh();

    QCOMPARE(materializeCalls, 0);
    QCOMPARE(selectCalls, 0);
    QVERIFY(controls.enabled.isEnabled());
    QVERIFY(controls.enabled.isChecked());
    QVERIFY(controls.feather.isEnabled());
    QVERIFY(controls.sidecars.currentText().contains(
        QStringLiteral("Generation incomplete")));
    QCOMPARE(controls.sidecars.currentData(Qt::ToolTipRole).toString(),
             QStringLiteral("Generation incomplete"));
}

QTEST_MAIN(TestMaskTab)
#include "test_mask_tab.moc"
