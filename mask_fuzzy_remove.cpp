#include "mask_fuzzy_remove.h"

#include "mask_frame_map_core.h"

#include <QBitArray>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <limits>

namespace editor::masks {
namespace {

struct Component {
    QBitArray pixels;
    qint64 count = 0;
    qint64 overlap = 0;
    QRect bounds;
    double centerX = 0.0;
    double centerY = 0.0;
};

bool cancelled(const FuzzyRemoveCancelFlag& flag)
{
    return flag && flag->load(std::memory_order_relaxed);
}

QString framePath(const QString& directory, std::int64_t ordinal)
{
    return QDir(directory).filePath(
        QStringLiteral("frame_%1.png").arg(ordinal + 1, 6, 10, QLatin1Char('0')));
}

QVector<FuzzyRemovePixelRun> runsForPixels(
    const QBitArray& pixels, int width, int height)
{
    QVector<FuzzyRemovePixelRun> runs;
    for (int y = 0; y < height; ++y) {
        int x = 0;
        while (x < width) {
            while (x < width && !pixels.testBit(y * width + x)) ++x;
            if (x >= width) break;
            const int begin = x;
            while (x + 1 < width && pixels.testBit(y * width + x + 1)) ++x;
            runs.push_back(FuzzyRemovePixelRun{y, begin, x});
            ++x;
        }
    }
    return runs;
}

FuzzyRemoveFrameSelection selectionFor(
    std::int64_t ordinal, const Component& component, int width, int height)
{
    return FuzzyRemoveFrameSelection{
        ordinal,
        runsForPixels(component.pixels, width, height),
        component.bounds,
        component.count};
}

Component floodComponent(
    const QImage& image,
    int seed,
    int threshold,
    QBitArray* visited,
    const QBitArray* overlapMask = nullptr)
{
    const int width = image.width();
    const int height = image.height();
    Component result{QBitArray(width * height, false)};
    if (seed < 0 || seed >= width * height || visited->testBit(seed)) return result;
    const int seedX = seed % width;
    const int seedY = seed / width;
    if (image.constScanLine(seedY)[seedX] < threshold) {
        visited->setBit(seed);
        return result;
    }

    std::deque<int> pending;
    visited->setBit(seed);
    result.pixels.setBit(seed);
    pending.push_back(seed);
    qint64 sumX = 0;
    qint64 sumY = 0;
    int minX = width;
    int minY = height;
    int maxX = -1;
    int maxY = -1;
    while (!pending.empty()) {
        const int index = pending.front();
        pending.pop_front();
        const int x = index % width;
        const int y = index / width;
        ++result.count;
        sumX += x;
        sumY += y;
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
        if (overlapMask && overlapMask->testBit(index)) ++result.overlap;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if ((dx == 0 && dy == 0) || x + dx < 0 || y + dy < 0 ||
                    x + dx >= width || y + dy >= height) {
                    continue;
                }
                const int next = (y + dy) * width + x + dx;
                if (visited->testBit(next)) continue;
                visited->setBit(next);
                if (image.constScanLine(y + dy)[x + dx] >= threshold) {
                    result.pixels.setBit(next);
                    pending.push_back(next);
                }
            }
        }
    }
    if (result.count > 0) {
        result.bounds = QRect(QPoint(minX, minY), QPoint(maxX, maxY));
        result.centerX = static_cast<double>(sumX) / result.count;
        result.centerY = static_cast<double>(sumY) / result.count;
    }
    return result;
}

Component componentAt(const QImage& input, int seedX, int seedY, int threshold)
{
    const QImage image = input.convertToFormat(QImage::Format_Grayscale8);
    QBitArray visited(image.width() * image.height(), false);
    if (seedX < 0 || seedY < 0 || seedX >= image.width() || seedY >= image.height()) {
        return {};
    }
    return floodComponent(
        image, seedY * image.width() + seedX, threshold, &visited);
}

QBitArray dilated(const QBitArray& source, int width, int height, int radius)
{
    QBitArray result(width * height, false);
    QVector<int> integral((width + 1) * (height + 1), 0);
    for (int y = 0; y < height; ++y) {
        int row = 0;
        for (int x = 0; x < width; ++x) {
            row += source.testBit(y * width + x) ? 1 : 0;
            integral[(y + 1) * (width + 1) + x + 1] =
                integral[y * (width + 1) + x + 1] + row;
        }
    }
    for (int y = 0; y < height; ++y) {
        const int top = std::max(0, y - radius);
        const int bottom = std::min(height - 1, y + radius);
        for (int x = 0; x < width; ++x) {
            const int left = std::max(0, x - radius);
            const int right = std::min(width - 1, x + radius);
            const int sum =
                integral[(bottom + 1) * (width + 1) + right + 1] -
                integral[top * (width + 1) + right + 1] -
                integral[(bottom + 1) * (width + 1) + left] +
                integral[top * (width + 1) + left];
            if (sum > 0) result.setBit(y * width + x);
        }
    }
    return result;
}

QVector<Component> overlappingComponents(
    const QImage& input,
    const QBitArray& previous,
    int radius,
    int threshold)
{
    const QImage image = input.convertToFormat(QImage::Format_Grayscale8);
    const int width = image.width();
    const int height = image.height();
    const QBitArray overlapMask = dilated(previous, width, height, radius);
    QBitArray visited(width * height, false);
    QVector<Component> candidates;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int index = y * width + x;
            if (visited.testBit(index) || image.constScanLine(y)[x] < threshold) continue;
            Component component =
                floodComponent(image, index, threshold, &visited, &overlapMask);
            if (component.count > 0 && component.overlap > 0) {
                candidates.push_back(std::move(component));
            }
        }
    }
    return candidates;
}

double candidateScore(const Component& candidate, const Component& previous)
{
    const double overlapCoverage =
        static_cast<double>(candidate.overlap) /
        static_cast<double>(std::max<qint64>(1, std::min(candidate.count, previous.count)));
    const double dx = candidate.centerX - previous.centerX;
    const double dy = candidate.centerY - previous.centerY;
    const double diagonal = std::max(
        1.0, std::hypot(previous.bounds.width(), previous.bounds.height()));
    const double distancePenalty = std::min(1.0, std::hypot(dx, dy) / diagonal);
    return overlapCoverage - (0.20 * distancePenalty);
}

QString validateCandidate(
    const Component& candidate,
    const Component& previous,
    const FuzzyRemoveRequest& request,
    int framePixels)
{
    if (candidate.count <= 0) return QStringLiteral("component disappeared");
    const double frameFraction =
        static_cast<double>(candidate.count) / std::max(1, framePixels);
    if (frameFraction > request.maximumFrameFraction) {
        return QStringLiteral("stopped before a likely subject merge");
    }
    const double ratio =
        static_cast<double>(candidate.count) / std::max<qint64>(1, previous.count);
    if (ratio > request.maximumAreaGrowth) {
        return QStringLiteral("stopped on implausible area growth");
    }
    if (ratio < request.minimumAreaRatio) {
        return QStringLiteral("stopped on implausible area collapse");
    }
    return {};
}

QImage previewImage(const QImage& source, const Component& selected)
{
    QImage preview = source.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < preview.height(); ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(preview.scanLine(y));
        for (int x = 0; x < preview.width(); ++x) {
            const int index = y * preview.width() + x;
            const int gray = qGray(row[x]);
            row[x] = selected.pixels.testBit(index)
                ? qRgba(255, gray / 4, gray / 4, 255)
                : qRgba(gray, gray, gray, 255);
        }
    }
    QPainter painter(&preview);
    painter.setPen(QPen(QColor(255, 220, 80), 2));
    painter.drawRect(selected.bounds.adjusted(-1, -1, 1, 1));
    return preview;
}

QJsonObject requestJson(const FuzzyRemoveAnalysis& analysis)
{
    const FuzzyRemoveRequest& request = analysis.request;
    return QJsonObject{
        {QStringLiteral("algorithm"), QString::fromLatin1(kFuzzyRemoveAlgorithmVersion)},
        {QStringLiteral("source_sidecar"), QFileInfo(request.sourceDirectory).absoluteFilePath()},
        {QStringLiteral("source_media"), QFileInfo(request.sourceMediaPath).absoluteFilePath()},
        {QStringLiteral("source_frame"), static_cast<qint64>(request.sourceFrame)},
        {QStringLiteral("source_presentation_timestamp"),
         static_cast<qint64>(request.sourcePresentationTimestamp)},
        {QStringLiteral("seed_mask_ordinal"), static_cast<qint64>(analysis.seedMaskOrdinal)},
        {QStringLiteral("x_norm"), request.xNorm},
        {QStringLiteral("y_norm"), request.yNorm},
        {QStringLiteral("spatial_reach_pixels"), request.spatialReachPixels},
        {QStringLiteral("temporal_reach_frames"), request.temporalReachFrames},
        {QStringLiteral("foreground_threshold"), request.foregroundThreshold},
        {QStringLiteral("maximum_area_growth"), request.maximumAreaGrowth},
        {QStringLiteral("minimum_area_ratio"), request.minimumAreaRatio},
        {QStringLiteral("maximum_frame_fraction"), request.maximumFrameFraction},
        {QStringLiteral("ambiguity_ratio"), request.ambiguityRatio},
        {QStringLiteral("first_mask_ordinal"), static_cast<qint64>(analysis.firstMaskOrdinal)},
        {QStringLiteral("last_mask_ordinal"), static_cast<qint64>(analysis.lastMaskOrdinal)},
        {QStringLiteral("changed_frames"), analysis.frames.size()},
        {QStringLiteral("removed_pixels"), analysis.selectedPixels},
        {QStringLiteral("backward_stop_reason"), analysis.backwardStopReason},
        {QStringLiteral("forward_stop_reason"), analysis.forwardStopReason}};
}

QString sourceFingerprint(const QString& directory)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QFileInfo(directory).canonicalFilePath().toUtf8());
    QDirIterator it(
        directory,
        QStringList{QStringLiteral("frame_*.png"),
                    QStringLiteral("jcut_*.json"),
                    QStringLiteral("jcut_*.tsv")},
        QDir::Files,
        QDirIterator::Subdirectories);
    QStringList records;
    while (it.hasNext()) {
        const QFileInfo info(it.next());
        records.push_back(
            QDir(directory).relativeFilePath(info.absoluteFilePath()) +
            QLatin1Char('|') + QString::number(info.size()) +
            QLatin1Char('|') + QString::number(info.lastModified().toMSecsSinceEpoch()));
    }
    std::sort(records.begin(), records.end());
    for (const QString& record : records) {
        hash.addData(record.toUtf8());
        hash.addData("\n");
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString recipeHash(const FuzzyRemoveAnalysis& analysis)
{
    QJsonObject recipe = requestJson(analysis);
    recipe[QStringLiteral("source_fingerprint")] =
        sourceFingerprint(analysis.request.sourceDirectory);
    const QByteArray bytes = QJsonDocument(recipe).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool cloneSidecar(
    const QString& source,
    const QString& destination,
    const FuzzyRemoveCancelFlag& cancel,
    const FuzzyRemoveProgress& progress,
    int* completed,
    int total,
    QString* error)
{
    if (!QDir().mkpath(destination)) {
        *error = QStringLiteral("Could not create mask-edit staging directory.");
        return false;
    }
    QDirIterator it(source, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (cancelled(cancel)) return false;
        const QString sourcePath = it.next();
        const QString relative = QDir(source).relativeFilePath(sourcePath);
        const QString destinationPath = QDir(destination).filePath(relative);
        if (!QDir().mkpath(QFileInfo(destinationPath).absolutePath())) {
            *error = QStringLiteral("Could not create mask-edit staging subdirectory.");
            return false;
        }
        if (!QFile::copy(sourcePath, destinationPath)) {
            *error = QStringLiteral("Could not stage %1.").arg(relative);
            return false;
        }
        ++*completed;
        if (progress) progress(*completed, total, QStringLiteral("Staging sidecar"));
    }
    return true;
}

bool saveRemoved(
    const QImage& input,
    const FuzzyRemoveFrameSelection& selected,
    const QString& path)
{
    QImage output = input.convertToFormat(QImage::Format_Grayscale8);
    for (const FuzzyRemovePixelRun& run : selected.runs) {
        if (run.y < 0 || run.y >= output.height()) return false;
        uchar* row = output.scanLine(run.y);
        const int begin = std::clamp(run.xBegin, 0, output.width() - 1);
        const int end = std::clamp(run.xEnd, 0, output.width() - 1);
        if (end < begin) return false;
        std::fill(row + begin, row + end + 1, static_cast<uchar>(0));
    }
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) &&
        output.save(&file, "PNG") &&
        file.commit();
}

bool removeStagingDirectory(const QString& path)
{
    const QFileInfo info(path);
    return !info.exists() ||
        (info.isDir() && info.fileName().contains(QStringLiteral(".partial_")) &&
         QDir(path).removeRecursively());
}

bool derivedCacheContainsSourceFiles(
    const QString& sourceDirectory,
    const QString& derivedDirectory)
{
    QDirIterator sourceFiles(
        sourceDirectory, QDir::Files, QDirIterator::Subdirectories);
    while (sourceFiles.hasNext()) {
        const QString sourcePath = sourceFiles.next();
        const QString relative =
            QDir(sourceDirectory).relativeFilePath(sourcePath);
        if (!QFileInfo(QDir(derivedDirectory).filePath(relative)).isFile()) {
            return false;
        }
    }
    return true;
}

} // namespace

FuzzyRemoveAnalysis analyzeFuzzyRemoveMaskRegion(
    const FuzzyRemoveRequest& request,
    const FuzzyRemoveCancelFlag& cancel,
    const FuzzyRemoveProgress& progress)
{
    FuzzyRemoveAnalysis analysis;
    analysis.request = request;
    const int temporalLimit = std::max(0, request.temporalReachFrames);
    const int progressTotal = std::max(1, temporalLimit * 2);
    if (progress) {
        progress(0, progressTotal, QStringLiteral("Analyzing mask continuity"));
    }
    const auto mapped = jcut::masks::mappedMaskFrameForDecodedSampleCore(
        std::filesystem::path(request.sourceDirectory.toStdString()),
        std::filesystem::path(request.sourceMediaPath.toStdString()),
        request.sourceFrame,
        request.sourcePresentationTimestamp);
    if (!mapped) {
        analysis.error =
            QStringLiteral("The presented frame does not resolve to an exact mask sample.");
        return analysis;
    }
    analysis.seedMaskOrdinal = *mapped;
    const QImage seedImage(framePath(request.sourceDirectory, *mapped));
    if (seedImage.isNull()) {
        analysis.error = QStringLiteral("The selected mask frame could not be loaded.");
        return analysis;
    }
    const int seedX = std::clamp(
        qRound(request.xNorm * seedImage.width()), 0, seedImage.width() - 1);
    const int seedY = std::clamp(
        qRound(request.yNorm * seedImage.height()), 0, seedImage.height() - 1);
    Component seed = componentAt(
        seedImage, seedX, seedY, std::clamp(request.foregroundThreshold, 1, 255));
    if (seed.count == 0) {
        analysis.error = QStringLiteral("The selected point is not inside mask foreground.");
        return analysis;
    }
    if (static_cast<double>(seed.count) /
            std::max(1, seedImage.width() * seedImage.height()) >
        request.maximumFrameFraction) {
        analysis.error =
            QStringLiteral("The selected component is too large for safe automatic removal.");
        return analysis;
    }

    analysis.seedPreview = previewImage(seedImage, seed);
    analysis.frames.push_back(selectionFor(
        *mapped, seed, seedImage.width(), seedImage.height()));
    analysis.selectedPixels = seed.count;

    auto walk = [&](int direction, QString* stopReason) {
        Component previous = seed;
        const int progressBase = direction < 0 ? 0 : temporalLimit;
        for (int step = 1; step <= temporalLimit; ++step) {
            if (cancelled(cancel)) {
                analysis.cancelled = true;
                *stopReason = QStringLiteral("cancelled");
                return;
            }
            if (progress) {
                progress(progressBase + step,
                         progressTotal,
                         QStringLiteral("Analyzing mask continuity"));
            }
            const std::int64_t ordinal = *mapped + direction * step;
            if (ordinal < 0) {
                *stopReason = QStringLiteral("reached start of sidecar");
                return;
            }
            const QImage image(framePath(request.sourceDirectory, ordinal));
            if (image.isNull()) {
                *stopReason = QStringLiteral("reached end of sidecar");
                return;
            }
            if (image.size() != seedImage.size()) {
                *stopReason = QStringLiteral("stopped on mask-size change");
                return;
            }
            QVector<Component> candidates = overlappingComponents(
                image,
                previous.pixels,
                std::max(0, request.spatialReachPixels),
                std::clamp(request.foregroundThreshold, 1, 255));
            if (candidates.isEmpty()) {
                *stopReason = QStringLiteral("component disappeared");
                return;
            }
            std::sort(candidates.begin(), candidates.end(),
                      [&previous](const Component& left, const Component& right) {
                return candidateScore(left, previous) > candidateScore(right, previous);
            });
            const double bestScore = candidateScore(candidates.front(), previous);
            if (candidates.size() > 1 &&
                candidateScore(candidates.at(1), previous) >=
                    bestScore * request.ambiguityRatio) {
                *stopReason = QStringLiteral("stopped on ambiguous component match");
                return;
            }
            const QString invalid = validateCandidate(
                candidates.front(),
                previous,
                request,
                image.width() * image.height());
            if (!invalid.isEmpty()) {
                *stopReason = invalid;
                return;
            }
            Component selected = std::move(candidates.front());
            FuzzyRemoveFrameSelection selection =
                selectionFor(ordinal, selected, image.width(), image.height());
            analysis.selectedPixels += selection.pixelCount;
            if (direction < 0) {
                analysis.frames.prepend(std::move(selection));
            } else {
                analysis.frames.push_back(std::move(selection));
            }
            previous = std::move(selected);
        }
        *stopReason = QStringLiteral("reached temporal limit");
    };
    walk(-1, &analysis.backwardStopReason);
    if (!analysis.cancelled) walk(1, &analysis.forwardStopReason);
    if (analysis.cancelled) return analysis;
    analysis.firstMaskOrdinal = analysis.frames.constFirst().maskOrdinal;
    analysis.lastMaskOrdinal = analysis.frames.constLast().maskOrdinal;
    return analysis;
}

FuzzyRemoveResult materializeFuzzyRemoveMaskRegion(
    const FuzzyRemoveAnalysis& analysis,
    const FuzzyRemoveCancelFlag& cancel,
    const FuzzyRemoveProgress& progress)
{
    FuzzyRemoveResult result;
    if (!analysis.succeeded()) {
        result.error = analysis.error.isEmpty()
            ? QStringLiteral("Mask selection analysis is not materializable.")
            : analysis.error;
        result.cancelled = analysis.cancelled;
        return result;
    }
    result.recipeHash = recipeHash(analysis);
    const QFileInfo sourceInfo(analysis.request.sourceDirectory);
    result.outputDirectory = sourceInfo.dir().filePath(
        sourceInfo.completeBaseName() +
        QStringLiteral("_fuzzy_remove_") +
        result.recipeHash.left(16));
    const QString manifestPath =
        QDir(result.outputDirectory).filePath(QStringLiteral("jcut_fuzzy_remove.json"));
    if (QFileInfo::exists(manifestPath)) {
        QFile manifest(manifestPath);
        if (manifest.open(QIODevice::ReadOnly)) {
            const QJsonObject existing =
                QJsonDocument::fromJson(manifest.readAll()).object();
            if (existing.value(QStringLiteral("recipe_hash")).toString() ==
                result.recipeHash) {
                if (!derivedCacheContainsSourceFiles(
                        analysis.request.sourceDirectory,
                        result.outputDirectory)) {
                    result.error =
                        QStringLiteral("The derived mask cache is incomplete.");
                    return result;
                }
                for (const FuzzyRemoveFrameSelection& selection : analysis.frames) {
                    const QImage cached(framePath(
                        result.outputDirectory, selection.maskOrdinal));
                    if (cached.isNull()) {
                        result.error =
                            QStringLiteral("The derived mask cache is incomplete.");
                        return result;
                    }
                }
                result.changedFrames = analysis.frames.size();
                result.removedPixels = analysis.selectedPixels;
                result.reusedExisting = true;
                return result;
            }
        }
        result.error = QStringLiteral("A conflicting derived mask cache already exists.");
        return result;
    }

    int sourceFileCount = 0;
    QDirIterator countIt(
        analysis.request.sourceDirectory, QDir::Files, QDirIterator::Subdirectories);
    while (countIt.hasNext()) {
        countIt.next();
        ++sourceFileCount;
    }
    const int totalSteps = sourceFileCount + analysis.frames.size() + 2;
    int completed = 0;
    const QString staging =
        result.outputDirectory + QStringLiteral(".partial_") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto fail = [&](const QString& error, bool wasCancelled = false) {
        result.error = error;
        result.cancelled = wasCancelled;
        removeStagingDirectory(staging);
        return result;
    };
    if (!cloneSidecar(
            analysis.request.sourceDirectory,
            staging,
            cancel,
            progress,
            &completed,
            totalSteps,
            &result.error)) {
        return fail(
            cancelled(cancel) ? QStringLiteral("Mask removal cancelled.") : result.error,
            cancelled(cancel));
    }
    for (const FuzzyRemoveFrameSelection& selection : analysis.frames) {
        if (cancelled(cancel)) {
            return fail(QStringLiteral("Mask removal cancelled."), true);
        }
        const QImage source(framePath(analysis.request.sourceDirectory, selection.maskOrdinal));
        const QString destination = framePath(staging, selection.maskOrdinal);
        if (source.isNull() || !saveRemoved(source, selection, destination)) {
            return fail(QStringLiteral("Could not publish edited mask frame %1.")
                            .arg(selection.maskOrdinal + 1));
        }
        const QImage verify(destination);
        if (verify.isNull() || verify.size() != source.size()) {
            return fail(QStringLiteral("Edited mask frame validation failed."));
        }
        ++completed;
        if (progress) progress(completed, totalSteps, QStringLiteral("Writing mask edits"));
    }

    QJsonObject manifestObject = requestJson(analysis);
    manifestObject[QStringLiteral("schema")] = QStringLiteral("jcut_fuzzy_remove_v2");
    manifestObject[QStringLiteral("recipe_hash")] = result.recipeHash;
    manifestObject[QStringLiteral("source_fingerprint")] =
        sourceFingerprint(analysis.request.sourceDirectory);
    QSaveFile manifest(QDir(staging).filePath(QStringLiteral("jcut_fuzzy_remove.json")));
    if (!manifest.open(QIODevice::WriteOnly) ||
        manifest.write(QJsonDocument(manifestObject).toJson(QJsonDocument::Indented)) < 0 ||
        !manifest.commit()) {
        return fail(QStringLiteral("Could not publish the mask edit recipe."));
    }
    ++completed;
    if (progress) progress(completed, totalSteps, QStringLiteral("Validating sidecar"));

    if (!QDir().rename(staging, result.outputDirectory)) {
        return fail(QStringLiteral("Could not atomically publish the derived mask sidecar."));
    }
    ++completed;
    if (progress) progress(completed, totalSteps, QStringLiteral("Complete"));
    result.changedFrames = analysis.frames.size();
    result.removedPixels = analysis.selectedPixels;
    return result;
}

FuzzyRemoveResult fuzzyRemoveMaskRegion(
    const FuzzyRemoveRequest& request,
    const FuzzyRemoveCancelFlag& cancel,
    const FuzzyRemoveProgress& progress)
{
    const FuzzyRemoveAnalysis analysis =
        analyzeFuzzyRemoveMaskRegion(request, cancel, progress);
    return materializeFuzzyRemoveMaskRegion(analysis, cancel, progress);
}

} // namespace editor::masks
