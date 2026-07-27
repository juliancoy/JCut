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

struct OfflineExportHarnessConfig {
    QString statePath;
    QString outputPath;
    QString outputFormat;
    int64_t startFrame = 0;
    int64_t endFrame = 0;
    QSize outputSize;
    bool outputSizeOverride = false;
    bool gpuExportPreviewEnabled = false;
    bool createImageSequence = false;
    int64_t previousRangeEndFrame = -1;
};

int runOfflineExportHarness(const OfflineExportHarnessConfig& config);

}  // namespace editor
