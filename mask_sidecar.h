#pragma once

#include <QString>
#include <QVector>

#include <cstdint>

struct TimelineClip;

namespace editor::masks {

struct MaskSidecar {
    QString id;
    QString displayName;
    QString directory;
    QString sourceType = QStringLiteral("sam3_binary_frames");
    int frameCount = 0;
    int64_t firstFrame = -1;
    int64_t lastFrame = -1;

    bool isValid() const { return !id.isEmpty() && !directory.isEmpty() && frameCount > 0; }
};

QString stableMaskSidecarId(const QString& directory);
MaskSidecar inspectMaskSidecar(const QString& directory, const QString& mediaStem = {});
QVector<MaskSidecar> discoverMaskSidecars(const TimelineClip& clip);

} // namespace editor::masks
