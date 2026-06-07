#pragma once

#include "vulkan_facedetections_offscreen_options.h"

#include <QJsonObject>
#include <QProcess>
#include <QStringList>

QStringList benchmarkBaseArgs(int argc, char **argv);
QJsonObject benchmarkSummaryRow(int slotCount, int exitCode,
                                QProcess::ExitStatus exitStatus,
                                const QString &outputDir,
                                const QJsonObject &summary);
int runPipelineSlotBenchmark(int argc, char **argv, const Options &options);
