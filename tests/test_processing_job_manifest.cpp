#include "processing_job_manifest.h"
#include "processing_job_docker.h"
#include "birefnet_job_core.h"
#include "prompt_mask_job_core.h"
#include "transcription_job_core.h"

#include <QtTest/QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <algorithm>
#include <vector>

class ProcessingJobManifestTest : public QObject {
  Q_OBJECT

private slots:
  void defaultJobRootIsStableAndScopedToInputDirectory() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString input =
        QDir(tempDir.path()).absoluteFilePath(QStringLiteral("My Video!.mp4"));

    const QString root =
        jcut::jobs::defaultJobRootForInput(input, QStringLiteral("sam3"),
                                           QStringLiteral("a person"));

    QVERIFY(root.startsWith(QDir(tempDir.path()).absoluteFilePath(QStringLiteral(".jcut_jobs"))));
    QVERIFY(root.contains(QStringLiteral("sam3_a_person_My_Video")));
    QCOMPARE(jcut::jobs::manifestPathForJobRoot(root),
             QDir(root).absoluteFilePath(QStringLiteral("manifest.json")));
  }

  void sharedPromptMaskPlanMatchesQtPathsAndArguments() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString input =
        QDir(tempDir.path()).absoluteFilePath(
            QStringLiteral("My Video!.mp4"));
    QFile source(input);
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("video");
    source.close();

    jcut::masks::PromptMaskJobRequest request;
    request.scriptPath =
        QDir(tempDir.path()).absoluteFilePath(
            QStringLiteral("sam3.sh")).toStdString();
    request.mediaPath = input.toStdString();
    request.prompt = "a person";
    request.modelCachePath =
        QDir(tempDir.path()).absoluteFilePath(
            QStringLiteral("hf")).toStdString();
    request.runtimeCachePath =
        QDir(tempDir.path()).absoluteFilePath(
            QStringLiteral("runtime")).toStdString();
    request.scaleWidth = 960;
    request.intermediateFramesFormat = "png";

    const auto plan =
        jcut::masks::buildPromptMaskJobPlan(request);
    QCOMPARE(
        QString::fromStdString(plan.jobRoot),
        jcut::jobs::defaultJobRootForInput(
            input,
            QStringLiteral("sam3"),
            QStringLiteral("a person")));
    QCOMPARE(
        QString::fromStdString(plan.manifestPath),
        jcut::jobs::manifestPathForJobRoot(
            QString::fromStdString(plan.jobRoot)));
    QVERIFY(
        QString::fromStdString(plan.binaryMasksPath).endsWith(
            QStringLiteral(
                "My Video!_sam3_a_person_binary_masks")));
    const std::vector<std::string> expectedArguments{
        "--prompt",
        "--binary-mask-dir",
        "--no-centers-json",
        "--scale-width",
        "--intermediate-frames-format",
    };
    for (const std::string& argument : expectedArguments) {
      QVERIFY(std::find(
                  plan.command.begin(),
                  plan.command.end(),
                  argument) != plan.command.end());
    }
  }

  void transcriptionControllerSupportsStdinLogsAndManifest() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString mediaPath =
        QDir(tempDir.path()).absoluteFilePath(
            QStringLiteral("interview.wav"));
    QFile media(mediaPath);
    QVERIFY(media.open(QIODevice::WriteOnly));
    media.write("audio");
    media.close();

    const QString scriptPath =
        QDir(tempDir.path()).absoluteFilePath(
            QStringLiteral("fake-whisperx.sh"));
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write(
        "#!/usr/bin/env bash\n"
        "set -euo pipefail\n"
        "read -r token\n"
        "printf '{\"segments\":[],\"token\":\"%s\"}\\n' \"$token\" "
        "> \"$(dirname \"$1\")/$(basename \"$1\" .wav).json\"\n"
        "echo \"transcribed $1\"\n");
    script.close();
    QVERIFY(QFile::setPermissions(
        scriptPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner | QFileDevice::ReadGroup |
            QFileDevice::ExeGroup | QFileDevice::ReadOther |
            QFileDevice::ExeOther));

    jcut::jobs::TranscriptionJobControllerCore controller;
    std::string error;
    QVERIFY2(controller.start(
                 {7, scriptPath.toStdString(),
                  mediaPath.toStdString()},
                 &error),
             error.c_str());
    QTRY_VERIFY_WITH_TIMEOUT(
        controller.snapshot().state ==
            jcut::jobs::ProcessJobSnapshotCore::State::Running,
        3000);
    QVERIFY2(controller.writeStdin("secret-token", &error),
             error.c_str());
    controller.wait();
    const jcut::jobs::TranscriptionJobSnapshotCore snapshot =
        controller.snapshot();
    QCOMPARE(
        snapshot.state,
        jcut::jobs::ProcessJobSnapshotCore::State::Completed);
    QCOMPARE(snapshot.clipId, 7);
    QVERIFY(snapshot.outputReady);
    QVERIFY(QFileInfo::exists(
        QString::fromStdString(snapshot.logPath)));
    QVERIFY(QFileInfo::exists(
        QString::fromStdString(snapshot.manifestPath)));
    QFile transcript(
        QString::fromStdString(snapshot.outputTranscriptPath));
    QVERIFY(transcript.open(QIODevice::ReadOnly));
    QVERIFY(transcript.readAll().contains("secret-token"));
  }

  void birefnetControllerTracksProgressAndAlphaOutput() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString mediaPath =
        QDir(tempDir.path()).absoluteFilePath(
            QStringLiteral("portrait.mp4"));
    QFile media(mediaPath);
    QVERIFY(media.open(QIODevice::WriteOnly));
    media.write("video");
    media.close();

    const QString scriptPath =
        QDir(tempDir.path()).absoluteFilePath(
            QStringLiteral("fake-birefnet.sh"));
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write(
        "#!/usr/bin/env bash\n"
        "set -euo pipefail\n"
        "output=''\n"
        "while (($#)); do\n"
        "  if [[ \"$1\" == '--output-dir' ]]; then "
        "output=\"$2\"; shift 2; else shift; fi\n"
        "done\n"
        "mkdir -p \"$output\"\n"
        "printf '{\"current_frame\":2,\"total_frames\":4,"
        "\"percent\":50.0}\\n' "
        "> \"$BIREFNET_JOB_ROOT/progress.json\"\n"
        "printf '{\"schema\":\"jcut_alpha_v1\"}\\n' "
        "> \"$output/jcut_alpha.json\"\n"
        "echo 'BiRefNet fake worker complete'\n");
    script.close();
    QVERIFY(QFile::setPermissions(
        scriptPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner | QFileDevice::ReadGroup |
            QFileDevice::ExeGroup | QFileDevice::ReadOther |
            QFileDevice::ExeOther));

    jcut::jobs::BiRefNetJobRequestCore request;
    request.clipId = 11;
    request.scriptPath = scriptPath.toStdString();
    request.mediaPath = mediaPath.toStdString();
    request.modelCachePath =
        QDir(tempDir.path()).absoluteFilePath(
            QStringLiteral("model-cache")).toStdString();
    request.runtimeCachePath =
        QDir(tempDir.path()).absoluteFilePath(
            QStringLiteral("runtime-cache")).toStdString();
    request.device = "cpu";
    request.fp16 = false;
    request.alphaTolerance = 0.25;

    const auto plan =
        jcut::jobs::buildBiRefNetJobPlanCore(request);
    QVERIFY(std::find(
                plan.command.begin(),
                plan.command.end(),
                "--cpu") != plan.command.end());
    QVERIFY(std::find(
                plan.command.begin(),
                plan.command.end(),
                "--fp32") != plan.command.end());
    QVERIFY(plan.environment.contains("BIREFNET_JOB_ROOT"));

    jcut::jobs::BiRefNetJobControllerCore controller;
    std::string error;
    QVERIFY2(controller.start(request, &error), error.c_str());
    controller.wait();
    const jcut::jobs::BiRefNetJobSnapshotCore snapshot =
        controller.snapshot();
    QCOMPARE(
        snapshot.state,
        jcut::jobs::ProcessJobSnapshotCore::State::Completed);
    QCOMPARE(snapshot.clipId, 11);
    QCOMPARE(snapshot.currentFrame, 2);
    QCOMPARE(snapshot.totalFrames, 4);
    QCOMPARE(snapshot.percent, 50.0);
    QVERIFY(snapshot.outputReady);
    QVERIFY(QFileInfo::exists(
        QString::fromStdString(snapshot.manifestPath)));
    QVERIFY(QFileInfo::exists(
        QString::fromStdString(snapshot.logPath)));
  }

  void writesAndUpdatesManifestStatus() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString input =
        QDir(tempDir.path()).absoluteFilePath(QStringLiteral("clip.mp4"));
    QFile file(input);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("video");
    file.close();

    const QString root =
        jcut::jobs::defaultJobRootForInput(input, QStringLiteral("facedetections"));
    const QString manifestPath = jcut::jobs::manifestPathForJobRoot(root);
    const QJsonObject manifest = jcut::jobs::makeManifest(
        QStringLiteral("facedetections"),
        root,
        input,
        QJsonObject{{QStringLiteral("detector"), QStringLiteral("scrfd")}},
        QJsonObject{{QStringLiteral("checkpoint"), QStringLiteral("facedetections.part")}},
        QStringList{QStringLiteral("generator"), QStringLiteral("--resume")});

    QString error;
    QVERIFY2(jcut::jobs::writeManifest(manifestPath, manifest, &error),
             qPrintable(error));
    QVERIFY2(jcut::jobs::updateManifestStatus(
                 manifestPath,
                 QStringLiteral("paused"),
                 QJsonObject{{QStringLiteral("pause_reason"), QStringLiteral("test")}},
                 &error),
             qPrintable(error));

    QJsonObject loaded;
    QVERIFY2(jcut::jobs::readManifest(manifestPath, &loaded, &error),
             qPrintable(error));
    QCOMPARE(loaded.value(QStringLiteral("schema")).toString(),
             QStringLiteral("jcut_processing_job_v1"));
    QCOMPARE(loaded.value(QStringLiteral("operation")).toString(),
             QStringLiteral("facedetections"));
    QCOMPARE(loaded.value(QStringLiteral("status")).toString(),
             QStringLiteral("paused"));
    QCOMPARE(loaded.value(QStringLiteral("pause_reason")).toString(),
             QStringLiteral("test"));
    QVERIFY(loaded.value(QStringLiteral("input")).toObject().value(QStringLiteral("exists")).toBool());
  }

  void matchesDockerContainerFromManifestName() {
    const QJsonObject manifest{
        {QStringLiteral("operation"), QStringLiteral("sam3")},
        {QStringLiteral("process"),
         QJsonObject{{QStringLiteral("docker"),
                      QJsonObject{{QStringLiteral("container_name"),
                                   QStringLiteral("jcut-sam3-clip-person-123")}}}}},
    };
    const QVector<jcut::jobs::DockerContainerInfo> containers{
        jcut::jobs::DockerContainerInfo{
            QStringLiteral("abc123"),
            QStringLiteral("jcut-sam3-clip-person-123"),
            QStringLiteral("sam3:cu126"),
            QStringLiteral("Up 1 hour"),
            QStringLiteral("running"),
            QString(),
            QJsonObject(),
        },
    };

    const jcut::jobs::DockerContainerInfo* match =
        jcut::jobs::findDockerContainerForManifest(manifest, containers);
    QVERIFY(match != nullptr);
    QCOMPARE(match->id, QStringLiteral("abc123"));
    QVERIFY(jcut::jobs::dockerContainerIsRunning(*match));
  }

  void stableDockerNamesAreReadableAndCollisionResistant() {
    const QString first = jcut::jobs::stableDockerContainerName(
        QStringLiteral("BiRefNet"),
        QStringLiteral("/project/one/.jcut_jobs/birefnet_BiRefNet-matting_My Video"));
    const QString repeated = jcut::jobs::stableDockerContainerName(
        QStringLiteral("BiRefNet"),
        QStringLiteral("/project/one/.jcut_jobs/birefnet_BiRefNet-matting_My Video"));
    const QString otherProject = jcut::jobs::stableDockerContainerName(
        QStringLiteral("BiRefNet"),
        QStringLiteral("/project/two/.jcut_jobs/birefnet_BiRefNet-matting_My Video"));

    QCOMPARE(first, repeated);
    QVERIFY(first.startsWith(QStringLiteral("jcut-birefnet-birefnet-matting-my-video-")));
    QVERIFY(QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9_.-]+$")).match(first).hasMatch());
    QVERIFY(first != otherProject);
  }

  void recoversDockerContainerFromSamJobRootInCommand() {
    const QJsonObject manifest{
        {QStringLiteral("operation"), QStringLiteral("sam3")},
        {QStringLiteral("job_root"),
         QStringLiteral("/mnt/Cancer/PanelVid2TikTok/TechUnity/.jcut_jobs/sam3_person_TechUnity2026")},
    };
    const QVector<jcut::jobs::DockerContainerInfo> containers{
        jcut::jobs::DockerContainerInfo{
            QStringLiteral("badf7f8e26e1"),
            QStringLiteral("adoring_hofstadter"),
            QStringLiteral("sam3:cu126"),
            QStringLiteral("Up 7 hours"),
            QStringLiteral("running"),
            QStringLiteral("python /workspace/sam3_run.py --job-dir "
                           "\"/out/.jcut_jobs/sam3_person_TechUnity2026\""),
            QJsonObject(),
        },
    };

    const jcut::jobs::DockerContainerInfo* match =
        jcut::jobs::findDockerContainerForManifest(manifest, containers);
    QVERIFY(match != nullptr);
    QCOMPARE(match->name, QStringLiteral("adoring_hofstadter"));
  }

  void recoversLegacyBiRefNetContainerFromOutputMount() {
    const QJsonObject manifest{
        {QStringLiteral("operation"), QStringLiteral("birefnet")},
        {QStringLiteral("artifacts"),
         QJsonObject{{QStringLiteral("alpha_masks_dir"),
                      QStringLiteral("/media/project/clip_birefnet_alpha_masks")}}},
    };
    jcut::jobs::DockerContainerInfo container{
        QStringLiteral("abc123"),
        QStringLiteral("eloquent_morse"),
        QStringLiteral("jcut-birefnet:cu126"),
        QStringLiteral("Up 3 hours"),
        QStringLiteral("running"),
        QStringLiteral("python /workspace/birefnet_run.py"),
        QJsonObject(),
    };
    container.mounts.insert(QStringLiteral("/output"),
                            QStringLiteral("/media/project/clip_birefnet_alpha_masks"));

    const QVector<jcut::jobs::DockerContainerInfo> containers{container};
    const jcut::jobs::DockerContainerInfo* match =
        jcut::jobs::findDockerContainerForManifest(manifest, containers);
    QVERIFY(match != nullptr);
    QCOMPARE(match->name, QStringLiteral("eloquent_morse"));
  }
};

QTEST_MAIN(ProcessingJobManifestTest)
#include "test_processing_job_manifest.moc"
