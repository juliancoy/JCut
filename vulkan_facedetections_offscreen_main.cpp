#include "vulkan_facedetections_offscreen_runner.h"

#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QString>

namespace {

QString outputDirFromArgv(int argc, char** argv)
{
  for (int i = 1; i + 1 < argc; ++i) {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == QStringLiteral("--out-dir")) {
      return QString::fromLocal8Bit(argv[i + 1]).trimmed();
    }
  }
  return {};
}

void writeGeneratorExitFile(const QString& outputDir, int exitCode)
{
  if (outputDir.isEmpty()) {
    return;
  }
  QDir().mkpath(outputDir);
  QFile file(QDir(outputDir).absoluteFilePath(QStringLiteral("generator.exit")));
  if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    file.write(QByteArray::number(exitCode));
    file.write("\n");
  }
}

int runLockedGenerator(int argc, char** argv)
{
  const QString outputDir = outputDirFromArgv(argc, argv);
  std::unique_ptr<QLockFile> outputLock;
  if (!outputDir.isEmpty()) {
    QDir().mkpath(outputDir);
    outputLock = std::make_unique<QLockFile>(
        QDir(outputDir).absoluteFilePath(QStringLiteral("generator.lock")));
    outputLock->setStaleLockTime(0);
    if (!outputLock->tryLock(0)) {
      const QString message =
          QStringLiteral("Another FaceDetections generator is already writing this output directory: %1")
              .arg(outputDir);
      QFile stderrFile(QDir(outputDir).absoluteFilePath(QStringLiteral("generator.stderr.log")));
      if (stderrFile.open(QIODevice::Append)) {
        stderrFile.write(message.toLocal8Bit());
        stderrFile.write("\n");
      }
      fprintf(stderr, "%s\n", message.toLocal8Bit().constData());
      writeGeneratorExitFile(outputDir, 73);
      return 73;
    }
  }
  const int exitCode = runVulkanFacestreamOffscreenWithArgv(argc, argv);
  writeGeneratorExitFile(outputDir, exitCode);
  return exitCode;
}

} // namespace

#ifdef JCUT_FACESTREAM_OFFSCREEN_STANDALONE
int main(int argc, char **argv) {
  return runLockedGenerator(argc, argv);
}
#endif
