#pragma once

#include <QJsonObject>

#include "editor_shared.h"

namespace editor
{

QString effectPresetToJson(ClipEffectPreset preset);
ClipEffectPreset effectPresetFromJson(const QString& value);
QString tilingPatternToJson(ClipTilingPattern pattern);
ClipTilingPattern tilingPatternFromJson(const QString& value);
QJsonObject clipToJson(const TimelineClip &clip);
TimelineClip clipFromJson(const QJsonObject &obj);

} // namespace editor
