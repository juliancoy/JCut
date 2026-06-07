#pragma once

#include "cpu_overlay_render_backend.h"

#include <QSize>
#include <QString>

namespace jcut::direct_vulkan_preview {

QString playbackStatusOverlayTextureKey(const QSize& imageSize, const QString& text, qreal progress);
render_detail::OverlayImage renderPlaybackStatusOverlay(const QSize& imageSize, const QString& text, qreal progress);

} // namespace jcut::direct_vulkan_preview
