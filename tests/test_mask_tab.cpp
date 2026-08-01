#include <QtTest/QtTest>

#include "../mask_tab.h"
#include "../mask_sidecar.h"
#include "../clip_serialization.h"
#include "mask_sidecar_test_utils.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>

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
    QPushButton refine;
    QSpinBox refineRadius;
    QSpinBox zLevel;
    QDoubleSpinBox feather;
    QDoubleSpinBox dilate;
    QCheckBox foreground;
    QCheckBox repeat;
    QDoubleSpinBox repeatX;
    QDoubleSpinBox repeatY;

    MaskWidgets()
    {
        repeatX.setRange(-1000.0, 1000.0);
        repeatY.setRange(-1000.0, 1000.0);
        refineRadius.setRange(0, 512);
        refineRadius.setValue(24);
    }

    MaskTab::Widgets dependencies()
    {
        MaskTab::Widgets widgets;
        widgets.clipLabel = &label;
        widgets.enabledCheck = &enabled;
        widgets.framesDirEdit = &directory;
        widgets.sidecarCombo = &sidecars;
        widgets.browseButton = &browse;
        widgets.newPromptButton = &prompt;
        widgets.biRefNetRefineButton = &refine;
        widgets.biRefNetGuideRadiusSpin = &refineRadius;
        widgets.zLevelSpin = &zLevel;
        widgets.featherSpin = &feather;
        widgets.dilateSpin = &dilate;
        widgets.foregroundLayerCheck = &foreground;
        widgets.repeatEnabledCheck = &repeat;
        widgets.repeatDeltaXSpin = &repeatX;
        widgets.repeatDeltaYSpin = &repeatY;
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
    void onlyExplicitZEditsFreezeAutomaticOrdering();
    void treatmentEditsApplyOnlyToSelectedMaskChild();
    void liveTreatmentEditsOnlyUpdateSelectedChildAndPreview();
    void samMaskCanLaunchGuidedBiRefNetRefinement();
    void fuzzyRemoveRecipeRoundTrips();
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

void TestMaskTab::onlyExplicitZEditsFreezeAutomaticOrdering()
{
    const TimelineClip source = makeSourceClip();
    TimelineClip selected = makeMaskChild(
        source, QStringLiteral("/missing/source_alpha_masks"));
    selected.generatedFromMaskId = editor::masks::stableMaskSidecarId(
        selected.maskFramesDir);
    selected.zLevel = 7;
    selected.zLevelUserSet = false;
    MaskWidgets controls;

    MaskTab::Dependencies deps;
    deps.getSelectedClip = [&selected]() { return &selected; };
    deps.updateClipById = [&selected](
                              const QString& id,
                              const std::function<void(TimelineClip&)>& update) {
        if (id != selected.id) return false;
        update(selected);
        return true;
    };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.isMaskInspectorActive = []() { return true; };

    MaskTab tab(controls.dependencies(), deps);
    tab.wire();
    tab.refresh();

    controls.feather.setValue(3.0);
    QTest::qWait(100);
    QCOMPARE(selected.maskFeather, 3.0);
    QCOMPARE(selected.zLevel, 7);
    QVERIFY(!selected.zLevelUserSet);

    controls.zLevel.setValue(12);
    QTest::qWait(100);
    QCOMPARE(selected.zLevel, 12);
    QVERIFY(selected.zLevelUserSet);
}

void TestMaskTab::treatmentEditsApplyOnlyToSelectedMaskChild()
{
    TimelineClip source = makeSourceClip();
    source.maskFeather = 91.0;
    source.maskForegroundLayerEnabled = false;
    source.maskRepeatEnabled = false;
    source.maskRepeatDeltaX = 7.0;
    source.maskRepeatDeltaY = 8.0;

    TimelineClip selected = makeMaskChild(
        source, QStringLiteral("/missing/source_alpha_masks"));
    selected.generatedFromMaskId = editor::masks::stableMaskSidecarId(
        selected.maskFramesDir);
    selected.maskFeather = 2.0;
    selected.maskRepeatDeltaX = 160.0;
    selected.maskRepeatDeltaY = 0.0;
    MaskWidgets controls;

    MaskTab::Dependencies deps;
    deps.getSelectedClip = [&selected]() { return &selected; };
    deps.updateClipById = [&selected](
                              const QString& id,
                              const std::function<void(TimelineClip&)>& update) {
        if (id != selected.id) return false;
        update(selected);
        return true;
    };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.isMaskInspectorActive = []() { return true; };

    MaskTab tab(controls.dependencies(), deps);
    tab.wire();
    tab.refresh();

    QCOMPARE(controls.feather.value(), 2.0);
    QCOMPARE(controls.repeatX.value(), 160.0);

    controls.feather.setValue(12.5);
    controls.foreground.setChecked(true);
    controls.repeat.setChecked(true);
    controls.repeatX.setValue(42.0);
    controls.repeatY.setValue(-12.0);
    QTest::qWait(100);

    QCOMPARE(selected.maskFeather, 12.5);
    QVERIFY(selected.maskForegroundLayerEnabled);
    QVERIFY(selected.maskRepeatEnabled);
    QCOMPARE(selected.maskRepeatDeltaX, 42.0);
    QCOMPARE(selected.maskRepeatDeltaY, -12.0);

    QCOMPARE(source.maskFeather, 91.0);
    QVERIFY(!source.maskForegroundLayerEnabled);
    QVERIFY(!source.maskRepeatEnabled);
    QCOMPARE(source.maskRepeatDeltaX, 7.0);
    QCOMPARE(source.maskRepeatDeltaY, 8.0);
}

void TestMaskTab::liveTreatmentEditsOnlyUpdateSelectedChildAndPreview()
{
    TimelineClip source = makeSourceClip();
    source.maskFeather = 91.0;
    source.maskDilate = 77.0;

    TimelineClip selected = makeMaskChild(
        source, QStringLiteral("/missing/source_alpha_masks"));
    selected.generatedFromMaskId = editor::masks::stableMaskSidecarId(
        selected.maskFramesDir);
    selected.maskFeather = 2.0;
    selected.maskDilate = 1.0;

    int updateCalls = 0;
    int previewCalls = 0;
    int refreshCalls = 0;
    int saveCalls = 0;
    int historyCalls = 0;
    int materializeCalls = 0;
    int selectCalls = 0;
    MaskWidgets controls;

    MaskTab::Dependencies deps;
    deps.getSelectedClip = [&selected]() { return &selected; };
    deps.updateClipById = [&selected, &updateCalls](
                              const QString& id,
                              const std::function<void(TimelineClip&)>& update) {
        if (id != selected.id) return false;
        ++updateCalls;
        update(selected);
        return true;
    };
    deps.setPreviewTimelineClips = [&previewCalls]() { ++previewCalls; };
    deps.refreshInspector = [&refreshCalls]() { ++refreshCalls; };
    deps.scheduleSaveState = [&saveCalls]() { ++saveCalls; };
    deps.pushHistorySnapshot = [&historyCalls]() { ++historyCalls; };
    deps.materializeMaskMatteForSidecar =
        [&materializeCalls](const QString&, const QString&) {
            ++materializeCalls;
            return QStringLiteral("unexpected-child");
        };
    deps.selectClipById = [&selectCalls](const QString&) { ++selectCalls; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.isMaskInspectorActive = []() { return true; };

    MaskTab tab(controls.dependencies(), deps);
    tab.wire();
    tab.refresh();

    controls.feather.setValue(3.0);
    controls.feather.setValue(4.0);
    controls.dilate.setValue(2.0);
    controls.dilate.setValue(3.0);

    QCOMPARE(updateCalls, 0);
    QCOMPARE(previewCalls, 0);
    QTest::qWait(100);

    QCOMPARE(selected.maskFeather, 4.0);
    QCOMPARE(selected.maskDilate, 3.0);
    QCOMPARE(source.maskFeather, 91.0);
    QCOMPARE(source.maskDilate, 77.0);
    QCOMPARE(updateCalls, 1);
    QCOMPARE(previewCalls, 1);
    QCOMPARE(refreshCalls, 0);
    QCOMPARE(saveCalls, 0);
    QCOMPARE(historyCalls, 0);
    QCOMPARE(materializeCalls, 0);
    QCOMPARE(selectCalls, 0);

    QMetaObject::invokeMethod(&controls.dilate, "editingFinished", Qt::DirectConnection);

    QCOMPARE(selected.maskDilate, 3.0);
    QCOMPARE(updateCalls, 2);
    QCOMPARE(previewCalls, 2);
    QCOMPARE(refreshCalls, 1);
    QCOMPARE(saveCalls, 1);
    QCOMPARE(historyCalls, 1);
    QCOMPARE(materializeCalls, 0);
    QCOMPARE(selectCalls, 0);
}

void TestMaskTab::samMaskCanLaunchGuidedBiRefNetRefinement()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString mediaPath = temporary.filePath(QStringLiteral("shot.mp4"));
    QFile media(mediaPath);
    QVERIFY(media.open(QIODevice::WriteOnly));
    media.write("video");
    media.close();

    const QString samDirectory = temporary.filePath(
        QStringLiteral("shot_sam3_person_binary_masks"));
    QVERIFY(QDir().mkpath(samDirectory));
    QImage frame(4, 4, QImage::Format_Grayscale8);
    frame.fill(255);
    QVERIFY(frame.save(
        QDir(samDirectory).filePath(QStringLiteral("frame_000001.png"))));
    QFile map(QDir(samDirectory).filePath(QStringLiteral("jcut_frame_map.tsv")));
    QVERIFY(map.open(QIODevice::WriteOnly | QIODevice::Text));
    map.write(
        "# source_frame\tsource_best_effort_timestamp\tmask_frame\n"
        "0\t0\t0\n");
    map.close();
    QVERIFY(mask_sidecar_test::writeSingleFrameMapMetadata(
        samDirectory, mediaPath));
    QVERIFY(mask_sidecar_test::writeSingleFrameCompletion(
        samDirectory, mediaPath, false));

    TimelineClip source = makeSourceClip();
    source.filePath = mediaPath;
    TimelineClip selected = makeMaskChild(source, samDirectory);
    MaskWidgets controls;
    controls.refineRadius.setValue(41);
    int refineCalls = 0;
    QString requestedSource;
    QString requestedGuidance;
    int requestedRadius = -1;

    MaskTab::Dependencies deps;
    deps.getSelectedClip = [&selected]() { return &selected; };
    deps.clipHasVisuals = [](const TimelineClip&) { return true; };
    deps.isMaskInspectorActive = []() { return true; };
    deps.refineMaskWithBiRefNet =
        [&](const QString& sourceId, const QString& guidance, int radius) {
            ++refineCalls;
            requestedSource = sourceId;
            requestedGuidance = guidance;
            requestedRadius = radius;
        };

    MaskTab tab(controls.dependencies(), deps);
    tab.wire();
    tab.refresh();

    QVERIFY(controls.refine.isEnabled());
    controls.refine.click();
    QCOMPARE(refineCalls, 1);
    QCOMPARE(requestedSource, source.id);
    QCOMPARE(QDir::cleanPath(requestedGuidance), QDir::cleanPath(samDirectory));
    QCOMPARE(requestedRadius, 41);
}

void TestMaskTab::fuzzyRemoveRecipeRoundTrips()
{
    TimelineClip clip = makeMaskChild(
        makeSourceClip(), QStringLiteral("/tmp/current-derived-mask"));
    clip.maskOriginalFramesDir = QStringLiteral("/tmp/original-mask");
    TimelineClip::MaskFuzzyRemoveEdit edit;
    edit.recipeHash = QStringLiteral("abc123");
    edit.sourceSidecarDirectory = QStringLiteral("/tmp/original-mask");
    edit.materializedSidecarDirectory = QStringLiteral("/tmp/current-derived-mask");
    edit.sourceFrame = 42;
    edit.sourcePresentationTimestamp = 84084;
    edit.seedMaskOrdinal = 40;
    edit.firstMaskOrdinal = 35;
    edit.lastMaskOrdinal = 49;
    edit.xNorm = 0.25;
    edit.yNorm = 0.75;
    edit.spatialReachPixels = 9;
    edit.temporalReachFrames = 80;
    edit.changedFrames = 15;
    edit.removedPixels = 12345;
    clip.maskFuzzyRemoveEdits.push_back(edit);

    const TimelineClip loaded = editor::clipFromJson(editor::clipToJson(clip));
    QCOMPARE(loaded.maskOriginalFramesDir, clip.maskOriginalFramesDir);
    QCOMPARE(loaded.maskFuzzyRemoveEdits.size(), 1);
    const TimelineClip::MaskFuzzyRemoveEdit& loadedEdit =
        loaded.maskFuzzyRemoveEdits.constFirst();
    QCOMPARE(loadedEdit.recipeHash, edit.recipeHash);
    QCOMPARE(loadedEdit.sourceSidecarDirectory, edit.sourceSidecarDirectory);
    QCOMPARE(loadedEdit.materializedSidecarDirectory,
             edit.materializedSidecarDirectory);
    QCOMPARE(loadedEdit.sourceFrame, edit.sourceFrame);
    QCOMPARE(loadedEdit.sourcePresentationTimestamp,
             edit.sourcePresentationTimestamp);
    QCOMPARE(loadedEdit.seedMaskOrdinal, edit.seedMaskOrdinal);
    QCOMPARE(loadedEdit.firstMaskOrdinal, edit.firstMaskOrdinal);
    QCOMPARE(loadedEdit.lastMaskOrdinal, edit.lastMaskOrdinal);
    QCOMPARE(loadedEdit.xNorm, edit.xNorm);
    QCOMPARE(loadedEdit.yNorm, edit.yNorm);
    QCOMPARE(loadedEdit.changedFrames, edit.changedFrames);
    QCOMPARE(loadedEdit.removedPixels, edit.removedPixels);
}

QTEST_MAIN(TestMaskTab)
#include "test_mask_tab.moc"
