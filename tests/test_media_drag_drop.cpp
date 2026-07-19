#include <QtTest/QtTest>

#include "../media_drag_payload.h"
#include "../timeline_widget.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTemporaryFile>
#include <memory>

class TestMediaDragDrop final : public QObject
{
    Q_OBJECT

private slots:
    void unavailablePathProducesNoPayload();
    void explorerPayloadInsertsClipOnTimeline();
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

QTEST_MAIN(TestMediaDragDrop)
#include "test_media_drag_drop.moc"
