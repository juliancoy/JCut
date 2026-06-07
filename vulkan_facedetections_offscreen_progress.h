#pragma once

#include <QString>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>

struct AdaptiveEtaSample {
  int current = 0;
  int processed = 0;
  double elapsedSec = 0.0;
};

struct AdaptiveEtaEstimate {
  double decodeFps = 0.0;
  double processedFps = 0.0;
  double etaSec = -1.0;
};

class AdaptiveEtaTracker {
public:
  void observe(int current, int processed, double elapsedSec) {
    if (elapsedSec <= 0.0 || !std::isfinite(elapsedSec) || current < 0 ||
        processed < 0) {
      return;
    }
    if (!m_samples.empty()) {
      const AdaptiveEtaSample &last = m_samples.back();
      if (current == last.current && processed == last.processed) {
        return;
      }
      if (elapsedSec <= last.elapsedSec) {
        return;
      }
    }

    m_samples.push_back(AdaptiveEtaSample{current, processed, elapsedSec});
    while (m_samples.size() > 2 &&
           (elapsedSec - m_samples.front().elapsedSec) > kRecentWindowSec) {
      m_samples.pop_front();
    }

    const AdaptiveEtaEstimate recent = recentEstimate();
    if (recent.decodeFps > 0.0) {
      m_smoothedDecodeFps = smooth(m_smoothedDecodeFps, recent.decodeFps);
    }
    if (recent.processedFps > 0.0) {
      m_smoothedProcessedFps =
          smooth(m_smoothedProcessedFps, recent.processedFps);
    }
  }

  AdaptiveEtaEstimate estimate(int totalFrames, int current, int processed,
                               double elapsedSec) const {
    AdaptiveEtaEstimate result;
    if (elapsedSec <= 0.0 || !std::isfinite(elapsedSec)) {
      return result;
    }

    const double avgDecodeFps =
        current > 0 ? static_cast<double>(current) / elapsedSec : 0.0;
    const double avgProcessedFps =
        processed > 0 ? static_cast<double>(processed) / elapsedSec : 0.0;
    const AdaptiveEtaEstimate recent = recentEstimate();

    result.decodeFps = selectRate(avgDecodeFps, recent.decodeFps,
                                  m_smoothedDecodeFps, current);
    result.processedFps = selectRate(avgProcessedFps, recent.processedFps,
                                     m_smoothedProcessedFps, processed);
    if (result.decodeFps > 0.0) {
      result.etaSec = static_cast<double>(qMax(0, totalFrames - current)) /
                      result.decodeFps;
    }
    return result;
  }

private:
  static constexpr double kRecentWindowSec = 20.0;
  static constexpr double kMinRecentWindowSec = 2.0;
  static constexpr int kWarmupFrames = 180;

  AdaptiveEtaEstimate recentEstimate() const {
    AdaptiveEtaEstimate result;
    if (m_samples.size() < 2) {
      return result;
    }
    const AdaptiveEtaSample &first = m_samples.front();
    const AdaptiveEtaSample &last = m_samples.back();
    const double dt = last.elapsedSec - first.elapsedSec;
    if (dt < kMinRecentWindowSec) {
      return result;
    }
    const int decodedDelta = last.current - first.current;
    const int processedDelta = last.processed - first.processed;
    if (decodedDelta > 0) {
      result.decodeFps = static_cast<double>(decodedDelta) / dt;
    }
    if (processedDelta > 0) {
      result.processedFps = static_cast<double>(processedDelta) / dt;
    }
    return result;
  }

  static double smooth(double prior, double current) {
    if (prior <= 0.0) {
      return current;
    }
    constexpr double alpha = 0.25;
    return (alpha * current) + ((1.0 - alpha) * prior);
  }

  static double selectRate(double averageRate, double recentRate,
                           double smoothedRate, int completedUnits) {
    const double adaptiveRate =
        smoothedRate > 0.0 ? smoothedRate
                           : (recentRate > 0.0 ? recentRate : averageRate);
    if (completedUnits <= 0) {
      return adaptiveRate;
    }
    if (averageRate <= 0.0) {
      return adaptiveRate;
    }
    const double warmup = std::clamp(static_cast<double>(completedUnits) /
                                         static_cast<double>(kWarmupFrames),
                                     0.0, 1.0);
    if (adaptiveRate <= 0.0) {
      return averageRate;
    }
    return ((1.0 - warmup) * averageRate) + (warmup * adaptiveRate);
  }

  std::deque<AdaptiveEtaSample> m_samples;
  double m_smoothedDecodeFps = 0.0;
  double m_smoothedProcessedFps = 0.0;
};

QString formatDuration(double seconds);
void renderProgressLine(int frameOffset, int totalFrames, int frameNumber,
                        int processed, int totalDetections,
                        int currentFrameDetections, double elapsedSec,
                        double avgDetectMs, AdaptiveEtaTracker *etaTracker);
bool shouldRenderProgress(
    int frameOffset, int totalFrames, int processed, int *lastRenderedPercent,
    std::chrono::steady_clock::time_point *lastRenderedAt);
