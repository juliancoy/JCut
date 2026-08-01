#pragma once

#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace editor::masks {

inline constexpr auto kFuzzyRemoveAlgorithmVersion = "guarded_component_v2";

struct FuzzyRemoveRequest {
    QString sourceDirectory;
    QString sourceMediaPath;
    std::int64_t sourceFrame = -1;
    std::int64_t sourcePresentationTimestamp = -1;
    double xNorm = 0.0;
    double yNorm = 0.0;
    int spatialReachPixels = 12;
    int temporalReachFrames = 120;
    int foregroundThreshold = 128;
    double maximumAreaGrowth = 2.5;
    double minimumAreaRatio = 0.20;
    double maximumFrameFraction = 0.25;
    double ambiguityRatio = 0.80;
};

struct FuzzyRemovePixelRun {
    int y = 0;
    int xBegin = 0;
    int xEnd = 0; // inclusive
};

struct FuzzyRemoveFrameSelection {
    std::int64_t maskOrdinal = -1;
    QVector<FuzzyRemovePixelRun> runs;
    QRect bounds;
    qint64 pixelCount = 0;
};

struct FuzzyRemoveAnalysis {
    FuzzyRemoveRequest request;
    QVector<FuzzyRemoveFrameSelection> frames;
    QImage seedPreview;
    std::int64_t seedMaskOrdinal = -1;
    std::int64_t firstMaskOrdinal = -1;
    std::int64_t lastMaskOrdinal = -1;
    qint64 selectedPixels = 0;
    QString backwardStopReason;
    QString forwardStopReason;
    QString error;
    bool cancelled = false;

    bool succeeded() const { return error.isEmpty() && !cancelled && !frames.isEmpty(); }
};

struct FuzzyRemoveResult {
    QString outputDirectory;
    int changedFrames = 0;
    qint64 removedPixels = 0;
    QString recipeHash;
    QString error;
    bool cancelled = false;
    bool reusedExisting = false;

    bool succeeded() const
    {
        return error.isEmpty() && !cancelled && changedFrames > 0 &&
            !outputDirectory.isEmpty();
    }
};

using FuzzyRemoveCancelFlag = std::shared_ptr<std::atomic_bool>;
using FuzzyRemoveProgress =
    std::function<void(int completedSteps, int totalSteps, const QString& phase)>;

// Read-only analysis. It tracks exactly one component per adjacent frame and
// stops on disappearance, ambiguity, implausible area change, or subject merge.
FuzzyRemoveAnalysis analyzeFuzzyRemoveMaskRegion(
    const FuzzyRemoveRequest& request,
    const FuzzyRemoveCancelFlag& cancel = {},
    const FuzzyRemoveProgress& progress = {});

// Materializes a reviewed analysis into a deterministic copy-on-write cache.
// Publication is atomic: a staging directory is renamed only after every frame
// and the recipe manifest have been written and validated.
FuzzyRemoveResult materializeFuzzyRemoveMaskRegion(
    const FuzzyRemoveAnalysis& analysis,
    const FuzzyRemoveCancelFlag& cancel = {},
    const FuzzyRemoveProgress& progress = {});

// Convenience for non-interactive callers and regression tests.
FuzzyRemoveResult fuzzyRemoveMaskRegion(
    const FuzzyRemoveRequest& request,
    const FuzzyRemoveCancelFlag& cancel = {},
    const FuzzyRemoveProgress& progress = {});

} // namespace editor::masks
