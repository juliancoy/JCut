#include <QtTest/QtTest>

#include "../render_internal.h"

class TestEditorRuntimeVulkanExportBackend : public QObject {
    Q_OBJECT

private slots:
    void linksRealVulkanExportBackend();
};

void TestEditorRuntimeVulkanExportBackend::linksRealVulkanExportBackend()
{
    render_detail::OffscreenVulkanRenderer renderer;
    const QString backend = renderer.backendId();
    QCOMPARE(backend, QStringLiteral("vulkan"));
    QVERIFY2(backend != QStringLiteral("vulkan_stub"),
             qPrintable(QStringLiteral("jcut_editor_runtime linked the export stub: %1").arg(backend)));
}

QTEST_MAIN(TestEditorRuntimeVulkanExportBackend)

#include "test_editor_runtime_vulkan_export_backend.moc"
