#include <QtTest/QtTest>

#include <QFile>
#include <QString>

class TestDirectVulkanHandoffPipelineContract : public QObject {
    Q_OBJECT

private slots:
    void directPreviewUsesExtractedPipelineBeforeRenderPass();
    void directPreviewRecordsTextureUploadsBeforeRenderPass();
    void directPreviewDoesNotUseSubmitBasedHandoffApis();
    void directPreviewRequiresHardwarePayloadsFromCache();
    void handoffPipelineRejectsCpuOnlyFrames();
    void strictDisplayabilityDoesNotAcceptCpuFallback();
    void directPreviewDisablesCpuAndQtTextOverlayFallbacks();
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

void TestDirectVulkanHandoffPipelineContract::directPreviewRequiresHardwarePayloadsFromCache()
{
    const QString source = readSourceFile(QStringLiteral("vulkan_preview_surface.cpp"));
    QVERIFY2(!source.isEmpty(), "vulkan_preview_surface.cpp must be readable");

    QVERIFY2(source.contains(QStringLiteral("constexpr bool requireDirectVulkanPayload = true;")),
             "direct Vulkan preview must request strict hardware/GPU payloads");
    QVERIFY2(source.contains(QStringLiteral("requireDirectVulkanPayload);")),
             "strict payload requirement must be passed into visible frame requests");
}

void TestDirectVulkanHandoffPipelineContract::handoffPipelineRejectsCpuOnlyFrames()
{
    const QString source = readSourceFile(QStringLiteral("direct_vulkan_frame_handoff_pipeline.cpp"));
    QVERIFY2(!source.isEmpty(), "direct_vulkan_frame_handoff_pipeline.cpp must be readable");

    QVERIFY2(source.contains(QStringLiteral("!status.externalVulkanFrame && !status.frame.hasHardwareFrame()")),
             "handoff pipeline must reject frames that are not external Vulkan or hardware frames");
    QVERIFY2(source.contains(QStringLiteral("CPU upload fallback is disabled")),
             "handoff pipeline must report CPU fallback rejection explicitly");
    QVERIFY2(!source.contains(QStringLiteral("status.frame.cpuImage()")),
             "handoff pipeline must not consume CPU image payloads");
}

void TestDirectVulkanHandoffPipelineContract::strictDisplayabilityDoesNotAcceptCpuFallback()
{
    const QString source = readSourceFile(QStringLiteral("timeline_cache_requests.cpp"));
    QVERIFY2(!source.isEmpty(), "timeline_cache_requests.cpp must be readable");

    QVERIFY2(source.contains(QStringLiteral("requireHardwareOrGpuPayload &&")),
             "timeline cache must distinguish strict hardware/GPU preview requests");
    QVERIFY2(source.contains(QStringLiteral("!frame.hasHardwareFrame() && !frame.hasGpuTexture()")),
             "strict visible preview must reject non-hardware/non-GPU payloads");
    QVERIFY2(source.contains(QStringLiteral("strictPayloadRejected")),
             "strict CPU-payload rejection must be counted for diagnostics");
}

void TestDirectVulkanHandoffPipelineContract::directPreviewDisablesCpuAndQtTextOverlayFallbacks()
{
    const QString backend = readSourceFile(QStringLiteral("direct_vulkan_preview_backend.cpp"));
    QVERIFY2(!backend.isEmpty(), "direct_vulkan_preview_backend.cpp must be readable");
    QVERIFY2(backend.contains(QStringLiteral("kAllowCpuRasterTextOverlaysInDirectVulkanPreview = false")),
             "direct Vulkan preview must not CPU-rasterize title/transcript/speaker/status text overlays");

    const QString presenter = readSourceFile(QStringLiteral("direct_vulkan_preview_presenter.cpp"));
    QVERIFY2(!presenter.isEmpty(), "direct_vulkan_preview_presenter.cpp must be readable");
    QVERIFY2(presenter.contains(QStringLiteral("kAllowQtPainterOverlayInDirectVulkanPreview = false")),
             "direct Vulkan preview must not paint presentation overlays through Qt/QPainter");

    const QString profiling = readSourceFile(QStringLiteral("vulkan_preview_surface_profiling.cpp"));
    QVERIFY2(!profiling.isEmpty(), "vulkan_preview_surface_profiling.cpp must be readable");
    QVERIFY2(profiling.contains(QStringLiteral("vulkan_text_overlay_cpu_rasterization_enabled")),
             "profiling must expose the CPU text overlay contract");
    QVERIFY2(profiling.contains(QStringLiteral("vulkan_text_overlay_qt_painter_enabled")),
             "profiling must expose the Qt painter overlay contract");
    QVERIFY2(profiling.contains(QStringLiteral("vulkan_speaker_label_gpu_text_enabled")),
             "profiling must expose that speaker labels use the Vulkan text pass");
    QVERIFY2(profiling.contains(QStringLiteral("vulkan_transcript_overlay_gpu_text_enabled")),
             "profiling must expose that transcript subtitles use the Vulkan text pass");

    const QString textRenderer = readSourceFile(QStringLiteral("vulkan_text_renderer.cpp"));
    QVERIFY2(!textRenderer.isEmpty(), "vulkan_text_renderer.cpp must be readable");
    QVERIFY2(textRenderer.contains(QStringLiteral("VulkanTextPipeline")),
             "speaker labels must use the dedicated Vulkan text pipeline");
    QVERIFY2(textRenderer.contains(QStringLiteral("drawSpeakerLabel")),
             "speaker labels must be drawn by the Vulkan text renderer");
    QVERIFY2(textRenderer.contains(QStringLiteral("drawTranscriptOverlay")),
             "transcript subtitles must be drawn by the Vulkan text renderer");
    QVERIFY2(!textRenderer.contains(QStringLiteral("QPainter")),
             "Vulkan text renderer must not use Qt painter text rendering");

    QVERIFY2(backend.contains(QStringLiteral("drawTranscriptOverlay(cb")),
             "direct Vulkan preview must route transcript subtitles through the Vulkan text renderer");
    QVERIFY2(!backend.contains(QStringLiteral("renderTranscriptOverlay(")),
             "direct Vulkan preview must not retain a CPU-rendered transcript overlay path");
}

QTEST_MAIN(TestDirectVulkanHandoffPipelineContract)
#include "test_direct_vulkan_handoff_pipeline_contract.moc"
