#include <QtTest/QtTest>

#include "../media_drag_payload.h"
#include "../timeline_widget.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTemporaryFile>
#include <algorithm>
#include <memory>

class TestMediaDragDrop final : public QObject
{
    Q_OBJECT

private slots:
    void unavailablePathProducesNoPayload();
    void explorerPayloadInsertsClipOnTimeline();
    void loadingTimelineRemovesDuplicateGeneratedMaskTracks();
    void clipClipboardPreservesRelativeLayoutAndUsesFreshIds();
    void cutClipboardRemovesAndRestoresSelection();
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

void TestMediaDragDrop::loadingTimelineRemovesDuplicateGeneratedMaskTracks()
{
    TimelineClip source;
    source.id = QStringLiteral("source");
    source.mediaType = ClipMediaType::Video;
    source.trackIndex = 0;
    source.durationFrames = 100;

    TimelineClip matte = source;
    matte.id = QStringLiteral("source-mask");
    matte.clipRole = ClipRole::MaskMatte;
    matte.linkedSourceClipId = source.id;
    matte.syncLockedToSource = true;
    matte.sourceTransformLocked = true;
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

QTEST_MAIN(TestMediaDragDrop)
#include "test_media_drag_drop.moc"
