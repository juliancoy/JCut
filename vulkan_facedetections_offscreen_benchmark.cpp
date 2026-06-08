#include "vulkan_facedetections_offscreen_benchmark.h"

#include "json_io_utils.h"
#include "vulkan_facedetections_offscreen_artifact_io.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>

#include <iostream>

QStringList benchmarkBaseArgs(int argc, char **argv) {
  QStringList args;
  for (int i = 1; i < argc; ++i) {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == QStringLiteral("--benchmark-pipeline-slots")) {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        ++i;
      }
      continue;
    }
    if (arg == QStringLiteral("--out-dir") ||
        arg == QStringLiteral("--detector-pipeline-slots") ||
        arg == QStringLiteral("--detector-workers")) {
      if (i + 1 < argc) {
        ++i;
      }
      continue;
    }
    args.push_back(arg);
  }
  return args;
}

QJsonObject benchmarkSummaryRow(int slotCount, int exitCode,
                                QProcess::ExitStatus exitStatus,
                                const QString &outputDir,
                                const QJsonObject &summary) {
  const int processedFrames =
      summary.value(QStringLiteral("processed_frames")).toInt(0);
  const double wallSec =
      summary.value(QStringLiteral("wall_sec")).toDouble(0.0);
  const double processedFps =
      wallSec > 0.0 ? static_cast<double>(processedFrames) / wallSec : 0.0;
  return QJsonObject{
      {QStringLiteral("detector_pipeline_slots"), slotCount},
      {QStringLiteral("detector_workers"), slotCount},
      {QStringLiteral("exit_code"), exitCode},
      {QStringLiteral("exit_status"), exitStatus == QProcess::NormalExit
                                          ? QStringLiteral("normal")
                                          : QStringLiteral("crashed")},
      {QStringLiteral("ok"),
       exitStatus == QProcess::NormalExit && exitCode == 0},
      {QStringLiteral("output_dir"), QDir(outputDir).absolutePath()},
      {QStringLiteral("processed_frames"), processedFrames},
      {QStringLiteral("wall_sec"), wallSec},
      {QStringLiteral("processed_fps"), processedFps},
      {QStringLiteral("decoded_frames"),
       summary.value(QStringLiteral("decoded_frames")).toInt(0)},
      {QStringLiteral("total_detections"),
       summary.value(QStringLiteral("total_detections")).toInt(0)},
      {QStringLiteral("avg_stage_decode_ms"),
       summary.value(QStringLiteral("avg_stage_decode_ms")).toDouble(0.0)},
      {QStringLiteral("avg_stage_handoff_prepare_ms"),
       summary.value(QStringLiteral("avg_stage_handoff_prepare_ms"))
           .toDouble(0.0)},
      {QStringLiteral("avg_stage_inference_wall_ms"),
       summary.value(QStringLiteral("avg_stage_inference_wall_ms"))
           .toDouble(0.0)},
      {QStringLiteral("avg_stage_tracking_ms"),
       summary.value(QStringLiteral("avg_stage_tracking_ms")).toDouble(0.0)},
      {QStringLiteral("avg_stage_checkpoint_enqueue_ms"),
       summary.value(QStringLiteral("avg_stage_checkpoint_enqueue_ms"))
           .toDouble(0.0)},
      {QStringLiteral("checkpoint_writer_avg_write_ms"),
       summary.value(QStringLiteral("checkpoint_writer_avg_write_ms"))
           .toDouble(0.0)},
      {QStringLiteral("checkpoint_writer_max_backlog"),
       summary.value(QStringLiteral("checkpoint_writer_max_backlog")).toInt(0)},
      {QStringLiteral("checkpoint_writer_backpressure_ms"),
       summary.value(QStringLiteral("checkpoint_writer_backpressure_ms"))
           .toDouble(0.0)},
      {QStringLiteral("detector_workers_requested"),
       summary.value(QStringLiteral("detector_workers_requested")).toInt(1)},
      {QStringLiteral("detector_workers_active"),
       summary.value(QStringLiteral("detector_workers_active")).toInt(1)},
      {QStringLiteral("detector_workers_note"),
       summary.value(QStringLiteral("detector_workers_note")).toString()},
      {QStringLiteral("require_zero_copy_satisfied"),
       summary.value(QStringLiteral("require_zero_copy_satisfied"))
           .toBool(false)},
      {QStringLiteral("decode_zero_copy"),
       summary.value(QStringLiteral("decode_zero_copy")).toBool(false)}};
}

int runPipelineSlotBenchmark(int argc, char **argv, const Options &options) {
  const QString executable = QCoreApplication::applicationFilePath();
  if (executable.isEmpty() || !QFileInfo::exists(executable)) {
    std::cerr << "Pipeline benchmark failed: cannot resolve current executable "
                 "path.\n";
    return 2;
  }
  if (options.preflight) {
    std::cerr
        << "--benchmark-pipeline-slots cannot be combined with --preflight.\n";
    return 2;
  }

  const QString benchmarkRoot =
      QDir(options.outputDir)
          .absoluteFilePath(QStringLiteral("pipeline_benchmark_%1")
                                .arg(QDateTime::currentDateTimeUtc().toString(
                                    QStringLiteral("yyyyMMdd_HHmmss_zzz"))));
  if (!QDir().mkpath(benchmarkRoot)) {
    std::cerr << "Pipeline benchmark failed: cannot create output directory "
              << benchmarkRoot.toStdString() << "\n";
    return 2;
  }

  const QStringList baseArgs = benchmarkBaseArgs(argc, argv);
  QJsonArray runs;
  int bestSlots = -1;
  double bestProcessedFps = -1.0;
  bool allOk = true;

  for (int slotCount : options.benchmarkPipelineSlotValues) {
    const QString runDir =
        QDir(benchmarkRoot)
            .absoluteFilePath(QStringLiteral("slots_%1").arg(slotCount));
    QDir(runDir).removeRecursively();
    if (!QDir().mkpath(runDir)) {
      std::cerr << "Pipeline benchmark failed: cannot create run directory "
                << runDir.toStdString() << "\n";
      return 2;
    }

    QStringList childArgs = baseArgs;
    childArgs << QStringLiteral("--out-dir") << runDir
              << QStringLiteral("--detector-workers")
              << QString::number(slotCount)
              << QStringLiteral("--detector-pipeline-slots")
              << QString::number(slotCount)
              << QStringLiteral("--no-preview-window")
              << QStringLiteral("--no-control-window")
              << QStringLiteral("--no-progress");

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(executable, childArgs);
    if (!process.waitForStarted()) {
      std::cerr << "Pipeline benchmark failed to start child for slots="
                << slotCount << ": " << process.errorString().toStdString()
                << "\n";
      return 2;
    }
    process.waitForFinished(-1);
    const QByteArray output = process.readAllStandardOutput();
    const int exitCode = process.exitCode();
    const QProcess::ExitStatus exitStatus = process.exitStatus();
    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
      allOk = false;
      std::cerr << "Pipeline benchmark child failed for slots=" << slotCount
                << " exit_code=" << exitCode << " status="
                << (exitStatus == QProcess::NormalExit ? "normal" : "crashed")
                << "\n"
                << output.constData() << "\n";
    }

    QJsonObject summary;
    QString readError;
    const QString summaryPath =
        QDir(runDir).filePath(QStringLiteral("summary.json"));
    if (!jcut::jsonio::readJsonFile(summaryPath, &summary, &readError)) {
      allOk = false;
      std::cerr << "Pipeline benchmark could not read summary for slots="
                << slotCount << ": " << readError.toStdString() << "\n";
    }
    const QJsonObject row =
        benchmarkSummaryRow(slotCount, exitCode, exitStatus, runDir, summary);
    runs.append(row);
    const double processedFps =
        row.value(QStringLiteral("processed_fps")).toDouble(0.0);
    if (row.value(QStringLiteral("ok")).toBool(false) &&
        processedFps > bestProcessedFps) {
      bestProcessedFps = processedFps;
      bestSlots = slotCount;
    }
  }

  QJsonArray slotsTested;
  for (int slotCount : options.benchmarkPipelineSlotValues) {
    slotsTested.append(slotCount);
  }
  const QJsonObject result{
      {QStringLiteral("schema"),
       QStringLiteral("jcut_facedetections_pipeline_benchmark_v1")},
      {QStringLiteral("created_utc_ms"),
       QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()},
      {QStringLiteral("video"), options.videoPath},
      {QStringLiteral("benchmark_root"), benchmarkRoot},
      {QStringLiteral("slots_tested"), slotsTested},
      {QStringLiteral("best_detector_pipeline_slots"), bestSlots},
      {QStringLiteral("best_detector_workers"), bestSlots},
      {QStringLiteral("best_processed_fps"), bestProcessedFps},
      {QStringLiteral("runs"), runs},
      {QStringLiteral("note"),
       QStringLiteral("Benchmark child runs force preview/control/progress off "
                      "and set detector workers equal to pipeline slots. "
                      "Each run uses an isolated output directory so resumable checkpoint "
                      "files do not contaminate comparisons.")}};
  const QString resultPath =
      QDir(benchmarkRoot)
          .filePath(QStringLiteral("pipeline_benchmark_summary.json"));
  writeJson(resultPath, result);
  std::cout << "pipeline_benchmark "
            << jcut::jsonio::serializeCompact(result).constData() << "\n";
  return allOk ? 0 : 2;
}
