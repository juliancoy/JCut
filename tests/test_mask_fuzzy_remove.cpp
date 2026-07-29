#include <QtTest/QtTest>

#include "../mask_fuzzy_remove.h"

#include <QDir>
#include <QImage>
#include <QTemporaryDir>

class TestMaskFuzzyRemove final : public QObject
{
    Q_OBJECT

private slots:
    void tracksConnectedRegionThroughSpaceAndTimeWithoutEditingOriginal();
};

namespace {

void writeFrame(const QString& directory, int oneBasedFrame, int extraX, bool includeExtra)
{
    QImage image(32, 24, QImage::Format_Grayscale8);
    image.fill(0);
    for (int y = 5; y < 19; ++y) {
        for (int x = 18; x < 29; ++x) image.scanLine(y)[x] = 255;
    }
    if (includeExtra) {
        for (int y = 9; y < 13; ++y) {
            for (int x = extraX; x < extraX + 4; ++x) image.scanLine(y)[x] = 255;
        }
    }
    QVERIFY(image.save(QDir(directory).filePath(
        QStringLiteral("frame_%1.png").arg(oneBasedFrame, 6, 10, QLatin1Char('0')))));
}

QImage frame(const QString& directory, int oneBasedFrame)
{
    return QImage(QDir(directory).filePath(
        QStringLiteral("frame_%1.png").arg(oneBasedFrame, 6, 10, QLatin1Char('0'))))
        .convertToFormat(QImage::Format_Grayscale8);
}

} // namespace

void TestMaskFuzzyRemove::tracksConnectedRegionThroughSpaceAndTimeWithoutEditingOriginal()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString source = QDir(root.path()).filePath(QStringLiteral("clip_masks"));
    QVERIFY(QDir().mkpath(source));
    writeFrame(source, 1, 3, true);
    writeFrame(source, 2, 4, true);
    writeFrame(source, 3, 5, true);
    writeFrame(source, 4, 0, false);
    writeFrame(source, 5, 6, true);

    editor::masks::FuzzyRemoveRequest request;
    request.sourceDirectory = source;
    request.sourceFrame = 1;
    request.sourcePresentationTimestamp = 1;
    request.xNorm = 5.0 / 32.0;
    request.yNorm = 10.0 / 24.0;
    request.spatialReachPixels = 2;
    request.temporalReachFrames = 10;
    const editor::masks::FuzzyRemoveResult result =
        editor::masks::fuzzyRemoveMaskRegion(request);

    QVERIFY2(result.succeeded(), qPrintable(result.error));
    QCOMPARE(result.changedFrames, 3);
    QVERIFY(frame(source, 2).constScanLine(10)[5] > 0);
    QVERIFY(frame(result.outputDirectory, 1).constScanLine(10)[4] == 0);
    QVERIFY(frame(result.outputDirectory, 2).constScanLine(10)[5] == 0);
    QVERIFY(frame(result.outputDirectory, 3).constScanLine(10)[6] == 0);
    QVERIFY(frame(result.outputDirectory, 5).constScanLine(10)[7] > 0);
    QVERIFY(frame(result.outputDirectory, 2).constScanLine(10)[20] > 0);
    QVERIFY(QFileInfo::exists(
        QDir(result.outputDirectory).filePath(QStringLiteral("jcut_fuzzy_remove.json"))));
}

QTEST_MAIN(TestMaskFuzzyRemove)
#include "test_mask_fuzzy_remove.moc"
