#include <QtTest/QtTest>

#include "../standalone_preview_renderer.h"

#include <QTemporaryDir>

#include <fstream>

class TestImGuiStandaloneRender : public QObject {
    Q_OBJECT

private slots:
    void testRenderPreviewFrameDecodesImageClip();
    void testLegacyClipWithoutMediaKindIsVisual();
};

void TestImGuiStandaloneRender::testRenderPreviewFrameDecodesImageClip()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString imagePath = tempDir.filePath(QStringLiteral("frame.ppm"));
    {
        std::ofstream output(imagePath.toStdString(), std::ios::binary);
        QVERIFY(output.is_open());
        output << "P6\n2 2\n255\n";
        const unsigned char pixels[] = {
            255, 0, 0,   0, 255, 0,
            0, 0, 255,   255, 255, 0
        };
        output.write(reinterpret_cast<const char*>(pixels), sizeof(pixels));
        QVERIFY(output.good());
    }

    jcut::EditorDocumentCore document;
    document.projectName = "Preview";
    document.tracks.push_back({1, "Video", true});
    document.mediaItems.push_back({imagePath.toStdString(), "frame", "image"});
    document.clips.push_back({1, 1, "frame", 0, 30, true, imagePath.toStdString()});
    document.transport.currentFrame = 0;
    document.exportRequest.outputSize = {320, 240};

    const jcut::standalone_render::PreviewRenderResult result =
        jcut::standalone_render::renderPreviewFrame({
            document,
            document.exportRequest.outputSize,
            0
        });

    QVERIFY2(result.success, result.message.c_str());
    QVERIFY(!result.image.empty());
    QCOMPARE(result.image.size.width, 320);
    QCOMPARE(result.image.size.height, 240);

    const int centerX = result.image.size.width / 2;
    const int centerY = result.image.size.height / 2;
    const std::size_t offset =
        static_cast<std::size_t>(centerY * result.image.strideBytes + centerX * 4);
    const bool nonBlack =
        result.image.bytes[offset + 0] > 0 ||
        result.image.bytes[offset + 1] > 0 ||
        result.image.bytes[offset + 2] > 0;
    QVERIFY(nonBlack);
}

void TestImGuiStandaloneRender::testLegacyClipWithoutMediaKindIsVisual()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString imagePath = tempDir.filePath(QStringLiteral("legacy.ppm"));
    {
        std::ofstream output(imagePath.toStdString(), std::ios::binary);
        QVERIFY(output.is_open());
        output << "P6\n2 2\n255\n";
        const unsigned char pixels[] = {
            240, 32, 32,   240, 32, 32,
            240, 32, 32,   240, 32, 32
        };
        output.write(reinterpret_cast<const char*>(pixels), sizeof(pixels));
        QVERIFY(output.good());
    }

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Video", true});
    document.clips.push_back({1, 1, "clip", 0, 30, true, imagePath.toStdString()});
    document.exportRequest.outputSize = {160, 120};

    const jcut::standalone_render::PreviewRenderResult result =
        jcut::standalone_render::renderPreviewFrame({
            document,
            document.exportRequest.outputSize,
            12
        });

    QVERIFY2(result.success, result.message.c_str());
    QVERIFY(!result.image.empty());
}

QTEST_MAIN(TestImGuiStandaloneRender)
#include "test_imgui_standalone_render.moc"
