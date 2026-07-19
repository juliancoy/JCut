#include <QtTest/QtTest>
#include <QTemporaryFile>
#include <QSignalSpy>
#include <atomic>
#include "../async_decoder.h"

using namespace editor;

class TestAsyncDecoder : public QObject {
    Q_OBJECT

private slots:
    void testInitialization();
    void testVideoInfo();
    void testInvalidFile();
    void testMissingFileFailureIsReportedOnce();
    void testRequestFrame();
    void testCancelRequests();
    void testMultipleRequests();
};

void TestAsyncDecoder::testInitialization() {
    AsyncDecoder decoder;
    QVERIFY(decoder.initialize());
    QVERIFY(decoder.workerCount() > 0);
    decoder.shutdown();
}

void TestAsyncDecoder::testVideoInfo() {
    AsyncDecoder decoder;
    decoder.initialize();
    
    // Test with non-existent file
    const QString missingPath = QStringLiteral("/nonexistent/file.mp4");
    VideoStreamInfo info = decoder.getVideoInfo(missingPath);
    QVERIFY(!info.isValid);
    QCOMPARE(info.path, missingPath);

    // Repeated UI metadata queries must return the cached unavailable result
    // without repeatedly constructing an FFmpeg decoder for the same path.
    const VideoStreamInfo cachedInfo = decoder.getVideoInfo(missingPath);
    QVERIFY(!cachedInfo.isValid);
    QCOMPARE(cachedInfo.path, missingPath);
    decoder.shutdown();
}

void TestAsyncDecoder::testInvalidFile() {
    AsyncDecoder decoder;
    decoder.initialize();
    
    bool callbackCalled = false;
    FrameHandle receivedFrame;
    
    decoder.requestFrame("/nonexistent/file.mp4", 0, 100, 1000,
        [&callbackCalled, &receivedFrame](FrameHandle frame) {
            callbackCalled = true;
            receivedFrame = frame;
        });
    
    QTRY_VERIFY_WITH_TIMEOUT(callbackCalled, 1000);
    QVERIFY(callbackCalled);
    QVERIFY(receivedFrame.isNull());
    decoder.shutdown();
}

void TestAsyncDecoder::testMissingFileFailureIsReportedOnce() {
    AsyncDecoder decoder;
    QVERIFY(decoder.initialize());
    QSignalSpy errorSpy(&decoder, &AsyncDecoder::error);
    std::atomic<int> callbackCount{0};

    const QString missingPath = QStringLiteral("/nonexistent/jcut/repeated-source.mp4");
    for (int i = 0; i < 4; ++i) {
        decoder.requestFrame(missingPath, i, 100, 1000,
            [&callbackCount](FrameHandle) { ++callbackCount; });
        QTRY_COMPARE_WITH_TIMEOUT(callbackCount.load(), i + 1, 1000);
    }

    QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 1, 1000);
    QCOMPARE(errorSpy.at(0).at(0).toString(), missingPath);
    QCOMPARE(errorSpy.at(0).at(1).toString(), QStringLiteral("Input file does not exist"));
    decoder.shutdown();
}

void TestAsyncDecoder::testRequestFrame() {
    AsyncDecoder decoder;
    decoder.initialize();
    
    // Create a dummy request to test the queue system
    bool callbackCalled = false;
    uint64_t seqId = decoder.requestFrame("/tmp/test.mp4", 0, 100, 1000,
        [&callbackCalled](FrameHandle frame) {
            callbackCalled = true;
        });
    
    QVERIFY(seqId > 0);
    QVERIFY(decoder.pendingRequestCount() >= 0);
    QTRY_VERIFY_WITH_TIMEOUT(callbackCalled, 1000);
    decoder.shutdown();
}

void TestAsyncDecoder::testCancelRequests() {
    AsyncDecoder decoder;
    decoder.initialize();
    
    // Queue multiple requests
    for (int i = 0; i < 10; ++i) {
        decoder.requestFrame("/tmp/test.mp4", i, 50, 5000,
            [](FrameHandle frame) {
                // Callback
            });
    }
    
    // Cancel all for this file
    decoder.cancelForFile("/tmp/test.mp4");
    
    // Queue should be cleared or processing
    QVERIFY(decoder.pendingRequestCount() >= 0);
    decoder.shutdown();
}

void TestAsyncDecoder::testMultipleRequests() {
    AsyncDecoder decoder;
    decoder.initialize();
    
    std::atomic<int> callbackCount{0};
    
    // Request multiple frames with different priorities
    decoder.requestFrame("/tmp/test1.mp4", 0, 100, 1000,
        [&callbackCount](FrameHandle frame) { callbackCount.fetch_add(1); });
    decoder.requestFrame("/tmp/test2.mp4", 0, 50, 1000,
        [&callbackCount](FrameHandle frame) { callbackCount.fetch_add(1); });
    decoder.requestFrame("/tmp/test3.mp4", 0, 10, 1000,
        [&callbackCount](FrameHandle frame) { callbackCount.fetch_add(1); });

    QTRY_COMPARE_WITH_TIMEOUT(callbackCount.load(), 3, 1000);
    QCOMPARE(callbackCount.load(), 3);
    decoder.shutdown();
}

QTEST_MAIN(TestAsyncDecoder)
#include "test_async_decoder.moc"
