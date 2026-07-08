#include <QtTest/QtTest>

#include "../auto_sync_plan.h"

class TestAutoSyncPlan : public QObject {
    Q_OBJECT

private slots:
    void lockedAudioClipAnchorsFreeAudioClip();
    void multipleLockedAudioClipsAreAllAnchors();
    void lockedTargetsDoNotOwnMarkerReplacement();
    void allLockedAudioClipsProducesNoFreeTargets();
};

namespace {

TimelineClip makeClip(const QString& id, ClipMediaType mediaType, bool hasAudio, bool locked)
{
    TimelineClip clip;
    clip.id = id;
    clip.label = id;
    clip.filePath = QStringLiteral("/media/%1").arg(id);
    clip.mediaType = mediaType;
    clip.hasAudio = hasAudio;
    clip.locked = locked;
    return clip;
}

AutoSyncSelectionPlan planFor(const QVector<TimelineClip>& clips, const QSet<QString>& selected)
{
    return buildAutoSyncSelectionPlan(
        clips,
        selected,
        [](const TimelineClip& clip) {
            return clip.mediaType == ClipMediaType::Video || clip.mediaType == ClipMediaType::Image;
        },
        [](const TimelineClip& clip) {
            return (clip.hasAudio || clip.mediaType == ClipMediaType::Audio)
                ? QStringLiteral("/audio/%1.wav").arg(clip.id)
                : QString();
        },
        [](const TimelineClip& clip) {
            return (clip.mediaType == ClipMediaType::Video || clip.mediaType == ClipMediaType::Image)
                ? QStringLiteral("/video/%1.mov").arg(clip.id)
                : QString();
        });
}

} // namespace

void TestAutoSyncPlan::lockedAudioClipAnchorsFreeAudioClip()
{
    const TimelineClip locked = makeClip(QStringLiteral("locked"), ClipMediaType::Audio, true, true);
    const TimelineClip free = makeClip(QStringLiteral("free"), ClipMediaType::Audio, true, false);

    const AutoSyncSelectionPlan plan = planFor({locked, free}, {locked.id, free.id});

    QVERIFY(plan.ok);
    QCOMPARE(plan.primaryAudioAnchor.clip.id, locked.id);
    QCOMPARE(plan.lockedAudioAnchors.size(), 1);
    QCOMPARE(plan.targets.size(), 1);
    QCOMPARE(plan.targets.constFirst().clip.id, free.id);
    QCOMPARE(plan.targets.constFirst().mode, QStringLiteral("audio"));
    QCOMPARE(plan.targets.constFirst().anchors.size(), 1);
    QCOMPARE(plan.targets.constFirst().anchors.constFirst().clip.id, locked.id);
    QVERIFY(plan.syncMarkerClipIds.contains(free.id));
    QVERIFY(!plan.syncMarkerClipIds.contains(locked.id));
}

void TestAutoSyncPlan::multipleLockedAudioClipsAreAllAnchors()
{
    const TimelineClip lockedA = makeClip(QStringLiteral("locked-a"), ClipMediaType::Audio, true, true);
    const TimelineClip lockedB = makeClip(QStringLiteral("locked-b"), ClipMediaType::Audio, true, true);
    const TimelineClip free = makeClip(QStringLiteral("free"), ClipMediaType::Audio, true, false);

    const AutoSyncSelectionPlan plan = planFor({lockedA, lockedB, free}, {lockedA.id, lockedB.id, free.id});

    QVERIFY(plan.ok);
    QCOMPARE(plan.lockedAudioAnchors.size(), 2);
    QCOMPARE(plan.targets.size(), 1);
    QCOMPARE(plan.targets.constFirst().anchors.size(), 2);
    QCOMPARE(plan.targets.constFirst().anchors.at(0).clip.id, lockedA.id);
    QCOMPARE(plan.targets.constFirst().anchors.at(1).clip.id, lockedB.id);
}

void TestAutoSyncPlan::lockedTargetsDoNotOwnMarkerReplacement()
{
    const TimelineClip freeAudio = makeClip(QStringLiteral("free-audio"), ClipMediaType::Audio, true, false);
    const TimelineClip lockedAudio = makeClip(QStringLiteral("locked-audio"), ClipMediaType::Audio, true, true);
    const TimelineClip lockedVideo = makeClip(QStringLiteral("locked-video"), ClipMediaType::Video, false, true);
    const TimelineClip freeVideo = makeClip(QStringLiteral("free-video"), ClipMediaType::Video, false, false);

    const AutoSyncSelectionPlan plan =
        planFor({freeAudio, lockedAudio, lockedVideo, freeVideo},
                {freeAudio.id, lockedAudio.id, lockedVideo.id, freeVideo.id});

    QVERIFY(plan.ok);
    QCOMPARE(plan.targets.size(), 2);
    QCOMPARE(plan.targets.at(0).clip.id, freeAudio.id);
    QCOMPARE(plan.targets.at(1).clip.id, freeVideo.id);
    QVERIFY(plan.syncMarkerClipIds.contains(freeAudio.id));
    QVERIFY(plan.syncMarkerClipIds.contains(freeVideo.id));
    QVERIFY(!plan.syncMarkerClipIds.contains(lockedAudio.id));
    QVERIFY(!plan.syncMarkerClipIds.contains(lockedVideo.id));
}

void TestAutoSyncPlan::allLockedAudioClipsProducesNoFreeTargets()
{
    const TimelineClip lockedA = makeClip(QStringLiteral("locked-a"), ClipMediaType::Audio, true, true);
    const TimelineClip lockedB = makeClip(QStringLiteral("locked-b"), ClipMediaType::Audio, true, true);

    const AutoSyncSelectionPlan plan = planFor({lockedA, lockedB}, {lockedA.id, lockedB.id});

    QVERIFY(!plan.ok);
    QCOMPARE(plan.targets.size(), 0);
    QCOMPARE(plan.lockedAudioAnchors.size(), 2);
    QCOMPARE(plan.message, QStringLiteral("Select another clip with audio, or a visual-only clip, alongside the audio anchor."));
}

QTEST_MAIN(TestAutoSyncPlan)
#include "test_auto_sync_plan.moc"
