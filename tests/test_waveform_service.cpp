#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QVector>

#include <cmath>
#include <cstdint>

#include "../debug_controls.h"
#include "../waveform_service.h"

namespace {

void appendLe16(QByteArray* out, quint16 value) {
    out->append(static_cast<char>(value & 0xff));
    out->append(static_cast<char>((value >> 8) & 0xff));
}

void appendLe32(QByteArray* out, quint32 value) {
    out->append(static_cast<char>(value & 0xff));
    out->append(static_cast<char>((value >> 8) & 0xff));
    out->append(static_cast<char>((value >> 16) & 0xff));
    out->append(static_cast<char>((value >> 24) & 0xff));
}

QString writeMonoPcm16Wav(const QString& path, const QVector<float>& samples, int sampleRate) {
    const int channels = 1;
    const int bitsPerSample = 16;
    const int bytesPerSample = bitsPerSample / 8;
    const quint32 dataBytes = static_cast<quint32>(samples.size() * channels * bytesPerSample);
    const quint32 byteRate = static_cast<quint32>(sampleRate * channels * bytesPerSample);
    const quint16 blockAlign = static_cast<quint16>(channels * bytesPerSample);
    const quint32 riffSize = 36u + dataBytes;

    QByteArray wav;
    wav.reserve(static_cast<int>(44 + dataBytes));
    wav.append("RIFF", 4);
    appendLe32(&wav, riffSize);
    wav.append("WAVE", 4);

    wav.append("fmt ", 4);
    appendLe32(&wav, 16); // PCM fmt chunk size
    appendLe16(&wav, 1);  // PCM
    appendLe16(&wav, static_cast<quint16>(channels));
    appendLe32(&wav, static_cast<quint32>(sampleRate));
    appendLe32(&wav, byteRate);
    appendLe16(&wav, blockAlign);
    appendLe16(&wav, bitsPerSample);

    wav.append("data", 4);
    appendLe32(&wav, dataBytes);

    for (float sample : samples) {
        const float bounded = std::clamp(sample, -1.0f, 1.0f);
        const int16_t s16 = static_cast<int16_t>(std::lround(bounded * 32767.0f));
        appendLe16(&wav, static_cast<quint16>(s16));
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QString();
    }
    if (file.write(wav) != wav.size()) {
        return QString();
    }
    file.close();
    return path;
}

bool queryEnvelopeWithWait(const QString& path,
                           int64_t sampleStart,
                           int64_t sampleEnd,
                           int columns,
                           QVector<float>* minOut,
                           QVector<float>* maxOut,
                           const QString& variantKey = QString(),
                           const editor::WaveformService::WaveformProcessSettings* settings = nullptr,
                           int timeoutMs = 6000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (editor::WaveformService::instance().queryEnvelope(path,
                                                              sampleStart,
                                                              sampleEnd,
                                                              columns,
                                                              minOut,
                                                              maxOut,
                                                              variantKey,
                                                              settings)) {
            return true;
        }
        QTest::qWait(25);
    }
    return false;
}

float columnPeak(const QVector<float>& mins, const QVector<float>& maxs, int idx) {
    if (idx < 0 || idx >= mins.size() || idx >= maxs.size()) {
        return 0.0f;
    }
    return std::max(std::abs(mins[idx]), std::abs(maxs[idx]));
}

} // namespace

class TestWaveformService : public QObject {
    Q_OBJECT

private slots:
    void testDecodeReadinessAndShape();
    void testEnvelopeTracksAmplitudeAcrossRange();
    void testProcessedVariantCachingAndBaseIsolation();
};

void TestWaveformService::testDecodeReadinessAndShape() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const int sampleRate = 48000;
    QVector<float> samples(sampleRate, 0.35f); // 1s DC signal
    const QString wavPath = writeMonoPcm16Wav(dir.filePath(QStringLiteral("dc.wav")), samples, sampleRate);
    QVERIFY(!wavPath.isEmpty());

    QVector<float> mins;
    QVector<float> maxs;
    QVERIFY(queryEnvelopeWithWait(wavPath, 0, samples.size(), 64, &mins, &maxs));
    QCOMPARE(mins.size(), 64);
    QCOMPARE(maxs.size(), 64);
}

void TestWaveformService::testEnvelopeTracksAmplitudeAcrossRange() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const int sampleRate = 48000;
    QVector<float> samples(sampleRate * 2, 0.0f);
    for (int i = 0; i < sampleRate; ++i) {
        samples[i] = 0.2f;
    }
    for (int i = sampleRate; i < sampleRate * 2; ++i) {
        samples[i] = 0.8f;
    }

    const QString wavPath = writeMonoPcm16Wav(dir.filePath(QStringLiteral("step.wav")), samples, sampleRate);
    QVERIFY(!wavPath.isEmpty());

    QVector<float> leftMins;
    QVector<float> leftMaxs;
    QVector<float> rightMins;
    QVector<float> rightMaxs;
    const int probeColumns = 128;
    QVERIFY(queryEnvelopeWithWait(wavPath, 0, sampleRate, probeColumns, &leftMins, &leftMaxs));
    QVERIFY(queryEnvelopeWithWait(wavPath, sampleRate, samples.size(), probeColumns, &rightMins, &rightMaxs));

    float leftPeakAvg = 0.0f;
    float rightPeakAvg = 0.0f;
    for (int i = 0; i < probeColumns; ++i) {
        leftPeakAvg += columnPeak(leftMins, leftMaxs, i);
        rightPeakAvg += columnPeak(rightMins, rightMaxs, i);
    }
    leftPeakAvg /= static_cast<float>(probeColumns);
    rightPeakAvg /= static_cast<float>(probeColumns);

    QVERIFY(rightPeakAvg > leftPeakAvg + 0.35f);
    QVERIFY(rightPeakAvg > 0.70f);
    QVERIFY(leftPeakAvg < 0.35f);
}

void TestWaveformService::testProcessedVariantCachingAndBaseIsolation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const int sampleRate = 48000;
    QVector<float> samples(sampleRate, 0.25f);
    const QString wavPath = writeMonoPcm16Wav(dir.filePath(QStringLiteral("variant.wav")), samples, sampleRate);
    QVERIFY(!wavPath.isEmpty());

    QVector<float> baseMin;
    QVector<float> baseMax;
    QVERIFY(queryEnvelopeWithWait(wavPath, 0, samples.size(), 32, &baseMin, &baseMax));

    editor::WaveformService::WaveformProcessSettings amplify6;
    amplify6.amplifyEnabled = true;
    amplify6.amplifyDb = 6.0f;

    QVector<float> processedMin;
    QVector<float> processedMax;
    QVERIFY(queryEnvelopeWithWait(wavPath,
                                  0,
                                  samples.size(),
                                  32,
                                  &processedMin,
                                  &processedMax,
                                  QStringLiteral("amp6"),
                                  &amplify6));

    const float basePeak = columnPeak(baseMin, baseMax, 0);
    const float processedPeak = columnPeak(processedMin, processedMax, 0);
    QVERIFY(processedPeak > basePeak + 0.15f);
    QVERIFY(processedPeak <= 1.0f);

    QVector<float> baseMinAfter;
    QVector<float> baseMaxAfter;
    QVERIFY(queryEnvelopeWithWait(wavPath, 0, samples.size(), 32, &baseMinAfter, &baseMaxAfter));
    const float basePeakAfter = columnPeak(baseMinAfter, baseMaxAfter, 0);
    QVERIFY(std::abs(basePeakAfter - basePeak) < 0.02f);
}

QTEST_MAIN(TestWaveformService)
#include "test_waveform_service.moc"
