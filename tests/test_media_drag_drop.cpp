#include <QtTest/QtTest>

#include "../media_drag_payload.h"
#include "../mask_sidecar.h"
#include "../editor_shared_render_sync.h"
#include "../timeline_clip_title.h"
#include "../timeline_widget.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QImage>
#include <QMimeData>
#include <QPainter>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace {

const TimelineClip* clipById(const TimelineWidget& timeline, const QString& clipId)
{
    const auto found = std::find_if(
        timeline.clips().cbegin(), timeline.clips().cend(), [&](const TimelineClip& clip) {
            return clip.id == clipId;
        });
    return found == timeline.clips().cend() ? nullptr : &*found;
}

TimelineClip makeMaskTestSource()
{
    TimelineClip source;
    source.id = QStringLiteral("source");
    source.filePath = QStringLiteral("/missing/source.mp4");
    source.label = QStringLiteral("Source");
    source.mediaType = ClipMediaType::Video;
    source.sourceFps = 24.0;
    source.sourceDurationFrames = 1000;
    source.sourceInFrame = 25;
    source.startFrame = 10;
    source.durationFrames = 100;
    source.playbackRate = 1.0;
    source.trackIndex = 0;
    source.maskEnabled = true;
    source.maskFramesDir = QStringLiteral("/missing/source_masks");
    return source;
}

TimelineClip makeMaskTestChild(const TimelineClip& source)
{
    TimelineClip child = source;
    child.id = QStringLiteral("source-mask");
    child.label = QStringLiteral("Source Mask");
    child.clipRole = ClipRole::MaskMatte;
    child.linkedSourceClipId = source.id;
    child.generatedFromMaskId = QStringLiteral("stable-mask-id");
    child.syncLockedToSource = true;
    child.sourceTransformLocked = true;
    child.locked = true;
    child.hasAudio = false;
    child.audioEnabled = false;
    child.trackIndex = 1;
    return child;
}

} // namespace

class TestMediaDragDrop final : public QObject
{
    Q_OBJECT

private slots:
    void unavailablePathProducesNoPayload();
    void explorerPayloadInsertsClipOnTimeline();
    void loadingTimelineRemovesDuplicateGeneratedMaskTracks();
    void loadingTimelineRecoversParentStrandedOnDuplicateMaskTrack();
    void transcriptTitlesReconcileToImmutableGeneratedChildTrack();
    void clipClipboardPreservesRelativeLayoutAndUsesFreshIds();
    void cutClipboardRemovesAndRestoresSelection();
    void updatingParentTimingPropagatesToMaskChild();
    void materializingSidecarCreatesChildWithoutMutatingParent();
    void deletingParentCascadesToMaskChild();
    void cuttingExplicitParentChildSelectionRemovesAggregate();
    void splittingParentCreatesPairedMaskChildAndPartitionsOwnedState();
    void copyingParentIncludesAndRemapsMaskChild();
    void loadingTimelineRemovesOrphanedMaskChild();
    void maskChildRenderSyncMarkersCanonicalizeToParent();
    void generatedMaskTracksMoveOnlyWithTheirParentTrack();
    void clipTitlesDescribeRoleRelationshipAndEffectiveState();
    void clipTitlesRejectInvalidParentsAndUseCanonicalSourceFrames();
    void clipTooltipRefreshesAfterTrackMutations();
    void maskClipTitlePaintsAfterStatusDecorations();
    void sharedExportRangeCoreMatchesTimelineWidget();
};

void TestMediaDragDrop::unavailablePathProducesNoPayload()
{
    std::unique_ptr<QMimeData> payload(
        editor::createMediaDragMimeData(QStringLiteral("/missing/jcut/media.mp4")));
    QVERIFY(!payload);
}

void TestMediaDragDrop::explorerPayloadInsertsClipOnTimeline()
{
    QTemporaryFile mediaFile(QDir::tempPath() + QStringLiteral("/jcut-drag-XXXXXX.mp4"));
    QVERIFY(mediaFile.open());
    QVERIFY(mediaFile.write("not-a-real-video") > 0);
    mediaFile.flush();

    std::unique_ptr<QMimeData> payload(editor::createMediaDragMimeData(mediaFile.fileName()));
    QVERIFY(payload);
    QVERIFY(payload->hasUrls());
    QCOMPARE(payload->urls().size(), 1);
    QCOMPARE(payload->urls().constFirst().toLocalFile(), QFileInfo(mediaFile.fileName()).absoluteFilePath());

    TimelineWidget timeline;
    timeline.resize(900, 320);
    timeline.show();

    const QPoint dropPoint(300, 120);
    QDragEnterEvent enterEvent(dropPoint,
                               Qt::CopyAction,
                               payload.get(),
                               Qt::LeftButton,
                               Qt::NoModifier);
    QApplication::sendEvent(&timeline, &enterEvent);
    QVERIFY(enterEvent.isAccepted());

    QDropEvent dropEvent(QPointF(dropPoint),
                         Qt::CopyAction,
                         payload.get(),
                         Qt::LeftButton,
                         Qt::NoModifier);
    QApplication::sendEvent(&timeline, &dropEvent);

    QVERIFY(dropEvent.isAccepted());
    QCOMPARE(timeline.clips().size(), 1);
    QCOMPARE(timeline.clips().constFirst().filePath,
             QFileInfo(mediaFile.fileName()).absoluteFilePath());
    QCOMPARE(timeline.clips().constFirst().trackIndex, 0);
    QVERIFY(timeline.clips().constFirst().startFrame >= 0);
}

void TestMediaDragDrop::transcriptTitlesReconcileToImmutableGeneratedChildTrack()
{
    TimelineWidget timeline;
    TimelineTrack sourceTrack;
    sourceTrack.name = QStringLiteral("Transcript");
    TimelineTrack legacyTitleTrackA;
    legacyTitleTrackA.name = QStringLiteral("Speaker Titles");
    TimelineTrack legacyTitleTrackB;
    legacyTitleTrackB.name = QStringLiteral("Speaker Titles 2");
    timeline.setTracks(
        {sourceTrack, legacyTitleTrackA, legacyTitleTrackB});

    TimelineClip source;
    source.id = QStringLiteral("transcript-source");
    source.label = QStringLiteral("Interview");
    source.mediaType = ClipMediaType::Video;
    source.trackIndex = 0;
    source.durationFrames = 300;

    TimelineClip titleA;
    titleA.id = QStringLiteral("title-a");
    titleA.label = QStringLiteral("Speaker: Alice");
    titleA.clipRole = ClipRole::SpeakerTitle;
    titleA.linkedSourceClipId = source.id;
    titleA.mediaType = ClipMediaType::Title;
    titleA.trackIndex = 1;
    titleA.startFrame = 20;
    titleA.durationFrames = 90;

    TimelineClip titleB = titleA;
    titleB.id = QStringLiteral("title-b");
    titleB.label = QStringLiteral("Speaker: Bob");
    titleB.trackIndex = 2;
    titleB.startFrame = 50;

    timeline.setClips({source, titleA, titleB});
    QCOMPARE(timeline.tracks().size(), 2);
    QVERIFY(timeline.tracks().at(1).generatedChildTrack);
    QCOMPARE(timeline.tracks().at(1).parentClipId,
             source.id);
    QCOMPARE(timeline.tracks().at(1).name,
             QStringLiteral(
                 "↳ Transcript • Speaker Introductions"));

    const TimelineClip* storedTitleA =
        clipById(timeline, titleA.id);
    const TimelineClip* storedTitleB =
        clipById(timeline, titleB.id);
    QVERIFY(storedTitleA);
    QVERIFY(storedTitleB);
    QCOMPARE(storedTitleA->trackIndex, 1);
    QCOMPARE(storedTitleB->trackIndex, 1);
    QVERIFY(storedTitleA->locked);
    QVERIFY(storedTitleB->locked);

    QVERIFY(!timeline.updateClipById(
        titleA.id, [](TimelineClip& clip) {
            clip.label = QStringLiteral("Manual override");
        }));
    QVERIFY(!timeline.deleteClipById(titleA.id));
    QCOMPARE(clipById(timeline, titleA.id)->label,
             QStringLiteral("Speaker: Alice"));
}

void TestMediaDragDrop::loadingTimelineRemovesDuplicateGeneratedMaskTracks()
{
    TimelineClip source = makeMaskTestSource();
    source.trackIndex = 0;
    source.durationFrames = 100;

    TimelineClip matte = makeMaskTestChild(source);
    matte.trackIndex = 1;

    TimelineTrack sourceTrack;
    sourceTrack.name = QStringLiteral("Source");
    TimelineTrack ownedChildTrack;
    ownedChildTrack.name = QStringLiteral("Owned mask row");
    ownedChildTrack.generatedChildTrack = true;
    ownedChildTrack.parentClipId = source.id;
    ownedChildTrack.childClipId = matte.id;
    ownedChildTrack.visualMode = TrackVisualMode::Hidden;
    TimelineTrack duplicateChildTrack = ownedChildTrack;
    duplicateChildTrack.name = QStringLiteral("Duplicate mask row");
    duplicateChildTrack.visualMode = TrackVisualMode::Enabled;

    TimelineWidget timeline;
    timeline.setTracks({sourceTrack, ownedChildTrack, duplicateChildTrack});
    timeline.setClips({source, matte});

    QCOMPARE(timeline.tracks().size(), 2);
    QCOMPARE(timeline.clips().size(), 2);
    const auto loadedMatte = std::find_if(
        timeline.clips().cbegin(), timeline.clips().cend(), [](const TimelineClip& clip) {
            return clip.clipRole == ClipRole::MaskMatte;
        });
    QVERIFY(loadedMatte != timeline.clips().cend());
    QCOMPARE(loadedMatte->trackIndex, 1);
    QVERIFY(timeline.tracks().at(1).generatedChildTrack);
    QCOMPARE(timeline.tracks().at(1).childClipId, matte.id);
    QCOMPARE(timeline.tracks().at(1).visualMode, TrackVisualMode::Hidden);
}

void TestMediaDragDrop::loadingTimelineRecoversParentStrandedOnDuplicateMaskTrack()
{
    TimelineClip source = makeMaskTestSource();
    source.trackIndex = 1;
    TimelineClip matte = makeMaskTestChild(source);
    matte.trackIndex = 0;

    TimelineTrack ownedChildTrack;
    ownedChildTrack.name = QStringLiteral("Owned mask row");
    ownedChildTrack.generatedChildTrack = true;
    ownedChildTrack.parentClipId = source.id;
    ownedChildTrack.childClipId = matte.id;
    ownedChildTrack.visualMode = TrackVisualMode::Enabled;
    TimelineTrack occupiedDuplicate = ownedChildTrack;
    occupiedDuplicate.name = QStringLiteral("Duplicate mask row");
    occupiedDuplicate.visualMode = TrackVisualMode::Hidden;

    TimelineWidget timeline;
    timeline.setTracks({ownedChildTrack, occupiedDuplicate});
    timeline.setClips({source, matte});

    QCOMPARE(timeline.tracks().size(), 2);
    const TimelineClip* loadedSource = clipById(timeline, source.id);
    const TimelineClip* loadedMatte = clipById(timeline, matte.id);
    QVERIFY(loadedSource && loadedMatte);
    QVERIFY(loadedSource->trackIndex != loadedMatte->trackIndex);
    QVERIFY(!timeline.tracks().at(loadedSource->trackIndex).generatedChildTrack);
    // The duplicate row's non-structural visibility belonged to the stranded
    // parent. Repair must not reveal an intentionally hidden source plate.
    QCOMPARE(timeline.tracks().at(loadedSource->trackIndex).visualMode,
             TrackVisualMode::Hidden);
    QVERIFY(timeline.tracks().at(loadedMatte->trackIndex).generatedChildTrack);
    QCOMPARE(timeline.tracks().at(loadedMatte->trackIndex).childClipId, matte.id);
    QCOMPARE(timeline.tracks().at(loadedMatte->trackIndex).visualMode,
             TrackVisualMode::Enabled);
    QCOMPARE(std::count_if(timeline.tracks().cbegin(), timeline.tracks().cend(),
                           [](const TimelineTrack& track) {
                               return track.generatedChildTrack;
                           }),
             1);
}

void TestMediaDragDrop::clipClipboardPreservesRelativeLayoutAndUsesFreshIds()
{
    TimelineClip first;
    first.id = QStringLiteral("first");
    first.startFrame = 10;
    first.durationFrames = 20;
    first.trackIndex = 0;
    TimelineClip second = first;
    second.id = QStringLiteral("second");
    second.startFrame = 25;
    second.trackIndex = 1;

    TimelineWidget timeline;
    timeline.setClips({first, second});
    QVERIFY(timeline.selectAllClips());
    QVERIFY(timeline.copySelectedClips());
    timeline.setCurrentFrame(100);
    QVERIFY(timeline.pasteClipsAtCurrentFrame());

    QCOMPARE(timeline.clips().size(), 4);
    const QSet<QString> pastedIds = timeline.selectedClipIds();
    QCOMPARE(pastedIds.size(), 2);
    QVERIFY(!pastedIds.contains(QStringLiteral("first")));
    QVERIFY(!pastedIds.contains(QStringLiteral("second")));
    QVector<TimelineClip> pasted;
    for (const TimelineClip& clip : timeline.clips()) {
        if (pastedIds.contains(clip.id)) pasted.push_back(clip);
    }
    std::sort(pasted.begin(), pasted.end(), [](const TimelineClip& a, const TimelineClip& b) {
        return a.startFrame < b.startFrame;
    });
    QCOMPARE(pasted.size(), 2);
    QCOMPARE(pasted.at(0).startFrame, int64_t{100});
    QCOMPARE(pasted.at(1).startFrame, int64_t{115});
    QCOMPARE(pasted.at(1).trackIndex - pasted.at(0).trackIndex, 1);
}

void TestMediaDragDrop::cutClipboardRemovesAndRestoresSelection()
{
    TimelineClip clip;
    clip.id = QStringLiteral("cut-me");
    clip.startFrame = 12;
    clip.durationFrames = 30;
    clip.trackIndex = 0;

    TimelineWidget timeline;
    timeline.setClips({clip});
    timeline.setSelectedClipId(clip.id);
    QVERIFY(timeline.cutSelectedClips());
    QVERIFY(timeline.clips().isEmpty());
    timeline.setCurrentFrame(50);
    QVERIFY(timeline.pasteClipsAtCurrentFrame());
    QCOMPARE(timeline.clips().size(), 1);
    QCOMPARE(timeline.clips().constFirst().startFrame, int64_t{50});
    QVERIFY(timeline.clips().constFirst().id != clip.id);
    QCOMPARE(timeline.selectedClipIds().size(), 1);
}

void TestMediaDragDrop::updatingParentTimingPropagatesToMaskChild()
{
    const TimelineClip source = makeMaskTestSource();
    TimelineClip child = makeMaskTestChild(source);
    child.maskOpacity = 0.65;
    child.brightness = 0.2;

    TimelineWidget timeline;
    timeline.setClips({source, child});

    QVERIFY(timeline.updateClipById(source.id, [](TimelineClip& clip) {
        clip.startFrame = 80;
        clip.startSubframeSamples = 101;
        clip.sourceInFrame = 45;
        clip.sourceInSubframeSamples = 202;
        clip.durationFrames = 70;
        clip.durationSubframeSamples = 303;
        clip.playbackRate = 0.75;
    }));

    const TimelineClip* updatedSource = clipById(timeline, source.id);
    const TimelineClip* updatedChild = clipById(timeline, child.id);
    QVERIFY(updatedSource);
    QVERIFY(updatedChild);
    QCOMPARE(updatedChild->startFrame, updatedSource->startFrame);
    QCOMPARE(updatedChild->startSubframeSamples, updatedSource->startSubframeSamples);
    QCOMPARE(updatedChild->sourceInFrame, updatedSource->sourceInFrame);
    QCOMPARE(updatedChild->sourceInSubframeSamples, updatedSource->sourceInSubframeSamples);
    QCOMPARE(updatedChild->durationFrames, updatedSource->durationFrames);
    QCOMPARE(updatedChild->durationSubframeSamples, updatedSource->durationSubframeSamples);
    QCOMPARE(updatedChild->playbackRate, updatedSource->playbackRate);
    QCOMPARE(updatedChild->maskOpacity, 0.65);
    QCOMPARE(updatedChild->brightness, 0.2);
}

void TestMediaDragDrop::materializingSidecarCreatesChildWithoutMutatingParent()
{
    QTemporaryDir projectDir;
    QVERIFY(projectDir.isValid());

    TimelineClip source = makeMaskTestSource();
    source.filePath = projectDir.filePath(QStringLiteral("source.mp4"));
    source.maskEnabled = false;
    source.maskFramesDir.clear();
    source.generatedFromMaskId.clear();
    source.maskFeather = 9.0;

    TimelineWidget timeline;
    timeline.setClips({source});

    const QString sidecarDirectory = projectDir.filePath(QStringLiteral("source_alpha"));
    QVERIFY(QDir().mkpath(sidecarDirectory));
    QFile frame(QDir(sidecarDirectory).filePath(QStringLiteral("frame_000001.png")));
    QVERIFY(frame.open(QIODevice::WriteOnly));
    QVERIFY(frame.write("mask") > 0);
    frame.close();

    QVERIFY(timeline.createOrReplaceMaskMatteForSidecar(
        source.id, sidecarDirectory, true));
    QCOMPARE(timeline.clips().size(), 2);

    const TimelineClip* unchangedParent = clipById(timeline, source.id);
    QVERIFY(unchangedParent);
    QVERIFY(!unchangedParent->maskEnabled);
    QVERIFY(unchangedParent->maskFramesDir.isEmpty());
    QVERIFY(unchangedParent->generatedFromMaskId.isEmpty());
    QCOMPARE(unchangedParent->maskFeather, 9.0);

    const auto child = std::find_if(
        timeline.clips().cbegin(), timeline.clips().cend(), [](const TimelineClip& clip) {
            return clip.clipRole == ClipRole::MaskMatte;
        });
    QVERIFY(child != timeline.clips().cend());
    const QString childId = child->id;
    QCOMPARE(child->linkedSourceClipId, source.id);
    QCOMPARE(child->generatedFromMaskId,
             editor::masks::stableMaskSidecarId(sidecarDirectory));
    QCOMPARE(timeline.selectedClipId(), childId);

    QVERIFY(timeline.updateClipById(childId, [](TimelineClip& clip) {
        clip.maskOpacity = 0.42;
        clip.maskFeather = 3.5;
    }));
    int redundantChangeNotifications = 0;
    timeline.clipsChanged = [&redundantChangeNotifications]() {
        ++redundantChangeNotifications;
    };
    QVERIFY(timeline.createOrReplaceMaskMatteForSidecar(
        source.id, sidecarDirectory, true));
    QCOMPARE(redundantChangeNotifications, 0);
    QCOMPARE(std::count_if(timeline.clips().cbegin(), timeline.clips().cend(),
                           [](const TimelineClip& clip) {
                               return clip.clipRole == ClipRole::MaskMatte;
                           }),
             1);
    const TimelineClip* preservedChild = clipById(timeline, childId);
    QVERIFY(preservedChild);
    QCOMPARE(preservedChild->maskOpacity, 0.42);
    QCOMPARE(preservedChild->maskFeather, 3.5);

    QVERIFY(timeline.updateClipById(childId, [](TimelineClip& clip) {
        clip.generatedFromMaskId = QStringLiteral("legacy-stale-id");
    }));
    redundantChangeNotifications = 0;
    QVERIFY(QDir(sidecarDirectory).removeRecursively());
    timeline.setSelectedClipId(source.id);
    QVERIFY(timeline.createOrReplaceMaskMatteForSidecar(
        source.id, sidecarDirectory, true));
    QCOMPARE(timeline.selectedClipId(), childId);
    QCOMPARE(redundantChangeNotifications, 0);

    timeline.setSelectedClipId(source.id);
    QVERIFY(timeline.createOrReplaceMaskMatte(source.id, true));
    QCOMPARE(timeline.selectedClipId(), childId);
    QCOMPARE(redundantChangeNotifications, 0);

    const QString alternateDirectory =
        projectDir.filePath(QStringLiteral("source_beta_mask"));
    QVERIFY(QDir().mkpath(alternateDirectory));
    QFile alternateFrame(
        QDir(alternateDirectory).filePath(QStringLiteral("frame_000001.png")));
    QVERIFY(alternateFrame.open(QIODevice::WriteOnly));
    QVERIFY(alternateFrame.write("mask") > 0);
    alternateFrame.close();

    QVERIFY(timeline.updateClipById(source.id, [&](TimelineClip& clip) {
        clip.maskFramesDir = alternateDirectory;
    }));
    redundantChangeNotifications = 0;
    timeline.setSelectedClipId(source.id);
    QVERIFY(timeline.createOrReplaceMaskMatte(source.id, true));
    QVERIFY(timeline.selectedClipId() != childId);
    QCOMPARE(std::count_if(timeline.clips().cbegin(), timeline.clips().cend(),
                           [](const TimelineClip& clip) {
                               return clip.clipRole == ClipRole::MaskMatte;
                           }),
             2);
    QCOMPARE(redundantChangeNotifications, 1);

    QVERIFY(timeline.updateClipById(source.id, [&](TimelineClip& clip) {
        clip.maskFramesDir = sidecarDirectory;
    }));
    redundantChangeNotifications = 0;
    timeline.setSelectedClipId(source.id);
    QVERIFY(timeline.createOrReplaceMaskMatte(source.id, true));
    QCOMPARE(timeline.selectedClipId(), childId);
    QCOMPARE(redundantChangeNotifications, 0);
}

void TestMediaDragDrop::deletingParentCascadesToMaskChild()
{
    const TimelineClip source = makeMaskTestSource();
    const TimelineClip child = makeMaskTestChild(source);

    TimelineWidget timeline;
    timeline.setClips({source, child});
    QVERIFY(timeline.deleteClipById(source.id));

    QVERIFY(!clipById(timeline, source.id));
    QVERIFY(!clipById(timeline, child.id));
    QVERIFY(std::none_of(timeline.tracks().cbegin(), timeline.tracks().cend(),
                         [&](const TimelineTrack& track) {
                             return track.generatedChildTrack &&
                                    (track.parentClipId == source.id || track.childClipId == child.id);
                         }));
}

void TestMediaDragDrop::cuttingExplicitParentChildSelectionRemovesAggregate()
{
    const TimelineClip source = makeMaskTestSource();
    const TimelineClip child = makeMaskTestChild(source);

    TimelineWidget timeline;
    timeline.setClips({source, child});
    QVERIFY(timeline.selectAllClips());
    QVERIFY(timeline.cutSelectedClips());
    QVERIFY(timeline.clips().isEmpty());

    timeline.setCurrentFrame(200);
    QVERIFY(timeline.pasteClipsAtCurrentFrame());
    QCOMPARE(timeline.clips().size(), 2);
    const auto pastedParent = std::find_if(
        timeline.clips().cbegin(), timeline.clips().cend(), [](const TimelineClip& clip) {
            return clip.clipRole != ClipRole::MaskMatte;
        });
    const auto pastedChild = std::find_if(
        timeline.clips().cbegin(), timeline.clips().cend(), [](const TimelineClip& clip) {
            return clip.clipRole == ClipRole::MaskMatte;
        });
    QVERIFY(pastedParent != timeline.clips().cend());
    QVERIFY(pastedChild != timeline.clips().cend());
    QCOMPARE(pastedChild->linkedSourceClipId, pastedParent->id);
}

void TestMediaDragDrop::splittingParentCreatesPairedMaskChildAndPartitionsOwnedState()
{
    TimelineClip source = makeMaskTestSource();
    source.durationSubframeSamples = 713;
    TimelineClip::TransformKeyframe sourceTransformStart;
    sourceTransformStart.frame = 0;
    sourceTransformStart.translationX = 0.0;
    TimelineClip::TransformKeyframe sourceTransformEnd = sourceTransformStart;
    sourceTransformEnd.frame = 80;
    sourceTransformEnd.translationX = 80.0;
    source.transformKeyframes = {sourceTransformStart, sourceTransformEnd};

    TimelineClip child = makeMaskTestChild(source);
    child.zLevel = 7;
    child.zLevelUserSet = true;
    child.maskOpacity = 0.65;
    child.maskFeather = 4.0;
    child.maskInvert = true;

    TimelineClip::GradingKeyframe leftGrade;
    leftGrade.frame = 0;
    leftGrade.brightness = 0.15;
    TimelineClip::GradingKeyframe rightGrade = leftGrade;
    rightGrade.frame = 80;
    rightGrade.brightness = 0.8;
    child.gradingKeyframes = {leftGrade, rightGrade};

    TimelineClip::OpacityKeyframe leftOpacity;
    leftOpacity.frame = 0;
    leftOpacity.opacity = 0.25;
    TimelineClip::OpacityKeyframe rightOpacity = leftOpacity;
    rightOpacity.frame = 80;
    rightOpacity.opacity = 0.9;
    child.opacityKeyframes = {leftOpacity, rightOpacity};

    TimelineClip::CorrectionPolygon correction;
    correction.startFrame = 40;
    correction.endFrame = 70;
    child.correctionPolygons = {correction};

    TimelineWidget timeline;
    timeline.setClips({source, child});
    QVERIFY(timeline.splitClipAtFrame(source.id, 60));

    QVector<const TimelineClip*> parents;
    QVector<const TimelineClip*> children;
    for (const TimelineClip& clip : timeline.clips()) {
        (clip.clipRole == ClipRole::MaskMatte ? children : parents).push_back(&clip);
    }
    QCOMPARE(parents.size(), 2);
    QCOMPARE(children.size(), 2);

    const TimelineClip* leftParent = clipById(timeline, source.id);
    QVERIFY(leftParent);
    const auto rightParentIt = std::find_if(parents.cbegin(), parents.cend(), [&](const TimelineClip* clip) {
        return clip->id != source.id;
    });
    QVERIFY(rightParentIt != parents.cend());
    const TimelineClip* rightParent = *rightParentIt;
    QVERIFY(!rightParent->id.isEmpty());
    QVERIFY(rightParent->id != source.id);

    const auto childForParent = [&](const QString& parentId) -> const TimelineClip* {
        const auto found = std::find_if(children.cbegin(), children.cend(), [&](const TimelineClip* clip) {
            return clip->linkedSourceClipId == parentId;
        });
        return found == children.cend() ? nullptr : *found;
    };
    const TimelineClip* leftChild = childForParent(leftParent->id);
    const TimelineClip* rightChild = childForParent(rightParent->id);
    QVERIFY(leftChild);
    QVERIFY(rightChild);
    QVERIFY(leftChild->id != rightChild->id);

    QCOMPARE(leftParent->startFrame, int64_t{10});
    QCOMPARE(leftParent->durationFrames, int64_t{50});
    QCOMPARE(leftParent->durationSubframeSamples, int64_t{0});
    QCOMPARE(rightParent->startFrame, int64_t{60});
    QCOMPARE(rightParent->durationFrames, int64_t{50});
    QCOMPARE(rightParent->durationSubframeSamples, int64_t{713});
    // 50 timeline frames at 30 fps advance a 24 fps source by 40 frames.
    QCOMPARE(rightParent->sourceInFrame, int64_t{65});
    QCOMPARE(rightParent->sourceInSubframeSamples, int64_t{0});
    for (const auto pair : {std::pair{leftParent, leftChild}, std::pair{rightParent, rightChild}}) {
        QCOMPARE(pair.second->startFrame, pair.first->startFrame);
        QCOMPARE(pair.second->sourceInFrame, pair.first->sourceInFrame);
        QCOMPARE(pair.second->durationFrames, pair.first->durationFrames);
        QCOMPARE(pair.second->playbackRate, pair.first->playbackRate);
        QCOMPARE(pair.second->generatedFromMaskId, child.generatedFromMaskId);
        QCOMPARE(pair.second->maskOpacity, child.maskOpacity);
        QCOMPARE(pair.second->maskFeather, child.maskFeather);
        QCOMPARE(pair.second->maskInvert, child.maskInvert);
        QCOMPARE(pair.second->zLevel, child.zLevel);
        QVERIFY(pair.second->zLevelUserSet);
    }

    QCOMPARE(leftChild->gradingKeyframes.size(), 2);
    QCOMPARE(leftChild->gradingKeyframes.constFirst().frame, int64_t{0});
    QCOMPARE(leftChild->gradingKeyframes.constFirst().brightness, 0.15);
    QCOMPARE(leftChild->gradingKeyframes.constLast().frame, int64_t{49});
    QVERIFY(qAbs(leftChild->gradingKeyframes.constLast().brightness - 0.548125) < 0.000001);
    QCOMPARE(rightChild->gradingKeyframes.size(), 2);
    QCOMPARE(rightChild->gradingKeyframes.constFirst().frame, int64_t{0});
    QVERIFY(qAbs(rightChild->gradingKeyframes.constFirst().brightness - 0.55625) < 0.000001);
    QCOMPARE(rightChild->gradingKeyframes.constLast().frame, int64_t{30});
    QCOMPARE(rightChild->gradingKeyframes.constLast().brightness, 0.8);
    QCOMPARE(leftChild->opacityKeyframes.size(), 2);
    QCOMPARE(leftChild->opacityKeyframes.constFirst().frame, int64_t{0});
    QCOMPARE(leftChild->opacityKeyframes.constFirst().opacity, 0.25);
    QCOMPARE(leftChild->opacityKeyframes.constLast().frame, int64_t{49});
    QVERIFY(qAbs(leftChild->opacityKeyframes.constLast().opacity - 0.648125) < 0.000001);
    QCOMPARE(rightChild->opacityKeyframes.size(), 2);
    QCOMPARE(rightChild->opacityKeyframes.constFirst().frame, int64_t{0});
    QVERIFY(qAbs(rightChild->opacityKeyframes.constFirst().opacity - 0.65625) < 0.000001);
    QCOMPARE(rightChild->opacityKeyframes.constLast().frame, int64_t{30});
    QCOMPARE(rightChild->opacityKeyframes.constLast().opacity, 0.9);

    QCOMPARE(leftChild->correctionPolygons.size(), 1);
    QCOMPARE(leftChild->correctionPolygons.constFirst().startFrame, int64_t{40});
    QCOMPARE(leftChild->correctionPolygons.constFirst().endFrame, int64_t{49});
    QCOMPARE(rightChild->correctionPolygons.size(), 1);
    QCOMPARE(rightChild->correctionPolygons.constFirst().startFrame, int64_t{0});
    QCOMPARE(rightChild->correctionPolygons.constFirst().endFrame, int64_t{20});

    QCOMPARE(leftParent->transformKeyframes.size(), 2);
    QCOMPARE(leftParent->transformKeyframes.constLast().frame, int64_t{49});
    QVERIFY(qAbs(leftParent->transformKeyframes.constLast().translationX - 49.0) < 0.000001);
    QCOMPARE(rightParent->transformKeyframes.size(), 2);
    QCOMPARE(rightParent->transformKeyframes.constFirst().frame, int64_t{0});
    QVERIFY(qAbs(rightParent->transformKeyframes.constFirst().translationX - 50.0) < 0.000001);
    QCOMPARE(rightParent->transformKeyframes.constLast().frame, int64_t{30});
    QCOMPARE(rightParent->transformKeyframes.constLast().translationX, 80.0);
    QCOMPARE(leftChild->transformKeyframes.size(), leftParent->transformKeyframes.size());
    QCOMPARE(leftChild->transformKeyframes.constLast().frame,
             leftParent->transformKeyframes.constLast().frame);
    QCOMPARE(leftChild->transformKeyframes.constLast().translationX,
             leftParent->transformKeyframes.constLast().translationX);
    QCOMPARE(rightChild->transformKeyframes.size(), rightParent->transformKeyframes.size());
    QCOMPARE(rightChild->transformKeyframes.constFirst().frame,
             rightParent->transformKeyframes.constFirst().frame);
    QCOMPARE(rightChild->transformKeyframes.constFirst().translationX,
             rightParent->transformKeyframes.constFirst().translationX);
}

void TestMediaDragDrop::copyingParentIncludesAndRemapsMaskChild()
{
    const TimelineClip source = makeMaskTestSource();
    TimelineClip child = makeMaskTestChild(source);
    child.maskOpacity = 0.55;
    child.brightness = -0.2;

    TimelineWidget timeline;
    timeline.setClips({source, child});
    timeline.setSelectedClipId(source.id);
    QVERIFY(timeline.copySelectedClips());
    timeline.setCurrentFrame(200);
    QVERIFY(timeline.pasteClipsAtCurrentFrame());

    QCOMPARE(timeline.clips().size(), 4);
    QCOMPARE(timeline.selectedClipIds().size(), 2);
    const auto pastedParentIt = std::find_if(
        timeline.clips().cbegin(), timeline.clips().cend(), [&](const TimelineClip& clip) {
            return timeline.selectedClipIds().contains(clip.id) &&
                   clip.clipRole != ClipRole::MaskMatte;
        });
    const auto pastedChildIt = std::find_if(
        timeline.clips().cbegin(), timeline.clips().cend(), [&](const TimelineClip& clip) {
            return timeline.selectedClipIds().contains(clip.id) &&
                   clip.clipRole == ClipRole::MaskMatte;
        });
    QVERIFY(pastedParentIt != timeline.clips().cend());
    QVERIFY(pastedChildIt != timeline.clips().cend());
    QVERIFY(pastedParentIt->id != source.id);
    QVERIFY(pastedChildIt->id != child.id);
    QCOMPARE(pastedParentIt->startFrame, int64_t{200});
    QCOMPARE(pastedChildIt->startFrame, pastedParentIt->startFrame);
    QCOMPARE(pastedChildIt->linkedSourceClipId, pastedParentIt->id);
    QCOMPARE(pastedChildIt->generatedFromMaskId, child.generatedFromMaskId);
    QCOMPARE(pastedChildIt->maskOpacity, child.maskOpacity);
    QCOMPARE(pastedChildIt->brightness, child.brightness);
}

void TestMediaDragDrop::loadingTimelineRemovesOrphanedMaskChild()
{
    const TimelineClip source = makeMaskTestSource();
    TimelineClip orphan = makeMaskTestChild(source);
    orphan.id = QStringLiteral("orphan-mask");
    orphan.linkedSourceClipId = QStringLiteral("missing-parent");

    TimelineTrack orphanTrack;
    orphanTrack.generatedChildTrack = true;
    orphanTrack.parentClipId = orphan.linkedSourceClipId;
    orphanTrack.childClipId = orphan.id;

    TimelineWidget timeline;
    timeline.setTracks({orphanTrack});
    timeline.setClips({orphan});

    QVERIFY(timeline.clips().isEmpty());
    QVERIFY(std::none_of(timeline.tracks().cbegin(), timeline.tracks().cend(),
                         [](const TimelineTrack& track) { return track.generatedChildTrack; }));
}

void TestMediaDragDrop::maskChildRenderSyncMarkersCanonicalizeToParent()
{
    const TimelineClip source = makeMaskTestSource();
    const TimelineClip child = makeMaskTestChild(source);
    TimelineWidget timeline;
    timeline.setClips({source, child});
    timeline.setCurrentFrame(40);

    QVERIFY(timeline.setRenderSyncMarkerAtCurrentFrame(
        child.id, RenderSyncAction::SkipFrame, 2));
    QCOMPARE(timeline.renderSyncMarkers().size(), 1);
    QCOMPARE(timeline.renderSyncMarkers().constFirst().clipId, source.id);
    QVERIFY(timeline.renderSyncMarkerAtFrame(child.id, 40));

    RenderSyncMarker legacyChildMarker;
    legacyChildMarker.clipId = child.id;
    legacyChildMarker.frame = 55;
    legacyChildMarker.action = RenderSyncAction::DuplicateFrame;
    timeline.setRenderSyncMarkers({legacyChildMarker});
    QCOMPARE(timeline.renderSyncMarkers().size(), 1);
    QCOMPARE(timeline.renderSyncMarkers().constFirst().clipId, source.id);
    QCOMPARE(timeline.renderSyncMarkers().constFirst().frame, int64_t{55});
}

void TestMediaDragDrop::generatedMaskTracksMoveOnlyWithTheirParentTrack()
{
    TimelineClip firstSource = makeMaskTestSource();
    firstSource.id = QStringLiteral("first-source");
    firstSource.trackIndex = 0;
    TimelineClip firstChild = makeMaskTestChild(firstSource);
    firstChild.id = QStringLiteral("first-mask");
    firstChild.trackIndex = 1;

    TimelineClip secondSource = makeMaskTestSource();
    secondSource.id = QStringLiteral("second-source");
    secondSource.trackIndex = 2;
    TimelineClip secondChild = makeMaskTestChild(secondSource);
    secondChild.id = QStringLiteral("second-mask");
    secondChild.trackIndex = 3;

    TimelineWidget timeline;
    timeline.setClips({firstSource, firstChild, secondSource, secondChild});
    QCOMPARE(timeline.tracks().size(), 4);
    QVERIFY(timeline.tracks().at(1).generatedChildTrack);
    QVERIFY(timeline.tracks().at(3).generatedChildTrack);

    QVERIFY(!timeline.moveTrack(1, 2));
    QVERIFY(!timeline.moveTrackUp(1));
    QVERIFY(!timeline.moveTrackDown(1));
    QVERIFY(!timeline.renameTrack(1));
    QVERIFY(!timeline.deleteTrack(1));

    QVERIFY(timeline.moveTrackDown(0));
    const TimelineClip* movedFirstSource = clipById(timeline, firstSource.id);
    const TimelineClip* movedFirstChild = clipById(timeline, firstChild.id);
    const TimelineClip* movedSecondSource = clipById(timeline, secondSource.id);
    const TimelineClip* movedSecondChild = clipById(timeline, secondChild.id);
    QVERIFY(movedFirstSource && movedFirstChild && movedSecondSource && movedSecondChild);
    QCOMPARE(movedSecondSource->trackIndex, 0);
    QCOMPARE(movedSecondChild->trackIndex, 1);
    QCOMPARE(movedFirstSource->trackIndex, 2);
    QCOMPARE(movedFirstChild->trackIndex, 3);
    QCOMPARE(timeline.tracks().at(1).parentClipId, secondSource.id);
    QCOMPARE(timeline.tracks().at(3).parentClipId, firstSource.id);
}

void TestMediaDragDrop::clipTitlesDescribeRoleRelationshipAndEffectiveState()
{
    TimelineClip source = makeMaskTestSource();
    source.label = QStringLiteral("TechUnity2026.mp4");
    source.hasAudio = true;
    source.audioEnabled = false;
    source.locked = true;
    source.playbackRate = 0.75;

    TimelineClip alpha = makeMaskTestChild(source);
    alpha.id = QStringLiteral("source-alpha");
    alpha.label = QStringLiteral("TechUnity2026.mp4 · alpha Mask");
    alpha.trackIndex = 1;
    alpha.zLevel = -399;
    alpha.maskEnabled = true;
    alpha.playbackRate = 2.0; // Deliberately stale: the parent owns timing.

    TimelineClip microphone = alpha;
    microphone.id = QStringLiteral("source-microphone");
    microphone.label =
        QStringLiteral("TechUnity2026.mp4 · microphone and microphone stand Mask");
    microphone.trackIndex = 2;
    microphone.zLevel = -398;

    TimelineClip person = alpha;
    person.id = QStringLiteral("source-person");
    person.label = QStringLiteral("TechUnity2026.mp4 · person Mask");
    person.trackIndex = 3;
    person.zLevel = -397;

    alpha.maskSidecarAvailable = false;
    alpha.maskSidecarAvailabilityIssue = QStringLiteral("Generation incomplete");

    TimelineTrack sourceTrack;
    sourceTrack.visualMode = TrackVisualMode::Hidden;
    TimelineTrack alphaTrack;
    alphaTrack.generatedChildTrack = true;
    TimelineTrack microphoneTrack = alphaTrack;
    microphoneTrack.visualMode = TrackVisualMode::Hidden;
    TimelineTrack personTrack = alphaTrack;
    personTrack.visualMode = TrackVisualMode::ForceOpaque;

    const QVector<TimelineClip> clips{source, alpha, microphone, person};
    const QVector<TimelineTrack> tracks{
        sourceTrack, alphaTrack, microphoneTrack, personTrack};
    const TimelineClipTitleModel titleModel(clips, tracks);

    const TimelineClipTitlePresentation sourceTitle = titleModel.describe(clips.at(0));
    QCOMPARE(sourceTitle.badge, QStringLiteral("SOURCE"));
    QCOMPARE(sourceTitle.primary, QStringLiteral("TechUnity2026.mp4"));
    QVERIFY(sourceTitle.attributes.contains(QStringLiteral("Provider only")));
    QVERIFY(sourceTitle.attributes.contains(QStringLiteral("1/3 masks active")));
    QVERIFY(sourceTitle.attributes.contains(QStringLiteral("1 unavailable")));
    QVERIFY(sourceTitle.attributes.contains(QStringLiteral("Audio off")));
    QVERIFY(sourceTitle.attributes.contains(QStringLiteral("Locked")));
    QVERIFY(sourceTitle.attributes.contains(QStringLiteral("0.75×")));

    const TimelineClipTitlePresentation microphoneTitle =
        titleModel.describe(clips.at(2), true);
    QCOMPARE(microphoneTitle.badge, QStringLiteral("MASK"));
    QCOMPARE(microphoneTitle.primary,
             QStringLiteral("microphone and microphone stand"));
    QVERIFY(microphoneTitle.attributes.contains(QStringLiteral("Hidden")));
    QVERIFY(microphoneTitle.attributes.contains(QStringLiteral("Z -398")));
    QVERIFY(microphoneTitle.attributes.contains(
        QStringLiteral("↳ TechUnity2026.mp4")));
    QVERIFY(microphoneTitle.attributes.contains(QStringLiteral("Source-locked")));
    QVERIFY(microphoneTitle.attributes.contains(QStringLiteral("0.75×")));
    QVERIFY(!microphoneTitle.attributes.contains(QStringLiteral("2×")));
    QVERIFY(microphoneTitle.tooltipText.contains(
        QStringLiteral("Source parent: TechUnity2026.mp4")));

    const TimelineClipTitlePresentation alphaTitle =
        titleModel.describe(clips.at(1), true);
    QVERIFY(alphaTitle.attributes.contains(QStringLiteral("Unavailable")));
    QVERIFY(alphaTitle.attributes.contains(QStringLiteral("Generation incomplete")));
    QVERIFY(!alphaTitle.attributes.contains(QStringLiteral("Enabled")));
    QVERIFY(!alphaTitle.attributes.contains(QStringLiteral("Hidden")));
    QVERIFY(!alphaTitle.attributes.contains(QStringLiteral("Mask off")));
    QVERIFY(alphaTitle.tooltipText.contains(
        QStringLiteral("Mask availability: Generation incomplete")));

    const TimelineClipTitlePresentation personTitle = titleModel.describe(clips.at(3));
    QVERIFY(personTitle.attributes.contains(QStringLiteral("Enabled")));
    QVERIFY(personTitle.attributes.contains(QStringLiteral("Opaque")));
}

void TestMediaDragDrop::clipTitlesRejectInvalidParentsAndUseCanonicalSourceFrames()
{
    TimelineClip source = makeMaskTestSource();
    source.playbackRate = 0.75;
    TimelineClip child = makeMaskTestChild(source);
    child.startFrame = 500;
    child.sourceInFrame = 700;
    child.sourceFps = 60.0;
    child.playbackRate = 2.0;

    TimelineTrack sourceTrack;
    TimelineTrack childTrack;
    childTrack.generatedChildTrack = true;
    const QVector<TimelineClip> validClips{source, child};
    const QVector<TimelineTrack> validTracks{sourceTrack, childTrack};

    RenderSyncMarker marker;
    marker.clipId = source.id;
    marker.frame = 20;
    marker.action = RenderSyncAction::SkipFrame;
    marker.count = 2;
    const QVector<RenderSyncMarker> markers{marker};
    const qreal timelinePosition = 40.0;
    const std::optional<int64_t> presentedSourceFrame =
        timelineClipSourceFrameAtTimelinePosition(
            child, validClips, timelinePosition, markers);
    QVERIFY(presentedSourceFrame.has_value());
    const ClipFrameMapping expected = clipFrameMappingForClock(
        source, renderFrameClockForTimelinePosition(timelinePosition), markers);
    QCOMPARE(*presentedSourceFrame, expected.sourceFrame);
    const ClipFrameMapping staleChildMapping = clipFrameMappingForClock(
        child, renderFrameClockForTimelinePosition(timelinePosition), markers);
    QVERIFY(*presentedSourceFrame != staleChildMapping.sourceFrame);

    TimelineClip invalidParent = source;
    invalidParent.id = QStringLiteral("effect-parent");
    invalidParent.mediaType = ClipMediaType::Image;
    TimelineClip invalidChild = child;
    invalidChild.linkedSourceClipId = invalidParent.id;
    const QVector<TimelineClip> invalidClips{invalidParent, invalidChild};
    const TimelineClipTitleModel invalidModel(invalidClips, validTracks);
    const TimelineClipTitlePresentation invalidTitle =
        invalidModel.describe(invalidClips.at(1), true);
    QVERIFY(invalidTitle.attributes.contains(QStringLiteral("Missing source")));
    QVERIFY(invalidTitle.attributes.contains(QStringLiteral("Inactive")));
    QVERIFY(!invalidTitle.attributes.contains(QStringLiteral("Enabled")));
    QVERIFY(!invalidTitle.attributes.contains(QStringLiteral("Source-locked")));
    QVERIFY(invalidTitle.tooltipText.contains(
        QStringLiteral("Timeline: Unavailable without a valid source parent")));
    QVERIFY(!timelineClipSourceFrameAtTimelinePosition(
        invalidClips.at(1), invalidClips, timelinePosition, markers).has_value());
    const TimelineClipTitlePresentation invalidParentTitle =
        invalidModel.describe(invalidClips.at(0));
    QCOMPARE(invalidParentTitle.badge, QStringLiteral("IMAGE"));
    QVERIFY(std::none_of(invalidParentTitle.attributes.cbegin(),
                         invalidParentTitle.attributes.cend(),
                         [](const QString& attribute) {
                             return attribute.contains(QStringLiteral("masks active"));
                         }));

    TimelineClip ordinaryAudio;
    ordinaryAudio.id = QStringLiteral("ordinary-audio");
    ordinaryAudio.label = QStringLiteral("Interview");
    ordinaryAudio.mediaType = ClipMediaType::Audio;
    ordinaryAudio.hasAudio = true;
    ordinaryAudio.trackIndex = 0;
    TimelineClip soloAudio = ordinaryAudio;
    soloAudio.id = QStringLiteral("solo-audio");
    soloAudio.label = QStringLiteral("Music");
    soloAudio.trackIndex = 1;
    soloAudio.audioSolo = true;
    TimelineTrack hiddenVisualAudioTrack;
    hiddenVisualAudioTrack.visualMode = TrackVisualMode::Hidden;
    TimelineTrack soloTrack;
    const QVector<TimelineClip> audioClips{ordinaryAudio, soloAudio};
    const QVector<TimelineTrack> audioTracks{hiddenVisualAudioTrack, soloTrack};
    const TimelineClipTitleModel audioModel(audioClips, audioTracks);
    const TimelineClipTitlePresentation ordinaryAudioTitle =
        audioModel.describe(audioClips.at(0));
    QVERIFY(ordinaryAudioTitle.attributes.contains(QStringLiteral("Muted by solo")));
    QVERIFY(!ordinaryAudioTitle.attributes.contains(QStringLiteral("Hidden")));
    QVERIFY(!ordinaryAudioTitle.attributes.contains(QStringLiteral("Opaque")));

    TimelineClip disabledSoloAudio = soloAudio;
    disabledSoloAudio.audioEnabled = false;
    const QVector<TimelineClip> disabledSoloClips{ordinaryAudio, disabledSoloAudio};
    const TimelineClipTitleModel disabledSoloModel(disabledSoloClips, audioTracks);
    const TimelineClipTitlePresentation audibleTitle =
        disabledSoloModel.describe(disabledSoloClips.at(0));
    QVERIFY(!audibleTitle.attributes.contains(QStringLiteral("Muted by solo")));
}

void TestMediaDragDrop::clipTooltipRefreshesAfterTrackMutations()
{
    TimelineClip audio;
    audio.id = QStringLiteral("tooltip-audio");
    audio.label = QStringLiteral("Interview");
    audio.filePath = QStringLiteral("/missing/interview.wav");
    audio.mediaType = ClipMediaType::Audio;
    audio.hasAudio = true;
    audio.audioEnabled = true;
    audio.startFrame = 0;
    audio.durationFrames = 100;
    audio.sourceDurationFrames = 100;
    audio.trackIndex = 0;

    TimelineTrack audioTrack;
    audioTrack.name = QStringLiteral("Dialogue");
    TimelineTrack spareTrack;
    spareTrack.name = QStringLiteral("Spare");

    TimelineWidget timeline;
    timeline.resize(900, 320);
    timeline.setTracks({audioTrack, spareTrack});
    timeline.setClips({audio});
    timeline.show();
    QApplication::processEvents();

    const QPoint firstHover(40, timeline.trackTop(0) + timeline.trackHeight(0) / 2);
    QTest::mouseMove(&timeline, firstHover);
    QApplication::processEvents();
    QVERIFY(!timeline.toolTip().contains(QStringLiteral("Track muted")));
    QVERIFY(timeline.toolTip().contains(QStringLiteral("Track: 1")));

    QVERIFY(timeline.updateTrackByIndex(0, [](TimelineTrack& track) {
        track.audioMuted = true;
    }));
    QTest::mouseMove(&timeline, firstHover + QPoint(1, 0));
    QApplication::processEvents();
    QVERIFY(timeline.toolTip().contains(QStringLiteral("Track muted")));

    QVERIFY(timeline.moveTrack(0, 1));
    const QPoint movedHover(40, timeline.trackTop(1) + timeline.trackHeight(1) / 2);
    QTest::mouseMove(&timeline, movedHover);
    QApplication::processEvents();
    QVERIFY(timeline.toolTip().contains(QStringLiteral("Track: 2")));
}

void TestMediaDragDrop::maskClipTitlePaintsAfterStatusDecorations()
{
    auto renderWithMaskLabel = [](const QString& label) {
        TimelineClip source = makeMaskTestSource();
        source.startFrame = 0;
        source.sourceInFrame = 0;
        source.durationFrames = 1000;
        source.sourceDurationFrames = 1000;
        TimelineClip child = makeMaskTestChild(source);
        child.label = label;
        child.startFrame = source.startFrame;
        child.sourceInFrame = source.sourceInFrame;
        child.durationFrames = source.durationFrames;
        child.sourceDurationFrames = source.sourceDurationFrames;
        child.maskEnabled = true;

        TimelineTrack sourceTrack;
        sourceTrack.name = QStringLiteral("Source");
        TimelineTrack childTrack;
        childTrack.name = QStringLiteral("Mask");
        childTrack.generatedChildTrack = true;
        childTrack.parentClipId = source.id;
        childTrack.childClipId = child.id;

        TimelineWidget timeline;
        timeline.resize(900, 320);
        timeline.setTracks({sourceTrack, childTrack});
        timeline.setClips({source, child});

        QImage image(timeline.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        timeline.render(&painter);
        painter.end();
        return image;
    };

    const QImage alphaTitle = renderWithMaskLabel(
        QStringLiteral("Source · ALPHA Mask"));
    const QImage personTitle = renderWithMaskLabel(
        QStringLiteral("Source · PERSON Mask"));
    QVERIFY2(alphaTitle != personTitle,
             "Mask title text must remain paint-visible after the Z/status bar sets NoPen");
}

void TestMediaDragDrop::sharedExportRangeCoreMatchesTimelineWidget()
{
    TimelineWidget timeline;
    timeline.setExportRanges({
        ExportRangeSegment{10, 100},
        ExportRangeSegment{150, 200}});
    std::vector<jcut::export_range::Range> shared = {
        {10, 100},
        {150, 200}};

    const auto compareRanges = [&]() {
        const QVector<ExportRangeSegment>& qt = timeline.exportRanges();
        QCOMPARE(qt.size(), static_cast<qsizetype>(shared.size()));
        for (qsizetype index = 0; index < qt.size(); ++index) {
            QCOMPARE(
                qt[index].startFrame,
                shared[static_cast<std::size_t>(index)].startFrame);
            QCOMPARE(
                qt[index].endFrame,
                shared[static_cast<std::size_t>(index)].endFrame);
        }
    };
    compareRanges();

    const std::array edits = {
        std::pair{jcut::export_range::Edit::SplitAtPlayhead, std::int64_t{50}},
        std::pair{jcut::export_range::Edit::SetStartAtPlayhead, std::int64_t{20}},
        std::pair{jcut::export_range::Edit::SetEndAtPlayhead, std::int64_t{180}},
        std::pair{jcut::export_range::Edit::Reset, std::int64_t{0}}};
    for (const auto& [edit, frame] : edits) {
        QVERIFY(jcut::export_range::apply(
            &shared, timeline.totalFrames(), edit, frame));
        QVERIFY(timeline.editExportRanges(edit, frame));
        compareRanges();
    }

    QVERIFY(!jcut::export_range::apply(
        &shared,
        timeline.totalFrames(),
        jcut::export_range::Edit::SplitAtPlayhead,
        0));
    QVERIFY(!timeline.editExportRanges(
        jcut::export_range::Edit::SplitAtPlayhead,
        0));
    compareRanges();

    timeline.setExportRanges({
        ExportRangeSegment{100, 20},
        ExportRangeSegment{20, 100},
        ExportRangeSegment{500, 600}});
    shared = {{100, 20}, {20, 100}, {500, 600}};
    jcut::export_range::normalize(
        &shared, timeline.totalFrames());
    compareRanges();
}

QTEST_MAIN(TestMediaDragDrop)
#include "test_media_drag_drop.moc"
