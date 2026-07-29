#pragma once

#include <QString>

#include <cstdint>

namespace editor::masks {

struct FuzzyRemoveRequest {
    QString sourceDirectory;
    QString sourceMediaPath;
    std::int64_t sourceFrame = -1;
    std::int64_t sourcePresentationTimestamp = -1;
    double xNorm = 0.0;
    double yNorm = 0.0;
    int spatialReachPixels = 12;
    int temporalReachFrames = 120;
};

struct FuzzyRemoveResult {
    QString outputDirectory;
    int changedFrames = 0;
    qint64 removedPixels = 0;
    QString error;

    bool succeeded() const { return error.isEmpty() && changedFrames > 0; }
};

// Creates a copy-on-write derived sidecar and removes the foreground component
// selected at the exact presented sample. The original sidecar is never edited.
FuzzyRemoveResult fuzzyRemoveMaskRegion(const FuzzyRemoveRequest& request);

} // namespace editor::masks
