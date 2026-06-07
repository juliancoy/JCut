#include "vulkan_facedetections_offscreen_progress.h"

#include <QChar>
#include <QString>
#include <QtGlobal>

#include <cmath>
#include <iomanip>
#include <iostream>

QString formatDuration(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0.0) {
    return QStringLiteral("--:--");
  }
  const int total = qMax(0, static_cast<int>(std::round(seconds)));
  const int hours = total / 3600;
  const int minutes = (total % 3600) / 60;
  const int secs = total % 60;
  if (hours > 0) {
    return QStringLiteral("%1:%2:%3")
        .arg(hours)
        .arg(minutes, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'));
  }
  return QStringLiteral("%1:%2")
      .arg(minutes, 2, 10, QChar('0'))
      .arg(secs, 2, 10, QChar('0'));
}

void renderProgressLine(int frameOffset, int totalFrames, int frameNumber,
                        int processed, int totalDetections,
                        int currentFrameDetections, double elapsedSec,
                        double avgDetectMs, AdaptiveEtaTracker *etaTracker) {
  const int current = qBound(0, frameOffset + 1, qMax(1, totalFrames));
  const double ratio = qBound(0.0,
                              static_cast<double>(current) /
                                  static_cast<double>(qMax(1, totalFrames)),
                              1.0);
  constexpr int barWidth = 32;
  const int filled =
      qBound(0, static_cast<int>(std::round(ratio * barWidth)), barWidth);
  std::string bar;
  bar.reserve(barWidth);
  for (int i = 0; i < barWidth; ++i) {
    bar.push_back(i < filled ? '#' : '-');
  }
  if (etaTracker) {
    etaTracker->observe(current, processed, elapsedSec);
  }
  const AdaptiveEtaEstimate eta =
      etaTracker
          ? etaTracker->estimate(totalFrames, current, processed, elapsedSec)
          : AdaptiveEtaEstimate{};
  const double decodedFps =
      eta.decodeFps > 0.0
          ? eta.decodeFps
          : (elapsedSec > 0.0 ? static_cast<double>(current) / elapsedSec
                              : 0.0);
  const double processedFps =
      eta.processedFps > 0.0
          ? eta.processedFps
          : (elapsedSec > 0.0 ? static_cast<double>(processed) / elapsedSec
                              : 0.0);

  std::cout << '\r' << '[' << bar << "] " << std::fixed << std::setprecision(1)
            << (ratio * 100.0) << "% " << current << '/' << totalFrames
            << " frame=" << frameNumber << " processed=" << processed
            << " frame_det=" << currentFrameDetections
            << " det=" << totalDetections << " fps=" << std::setprecision(1)
            << decodedFps << " proc/s=" << std::setprecision(2) << processedFps
            << " infer=" << std::setprecision(2) << avgDetectMs << "ms"
            << " eta=" << formatDuration(eta.etaSec).toStdString() << "    "
            << std::flush;
}

bool shouldRenderProgress(
    int frameOffset, int totalFrames, int processed, int *lastRenderedPercent,
    std::chrono::steady_clock::time_point *lastRenderedAt) {
  if (!lastRenderedPercent || !lastRenderedAt) {
    return true;
  }
  const int current = qBound(0, frameOffset + 1, qMax(1, totalFrames));
  if (current >= totalFrames) {
    return true;
  }
  if (processed <= 1 && *lastRenderedPercent < 0) {
    return true;
  }
  const int percent =
      static_cast<int>(std::floor((100.0 * static_cast<double>(current)) /
                                  static_cast<double>(qMax(1, totalFrames))));
  const auto now = std::chrono::steady_clock::now();
  const double elapsedSec =
      std::chrono::duration<double>(now - *lastRenderedAt).count();
  if (percent >= *lastRenderedPercent + 5 || elapsedSec >= 2.0) {
    *lastRenderedPercent = percent;
    *lastRenderedAt = now;
    return true;
  }
  return false;
}
