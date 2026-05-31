#include <QtTest/QtTest>

#include <QFile>
#include <QString>

class TestDirectVulkanHandoffPipelineContract : public QObject {
    Q_OBJECT

private slots:
    void directPreviewUsesExtractedPipelineBeforeRenderPass();
    void directPreviewRecordsTextureUploadsBeforeRenderPass();
    void directPreviewDoesNotUseSubmitBasedHandoffApis();
};

namespace {

QString readSourceFile(const QString& relativePath)
{
    QFile file(QStringLiteral(JCUT_SOURCE_DIR) + QLatin1Char('/') + relativePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

}

void TestDirectVulkanHandoffPipelineContract::directPreviewUsesExtractedPipelineBeforeRenderPass()
{
    const QString source = readSourceFile(QStringLiteral("direct_vulkan_preview_backend.cpp"));
    QVERIFY2(!source.isEmpty(), "direct_vulkan_preview_backend.cpp must be readable");

    const qsizetype recordIndex = source.indexOf(QStringLiteral("m_frameHandoffPipeline->record("));
    const qsizetype beginRenderPassIndex = source.indexOf(QStringLiteral("vkCmdBeginRenderPass"));
    QVERIFY2(recordIndex >= 0, "direct preview must call the extracted frame handoff pipeline");
    QVERIFY2(beginRenderPassIndex >= 0, "direct preview must explicitly begin its render pass");
    QVERIFY2(recordIndex < beginRenderPassIndex,
             "handoff transfer/compute recording must happen before vkCmdBeginRenderPass");
}

void TestDirectVulkanHandoffPipelineContract::directPreviewRecordsTextureUploadsBeforeRenderPass()
{
    const QString source = readSourceFile(QStringLiteral("direct_vulkan_preview_backend.cpp"));
    QVERIFY2(!source.isEmpty(), "direct_vulkan_preview_backend.cpp must be readable");

    const qsizetype beginRenderPassIndex = source.indexOf(QStringLiteral("vkCmdBeginRenderPass"));
    QVERIFY2(beginRenderPassIndex >= 0, "direct preview must explicitly begin its render pass");

    const QStringList uploadMarkers{
        QStringLiteral("uploadCurveLut(cb,"),
        QStringLiteral("uploadImageTexture(cb, overlayImage)")
    };
    for (const QString& marker : uploadMarkers) {
        qsizetype index = source.indexOf(marker);
        QVERIFY2(index >= 0, qPrintable(QStringLiteral("direct preview must contain %1").arg(marker)));
        while (index >= 0) {
            QVERIFY2(index < beginRenderPassIndex,
                     qPrintable(QStringLiteral("%1 must be recorded before vkCmdBeginRenderPass").arg(marker)));
            index = source.indexOf(marker, index + marker.size());
        }
    }
}

void TestDirectVulkanHandoffPipelineContract::directPreviewDoesNotUseSubmitBasedHandoffApis()
{
    const QString source = readSourceFile(QStringLiteral("direct_vulkan_preview_backend.cpp"));
    QVERIFY2(!source.isEmpty(), "direct_vulkan_preview_backend.cpp must be readable");

    QVERIFY2(!source.contains(QStringLiteral("m_frameHandoff->uploadFrame(")),
             "direct preview must not call the submit-based uploadFrame API");
    QVERIFY2(!source.contains(QStringLiteral("m_frameHandoff->importOffscreenFrame(")),
             "direct preview must not call the submit-based importOffscreenFrame API");
    QVERIFY2(!source.contains(QStringLiteral("uploadImageTexture(cb, status->frame.cpuImage()")),
             "direct preview must not implicitly fall back to CPU frame upload");
}

QTEST_MAIN(TestDirectVulkanHandoffPipelineContract)
#include "test_direct_vulkan_handoff_pipeline_contract.moc"
