#include <QtTest/QtTest>

#include "../clip_serialization.h"
#include "../editor_effect_presets.h"
#include "../render_vulkan_shared.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QFile>
#include <QTemporaryDir>

class TestEffectPresets : public QObject {
    Q_OBJECT

private slots:
    void clipSerializationPersistsEffectPresetState();
    void clipSerializationPersistsGeneratedClipRoleState();
    void samMaskMatteFactoryKeepsSourceTimingLocked();
    void alternatingMotionBackgroundFactoryCreatesVisualOnlySynthClip();
    void speakerTitleFactoryBuildsLowerThirdsForSpeakerChanges();
    void tickerPresetProducesAlternatingRowsAcrossOutput();
    void alternatingMotionBackgroundCoversOutputWithMovingRows();
    void orbitPresetProducesRequestedCopiesAroundCenter();
    void newsLowerThirdPresetBuildsFlyInHoldFlyOutKeyframes();
};

void TestEffectPresets::clipSerializationPersistsEffectPresetState()
{
    TimelineClip clip;
    clip.id = QStringLiteral("logo");
    clip.filePath = QStringLiteral("logo.png");
    clip.label = QStringLiteral("Logo");
    clip.mediaType = ClipMediaType::Image;
    clip.maskEnabled = true;
    clip.maskFramesDir = QStringLiteral("/tmp/masks");
    clip.maskForegroundLayerEnabled = true;
    clip.effectPreset = ClipEffectPreset::NewsLogoTicker;
    clip.effectRows = 32;
    clip.effectSpeed = 1.75;
    clip.effectScale = 0.85;
    clip.effectAlternateDirection = true;

    const QJsonObject json = editor::clipToJson(clip);
    QCOMPARE(json.value(QStringLiteral("maskForegroundLayerEnabled")).toBool(), true);
    QCOMPARE(json.value(QStringLiteral("effectPreset")).toString(), QStringLiteral("news_logo_ticker"));
    QCOMPARE(json.value(QStringLiteral("effectRows")).toInt(), 32);

    const TimelineClip loaded = editor::clipFromJson(json);
    QCOMPARE(loaded.maskForegroundLayerEnabled, true);
    QCOMPARE(loaded.effectPreset, ClipEffectPreset::NewsLogoTicker);
    QCOMPARE(loaded.effectRows, 32);
    QVERIFY(std::abs(loaded.effectSpeed - 1.75) < 0.000001);
    QVERIFY(std::abs(loaded.effectScale - 0.85) < 0.000001);
    QCOMPARE(loaded.effectAlternateDirection, true);
}

void TestEffectPresets::clipSerializationPersistsGeneratedClipRoleState()
{
    TimelineClip clip;
    clip.id = QStringLiteral("source-mask");
    clip.clipRole = ClipRole::MaskMatte;
    clip.linkedSourceClipId = QStringLiteral("source");
    clip.generatedFromMaskId = QStringLiteral("/tmp/source_sam3_person_binary_masks");
    clip.syncLockedToSource = true;
    clip.filePath = QStringLiteral("source.mp4");
    clip.mediaType = ClipMediaType::Video;
    clip.maskEnabled = true;
    clip.maskFramesDir = clip.generatedFromMaskId;
    clip.effectPreset = ClipEffectPreset::AlternatingMotionBackground;

    const QJsonObject json = editor::clipToJson(clip);
    QCOMPARE(json.value(QStringLiteral("clipRole")).toString(), QStringLiteral("mask_matte"));
    QCOMPARE(json.value(QStringLiteral("linkedSourceClipId")).toString(), QStringLiteral("source"));
    QCOMPARE(json.value(QStringLiteral("generatedFromMaskId")).toString(),
             QStringLiteral("/tmp/source_sam3_person_binary_masks"));
    QCOMPARE(json.value(QStringLiteral("syncLockedToSource")).toBool(), true);
    QCOMPARE(json.value(QStringLiteral("effectPreset")).toString(),
             QStringLiteral("alternating_motion_background"));

    const TimelineClip loaded = editor::clipFromJson(json);
    QCOMPARE(loaded.clipRole, ClipRole::MaskMatte);
    QCOMPARE(loaded.linkedSourceClipId, QStringLiteral("source"));
    QCOMPARE(loaded.generatedFromMaskId, QStringLiteral("/tmp/source_sam3_person_binary_masks"));
    QCOMPARE(loaded.syncLockedToSource, true);
    QCOMPARE(loaded.effectPreset, ClipEffectPreset::AlternatingMotionBackground);
}

void TestEffectPresets::samMaskMatteFactoryKeepsSourceTimingLocked()
{
    TimelineClip source;
    source.id = QStringLiteral("shot-01");
    source.filePath = QStringLiteral("/media/shot-01.mp4");
    source.label = QStringLiteral("Shot 01");
    source.mediaType = ClipMediaType::Video;
    source.hasAudio = true;
    source.audioEnabled = true;
    source.audioBusId = QStringLiteral("main");
    source.sourceFps = 23.976;
    source.sourceDurationFrames = 2400;
    source.sourceInFrame = 120;
    source.sourceInSubframeSamples = 9;
    source.startFrame = 300;
    source.startSubframeSamples = 11;
    source.durationFrames = 600;
    source.durationSubframeSamples = 13;
    source.trackIndex = 2;
    source.playbackRate = 0.75;
    source.maskEnabled = true;
    source.maskFramesDir = QStringLiteral("/tmp/shot-01_sam3_person_binary_masks");
    source.maskForegroundLayerEnabled = true;
    source.effectPreset = ClipEffectPreset::NewsLogoTicker;

    const TimelineClip matte = makeSamMaskMatteClip(source);
    QCOMPARE(matte.id, QStringLiteral("shot-01-mask-matte"));
    QCOMPARE(matte.clipRole, ClipRole::MaskMatte);
    QCOMPARE(matte.linkedSourceClipId, source.id);
    QCOMPARE(matte.generatedFromMaskId, source.maskFramesDir);
    QVERIFY(matte.syncLockedToSource);
    QCOMPARE(matte.filePath, source.filePath);
    QCOMPARE(matte.mediaType, source.mediaType);
    QCOMPARE(matte.sourceFps, source.sourceFps);
    QCOMPARE(matte.sourceDurationFrames, source.sourceDurationFrames);
    QCOMPARE(matte.sourceInFrame, source.sourceInFrame);
    QCOMPARE(matte.sourceInSubframeSamples, source.sourceInSubframeSamples);
    QCOMPARE(matte.startFrame, source.startFrame);
    QCOMPARE(matte.startSubframeSamples, source.startSubframeSamples);
    QCOMPARE(matte.durationFrames, source.durationFrames);
    QCOMPARE(matte.durationSubframeSamples, source.durationSubframeSamples);
    QCOMPARE(matte.trackIndex, source.trackIndex);
    QCOMPARE(matte.playbackRate, source.playbackRate);
    QVERIFY(!matte.hasAudio);
    QVERIFY(!matte.audioEnabled);
    QVERIFY(!matte.audioLinkedToVideo);
    QVERIFY(matte.maskEnabled);
    QCOMPARE(matte.maskFramesDir, source.maskFramesDir);
    QVERIFY(matte.maskShowOnly);
    QVERIFY(!matte.maskForegroundLayerEnabled);
    QCOMPARE(matte.effectPreset, ClipEffectPreset::None);
}

void TestEffectPresets::alternatingMotionBackgroundFactoryCreatesVisualOnlySynthClip()
{
    TimelineClip source;
    source.id = QStringLiteral("logo");
    source.filePath = QStringLiteral("/media/logo.png");
    source.label = QStringLiteral("Soul House");
    source.mediaType = ClipMediaType::Image;
    source.sourceInFrame = 0;
    source.startFrame = 120;
    source.durationFrames = 240;
    source.trackIndex = 1;
    source.hasAudio = true;
    source.audioEnabled = true;
    source.maskEnabled = true;
    source.maskFramesDir = QStringLiteral("/tmp/masks");

    const TimelineClip synth = makeAlternatingMotionBackgroundClip(source, 3);
    QVERIFY(!synth.id.isEmpty());
    QVERIFY(synth.id != source.id);
    QCOMPARE(synth.clipRole, ClipRole::EffectSynth);
    QCOMPARE(synth.linkedSourceClipId, source.id);
    QCOMPARE(synth.filePath, source.filePath);
    QCOMPARE(synth.startFrame, source.startFrame);
    QCOMPARE(synth.durationFrames, source.durationFrames);
    QCOMPARE(synth.trackIndex, 3);
    QVERIFY(!synth.hasAudio);
    QVERIFY(!synth.audioEnabled);
    QVERIFY(!synth.maskEnabled);
    QVERIFY(synth.maskFramesDir.isEmpty());
    QCOMPARE(synth.effectPreset, ClipEffectPreset::AlternatingMotionBackground);
    QCOMPARE(synth.effectRows, 8);
    QVERIFY(synth.effectAlternateDirection);
}

void TestEffectPresets::speakerTitleFactoryBuildsLowerThirdsForSpeakerChanges()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "Unable to create temporary transcript directory.");
    const QString transcriptPath = dir.filePath(QStringLiteral("clip.transcript.json"));
    QFile file(transcriptPath);
    QVERIFY2(file.open(QIODevice::WriteOnly), "Unable to write transcript profile fixture.");
    file.write(R"({
        "speaker_profiles": {
            "S1": {"name": "Jane Doe", "organization": "Director"},
            "S2": {"name": "John Roe", "organization": "Producer"}
        },
        "segments": []
    })");
    file.close();

    TimelineClip source;
    source.id = QStringLiteral("clip-01");
    source.mediaType = ClipMediaType::Video;
    source.sourceInFrame = 100;
    source.startFrame = 1000;
    source.durationFrames = 300;
    source.playbackRate = 1.0;

    TranscriptSection section;
    section.startFrame = 110;
    section.endFrame = 172;
    section.words.push_back(TranscriptWord{110, 112, QStringLiteral("S1"), QStringLiteral("hello"), false});
    section.words.push_back(TranscriptWord{120, 122, QStringLiteral("S1"), QStringLiteral("again"), false});
    section.words.push_back(TranscriptWord{140, 142, QStringLiteral("S2"), QStringLiteral("there"), false});
    section.words.push_back(TranscriptWord{170, 172, QStringLiteral("S1"), QStringLiteral("back"), false});

    const QVector<TimelineClip> titles = makeSpeakerTitleClipsForTranscriptIntroductions(
        source,
        transcriptPath,
        QVector<TranscriptSection>{section},
        4,
        90);

    QCOMPARE(titles.size(), 3);
    QCOMPARE(titles.at(0).clipRole, ClipRole::SpeakerTitle);
    QCOMPARE(titles.at(0).linkedSourceClipId, source.id);
    QVERIFY(titles.at(0).syncLockedToSource);
    QCOMPARE(titles.at(0).mediaType, ClipMediaType::Title);
    QCOMPARE(titles.at(0).trackIndex, 4);
    QCOMPARE(titles.at(0).startFrame, int64_t(1010));
    QCOMPARE(titles.at(0).durationFrames, int64_t(90));
    QCOMPARE(titles.at(0).titleKeyframes.constFirst().text, QStringLiteral("Jane Doe - Director"));
    QCOMPARE(titles.at(1).startFrame, int64_t(1040));
    QCOMPARE(titles.at(1).titleKeyframes.constFirst().text, QStringLiteral("John Roe - Producer"));
    QCOMPARE(titles.at(2).startFrame, int64_t(1070));
    QCOMPARE(titles.at(2).titleKeyframes.constFirst().text, QStringLiteral("Jane Doe - Director"));
}

void TestEffectPresets::tickerPresetProducesAlternatingRowsAcrossOutput()
{
    TimelineClip clip;
    clip.effectPreset = ClipEffectPreset::NewsLogoTicker;
    clip.effectRows = 4;
    clip.effectSpeed = 1.0;
    clip.effectScale = 1.0;
    clip.effectAlternateDirection = true;

    const QRectF output(0.0, 0.0, 1080.0, 1920.0);
    const QVector<QRectF> frame0 =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(200, 100), 0.0);
    const QVector<QRectF> frame10 =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(200, 100), 10.0);

    QVERIFY(frame0.size() > clip.effectRows);
    QCOMPARE(frame0.constFirst().height(), output.height() / clip.effectRows * 0.78);
    QVERIFY(frame0.constFirst().width() > frame0.constFirst().height());
    QVERIFY(std::abs(frame10.constFirst().x() - frame0.constFirst().x()) > 0.001);

    const qreal rowHeight = output.height() / clip.effectRows;
    auto firstRectInRow = [&](const QVector<QRectF>& rects, int row) {
        const qreal rowTop = output.top() + row * rowHeight;
        const qreal rowBottom = rowTop + rowHeight;
        for (const QRectF& rect : rects) {
            if (rect.center().y() >= rowTop && rect.center().y() < rowBottom) {
                return rect;
            }
        }
        return QRectF();
    };
    const QRectF row0Frame0 = firstRectInRow(frame0, 0);
    const QRectF row0Frame10 = firstRectInRow(frame10, 0);
    const QRectF row1Frame0 = firstRectInRow(frame0, 1);
    const QRectF row1Frame10 = firstRectInRow(frame10, 1);
    QVERIFY(row0Frame0.isValid());
    QVERIFY(row1Frame0.isValid());
    QVERIFY((row0Frame10.x() - row0Frame0.x()) * (row1Frame10.x() - row1Frame0.x()) < 0.0);
}

void TestEffectPresets::alternatingMotionBackgroundCoversOutputWithMovingRows()
{
    TimelineClip clip;
    clip.clipRole = ClipRole::EffectSynth;
    clip.effectPreset = ClipEffectPreset::AlternatingMotionBackground;
    clip.effectRows = 5;
    clip.effectSpeed = 1.2;
    clip.effectScale = 1.0;
    clip.effectAlternateDirection = true;

    const QRectF output(0.0, 0.0, 1280.0, 720.0);
    const QVector<QRectF> frame0 =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(256, 128), 0.0);
    const QVector<QRectF> frame30 =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(256, 128), 30.0);

    QVERIFY(frame0.size() > clip.effectRows);
    QVERIFY(frame0.constFirst().height() > output.height() / clip.effectRows);
    QVERIFY(frame0.constFirst().width() > frame0.constFirst().height());
    QVERIFY(std::abs(frame30.constFirst().x() - frame0.constFirst().x()) > 0.001);

    const qreal rowHeight = output.height() / clip.effectRows;
    auto firstRectInRow = [&](const QVector<QRectF>& rects, int row) {
        const qreal rowTop = output.top() + row * rowHeight;
        const qreal rowBottom = rowTop + rowHeight;
        for (const QRectF& rect : rects) {
            if (rect.center().y() >= rowTop && rect.center().y() < rowBottom) {
                return rect;
            }
        }
        return QRectF();
    };

    const QRectF row0Frame0 = firstRectInRow(frame0, 0);
    const QRectF row0Frame30 = firstRectInRow(frame30, 0);
    const QRectF row1Frame0 = firstRectInRow(frame0, 1);
    const QRectF row1Frame30 = firstRectInRow(frame30, 1);
    QVERIFY(row0Frame0.isValid());
    QVERIFY(row1Frame0.isValid());
    QVERIFY((row0Frame30.x() - row0Frame0.x()) * (row1Frame30.x() - row1Frame0.x()) < 0.0);
}

void TestEffectPresets::orbitPresetProducesRequestedCopiesAroundCenter()
{
    TimelineClip clip;
    clip.effectPreset = ClipEffectPreset::PersonOrbit;
    clip.effectRows = 12;
    clip.effectSpeed = 0.0;
    clip.effectScale = 1.0;

    const QRectF output(0.0, 0.0, 1080.0, 1920.0);
    const QVector<QRectF> rects =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(100, 100), 0.0);

    QCOMPARE(rects.size(), 12);
    const QPointF center = output.center();
    qreal minDistance = std::numeric_limits<qreal>::max();
    qreal maxDistance = 0.0;
    for (const QRectF& rect : rects) {
        const qreal dx = rect.center().x() - center.x();
        const qreal dy = rect.center().y() - center.y();
        const qreal distance = std::sqrt((dx * dx) + (dy * dy));
        minDistance = std::min(minDistance, distance);
        maxDistance = std::max(maxDistance, distance);
    }
    QVERIFY(minDistance > 0.0);
    QVERIFY(maxDistance > minDistance);
}

void TestEffectPresets::newsLowerThirdPresetBuildsFlyInHoldFlyOutKeyframes()
{
    TimelineClip clip;
    clip.mediaType = ClipMediaType::Title;
    clip.label = QStringLiteral("Jane Doe");
    clip.durationFrames = 120;

    QVERIFY(applyNewsLowerThirdFlyInPreset(clip));
    QCOMPARE(clip.titleKeyframes.size(), 4);
    QCOMPARE(clip.titleKeyframes.at(0).frame, int64_t(0));
    QVERIFY(clip.titleKeyframes.at(1).frame > clip.titleKeyframes.at(0).frame);
    QVERIFY(clip.titleKeyframes.at(2).frame > clip.titleKeyframes.at(1).frame);
    QCOMPARE(clip.titleKeyframes.at(3).frame, int64_t(119));
    QCOMPARE(clip.titleKeyframes.at(0).text, QStringLiteral("Jane Doe"));
    QVERIFY(clip.titleKeyframes.at(0).translationX < clip.titleKeyframes.at(1).translationX);
    QVERIFY(clip.titleKeyframes.at(3).translationX > clip.titleKeyframes.at(2).translationX);
    QCOMPARE(clip.titleKeyframes.at(0).opacity, 0.0);
    QCOMPARE(clip.titleKeyframes.at(1).opacity, 1.0);
    QCOMPARE(clip.titleKeyframes.at(2).opacity, 1.0);
    QCOMPARE(clip.titleKeyframes.at(3).opacity, 0.0);
    QVERIFY(clip.titleKeyframes.at(1).windowEnabled);
}

QTEST_MAIN(TestEffectPresets)
#include "test_effect_presets.moc"
