#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#ifndef JCUT_VULKAN_SHADER_DIR
#define JCUT_VULKAN_SHADER_DIR ""
#endif

inline QString jcutVulkanShaderDirectory()
{
    const QString overridePath = qEnvironmentVariable("JCUT_VULKAN_SHADER_DIR").trimmed();
    if (!overridePath.isEmpty()) {
        return QDir::cleanPath(overridePath);
    }

    const QString applicationDir = QCoreApplication::applicationDirPath();
    if (!applicationDir.isEmpty()) {
        const QStringList packagedCandidates = {
            QDir(applicationDir).absoluteFilePath(QStringLiteral("../share/jcut/shaders")),
            QDir(applicationDir).absoluteFilePath(QStringLiteral("generated/vulkan_shaders")),
            QDir(applicationDir).absoluteFilePath(QStringLiteral("../generated/vulkan_shaders")),
            QDir(applicationDir).absoluteFilePath(QStringLiteral("shaders")),
        };
        for (const QString& candidate : packagedCandidates) {
            if (QFileInfo::exists(QDir(candidate).filePath(QStringLiteral("effects.vert.spv")))) {
                return QDir::cleanPath(candidate);
            }
        }
    }

    return QDir::cleanPath(QStringLiteral(JCUT_VULKAN_SHADER_DIR));
}
