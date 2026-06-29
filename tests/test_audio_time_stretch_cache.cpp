#include "audio_time_stretch_cache.h"
#include "audio_source_key.h"

#include <QtTest/QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>

class TestAudioTimeStretchCache : public QObject {
    Q_OBJECT

private slots:
    void roundTripsValidSidecar()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sourcePath = dir.filePath(QStringLiteral("source.wav"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QVERIFY(source.write("source-audio") > 0);
        source.close();

        AudioTimeStretchCacheEntry entry;
        entry.samples = QVector<float>{0.0f, 0.25f, -0.5f, 1.0f};
        entry.sampleRate = 48000;
        entry.channelCount = 2;
        entry.valid = true;
        entry.fullyDecoded = true;

        QVERIFY(writeAudioTimeStretchSidecar(sourcePath, 1500, entry));
        QVERIFY(QFileInfo::exists(audioTimeStretchSidecarPathForSource(sourcePath, 1500)));

        AudioTimeStretchCacheEntry loaded;
        QVERIFY(readAudioTimeStretchSidecar(sourcePath, 1500, &loaded));
        QVERIFY(loaded.valid);
        QCOMPARE(loaded.sampleRate, entry.sampleRate);
        QCOMPARE(loaded.channelCount, entry.channelCount);
        QCOMPARE(loaded.fullyDecoded, entry.fullyDecoded);
        QCOMPARE(loaded.samples, entry.samples);
    }

    void invalidatesWhenSourceChanges()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sourcePath = dir.filePath(QStringLiteral("source.wav"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QVERIFY(source.write("source-audio") > 0);
        source.close();

        AudioTimeStretchCacheEntry entry;
        entry.samples = QVector<float>{0.0f, 1.0f};
        entry.sampleRate = 48000;
        entry.channelCount = 1;
        entry.valid = true;
        entry.fullyDecoded = true;
        QVERIFY(writeAudioTimeStretchSidecar(sourcePath, 1500, entry));

        AudioTimeStretchCacheEntry loaded;
        QVERIFY(readAudioTimeStretchSidecar(sourcePath, 1500, &loaded));

        QThread::msleep(2);
        QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Append));
        QVERIFY(source.write("changed") > 0);
        source.close();

        QVERIFY(!readAudioTimeStretchSidecar(sourcePath, 1500, &loaded));
        QVERIFY(!loaded.valid);
    }

    void streamSourceKeysUseDistinctSidecars()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sourcePath = dir.filePath(QStringLiteral("source.mov"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QVERIFY(source.write("source-audio") > 0);
        source.close();

        AudioTimeStretchCacheEntry streamOne;
        streamOne.samples = QVector<float>{0.1f, 0.2f};
        streamOne.sampleRate = 48000;
        streamOne.channelCount = 1;
        streamOne.valid = true;
        streamOne.fullyDecoded = true;

        AudioTimeStretchCacheEntry streamTwo = streamOne;
        streamTwo.samples = QVector<float>{0.7f, 0.8f};

        const QString streamOneKey = editor::audio::makeSourceKey(sourcePath, 1);
        const QString streamTwoKey = editor::audio::makeSourceKey(sourcePath, 2);
        QVERIFY(writeAudioTimeStretchSidecar(streamOneKey, 1500, streamOne));
        QVERIFY(writeAudioTimeStretchSidecar(streamTwoKey, 1500, streamTwo));
        QVERIFY(audioTimeStretchSidecarPathForSource(streamOneKey, 1500) !=
                audioTimeStretchSidecarPathForSource(streamTwoKey, 1500));

        AudioTimeStretchCacheEntry loaded;
        QVERIFY(readAudioTimeStretchSidecar(streamOneKey, 1500, &loaded));
        QCOMPARE(loaded.samples, streamOne.samples);
        QVERIFY(readAudioTimeStretchSidecar(streamTwoKey, 1500, &loaded));
        QCOMPARE(loaded.samples, streamTwo.samples);
    }

    void writeProgressReachesCompleteAfterCommit()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sourcePath = dir.filePath(QStringLiteral("source.wav"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QVERIFY(source.write("source-audio") > 0);
        source.close();

        AudioTimeStretchCacheEntry entry;
        entry.samples.resize((8 * 1024 * 1024 / static_cast<int>(sizeof(float))) + 4);
        for (int i = 0; i < entry.samples.size(); ++i) {
            entry.samples[i] = static_cast<float>(i % 17) / 17.0f;
        }
        entry.sampleRate = 48000;
        entry.channelCount = 2;
        entry.valid = true;
        entry.fullyDecoded = true;

        QVector<double> progressValues;
        QVERIFY(writeAudioTimeStretchSidecar(
            sourcePath,
            1500,
            entry,
            [&progressValues](double progress) { progressValues.append(progress); }));

        QVERIFY(!progressValues.isEmpty());
        QCOMPARE(progressValues.constLast(), 1.0);
        for (int i = 1; i < progressValues.size(); ++i) {
            QVERIFY(progressValues[i] >= progressValues[i - 1]);
        }
    }

    void segmentCoverageUsesRetimedDomain()
    {
        QCOMPARE(audioTimeStretchCacheSampleForSourceSample(3000, 1.5), static_cast<int64_t>(2000));
        QCOMPARE(audioTimeStretchCacheEndSampleForSourceEndSample(4536, 1.5), static_cast<int64_t>(3024));
        QCOMPARE(audioTimeStretchSourceSamplesCoveredByCacheSamples(1024, 1.5), static_cast<int64_t>(1536));

        QVERIFY(audioTimeStretchSegmentCoversSourceRange(
            2000,
            1024,
            3000,
            4536,
            1.5));
        QVERIFY(!audioTimeStretchSegmentCoversSourceRange(
            2000,
            1023,
            3000,
            4536,
            1.5));
        QVERIFY(!audioTimeStretchSegmentCoversSourceRange(
            2100,
            1024,
            3000,
            4536,
            1.5));
    }

    void rejectsPartialSidecarWrites()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sourcePath = dir.filePath(QStringLiteral("source.wav"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QVERIFY(source.write("source-audio") > 0);
        source.close();

        AudioTimeStretchCacheEntry entry;
        entry.samples = QVector<float>{0.0f, 0.25f, -0.5f, 1.0f};
        entry.sampleRate = 48000;
        entry.channelCount = 2;
        entry.valid = true;
        entry.fullyDecoded = false;

        QVERIFY(!writeAudioTimeStretchSidecar(sourcePath, 1500, entry));
        QVERIFY(!QFileInfo::exists(audioTimeStretchSidecarPathForSource(sourcePath, 1500)));
    }
};

QTEST_MAIN(TestAudioTimeStretchCache)
#include "test_audio_time_stretch_cache.moc"
