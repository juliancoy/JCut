#pragma once

#include <QSize>
#include <QString>
#include <QStringList>

namespace editor {

struct SpeakerExportHarnessConfig {
    QString statePath;
    QString outputPath;
    QString outputFormat;
    QString clipId;
    QStringList speakerIds;
    QSize outputSize;
    bool outputSizeOverride = false;
    bool useProxyOverride = false;
    bool useProxyMedia = false;
};

int runSpeakerExportHarness(const SpeakerExportHarnessConfig& config);

}  // namespace editor
