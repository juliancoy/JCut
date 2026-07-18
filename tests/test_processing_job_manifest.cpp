#include "processing_job_manifest.h"
#include "processing_job_docker.h"

#include <QtTest/QtTest>

#include <QDir>
#include <QTemporaryDir>

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
