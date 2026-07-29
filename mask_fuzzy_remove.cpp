#include "mask_fuzzy_remove.h"

#include "mask_frame_map_core.h"

#include <QBitArray>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <deque>
#include <filesystem>

namespace editor::masks {
namespace {

struct Component {
    QBitArray pixels;
    qint64 count = 0;
};

QString framePath(const QString& directory, std::int64_t ordinal)
{
    return QDir(directory).filePath(
        QStringLiteral("frame_%1.png").arg(ordinal + 1, 6, 10, QLatin1Char('0')));
}

bool cloneSidecar(const QString& source, const QString& destination, QString* error)
{
    if (!QDir().mkpath(destination)) {
        *error = QStringLiteral("Could not create derived mask directory.");
        return false;
    }
    QDirIterator it(source, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString sourcePath = it.next();
        const QString relative = QDir(source).relativeFilePath(sourcePath);
        const QString destinationPath = QDir(destination).filePath(relative);
        if (!QDir().mkpath(QFileInfo(destinationPath).absolutePath())) {
            *error = QStringLiteral("Could not create derived mask subdirectory.");
            return false;
        }
        std::error_code linkError;
        std::filesystem::create_hard_link(
            std::filesystem::path(sourcePath.toStdString()),
            std::filesystem::path(destinationPath.toStdString()),
            linkError);
        if (linkError && !QFile::copy(sourcePath, destinationPath)) {
            *error = QStringLiteral("Could not copy %1.").arg(relative);
            return false;
        }
    }
    return true;
}

Component componentAt(const QImage& input, int seedX, int seedY)
{
    const QImage image = input.convertToFormat(QImage::Format_Grayscale8);
    Component result{QBitArray(image.width() * image.height(), false), 0};
    if (seedX < 0 || seedY < 0 || seedX >= image.width() || seedY >= image.height() ||
        image.constScanLine(seedY)[seedX] < 128) {
        return result;
    }
    std::deque<int> pending;
    const int seed = seedY * image.width() + seedX;
    result.pixels.setBit(seed);
    pending.push_back(seed);
    while (!pending.empty()) {
        const int index = pending.front();
        pending.pop_front();
        ++result.count;
        const int x = index % image.width();
        const int y = index / image.width();
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if ((dx == 0 && dy == 0) || x + dx < 0 || y + dy < 0 ||
                    x + dx >= image.width() || y + dy >= image.height()) {
                    continue;
                }
                const int next = (y + dy) * image.width() + x + dx;
                if (!result.pixels.testBit(next) &&
                    image.constScanLine(y + dy)[x + dx] >= 128) {
                    result.pixels.setBit(next);
                    pending.push_back(next);
                }
            }
        }
    }
    return result;
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

Component connectedToPrevious(const QImage& input, const QBitArray& previous, int radius)
{
    const QImage image = input.convertToFormat(QImage::Format_Grayscale8);
    const int width = image.width();
    const int height = image.height();
    const QBitArray seeds = dilated(previous, width, height, radius);
    Component result{QBitArray(width * height, false), 0};
    std::deque<int> pending;
    for (int y = 0; y < height; ++y) {
        const uchar* row = image.constScanLine(y);
        for (int x = 0; x < width; ++x) {
            const int index = y * width + x;
            if (seeds.testBit(index) && row[x] >= 128) {
                result.pixels.setBit(index);
                pending.push_back(index);
            }
        }
    }
    while (!pending.empty()) {
        const int index = pending.front();
        pending.pop_front();
        ++result.count;
        const int x = index % width;
        const int y = index / width;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if ((dx == 0 && dy == 0) || x + dx < 0 || y + dy < 0 ||
                    x + dx >= width || y + dy >= height) continue;
                const int next = (y + dy) * width + x + dx;
                if (!result.pixels.testBit(next) &&
                    image.constScanLine(y + dy)[x + dx] >= 128) {
                    result.pixels.setBit(next);
                    pending.push_back(next);
                }
            }
        }
    }
    return result;
}

bool saveRemoved(const QImage& input, const QBitArray& selected, const QString& path)
{
    QImage output = input.convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < output.height(); ++y) {
        uchar* row = output.scanLine(y);
        for (int x = 0; x < output.width(); ++x) {
            if (selected.testBit(y * output.width() + x)) row[x] = 0;
        }
    }
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && output.save(&file, "PNG") && file.commit();
}

} // namespace

FuzzyRemoveResult fuzzyRemoveMaskRegion(const FuzzyRemoveRequest& request)
{
    FuzzyRemoveResult result;
    const auto mapped = jcut::masks::mappedMaskFrameForDecodedSampleCore(
        std::filesystem::path(request.sourceDirectory.toStdString()),
        std::filesystem::path(request.sourceMediaPath.toStdString()),
        request.sourceFrame,
        request.sourcePresentationTimestamp);
    if (!mapped) {
        result.error = QStringLiteral("The presented frame does not resolve to an exact mask sample.");
        return result;
    }
    const QImage seedImage(framePath(request.sourceDirectory, *mapped));
    if (seedImage.isNull()) {
        result.error = QStringLiteral("The selected mask frame could not be loaded.");
        return result;
    }
    const int seedX = std::clamp(qRound(request.xNorm * seedImage.width()), 0, seedImage.width() - 1);
    const int seedY = std::clamp(qRound(request.yNorm * seedImage.height()), 0, seedImage.height() - 1);
    Component seed = componentAt(seedImage, seedX, seedY);
    if (seed.count == 0) {
        result.error = QStringLiteral("The selected point is not inside the mask foreground.");
        return result;
    }

    const QFileInfo sourceInfo(request.sourceDirectory);
    const QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    result.outputDirectory = sourceInfo.dir().filePath(
        sourceInfo.completeBaseName() + QStringLiteral("_fuzzy_remove_") + stamp);
    if (!cloneSidecar(request.sourceDirectory, result.outputDirectory, &result.error)) {
        return result;
    }

    auto removeFrame = [&](std::int64_t ordinal, const QImage& image, const Component& component) {
        if (!saveRemoved(image, component.pixels, framePath(result.outputDirectory, ordinal))) {
            result.error = QStringLiteral("Could not save edited mask frame %1.").arg(ordinal + 1);
            return false;
        }
        ++result.changedFrames;
        result.removedPixels += component.count;
        return true;
    };
    if (!removeFrame(*mapped, seedImage, seed)) return result;

    const auto walk = [&](int direction) {
        Component previous = seed;
        for (int step = 1; step <= std::max(0, request.temporalReachFrames); ++step) {
            const std::int64_t ordinal = *mapped + direction * step;
            if (ordinal < 0) break;
            const QImage image(framePath(request.sourceDirectory, ordinal));
            if (image.isNull() || image.size() != seedImage.size()) break;
            Component selected = connectedToPrevious(
                image, previous.pixels, std::max(0, request.spatialReachPixels));
            if (selected.count == 0 || !removeFrame(ordinal, image, selected)) break;
            previous = std::move(selected);
        }
    };
    walk(-1);
    if (result.error.isEmpty()) walk(1);

    QSaveFile manifest(QDir(result.outputDirectory).filePath(QStringLiteral("jcut_fuzzy_remove.json")));
    if (manifest.open(QIODevice::WriteOnly)) {
        const QJsonObject json{
            {QStringLiteral("schema"), QStringLiteral("jcut_fuzzy_remove_v1")},
            {QStringLiteral("source_sidecar"), QFileInfo(request.sourceDirectory).absoluteFilePath()},
            {QStringLiteral("seed_mask_ordinal"), static_cast<qint64>(*mapped)},
            {QStringLiteral("spatial_reach_pixels"), request.spatialReachPixels},
            {QStringLiteral("temporal_reach_frames"), request.temporalReachFrames},
            {QStringLiteral("changed_frames"), result.changedFrames},
            {QStringLiteral("removed_pixels"), result.removedPixels}};
        manifest.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
        manifest.commit();
    }
    return result;
}

} // namespace editor::masks
