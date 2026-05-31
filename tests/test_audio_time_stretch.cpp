#include "audio_time_stretch.h"

#include <QtTest/QtTest>

#include <cmath>

class TestAudioTimeStretch : public QObject {
    Q_OBJECT

private slots:
    void testSolaImplementationIsDisabled();
    void testDefaultBackendDoesNotUseSolaFallback();
    void testRubberBandStreamsPastProcessLimit();
};

namespace {
QVector<float> sineStereoFrames(double frequencyHz, int frames, int sampleRate)
{
    QVector<float> samples(frames * 2);
    constexpr double kPi = 3.141592653589793238462643383279502884;
    for (int frame = 0; frame < frames; ++frame) {
        const float value = static_cast<float>(
            0.5 * std::sin((2.0 * kPi * frequencyHz * frame) / sampleRate));
        samples[frame * 2] = value;
        samples[frame * 2 + 1] = value;
    }
    return samples;
}

QVector<float> sineStereo(double frequencyHz, double seconds, int sampleRate)
{
    const int frames = static_cast<int>(std::llround(seconds * sampleRate));
    return sineStereoFrames(frequencyHz, frames, sampleRate);
}

double estimateFrequencyZeroCrossings(const QVector<float>& stereoSamples, int sampleRate)
{
    int crossings = 0;
    float previous = stereoSamples.isEmpty() ? 0.0f : stereoSamples.constFirst();
    for (int frame = 1; frame < stereoSamples.size() / 2; ++frame) {
        const float current = stereoSamples.at(frame * 2);
        if (previous < 0.0f && current >= 0.0f) {
            ++crossings;
        }
        previous = current;
    }
    const double seconds = static_cast<double>(stereoSamples.size() / 2) / sampleRate;
    return seconds > 0.0 ? static_cast<double>(crossings) / seconds : 0.0;
}
}

void TestAudioTimeStretch::testSolaImplementationIsDisabled()
{
    constexpr int sampleRate = 48000;
    constexpr double sourceFrequency = 440.0;
    const QVector<float> input = sineStereo(sourceFrequency, 2.0, sampleRate);

    for (const int speed : {2, 3}) {
        const QVector<float> stretched = timeStretchPreservePitchSola(input, 2, speed);
        QVERIFY2(stretched.isEmpty(), "SOLA is disabled and must not synthesize playback audio");
    }
}

void TestAudioTimeStretch::testDefaultBackendDoesNotUseSolaFallback()
{
    constexpr int sampleRate = 48000;
    constexpr double sourceFrequency = 440.0;
    const QVector<float> input = sineStereo(sourceFrequency, 2.0, sampleRate);

    for (const int speed : {2, 3}) {
        const QVector<float> stretched =
            timeStretchPreservePitch(input, 2, sampleRate, speed);
#if JCUT_HAVE_RUBBERBAND
        const int expectedFrames = (input.size() / 2) / speed;
        const int actualFrames = stretched.size() / 2;
        QVERIFY2(std::abs(actualFrames - expectedFrames) <= 4096,
                 qPrintable(QStringLiteral("speed=%1 actual=%2 expected=%3")
                                .arg(speed)
                                .arg(actualFrames)
                                .arg(expectedFrames)));

        const double measuredFrequency = estimateFrequencyZeroCrossings(stretched, sampleRate);
        QVERIFY2(std::abs(measuredFrequency - sourceFrequency) < 25.0,
                 qPrintable(QStringLiteral("speed=%1 frequency=%2").arg(speed).arg(measuredFrequency)));
#else
        QVERIFY2(stretched.isEmpty(), "Default time-stretch must not fall back to SOLA");
#endif

        const QVector<float> explicitSola =
            timeStretchPreservePitch(input,
                                     2,
                                     sampleRate,
                                     speed,
                                     AudioTimeStretchBackend::Sola);
        QVERIFY2(explicitSola.isEmpty(), "SOLA backend is disabled for the unified time-stretch entrypoint");
    }
}

void TestAudioTimeStretch::testRubberBandStreamsPastProcessLimit()
{
    constexpr int sampleRate = 48000;
    constexpr int framesPastRubberBandLimit = 530000;
    constexpr double speed = 1.5;
    const QVector<float> input = sineStereoFrames(330.0, framesPastRubberBandLimit, sampleRate);

    const QVector<float> stretched =
        timeStretchPreservePitch(input, 2, sampleRate, speed, AudioTimeStretchBackend::RubberBand);
#if JCUT_HAVE_RUBBERBAND
    const int expectedFrames = static_cast<int>(std::llround(framesPastRubberBandLimit / speed));
    const int actualFrames = stretched.size() / 2;
    QVERIFY2(!stretched.isEmpty(), "Rubber Band output should be produced for inputs longer than one process block");
    QVERIFY2(std::abs(actualFrames - expectedFrames) <= 4096,
             qPrintable(QStringLiteral("actual=%1 expected=%2").arg(actualFrames).arg(expectedFrames)));
#else
    QVERIFY(stretched.isEmpty());
#endif
}

QTEST_MAIN(TestAudioTimeStretch)
#include "test_audio_time_stretch.moc"
