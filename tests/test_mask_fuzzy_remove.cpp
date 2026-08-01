#include <QtTest/QtTest>

#include "../mask_fuzzy_remove.h"

#include <QDir>
#include <QFile>
#include <QFileInfoList>
#include <QImage>
#include <QTemporaryDir>

#include <filesystem>

class TestMaskFuzzyRemove final : public QObject
{
    Q_OBJECT

private slots:
    void tracksMovingComponentAndPreservesOriginal();
    void stopsBeforeComponentMergesIntoSubject();
    void stopsOnAmbiguousBranch();
    void stopsOnImplausibleAreaChange();
    void stopsOnImplausibleAreaCollapse();
    void preservesSoftAlphaOutsideSelection();
    void derivedFramesDoNotShareSourceInodes();
    void deterministicCacheReuse();
    void incompleteCacheIsNotReused();
    void analysisCancellationStopsTraversal();
    void cancellationPublishesNothing();
};

namespace {

QString framePath(const QString& directory, int oneBasedFrame)
{
    return QDir(directory).filePath(
        QStringLiteral("frame_%1.png").arg(
            oneBasedFrame, 6, 10, QLatin1Char('0')));
}

QImage blankFrame(int fill = 0)
{
    QImage image(48, 32, QImage::Format_Grayscale8);
    image.fill(fill);
    return image;
}

void fillRect(QImage* image, const QRect& rect, int value)
{
    const QRect bounded = rect.intersected(image->rect());
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        uchar* row = image->scanLine(y);
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            row[x] = static_cast<uchar>(value);
        }
    }
}

void saveFrame(const QString& directory, int oneBasedFrame, const QImage& image)
{
    QVERIFY2(image.save(framePath(directory, oneBasedFrame)),
             qPrintable(framePath(directory, oneBasedFrame)));
}

QImage loadFrame(const QString& directory, int oneBasedFrame)
{
    return QImage(framePath(directory, oneBasedFrame))
        .convertToFormat(QImage::Format_Grayscale8);
}

editor::masks::FuzzyRemoveRequest requestFor(
    const QString& source, int zeroBasedFrame, int x, int y)
{
    editor::masks::FuzzyRemoveRequest request;
    request.sourceDirectory = source;
    request.sourceFrame = zeroBasedFrame;
    request.sourcePresentationTimestamp = zeroBasedFrame;
    request.xNorm = static_cast<double>(x) / 48.0;
    request.yNorm = static_cast<double>(y) / 32.0;
    request.spatialReachPixels = 3;
    request.temporalReachFrames = 20;
    return request;
}

QString createSource(QTemporaryDir* root)
{
    const QString source = QDir(root->path()).filePath(QStringLiteral("clip_masks"));
    if (!QDir().mkpath(source)) return {};
    return source;
}

void addSubject(QImage* image, int value = 255)
{
    fillRect(image, QRect(30, 5, 14, 22), value);
}

} // namespace

void TestMaskFuzzyRemove::tracksMovingComponentAndPreservesOriginal()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString source = createSource(&root);
    QVERIFY(!source.isEmpty());
    for (int frame = 0; frame < 5; ++frame) {
        QImage image = blankFrame();
        addSubject(&image);
        if (frame != 3) fillRect(&image, QRect(4 + frame, 12, 4, 4), 255);
        saveFrame(source, frame + 1, image);
    }

    const editor::masks::FuzzyRemoveRequest request = requestFor(source, 1, 6, 13);
    const editor::masks::FuzzyRemoveAnalysis analysis =
        editor::masks::analyzeFuzzyRemoveMaskRegion(request);
    QVERIFY2(analysis.succeeded(), qPrintable(analysis.error));
    QCOMPARE(analysis.frames.size(), 3);
    QCOMPARE(analysis.forwardStopReason, QStringLiteral("component disappeared"));
    const editor::masks::FuzzyRemoveResult result =
        editor::masks::materializeFuzzyRemoveMaskRegion(analysis);

    QVERIFY2(result.succeeded(), qPrintable(result.error));
    QCOMPARE(result.changedFrames, 3);
    QVERIFY(loadFrame(source, 2).constScanLine(13)[6] > 0);
    QVERIFY(loadFrame(result.outputDirectory, 1).constScanLine(13)[5] == 0);
    QVERIFY(loadFrame(result.outputDirectory, 2).constScanLine(13)[6] == 0);
    QVERIFY(loadFrame(result.outputDirectory, 3).constScanLine(13)[7] == 0);
    QVERIFY(loadFrame(result.outputDirectory, 5).constScanLine(13)[8] > 0);
    QVERIFY(loadFrame(result.outputDirectory, 2).constScanLine(13)[35] > 0);
}

void TestMaskFuzzyRemove::stopsBeforeComponentMergesIntoSubject()
{
    QTemporaryDir root;
    const QString source = createSource(&root);
    for (int frame = 0; frame < 4; ++frame) {
        QImage image = blankFrame();
        addSubject(&image);
        fillRect(&image, QRect(10 + frame, 13, 4, 4), 255);
        if (frame >= 2) {
            fillRect(&image, QRect(13, 14, 18, 2), 255); // joins subject
        }
        saveFrame(source, frame + 1, image);
    }
    const auto analysis = editor::masks::analyzeFuzzyRemoveMaskRegion(
        requestFor(source, 1, 12, 14));
    QVERIFY2(analysis.succeeded(), qPrintable(analysis.error));
    QCOMPARE(analysis.frames.size(), 2);
    QVERIFY(analysis.forwardStopReason.contains(QStringLiteral("merge")) ||
            analysis.forwardStopReason.contains(QStringLiteral("growth")));
    QCOMPARE(analysis.lastMaskOrdinal, 1);
}

void TestMaskFuzzyRemove::stopsOnAmbiguousBranch()
{
    QTemporaryDir root;
    const QString source = createSource(&root);
    QImage first = blankFrame();
    fillRect(&first, QRect(20, 13, 5, 5), 255);
    saveFrame(source, 1, first);
    QImage split = blankFrame();
    fillRect(&split, QRect(15, 13, 5, 5), 255);
    fillRect(&split, QRect(25, 13, 5, 5), 255);
    saveFrame(source, 2, split);

    auto request = requestFor(source, 0, 22, 15);
    request.spatialReachPixels = 6;
    const auto analysis =
        editor::masks::analyzeFuzzyRemoveMaskRegion(request);
    QVERIFY2(analysis.succeeded(), qPrintable(analysis.error));
    QCOMPARE(analysis.frames.size(), 1);
    QCOMPARE(analysis.forwardStopReason,
             QStringLiteral("stopped on ambiguous component match"));
}

void TestMaskFuzzyRemove::stopsOnImplausibleAreaChange()
{
    QTemporaryDir root;
    const QString source = createSource(&root);
    QImage normal = blankFrame();
    fillRect(&normal, QRect(8, 10, 6, 6), 255);
    saveFrame(source, 1, normal);
    QImage grown = blankFrame();
    fillRect(&grown, QRect(5, 5, 18, 18), 255);
    saveFrame(source, 2, grown);

    const auto analysis = editor::masks::analyzeFuzzyRemoveMaskRegion(
        requestFor(source, 0, 10, 12));
    QVERIFY2(analysis.succeeded(), qPrintable(analysis.error));
    QCOMPARE(analysis.frames.size(), 1);
    QVERIFY(analysis.forwardStopReason.contains(QStringLiteral("growth")));
}

void TestMaskFuzzyRemove::stopsOnImplausibleAreaCollapse()
{
    QTemporaryDir root;
    const QString source = createSource(&root);
    QImage normal = blankFrame();
    fillRect(&normal, QRect(8, 8, 10, 10), 255);
    saveFrame(source, 1, normal);
    QImage collapsed = blankFrame();
    fillRect(&collapsed, QRect(11, 11, 2, 2), 255);
    saveFrame(source, 2, collapsed);

    const auto analysis = editor::masks::analyzeFuzzyRemoveMaskRegion(
        requestFor(source, 0, 12, 12));
    QVERIFY2(analysis.succeeded(), qPrintable(analysis.error));
    QCOMPARE(analysis.frames.size(), 1);
    QVERIFY(analysis.forwardStopReason.contains(QStringLiteral("collapse")));
}

void TestMaskFuzzyRemove::preservesSoftAlphaOutsideSelection()
{
    QTemporaryDir root;
    const QString source = createSource(&root);
    QImage image = blankFrame(19);
    addSubject(&image, 231);
    fillRect(&image, QRect(5, 12, 4, 4), 180);
    saveFrame(source, 1, image);

    const auto analysis = editor::masks::analyzeFuzzyRemoveMaskRegion(
        requestFor(source, 0, 6, 13));
    QVERIFY(analysis.succeeded());
    const auto result =
        editor::masks::materializeFuzzyRemoveMaskRegion(analysis);
    QVERIFY2(result.succeeded(), qPrintable(result.error));
    const QImage edited = loadFrame(result.outputDirectory, 1);
    QCOMPARE(edited.constScanLine(0)[0], static_cast<uchar>(19));
    QCOMPARE(edited.constScanLine(10)[35], static_cast<uchar>(231));
    QCOMPARE(edited.constScanLine(13)[6], static_cast<uchar>(0));
}

void TestMaskFuzzyRemove::derivedFramesDoNotShareSourceInodes()
{
    QTemporaryDir root;
    const QString source = createSource(&root);
    for (int frame = 0; frame < 2; ++frame) {
        QImage image = blankFrame();
        fillRect(&image, QRect(5, 12, 4, 4), 255);
        saveFrame(source, frame + 1, image);
    }
    auto request = requestFor(source, 0, 6, 13);
    request.temporalReachFrames = 0;
    const auto result = editor::masks::fuzzyRemoveMaskRegion(request);
    QVERIFY2(result.succeeded(), qPrintable(result.error));
    QVERIFY(!std::filesystem::equivalent(
        std::filesystem::path(framePath(source, 2).toStdString()),
        std::filesystem::path(
            framePath(result.outputDirectory, 2).toStdString())));
}

void TestMaskFuzzyRemove::deterministicCacheReuse()
{
    QTemporaryDir root;
    const QString source = createSource(&root);
    QImage image = blankFrame();
    fillRect(&image, QRect(5, 12, 4, 4), 255);
    saveFrame(source, 1, image);
    const auto analysis = editor::masks::analyzeFuzzyRemoveMaskRegion(
        requestFor(source, 0, 6, 13));
    const auto first =
        editor::masks::materializeFuzzyRemoveMaskRegion(analysis);
    const auto second =
        editor::masks::materializeFuzzyRemoveMaskRegion(analysis);
    QVERIFY(first.succeeded());
    QVERIFY(second.succeeded());
    QCOMPARE(second.outputDirectory, first.outputDirectory);
    QCOMPARE(second.recipeHash, first.recipeHash);
    QVERIFY(second.reusedExisting);
}

void TestMaskFuzzyRemove::incompleteCacheIsNotReused()
{
    QTemporaryDir root;
    const QString source = createSource(&root);
    for (int frame = 0; frame < 2; ++frame) {
        QImage image = blankFrame();
        fillRect(&image, QRect(5, 12, 4, 4), 255);
        saveFrame(source, frame + 1, image);
    }
    auto request = requestFor(source, 0, 6, 13);
    request.temporalReachFrames = 0;
    const auto analysis =
        editor::masks::analyzeFuzzyRemoveMaskRegion(request);
    const auto first =
        editor::masks::materializeFuzzyRemoveMaskRegion(analysis);
    QVERIFY2(first.succeeded(), qPrintable(first.error));
    QVERIFY(QFile::remove(framePath(first.outputDirectory, 2)));

    const auto second =
        editor::masks::materializeFuzzyRemoveMaskRegion(analysis);
    QVERIFY(!second.succeeded());
    QCOMPARE(second.error,
             QStringLiteral("The derived mask cache is incomplete."));
}

void TestMaskFuzzyRemove::analysisCancellationStopsTraversal()
{
    QTemporaryDir root;
    const QString source = createSource(&root);
    for (int frame = 0; frame < 8; ++frame) {
        QImage image = blankFrame();
        fillRect(&image, QRect(5 + frame, 12, 4, 4), 255);
        saveFrame(source, frame + 1, image);
    }
    auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto analysis = editor::masks::analyzeFuzzyRemoveMaskRegion(
        requestFor(source, 3, 9, 13),
        cancel,
        [cancel](int completed, int, const QString&) {
            if (completed >= 2) {
                cancel->store(true, std::memory_order_relaxed);
            }
        });
    QVERIFY(!analysis.succeeded());
    QVERIFY(analysis.cancelled);
    QVERIFY(analysis.backwardStopReason == QStringLiteral("cancelled") ||
            analysis.forwardStopReason == QStringLiteral("cancelled"));
}

void TestMaskFuzzyRemove::cancellationPublishesNothing()
{
    QTemporaryDir root;
    const QString source = createSource(&root);
    QImage image = blankFrame();
    fillRect(&image, QRect(5, 12, 4, 4), 255);
    saveFrame(source, 1, image);
    const auto analysis = editor::masks::analyzeFuzzyRemoveMaskRegion(
        requestFor(source, 0, 6, 13));
    auto cancel = std::make_shared<std::atomic_bool>(true);
    const auto result =
        editor::masks::materializeFuzzyRemoveMaskRegion(analysis, cancel);
    QVERIFY(!result.succeeded());
    QVERIFY(result.cancelled);
    QVERIFY(!QFileInfo::exists(result.outputDirectory));
    const QFileInfoList partials = QDir(root.path()).entryInfoList(
        QStringList{QStringLiteral("*.partial_*")},
        QDir::Dirs | QDir::NoDotAndDotDot);
    QVERIFY(partials.isEmpty());
}

QTEST_MAIN(TestMaskFuzzyRemove)
#include "test_mask_fuzzy_remove.moc"
