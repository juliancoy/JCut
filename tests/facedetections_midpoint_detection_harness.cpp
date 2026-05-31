#include "../decoder_context.h"
#include "../detector_settings.h"
#include "../json_io_utils.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QPen>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <algorithm>
#include <iostream>
#include <vector>

namespace {

struct Options {
    QString videoPath;
    QString paramsFile;
    QString outputDir;
    QString detector = QStringLiteral("scrfd-ncnn-vulkan");
    bool previewWindow = false;
    bool previewFiles = false;
    bool quiet = false;
    int logInterval = 1;
    int benchmarkFrames = 1;
    int warmupFrames = 0;
    int repeat = 1;
    QStringList passthroughArgs;
};

struct RunSummary {
    int processedFrames = 0;
    int totalDetections = 0;
    int trackCount = 0;
    QString backend;
    QString scrfdModelVariant;
    double avgDetectionMs = 0.0;
    double avgUploadMs = 0.0;
    double avgNcnnInputMs = 0.0;
    double avgNcnnExtractMs = 0.0;
    double avgNcnnExtractLevel8Ms = 0.0;
    double avgNcnnExtractLevel16Ms = 0.0;
    double avgNcnnExtractLevel32Ms = 0.0;
    double avgNcnnPostMs = 0.0;
    double avgNcnnTotalMs = 0.0;
    double wallSec = 0.0;
};

void printUsage()
{
    std::cout
        << "Usage: facedetections_midpoint_detection_harness <video-path> [options]\n"
        << "Options:\n"
        << "  --params-file PATH    Detector params file passed through to the runner.\n"
        << "  --out-dir DIR         Output directory. Defaults to /tmp/jcut_midpoint_detection_harness.\n"
        << "  --detector NAME       Detector name. Default: scrfd-ncnn-vulkan\n"
        << "  --preview-window      Show preview window.\n"
        << "  --preview-files       Write preview files.\n"
        << "  --quiet               Pass --quiet to the runner.\n"
        << "  --log-interval N      Runner log interval. Default: 1\n"
        << "  --benchmark-frames N  Number of sampled frames to benchmark from midpoint. Default: 1\n"
        << "  --warmup-frames N     Sampled frames to warm up before measurement. Default: 0\n"
        << "  --repeat N            Repeat the benchmark N times and report median. Default: 1\n"
        << "  --                    Remaining args are passed through to jcut_vulkan_facedetections_offscreen.\n";
}

bool parseArgs(const QStringList& args, Options* options)
{
    if (!options) {
        return false;
    }
    bool passthroughMode = false;
    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args.at(i);
        if (passthroughMode) {
            options->passthroughArgs.push_back(arg);
            continue;
        }
        if (arg == QStringLiteral("--")) {
            passthroughMode = true;
            continue;
        }
        if (arg == QStringLiteral("--params-file")) {
            if (i + 1 >= args.size()) return false;
            options->paramsFile = args.at(++i);
            continue;
        }
        if (arg == QStringLiteral("--out-dir")) {
            if (i + 1 >= args.size()) return false;
            options->outputDir = args.at(++i);
            continue;
        }
        if (arg == QStringLiteral("--detector")) {
            if (i + 1 >= args.size()) return false;
            options->detector = args.at(++i);
            continue;
        }
        if (arg == QStringLiteral("--preview-window")) {
            options->previewWindow = true;
            continue;
        }
        if (arg == QStringLiteral("--preview-files")) {
            options->previewFiles = true;
            continue;
        }
        if (arg == QStringLiteral("--quiet")) {
            options->quiet = true;
            continue;
        }
        if (arg == QStringLiteral("--log-interval")) {
            if (i + 1 >= args.size()) return false;
            options->logInterval = qMax(1, args.at(++i).toInt());
            continue;
        }
        if (arg == QStringLiteral("--benchmark-frames")) {
            if (i + 1 >= args.size()) return false;
            options->benchmarkFrames = qMax(1, args.at(++i).toInt());
            continue;
        }
        if (arg == QStringLiteral("--warmup-frames")) {
            if (i + 1 >= args.size()) return false;
            options->warmupFrames = qMax(0, args.at(++i).toInt());
            continue;
        }
        if (arg == QStringLiteral("--repeat")) {
            if (i + 1 >= args.size()) return false;
            options->repeat = qMax(1, args.at(++i).toInt());
            continue;
        }
        if (options->videoPath.isEmpty() && !arg.startsWith(QLatin1String("--"))) {
            options->videoPath = arg;
            continue;
        }
        options->passthroughArgs.push_back(arg);
    }
    return !options->videoPath.trimmed().isEmpty();
}

QString defaultOutputDir()
{
    return QDir::temp().filePath(QStringLiteral("jcut_midpoint_detection_harness"));
}

QString runnerPath()
{
    return QDir(QStringLiteral(JCUT_BINARY_DIR))
        .filePath(QStringLiteral("jcut_vulkan_facedetections_offscreen"));
}

int effectiveStrideForRun(const Options& options, const QString& videoPath)
{
    jcut::facedetections::DetectorRuntimeSettings settings;
    if (!options.paramsFile.trimmed().isEmpty()) {
        jcut::facedetections::loadDetectorRuntimeSettingsFile(options.paramsFile, &settings, nullptr);
    } else {
        const QString detectorSettingsPath =
            jcut::facedetections::detectorSettingsPathForVideo(videoPath);
        jcut::facedetections::loadDetectorRuntimeSettingsFile(detectorSettingsPath, &settings, nullptr);
    }
    return qMax(1, settings.stride);
}

QImage decodeFrameImage(const QString& videoPath, qint64 frameNumber)
{
    editor::DecoderContext decoder(videoPath);
    if (!decoder.initialize()) {
        return QImage();
    }
    const editor::FrameHandle frame = decoder.decodeFrame(frameNumber);
    decoder.shutdown();
    if (frame.isNull() || !frame.hasCpuImage()) {
        return QImage();
    }
    return frame.cpuImage();
}

QString writePng(const QImage& image, const QString& path)
{
    return !image.isNull() && image.save(path) ? path : QString{};
}

bool loadSummary(const QString& path, RunSummary* summaryOut)
{
    if (summaryOut) {
        *summaryOut = RunSummary{};
    }
    QJsonObject object;
    if (!jcut::jsonio::readJsonFile(path, &object, nullptr)) {
        return false;
    }
    if (summaryOut) {
        summaryOut->processedFrames = object.value(QStringLiteral("processed_frames")).toInt(0);
        summaryOut->totalDetections = object.value(QStringLiteral("total_detections")).toInt(0);
        summaryOut->trackCount = object.value(QStringLiteral("track_count")).toInt(0);
        summaryOut->backend = object.value(QStringLiteral("backend")).toString();
        summaryOut->scrfdModelVariant = object.value(QStringLiteral("scrfd_model_variant")).toString();
        summaryOut->avgDetectionMs = object.value(QStringLiteral("avg_vulkan_zero_copy_detection_ms")).toDouble(0.0);
        summaryOut->avgUploadMs = object.value(QStringLiteral("avg_decoder_vulkan_upload_ms")).toDouble(0.0);
        summaryOut->avgNcnnInputMs = object.value(QStringLiteral("avg_ncnn_input_ms")).toDouble(0.0);
        summaryOut->avgNcnnExtractMs = object.value(QStringLiteral("avg_ncnn_extract_ms")).toDouble(0.0);
        summaryOut->avgNcnnExtractLevel8Ms = object.value(QStringLiteral("avg_ncnn_extract_level8_ms")).toDouble(0.0);
        summaryOut->avgNcnnExtractLevel16Ms = object.value(QStringLiteral("avg_ncnn_extract_level16_ms")).toDouble(0.0);
        summaryOut->avgNcnnExtractLevel32Ms = object.value(QStringLiteral("avg_ncnn_extract_level32_ms")).toDouble(0.0);
        summaryOut->avgNcnnPostMs = object.value(QStringLiteral("avg_ncnn_post_ms")).toDouble(0.0);
        summaryOut->avgNcnnTotalMs = object.value(QStringLiteral("avg_ncnn_total_ms")).toDouble(0.0);
        summaryOut->wallSec = object.value(QStringLiteral("wall_sec")).toDouble(0.0);
    }
    return true;
}

bool loadFrameRecord(const QString& path, qint64 frameNumber, QJsonObject* recordOut)
{
    if (recordOut) {
        *recordOut = QJsonObject{};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    while (!stream.atEnd()) {
        quint32 magic = 0;
        quint32 version = 0;
        quint32 compressedSize = 0;
        stream >> magic;
        stream >> version;
        stream >> compressedSize;
        if (stream.status() != QDataStream::Ok || magic != 0x4A465342 || version != 1) {
            return false;
        }
        QByteArray compressed;
        compressed.resize(static_cast<int>(compressedSize));
        if (compressedSize > 0 &&
            stream.readRawData(compressed.data(), static_cast<int>(compressedSize)) !=
                static_cast<int>(compressedSize)) {
            return false;
        }
        QJsonObject object;
        if (!jcut::jsonio::parseCborRecordPayload(qUncompress(compressed), &object, nullptr)) {
            continue;
        }
        if (object.value(QStringLiteral("type")).toString() != QStringLiteral("frame")) {
            continue;
        }
        if (object.value(QStringLiteral("frame")).toInteger(-1) != frameNumber) {
            continue;
        }
        if (recordOut) {
            *recordOut = object;
        }
        return true;
    }
    return false;
}

QImage annotateFrameImage(const QImage& source, const QJsonObject& frameRecord, int* boxCountOut)
{
    if (boxCountOut) {
        *boxCountOut = 0;
    }
    if (source.isNull()) {
        return QImage();
    }
    QImage preview = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&preview);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QJsonArray trackDetections = frameRecord.value(QStringLiteral("track_detections")).toArray();
    const QJsonArray detectionBoxes = frameRecord.value(QStringLiteral("detection_boxes")).toArray();
    if (!trackDetections.isEmpty()) {
        painter.setPen(QPen(QColor(QStringLiteral("#66ff66")), 3.0));
        for (const QJsonValue& value : trackDetections) {
            const QJsonObject object = value.toObject();
            const QRectF box(object.value(QStringLiteral("track_box_x")).toDouble(),
                             object.value(QStringLiteral("track_box_y")).toDouble(),
                             object.value(QStringLiteral("track_box_w")).toDouble(),
                             object.value(QStringLiteral("track_box_h")).toDouble());
            if (!box.isValid() || box.isEmpty()) {
                continue;
            }
            painter.drawRect(box);
            if (boxCountOut) {
                ++(*boxCountOut);
            }
        }
    } else {
        painter.setPen(QPen(QColor(QStringLiteral("#66ff66")), 3.0));
        for (const QJsonValue& value : detectionBoxes) {
            const QJsonObject object = value.toObject();
            const QRectF box(object.value(QStringLiteral("x")).toDouble(),
                             object.value(QStringLiteral("y")).toDouble(),
                             object.value(QStringLiteral("w")).toDouble(),
                             object.value(QStringLiteral("h")).toDouble());
            if (!box.isValid() || box.isEmpty()) {
                continue;
            }
            painter.drawRect(box);
            if (boxCountOut) {
                ++(*boxCountOut);
            }
        }
    }

    return preview;
}

double medianValue(std::vector<double> values)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t mid = values.size() / 2;
    if ((values.size() % 2U) == 0U) {
        return (values[mid - 1] + values[mid]) * 0.5;
    }
    return values[mid];
}

int medianValue(std::vector<int> values)
{
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Options options;
    if (!parseArgs(app.arguments(), &options)) {
        printUsage();
        return 2;
    }
    if (!QFileInfo::exists(options.videoPath)) {
        std::cerr << "Video not found: " << options.videoPath.toStdString() << "\n";
        return 2;
    }

    editor::DecoderContext decoder(options.videoPath);
    if (!decoder.initialize()) {
        std::cerr << "Failed to initialize decoder context for: "
                  << options.videoPath.toStdString() << "\n";
        return 2;
    }
    const editor::VideoStreamInfo info = decoder.info();
    decoder.shutdown();

    if (info.durationFrames <= 0) {
        std::cerr << "Could not determine durationFrames for: "
                  << options.videoPath.toStdString() << "\n";
        return 2;
    }

    const qint64 midpointFrame = qMax<qint64>(0, info.durationFrames / 2);
    const int stride = effectiveStrideForRun(options, options.videoPath);
    qint64 alignedFrame = midpointFrame;
    if (stride > 1) {
        const qint64 remainder = midpointFrame % stride;
        if (remainder != 0) {
            alignedFrame += (stride - remainder);
            if (alignedFrame >= info.durationFrames) {
                alignedFrame = qMax<qint64>(0, midpointFrame - remainder);
            }
        }
    }
    const QString outDir = options.outputDir.trimmed().isEmpty()
        ? defaultOutputDir()
        : options.outputDir;
    QDir().mkpath(outDir);
    const QImage midpointImage = decodeFrameImage(options.videoPath, alignedFrame);
    const QString midpointSourcePngPath = writePng(
        midpointImage,
        QDir(outDir).filePath(QStringLiteral("midpoint_frame_%1_source.png").arg(alignedFrame)));
    const QString midpointPngPath = QDir(outDir).filePath(
        QStringLiteral("midpoint_frame_%1.png").arg(alignedFrame));
    const int totalSampledFrames = options.warmupFrames + options.benchmarkFrames;
    const int maxFrames = qMax(1, ((totalSampledFrames - 1) * stride) + 1);

    const QString binaryPath = runnerPath();
    if (!QFileInfo::exists(binaryPath)) {
        std::cerr << "Runner not found: " << binaryPath.toStdString() << "\n";
        return 2;
    }

    std::cout << "video=" << options.videoPath.toStdString()
              << " duration_frames=" << info.durationFrames
              << " fps=" << info.fps
              << " midpoint_frame=" << midpointFrame
              << " stride=" << stride
              << " aligned_frame=" << alignedFrame
              << " benchmark_frames=" << options.benchmarkFrames
              << " warmup_frames=" << options.warmupFrames
              << " repeat=" << options.repeat
              << " out_dir=" << outDir.toStdString()
              << "\n";
    if (!midpointSourcePngPath.isEmpty()) {
        std::cout << "midpoint_source_png=" << midpointSourcePngPath.toStdString() << "\n";
    }
    std::cout << "runner=" << binaryPath.toStdString() << "\n";
    std::vector<int> processedFrames;
    std::vector<int> totalDetections;
    std::vector<int> trackCounts;
    std::vector<double> avgDetectionMs;
    std::vector<double> avgUploadMs;
    std::vector<double> avgNcnnInputMs;
    std::vector<double> avgNcnnExtractMs;
    std::vector<double> avgNcnnExtractLevel8Ms;
    std::vector<double> avgNcnnExtractLevel16Ms;
    std::vector<double> avgNcnnExtractLevel32Ms;
    std::vector<double> avgNcnnPostMs;
    std::vector<double> avgNcnnTotalMs;
    std::vector<double> wallSec;
    std::vector<double> harnessWallSec;
    int exitCode = 0;

    for (int runIndex = 0; runIndex < options.repeat; ++runIndex) {
        const QString runOutDir =
            options.repeat > 1
                ? QDir(outDir).filePath(QStringLiteral("run_%1").arg(runIndex + 1))
                : outDir;
        if (QFileInfo::exists(runOutDir)) {
            QDir(runOutDir).removeRecursively();
        }
        QDir().mkpath(runOutDir);
        QStringList runnerArgs{
            options.videoPath,
            QStringLiteral("--detector"), options.detector,
            QStringLiteral("--start-frame"), QString::number(alignedFrame),
            QStringLiteral("--max-frames"), QString::number(maxFrames),
            QStringLiteral("--out-dir"), runOutDir,
            options.previewWindow ? QStringLiteral("--preview-window") : QStringLiteral("--no-preview-window"),
            options.previewFiles ? QStringLiteral("--preview-files") : QStringLiteral("--no-preview-files"),
            QStringLiteral("--log-interval"), QString::number(options.logInterval),
        };
        if (!options.paramsFile.trimmed().isEmpty()) {
            runnerArgs << QStringLiteral("--params-file") << options.paramsFile;
        }
        if (options.quiet) {
            runnerArgs << QStringLiteral("--quiet");
        }
        runnerArgs << options.passthroughArgs;

        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("LD_LIBRARY_PATH"),
                   QStringLiteral(EDITOR_FFMPEG_PREFIX) + QStringLiteral("/lib:") +
                       env.value(QStringLiteral("LD_LIBRARY_PATH")));
        process.setProcessEnvironment(env);
        process.setProgram(binaryPath);
        process.setArguments(runnerArgs);
        process.setProcessChannelMode(QProcess::ForwardedChannels);
        QElapsedTimer runTimer;
        runTimer.start();
        process.start();
        if (!process.waitForStarted(10000)) {
            std::cerr << "Failed to start runner: "
                      << process.errorString().toStdString() << "\n";
            return 2;
        }
        if (!process.waitForFinished(-1)) {
            std::cerr << "Runner did not finish cleanly.\n";
            return 2;
        }
        exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : 2;
        const double measuredWallSec =
            static_cast<double>(runTimer.nsecsElapsed()) / 1'000'000'000.0;
        if (exitCode != 0) {
            std::cerr << "Runner failed with exit code " << exitCode
                      << " for out_dir=" << runOutDir.toStdString() << "\n";
            return exitCode;
        }

        RunSummary summary;
        const QString summaryPath = QDir(runOutDir).filePath(QStringLiteral("summary.json"));
        if (!loadSummary(summaryPath, &summary)) {
            std::cerr << "Failed to load benchmark summary from: "
                      << summaryPath.toStdString() << "\n";
            return exitCode;
        }
        const QString requestedScrfdVariant = [&]() -> QString {
            const int index = options.passthroughArgs.indexOf(QStringLiteral("--scrfd-model"));
            if (index >= 0 && (index + 1) < options.passthroughArgs.size()) {
                return options.passthroughArgs.at(index + 1);
            }
            return QStringLiteral("500m");
        }();
        if (!summary.scrfdModelVariant.trimmed().isEmpty() &&
            summary.scrfdModelVariant.trimmed() != requestedScrfdVariant) {
            std::cerr << "Summary variant mismatch. Requested "
                      << requestedScrfdVariant.toStdString()
                      << " but summary reported "
                      << summary.scrfdModelVariant.toStdString() << "\n";
            return 2;
        }
        processedFrames.push_back(summary.processedFrames);
        totalDetections.push_back(summary.totalDetections);
        trackCounts.push_back(summary.trackCount);
        avgDetectionMs.push_back(summary.avgDetectionMs);
        avgUploadMs.push_back(summary.avgUploadMs);
        avgNcnnInputMs.push_back(summary.avgNcnnInputMs);
        avgNcnnExtractMs.push_back(summary.avgNcnnExtractMs);
        avgNcnnExtractLevel8Ms.push_back(summary.avgNcnnExtractLevel8Ms);
        avgNcnnExtractLevel16Ms.push_back(summary.avgNcnnExtractLevel16Ms);
        avgNcnnExtractLevel32Ms.push_back(summary.avgNcnnExtractLevel32Ms);
        avgNcnnPostMs.push_back(summary.avgNcnnPostMs);
        avgNcnnTotalMs.push_back(summary.avgNcnnTotalMs);
        wallSec.push_back(summary.wallSec);
        harnessWallSec.push_back(measuredWallSec);
        std::cout << "benchmark_run=" << (runIndex + 1)
                  << " processed_frames=" << summary.processedFrames
                  << " total_detections=" << summary.totalDetections
                  << " track_count=" << summary.trackCount
                  << " scrfd_model_variant=" << summary.scrfdModelVariant.toStdString()
                  << " avg_detection_ms=" << summary.avgDetectionMs
                  << " avg_upload_ms=" << summary.avgUploadMs
                  << " avg_ncnn_input_ms=" << summary.avgNcnnInputMs
                  << " avg_ncnn_extract_ms=" << summary.avgNcnnExtractMs
                  << " avg_ncnn_extract_level8_ms=" << summary.avgNcnnExtractLevel8Ms
                  << " avg_ncnn_extract_level16_ms=" << summary.avgNcnnExtractLevel16Ms
                  << " avg_ncnn_extract_level32_ms=" << summary.avgNcnnExtractLevel32Ms
                  << " avg_ncnn_post_ms=" << summary.avgNcnnPostMs
                  << " avg_ncnn_total_ms=" << summary.avgNcnnTotalMs
                  << " summary_wall_sec=" << summary.wallSec
                  << " harness_wall_sec=" << measuredWallSec
                  << " out_dir=" << runOutDir.toStdString()
                  << "\n";

        if (runIndex == 0) {
            QJsonObject frameRecord;
            const QString streamPath = QDir(runOutDir).filePath(QStringLiteral("facedetections.part"));
            if (loadFrameRecord(streamPath, alignedFrame, &frameRecord)) {
                int boxCount = 0;
                const QImage annotatedImage = annotateFrameImage(midpointImage, frameRecord, &boxCount);
                if (!writePng(annotatedImage, midpointPngPath).isEmpty()) {
                    std::cout << "midpoint_png=" << midpointPngPath.toStdString()
                              << " boxes=" << boxCount << "\n";
                }
            }
        }
    }

    std::cout << "benchmark_median"
              << " processed_frames=" << medianValue(processedFrames)
              << " total_detections=" << medianValue(totalDetections)
              << " track_count=" << medianValue(trackCounts)
              << " avg_detection_ms=" << medianValue(avgDetectionMs)
              << " avg_upload_ms=" << medianValue(avgUploadMs)
              << " avg_ncnn_input_ms=" << medianValue(avgNcnnInputMs)
              << " avg_ncnn_extract_ms=" << medianValue(avgNcnnExtractMs)
              << " avg_ncnn_extract_level8_ms=" << medianValue(avgNcnnExtractLevel8Ms)
              << " avg_ncnn_extract_level16_ms=" << medianValue(avgNcnnExtractLevel16Ms)
              << " avg_ncnn_extract_level32_ms=" << medianValue(avgNcnnExtractLevel32Ms)
              << " avg_ncnn_post_ms=" << medianValue(avgNcnnPostMs)
              << " avg_ncnn_total_ms=" << medianValue(avgNcnnTotalMs)
              << " summary_wall_sec=" << medianValue(wallSec)
              << " harness_wall_sec=" << medianValue(harnessWallSec)
              << "\n";
    return exitCode;
}
