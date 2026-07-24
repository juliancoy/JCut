#include "speaker_title_core.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jcut {
namespace {

constexpr double kPi = 3.14159265358979323846;

std::string profileString(
    const nlohmann::json& root,
    const std::string& speaker,
    const char* key)
{
    const nlohmann::json& value = root
        .value("speaker_profiles", nlohmann::json::object())
        .value(speaker, nlohmann::json::object())
        .value(key, nlohmann::json{});
    return value.is_string() ? value.get<std::string>() : std::string{};
}

std::int64_t boundedFrame(std::int64_t value, std::int64_t duration)
{
    return std::clamp<std::int64_t>(
        value, 0, std::max<std::int64_t>(0, duration - 1));
}

} // namespace

bool applySpeakerTitleFlyInCore(
    EditorClip* clip,
    const SpeakerTitleFlyInSettingsCore& settings)
{
    if (!clip || clip->mediaKind != "title") return false;
    const std::int64_t duration =
        std::max<std::int64_t>(30, clip->durationFrames);
    const std::int64_t inEnd = std::min<std::int64_t>(
        duration - 1, std::max<std::int64_t>(1, settings.flyInFrames));
    const std::int64_t holdEnd = std::min<std::int64_t>(
        duration - 1,
        std::max<std::int64_t>(
            inEnd + 1,
            duration - std::max<std::int64_t>(1, settings.flyOutFrames)));
    const std::int64_t outEnd = duration - 1;
    EditorTitleKeyframe base = clip->titleKeyframes.empty()
        ? EditorTitleKeyframe{} : clip->titleKeyframes.front();
    if (base.text.empty()) base.text =
        clip->label.empty() ? "Speaker Name" : clip->label;
    constexpr double defaultY = 112.0;
    constexpr double offscreenX = 520.0;
    constexpr double verticalOffset = 190.0;
    if (std::abs(base.translationY) < 4.0) base.translationY = defaultY;
    base.windowEnabled = settings.titleBackgroundEnabled;
    base.windowOpacity = std::max(base.windowOpacity, 0.55);
    base.windowPadding = std::max(base.windowPadding, 22.0);
    base.windowFrameEnabled = settings.titleBackgroundEnabled;
    base.windowFrameOpacity = std::max(base.windowFrameOpacity, 0.78);
    base.windowFrameWidth = std::max(base.windowFrameWidth, 2.0);
    base.dropShadowEnabled = !settings.titleExtrude3D;
    base.dropShadowOpacity = settings.titleExtrude3D
        ? 0.0 : std::max(base.dropShadowOpacity, 0.72);
    base.textMaterialStyle = settings.titleTextMaterialStyle;
    base.textPatternImagePath = settings.titleTextPatternImagePath;
    base.textPatternScale =
        std::clamp(settings.titlePatternScale, 0.10, 8.0);
    base.windowFrameMaterialStyle = settings.titleBorderMaterialStyle;
    base.windowFramePatternImagePath = settings.titleBorderPatternImagePath;
    base.windowFramePatternScale =
        std::clamp(settings.titlePatternScale, 0.10, 8.0);
    base.vulkan3DExtrudeEnabled = settings.titleExtrude3D;
    base.textExtrudeMode =
        settings.titleExtrude3D ? settings.titleExtrudeMode : "none";
    base.vulkan3DEnabled = settings.titleExtrude3D ||
        std::abs(settings.rotationStartXDegrees) > 0.001 ||
        std::abs(settings.rotationStartYDegrees) > 0.001 ||
        std::abs(settings.rotationStartZDegrees) > 0.001;
    base.vulkan3DExtrudeDepth =
        std::clamp(settings.titleExtrudeDepth, 0.0, 2.0);
    base.vulkan3DBevelScale =
        std::clamp(settings.titleBevelScale, 0.0, 2.0);
    base.linearInterpolation = true;

    if (settings.style == SpeakerTitleFlyInStyleCore::WrapAroundSpeaker) {
        const std::int64_t flyFrames = std::clamp<std::int64_t>(
            settings.flyInFrames, 4, std::max<std::int64_t>(4, duration - 1));
        const std::int64_t arriveFrame = std::clamp<std::int64_t>(
            flyFrames + std::max<std::int64_t>(2, flyFrames / 5),
            std::min<std::int64_t>(duration - 1, flyFrames + 1),
            duration - 1);
        const std::int64_t holdFrame = std::min<std::int64_t>(
            duration - 1,
            std::max<std::int64_t>(
                arriveFrame + 1,
                duration - std::max<std::int64_t>(1, settings.flyOutFrames)));
        const double radius = std::clamp(settings.wrapRadius, 0.24, 1.80);
        const double depth = std::clamp(settings.wrapDepth, 0.0, 1.0);
        const double startAngle =
            settings.wrapStartAngleDegrees * kPi / 180.0;
        const double endAngle =
            settings.wrapEndAngleDegrees * kPi / 180.0;
        const double pitch =
            std::clamp(settings.wrapPitchDegrees, -80.0, 80.0) *
            kPi / 180.0;
        const double roll = settings.wrapRollDegrees * kPi / 180.0;
        std::vector<EditorTitleKeyframe> keyframes;
        constexpr int samples = 19;
        for (int index = 0; index < samples; ++index) {
            const double rawT =
                static_cast<double>(index) / static_cast<double>(samples - 1);
            const double t = rawT * rawT * rawT *
                (rawT * (rawT * 6.0 - 15.0) + 10.0);
            const double angle = startAngle + (endAngle - startAngle) * t;
            double x = std::sin(angle) * radius;
            double y = std::cos(angle) * std::sin(pitch);
            const double z = -std::cos(angle) * std::cos(pitch);
            const double rotatedX = x * std::cos(roll) - y * std::sin(roll);
            const double rotatedY = x * std::sin(roll) + y * std::cos(roll);
            x = rotatedX;
            y = rotatedY;
            const double scale =
                std::clamp(1.0 + z * depth * 0.26, 0.48, 1.22);
            const bool behind = z < -0.42 &&
                std::abs(x) < radius * 0.42;
            const double visibility = behind
                ? std::clamp(0.12 - depth * 0.12, 0.0, 0.12)
                : std::clamp(0.72 + z * depth * 0.28, 0.18, 1.0);
            EditorTitleKeyframe keyframe = base;
            keyframe.frame = boundedFrame(
                static_cast<std::int64_t>(std::llround(rawT * flyFrames)),
                duration);
            keyframe.translationX = std::clamp(x * 360.0, -620.0, 620.0);
            keyframe.translationY =
                std::clamp(y * 160.0 - z * 34.0, -220.0, 220.0);
            keyframe.fontSize = std::min(base.fontSize, 30.0) * scale;
            keyframe.opacity = visibility;
            keyframe.vulkan3DEnabled = true;
            keyframe.vulkan3DYawDegrees = std::clamp(
                std::clamp(-angle * 180.0 / kPi * 0.52, -62.0, 62.0) +
                    settings.rotationStartYDegrees * (1.0 - t),
                -720.0, 720.0);
            keyframe.vulkan3DPitchDegrees = std::clamp(
                settings.wrapPitchDegrees +
                    settings.rotationStartXDegrees * (1.0 - t),
                -720.0, 720.0);
            keyframe.vulkan3DRollDegrees = std::clamp(
                settings.wrapRollDegrees +
                    settings.rotationStartZDegrees * (1.0 - t),
                -720.0, 720.0);
            keyframe.vulkan3DDepth = z * depth;
            keyframe.vulkan3DScale = scale;
            if (behind) {
                keyframe.windowOpacity = 0.0;
                keyframe.windowFrameOpacity = 0.0;
                keyframe.dropShadowOpacity = 0.0;
            }
            keyframes.push_back(std::move(keyframe));
        }
        keyframes.front().opacity = 0.0;
        keyframes.front().windowOpacity = 0.0;
        keyframes.front().windowFrameOpacity = 0.0;
        EditorTitleKeyframe arrived = base;
        arrived.frame = arriveFrame;
        arrived.translationX = 0.0;
        arrived.opacity = 1.0;
        arrived.vulkan3DEnabled = arrived.vulkan3DExtrudeEnabled;
        arrived.vulkan3DYawDegrees = 0.0;
        arrived.vulkan3DPitchDegrees = 0.0;
        arrived.vulkan3DRollDegrees = 0.0;
        arrived.vulkan3DDepth = 0.0;
        arrived.vulkan3DScale = 1.0;
        EditorTitleKeyframe hold = arrived;
        hold.frame = holdFrame;
        EditorTitleKeyframe after = arrived;
        after.frame = outEnd;
        after.translationX = offscreenX;
        after.opacity = 0.0;
        keyframes.push_back(std::move(arrived));
        keyframes.push_back(std::move(hold));
        keyframes.push_back(std::move(after));
        std::sort(keyframes.begin(), keyframes.end(),
                  [](const auto& lhs, const auto& rhs) {
                      return lhs.frame < rhs.frame;
                  });
        clip->titleKeyframes = std::move(keyframes);
        return true;
    }

    EditorTitleKeyframe before = base;
    before.frame = 0;
    before.opacity = 0.0;
    before.vulkan3DPitchDegrees = settings.rotationStartXDegrees;
    before.vulkan3DYawDegrees = settings.rotationStartYDegrees;
    before.vulkan3DRollDegrees = settings.rotationStartZDegrees;
    EditorTitleKeyframe arrived = base;
    arrived.frame = inEnd;
    arrived.translationX = 0.0;
    arrived.opacity = 1.0;
    arrived.vulkan3DPitchDegrees = 0.0;
    arrived.vulkan3DYawDegrees = 0.0;
    arrived.vulkan3DRollDegrees = 0.0;
    EditorTitleKeyframe hold = arrived;
    hold.frame = holdEnd;
    EditorTitleKeyframe after = arrived;
    after.frame = outEnd;
    after.opacity = 0.0;
    switch (settings.style) {
    case SpeakerTitleFlyInStyleCore::SlideFromRight:
        before.translationX = offscreenX;
        after.translationX = -offscreenX;
        break;
    case SpeakerTitleFlyInStyleCore::RiseFromBottom:
        before.translationY = arrived.translationY + verticalOffset;
        after.translationY = arrived.translationY + verticalOffset * 0.65;
        break;
    case SpeakerTitleFlyInStyleCore::DropFromTop:
        before.translationY = arrived.translationY - verticalOffset;
        after.translationY = arrived.translationY + verticalOffset * 0.65;
        break;
    case SpeakerTitleFlyInStyleCore::SlideFromLeft:
    default:
        before.translationX = -offscreenX;
        after.translationX = offscreenX;
        break;
    }
    clip->titleKeyframes = {
        std::move(before), std::move(arrived),
        std::move(hold), std::move(after)};
    return true;
}

std::vector<EditorClip> makeSpeakerTitleClipsCore(const EditorClip& sourceClip,
                                                  const TranscriptDocumentCore& transcript, int targetTrackId,
                                                  const SpeakerTitleFlyInSettingsCore& settings) {
    std::vector<EditorClip> clips;
    if (sourceClip.durationFrames <= 0)
        return clips;
    TranscriptRowBuildOptions options;
    options.timing = {30.0, 0, 0, 0};
    options.adjustOverlaps = false;
    options.insertGaps = false;
    options.includeOutsideActiveCut = true;
    std::vector<TranscriptRow> rows = transcript.rows(options);
    const auto profiles = transcript.speakerProfiles();
    const std::int64_t sourceStart = sourceClip.sourceInFrame;
    const std::int64_t sourceDuration =
        sourceClip.sourceDurationFrames > 0
            ? sourceClip.sourceDurationFrames
            : static_cast<std::int64_t>(
                  std::llround(sourceClip.durationFrames * std::max(0.001, sourceClip.playbackRate)));
    const std::int64_t sourceEnd = sourceStart + std::max<std::int64_t>(1, sourceDuration);
    const std::int64_t timelineEnd = sourceClip.startFrame + sourceClip.durationFrames;
    const double rate = std::max(0.001, sourceClip.playbackRate);
    const std::int64_t titleDuration = std::max<std::int64_t>(30, settings.titleDurationFrames);
    std::vector<TranscriptRow> keptRows;
    for (const TranscriptRow& row : rows) {
        if (!row.gap && !row.skipped && !row.text.empty() && !row.speakerId.empty() &&
            row.sourceStartFrame >= sourceStart && row.sourceStartFrame < sourceEnd) {
            keptRows.push_back(row);
        }
    }
    std::sort(keptRows.begin(), keptRows.end(), [](const TranscriptRow& a, const TranscriptRow& b) {
        return a.sourceStartFrame != b.sourceStartFrame ? a.sourceStartFrame < b.sourceStartFrame
                                                        : a.sourceEndFrame < b.sourceEndFrame;
    });
    if (keptRows.empty()) {
        return clips;
    }
    const auto timelineFrameForSource = [&](std::int64_t sourceFrame) {
        return std::clamp<std::int64_t>(sourceClip.startFrame +
                                            static_cast<std::int64_t>(std::llround((sourceFrame - sourceStart) / rate)),
                                        sourceClip.startFrame, timelineEnd - 1);
    };
    struct TitleEvent {
        std::string speakerId;
        std::int64_t startFrame = 0;
        int priority = 0;
    };
    std::vector<TitleEvent> events;
    std::size_t runStart = 0;
    for (std::size_t index = 1; index <= keptRows.size(); ++index) {
        const bool runEnded = index == keptRows.size() || keptRows[index].speakerId != keptRows[runStart].speakerId;
        if (!runEnded)
            continue;
        const TranscriptRow& first = keptRows[runStart];
        const std::int64_t runStartFrame = timelineFrameForSource(first.sourceStartFrame);
        events.push_back({first.speakerId, runStartFrame, 2});
        if (settings.showAtSectionEnd) {
            std::int64_t runEndSourceFrame = first.sourceEndFrame;
            for (std::size_t runIndex = runStart + 1; runIndex < index; ++runIndex) {
                runEndSourceFrame = std::max(runEndSourceFrame, keptRows[runIndex].sourceEndFrame);
            }
            const std::int64_t runEndExclusive = std::clamp<std::int64_t>(
                sourceClip.startFrame +
                    static_cast<std::int64_t>(
                        std::ceil((std::max(first.sourceStartFrame, runEndSourceFrame) - sourceStart + 1) / rate)),
                sourceClip.startFrame + 1, timelineEnd);
            events.push_back({first.speakerId, std::max(runStartFrame, runEndExclusive - titleDuration), 0});
        }
        runStart = index;
    }
    const std::int64_t cadence = std::max<std::int64_t>(0, settings.cadenceFrames);
    if (cadence > 0) {
        std::ptrdiff_t rowIndex = -1;
        for (std::int64_t cadenceFrame = sourceClip.startFrame + cadence; cadenceFrame < timelineEnd;
             cadenceFrame += cadence) {
            const std::int64_t cadenceSourceFrame =
                sourceStart + static_cast<std::int64_t>(std::llround((cadenceFrame - sourceClip.startFrame) * rate));
            while (rowIndex + 1 < static_cast<std::ptrdiff_t>(keptRows.size()) &&
                   keptRows[static_cast<std::size_t>(rowIndex + 1)].sourceStartFrame <= cadenceSourceFrame) {
                ++rowIndex;
            }
            if (rowIndex < 0)
                continue;
            const bool titleAlreadyScheduled =
                std::any_of(events.cbegin(), events.cend(), [&](const TitleEvent& event) {
                    return cadenceFrame >= event.startFrame && cadenceFrame < event.startFrame + titleDuration;
                });
            if (!titleAlreadyScheduled) {
                events.push_back({keptRows[static_cast<std::size_t>(rowIndex)].speakerId, cadenceFrame, 1});
            }
        }
    }
    std::sort(events.begin(), events.end(), [](const TitleEvent& a, const TitleEvent& b) {
        return a.startFrame != b.startFrame ? a.startFrame < b.startFrame : a.priority > b.priority;
    });
    events.erase(std::unique(events.begin(), events.end(),
                             [](const TitleEvent& a, const TitleEvent& b) { return a.startFrame == b.startFrame; }),
                 events.end());

    for (std::size_t eventIndex = 0; eventIndex < events.size(); ++eventIndex) {
        const TitleEvent& event = events[eventIndex];
        const auto profile = std::find_if(profiles.begin(), profiles.end(),
                                          [&](const auto& candidate) { return candidate.id == event.speakerId; });
        std::vector<std::string> lines;
        if (settings.showSpeakerName) {
            lines.push_back(profile != profiles.end() && !profile->name.empty() ? profile->name : event.speakerId);
        }
        if (settings.showSpeakerOrganization && profile != profiles.end() && !profile->organization.empty()) {
            lines.push_back(profile->organization);
        }
        if (lines.empty())
            continue;
        std::string title = lines.front();
        for (std::size_t index = 1; index < lines.size(); ++index) {
            title += "\n" + lines[index];
        }
        const std::int64_t start = event.startFrame;
        const std::int64_t nextEventFrame =
            eventIndex + 1 < events.size() ? events[eventIndex + 1].startFrame : timelineEnd;
        const std::int64_t duration = std::max<std::int64_t>(
            1, std::min({titleDuration, timelineEnd - start, std::max<std::int64_t>(1, nextEventFrame - start)}));
        EditorClip titleClip;
        titleClip.trackId = targetTrackId;
        titleClip.label = "Speaker: " + title;
        titleClip.startFrame = static_cast<int>(start);
        titleClip.durationFrames = static_cast<int>(duration);
        titleClip.sourceDurationFrames = duration;
        titleClip.mediaKind = "title";
        titleClip.videoEnabled = true;
        titleClip.audioEnabled = false;
        titleClip.audioPresenceKnown = true;
        titleClip.hasAudio = false;
        titleClip.clipRole = "speaker_title";
        titleClip.linkedSourceClipId = sourceClip.persistentId;
        titleClip.syncLockedToSource = true;
        titleClip.locked = true;
        EditorTitleKeyframe base;
        base.text = title;
        base.translationY = 0.68;
        base.fontSize = std::clamp(settings.titleFontSize, 12.0, 220.0);
        base.autoFitToOutput = settings.titleAutoFitToOutput;
        base.color = profileString(transcript.root(), event.speakerId, "primary_color");
        if (base.color.empty())
            base.color = "#f7fbff";
        base.logoPath = profileString(transcript.root(), event.speakerId, "logo_path");
        if (base.logoPath.empty()) {
            base.logoPath = profileString(transcript.root(), event.speakerId, "logoPath");
        }
        base.windowEnabled = settings.titleBackgroundEnabled;
        base.windowColor = profileString(transcript.root(), event.speakerId, "secondary_color");
        if (base.windowColor.empty())
            base.windowColor = "#07111d";
        base.windowOpacity = 0.72;
        base.windowPadding = 24.0;
        base.windowWidth = std::max(0.0, settings.titleBoxWidth);
        base.windowFrameEnabled = settings.titleBackgroundEnabled;
        base.windowFrameColor = profileString(transcript.root(), event.speakerId, "accent_color");
        if (base.windowFrameColor.empty())
            base.windowFrameColor = "#56c7ff";
        base.windowFrameOpacity = 0.85;
        base.windowFrameWidth = 2.0;
        titleClip.titleKeyframes = {base};
        EditorClip animated = titleClip;
        animated.durationFrames = static_cast<int>(std::min<std::int64_t>(
            titleDuration, std::max<std::int64_t>(1, duration - settings.titleStartDelayFrames)));
        applySpeakerTitleFlyInCore(&animated, settings);
        titleClip.titleKeyframes = std::move(animated.titleKeyframes);
        if (settings.titleStartDelayFrames > 0 && !titleClip.titleKeyframes.empty()) {
            EditorTitleKeyframe waiting = titleClip.titleKeyframes.front();
            waiting.frame = 0;
            waiting.opacity = 0.0;
            waiting.windowOpacity = 0.0;
            waiting.windowFrameOpacity = 0.0;
            waiting.dropShadowOpacity = 0.0;
            for (EditorTitleKeyframe& keyframe : titleClip.titleKeyframes) {
                keyframe.frame = std::min<std::int64_t>(duration - 1, keyframe.frame + settings.titleStartDelayFrames);
            }
            titleClip.titleKeyframes.insert(titleClip.titleKeyframes.begin(), std::move(waiting));
        }
        clips.push_back(std::move(titleClip));
    }
    return clips;
}

} // namespace jcut
