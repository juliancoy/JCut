#include <QtTest/QtTest>
#include "../frame_handle.h"

extern "C" {
#include <libavutil/frame.h>
}

using namespace editor;

class TestFrameHandle : public QObject {
    Q_OBJECT

private slots:
    void testDefaultConstruction();
    void testNullFrame();
    void testNullCpuFramePreservesEmptyPayload();
    void testLegacyQrhiApiRemainsLinkable();
    void testCpuFrameCreation();
    void testFrameComparison();
    void testMemoryUsage();
    void testSharedData();
    void testHardwareFramePreservesValidCropRect();
};

void TestFrameHandle::testDefaultConstruction() {
    FrameHandle frame;
    QVERIFY(frame.isNull());
    QVERIFY(!frame);
    QCOMPARE(frame.frameNumber(), -1);
    QVERIFY(frame.sourcePath().isEmpty());
}

void TestFrameHandle::testNullFrame() {
    FrameHandle frame;
    QVERIFY(!frame.hasCpuImage());
    QVERIFY(!frame.hasGpuTexture());
    QCOMPARE(frame.memoryUsage(), 0);
}

void TestFrameHandle::testNullCpuFramePreservesEmptyPayload() {
    const FrameHandle frame = FrameHandle::createCpuFrame(
        QImage(), 5, QStringLiteral("empty.png"));

    QVERIFY(!frame.isNull());
    QVERIFY(!frame.hasCpuImage());
    QVERIFY(frame.cpuImage().isNull());
    QCOMPARE(frame.frameNumber(), 5);
    QCOMPARE(frame.sourcePath(), QStringLiteral("empty.png"));
    QCOMPARE(frame.cpuMemoryUsage(), static_cast<size_t>(0));
    QCOMPARE(frame.memoryUsage(), static_cast<size_t>(0));
}

void TestFrameHandle::testLegacyQrhiApiRemainsLinkable() {
    const FrameHandle gpuFrame = FrameHandle::createGpuFrame(
        nullptr, 9, QStringLiteral("gpu.mov"));
    QVERIFY(gpuFrame.isNull());

    FrameHandle nullFrame;
    nullFrame.uploadToGpu(nullptr);
    QVERIFY(nullFrame.isNull());
    QVERIFY(!nullFrame.isGpuUploadPending());
}

void TestFrameHandle::testCpuFrameCreation() {
    QImage testImage(100, 100, QImage::Format_RGB32);
    testImage.fill(Qt::red);
    
    FrameHandle frame = FrameHandle::createCpuFrame(testImage, 42, "/path/to/video.mp4");
    
    QVERIFY(!frame.isNull());
    QVERIFY(frame.hasCpuImage());
    QVERIFY(!frame.hasGpuTexture());
    QCOMPARE(frame.frameNumber(), 42);
    QCOMPARE(frame.sourcePath(), QString("/path/to/video.mp4"));
    QCOMPARE(frame.size(), QSize(100, 100));
    QCOMPARE(frame.cpuMemoryUsage(), static_cast<size_t>(testImage.sizeInBytes()));
    QCOMPARE(frame.memoryUsage(), frame.cpuMemoryUsage());
    QCOMPARE(frame.gpuMemoryUsage(), static_cast<size_t>(0));
}

void TestFrameHandle::testFrameComparison() {
    QImage testImage(100, 100, QImage::Format_RGB32);
    testImage.fill(Qt::red);
    
    FrameHandle frame1 = FrameHandle::createCpuFrame(testImage, 42, "/path/to/video.mp4");
    FrameHandle frame2 = FrameHandle::createCpuFrame(testImage, 42, "/path/to/video.mp4");
    FrameHandle frame3 = FrameHandle::createCpuFrame(testImage, 43, "/path/to/video.mp4");
    
    // Same frame number and path should be equal
    QVERIFY(frame1 == frame2);
    
    // Different frame number should not be equal
    QVERIFY(frame1 != frame3);
}

void TestFrameHandle::testMemoryUsage() {
    QImage smallImage(100, 100, QImage::Format_RGB32);
    QImage largeImage(1000, 1000, QImage::Format_RGB32);
    
    FrameHandle smallFrame = FrameHandle::createCpuFrame(smallImage, 1, "test.mp4");
    FrameHandle largeFrame = FrameHandle::createCpuFrame(largeImage, 1, "test.mp4");
    
    QVERIFY(largeFrame.memoryUsage() > smallFrame.memoryUsage());
}

void TestFrameHandle::testSharedData() {
    QImage testImage(100, 100, QImage::Format_RGB32);
    FrameHandle frame1 = FrameHandle::createCpuFrame(testImage, 1, "test.mp4");
    
    // Copy should share data
    FrameHandle frame2 = frame1;
    QVERIFY(frame1 == frame2);
    
    // Both should have same data pointer
    QCOMPARE(frame1.data(), frame2.data());
}

void TestFrameHandle::testHardwareFramePreservesValidCropRect() {
    AVFrame* avFrame = av_frame_alloc();
    QVERIFY(avFrame != nullptr);
    avFrame->width = 1920;
    avFrame->height = 1088;
    avFrame->format = AV_PIX_FMT_NV12;
    QVERIFY(av_frame_get_buffer(avFrame, 32) >= 0);
    avFrame->crop_left = 8;
    avFrame->crop_right = 12;
    avFrame->crop_top = 4;
    avFrame->crop_bottom = 4;

    const FrameHandle frame = FrameHandle::createHardwareFrame(
        avFrame, 7, QStringLiteral("padded.mp4"), AV_PIX_FMT_NV12);
    av_frame_free(&avFrame);

    QVERIFY(frame.hasHardwareFrame());
    QCOMPARE(frame.hardwarePixelFormat(), static_cast<int>(AV_PIX_FMT_NV12));
    QCOMPARE(frame.hardwareSwPixelFormat(), static_cast<int>(AV_PIX_FMT_NV12));
    QCOMPARE(frame.cpuMemoryUsage(), static_cast<size_t>(0));
    QCOMPARE(frame.memoryUsage(), static_cast<size_t>(0));
    QCOMPARE(frame.gpuMemoryUsage(),
             static_cast<size_t>(1920 * 1088 * 3 / 2) * 4);
    const QRectF crop = frame.validTextureRectNormalized();
    QVERIFY(qAbs(crop.left() - (8.0 / 1920.0)) < 0.000001);
    QVERIFY(qAbs(crop.top() - (4.0 / 1088.0)) < 0.000001);
    QVERIFY(qAbs(crop.width() - (1900.0 / 1920.0)) < 0.000001);
    QVERIFY(qAbs(crop.height() - (1080.0 / 1088.0)) < 0.000001);
}

QTEST_MAIN(TestFrameHandle)
#include "test_frame_handle.moc"
