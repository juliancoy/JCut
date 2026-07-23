#include "editor_document_core_json.h"
#include "render_contract_json.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_set>

namespace {

using json = nlohmann::json;

float normalizedPlaybackSpeedValue(float speed)
{
    return std::clamp(speed, 0.1f, 3.0f);
}

template <typename T>
T valueOr(const json& object, const char* key, T fallback)
{
    if (!object.is_object()) {
        return fallback;
    }
    const json::const_iterator it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return fallback;
    }
    try {
        return it->get<T>();
    } catch (const json::exception&) {
        return fallback;
    }
}

std::string stringOr(const json& object, const char* key, const std::string& fallback = {})
{
    if (!object.is_object()) {
        return fallback;
    }
    const json::const_iterator it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return fallback;
    }
    return it->get<std::string>();
}

std::vector<jcut::EditorPoint> editorPointsOrIdentity(
    const json& object, const char* key)
{
    std::vector<jcut::EditorPoint> points;
    const auto found = object.find(key);
    if (found != object.end() && found->is_array()) {
        for (const json& point : *found) {
            if (point.is_object()) {
                points.push_back({
                    valueOr(point, "x", 0.0),
                    valueOr(point, "y", 0.0)});
            }
        }
    }
    return points.empty()
        ? std::vector<jcut::EditorPoint>{{0.0, 0.0}, {1.0, 1.0}}
        : points;
}

json editorPointsJson(const std::vector<jcut::EditorPoint>& points)
{
    json result = json::array();
    for (const jcut::EditorPoint& point : points) {
        result.push_back({{"x", point.x}, {"y", point.y}});
    }
    return result;
}

std::string clipKindFromState(const json& clip)
{
    const std::string mediaType = stringOr(clip, "mediaType", "unknown");
    if (!mediaType.empty()) {
        return mediaType;
    }
    return "unknown";
}

template <typename Keyframe>
void normalizeParsedKeyframes(
    std::vector<Keyframe>* keyframes,
    std::int64_t durationFrames)
{
    if (!keyframes) {
        return;
    }
    const std::int64_t maxFrame = std::max<std::int64_t>(0, durationFrames - 1);
    std::stable_sort(
        keyframes->begin(), keyframes->end(),
        [](const Keyframe& left, const Keyframe& right) {
            return left.frame < right.frame;
        });
    std::vector<Keyframe> normalized;
    normalized.reserve(keyframes->size());
    for (Keyframe keyframe : *keyframes) {
        keyframe.frame = std::clamp<std::int64_t>(keyframe.frame, 0, maxFrame);
        if (!normalized.empty() && normalized.back().frame == keyframe.frame) {
            normalized.back() = std::move(keyframe);
        } else {
            normalized.push_back(std::move(keyframe));
        }
    }
    *keyframes = std::move(normalized);
}

void parseTransformKeyframes(const json& values,
                             std::vector<jcut::EditorTransformKeyframe>* keyframes)
{
    if (!values.is_array() || !keyframes) {
        return;
    }
    for (const json& value : values) {
        if (!value.is_object()) {
            continue;
        }
        keyframes->push_back({
            valueOr(value, "frame", std::int64_t{0}),
            stringOr(value, "title"),
            valueOr(value, "translationX", 0.0),
            valueOr(value, "translationY", 0.0),
            valueOr(value, "rotation", 0.0),
            valueOr(value, "scaleX", 1.0),
            valueOr(value, "scaleY", 1.0),
            valueOr(value, "linearInterpolation", true)
        });
    }
}

void parseBoolKeyframes(
    const json& values,
    std::vector<jcut::EditorBoolKeyframe>* keyframes)
{
    if (!values.is_array() || !keyframes) {
        return;
    }
    for (const json& value : values) {
        if (!value.is_object()) {
            continue;
        }
        keyframes->push_back({
            valueOr(value, "frame", std::int64_t{0}),
            valueOr(value, "enabled", false)});
    }
}

void parseGradingKeyframes(const json& values,
                           std::vector<jcut::EditorGradingKeyframe>* keyframes)
{
    if (!values.is_array() || !keyframes) {
        return;
    }
    for (const json& value : values) {
        if (!value.is_object()) {
            continue;
        }
        const auto curvePointsOrDefault = [&](const char* key) {
            std::vector<jcut::EditorPoint> points;
            const json::const_iterator it = value.find(key);
            if (it != value.end() && it->is_array()) {
                for (const json& pointValue : *it) {
                    if (!pointValue.is_object()) {
                        continue;
                    }
                    points.push_back({
                        valueOr(pointValue, "x", 0.0),
                        valueOr(pointValue, "y", 0.0)
                    });
                }
            }
            if (points.empty()) {
                points = {{0.0, 0.0}, {1.0, 1.0}};
            }
            return points;
        };
        jcut::EditorGradingKeyframe keyframe;
        keyframe.frame = valueOr(value, "frame", std::int64_t{0});
        keyframe.brightness = valueOr(value, "brightness", 0.0);
        keyframe.contrast = valueOr(value, "contrast", 1.0);
        keyframe.saturation = valueOr(value, "saturation", 1.0);
        keyframe.opacity = valueOr(value, "opacity", 1.0);
        keyframe.linearInterpolation = valueOr(value, "linearInterpolation", true);
        keyframe.shadowsR = valueOr(value, "shadowsR", 0.0);
        keyframe.shadowsG = valueOr(value, "shadowsG", 0.0);
        keyframe.shadowsB = valueOr(value, "shadowsB", 0.0);
        keyframe.midtonesR = valueOr(value, "midtonesR", 0.0);
        keyframe.midtonesG = valueOr(value, "midtonesG", 0.0);
        keyframe.midtonesB = valueOr(value, "midtonesB", 0.0);
        keyframe.highlightsR = valueOr(value, "highlightsR", 0.0);
        keyframe.highlightsG = valueOr(value, "highlightsG", 0.0);
        keyframe.highlightsB = valueOr(value, "highlightsB", 0.0);
        keyframe.curvePointsR = curvePointsOrDefault("curvePointsR");
        keyframe.curvePointsG = curvePointsOrDefault("curvePointsG");
        keyframe.curvePointsB = curvePointsOrDefault("curvePointsB");
        keyframe.curvePointsLuma = curvePointsOrDefault("curvePointsLuma");
        keyframe.curveThreePointLock =
            valueOr(value, "curveThreePointLock", false);
        keyframe.curveSmoothingEnabled =
            valueOr(value, "curveSmoothingEnabled", true);
        keyframes->push_back(std::move(keyframe));
    }
}

void parseOpacityKeyframes(const json& values,
                           std::vector<jcut::EditorOpacityKeyframe>* keyframes)
{
    if (!values.is_array() || !keyframes) {
        return;
    }
    for (const json& value : values) {
        if (!value.is_object()) {
            continue;
        }
        keyframes->push_back({
            valueOr(value, "frame", std::int64_t{0}),
            valueOr(value, "opacity", 1.0),
            valueOr(value, "linearInterpolation", true)
        });
    }
}

void parseTitleKeyframes(const json& values,
                         std::vector<jcut::EditorTitleKeyframe>* keyframes)
{
    if (!values.is_array() || !keyframes) {
        return;
    }
    for (const json& value : values) {
        if (!value.is_object()) {
            continue;
        }
        jcut::EditorTitleKeyframe keyframe;
        keyframe.frame = valueOr(value, "frame", std::int64_t{0});
        keyframe.text = stringOr(value, "text");
        keyframe.translationX = valueOr(value, "translationX", 0.0);
        keyframe.translationY = valueOr(value, "translationY", 0.0);
        keyframe.fontSize = valueOr(value, "fontSize", 48.0);
        keyframe.opacity = valueOr(value, "opacity", 1.0);
        keyframe.fontFamily = stringOr(value, "fontFamily", "DejaVu Sans");
        keyframe.bold = valueOr(value, "bold", true);
        keyframe.italic = valueOr(value, "italic", false);
        keyframe.color = stringOr(value, "color", "#ffffff");
        keyframe.linearInterpolation = valueOr(value, "linearInterpolation", true);
        keyframe.autoFitToOutput = valueOr(value, "autoFitToOutput", false);
        keyframe.logoPath = stringOr(value, "logoPath");
        keyframe.textMaterialStyle = stringOr(value, "textMaterialStyle", "solid");
        keyframe.textPatternImagePath = stringOr(value, "textPatternImagePath");
        keyframe.textPatternScale = valueOr(value, "textPatternScale", 1.0);
        keyframe.dropShadowEnabled = valueOr(value, "dropShadowEnabled", true);
        keyframe.dropShadowColor = stringOr(value, "dropShadowColor", "#ff000000");
        keyframe.dropShadowOpacity = valueOr(value, "dropShadowOpacity", 0.6);
        keyframe.dropShadowOffsetX = valueOr(value, "dropShadowOffsetX", 2.0);
        keyframe.dropShadowOffsetY = valueOr(value, "dropShadowOffsetY", 2.0);
        keyframe.windowEnabled = valueOr(value, "windowEnabled", false);
        keyframe.windowColor = stringOr(value, "windowColor", "#ff000000");
        keyframe.windowOpacity = valueOr(value, "windowOpacity", 0.35);
        keyframe.windowPadding = valueOr(value, "windowPadding", 16.0);
        keyframe.windowWidth = valueOr(value, "windowWidth", 0.0);
        keyframe.windowFrameEnabled = valueOr(value, "windowFrameEnabled", false);
        keyframe.windowFrameColor = stringOr(value, "windowFrameColor", "#ffffffff");
        keyframe.windowFrameOpacity = valueOr(value, "windowFrameOpacity", 1.0);
        keyframe.windowFrameWidth = valueOr(value, "windowFrameWidth", 2.0);
        keyframe.windowFrameGap = valueOr(value, "windowFrameGap", 4.0);
        keyframe.windowFrameMaterialStyle = stringOr(value, "windowFrameMaterialStyle", "solid");
        keyframe.windowFramePatternImagePath = stringOr(value, "windowFramePatternImagePath");
        keyframe.windowFramePatternScale = valueOr(value, "windowFramePatternScale", 1.0);
        keyframe.vulkan3DEnabled = valueOr(value, "vulkan3DEnabled", false);
        keyframe.vulkan3DExtrudeEnabled = valueOr(value, "vulkan3DExtrudeEnabled", false);
        keyframe.textExtrudeMode = stringOr(value, "textExtrudeMode", "none");
        keyframe.vulkan3DExtrudeDepth = valueOr(value, "vulkan3DExtrudeDepth", 0.0);
        keyframe.vulkan3DBevelScale = valueOr(value, "vulkan3DBevelScale", 0.0);
        keyframe.vulkan3DYawDegrees = valueOr(value, "vulkan3DYawDegrees", 0.0);
        keyframe.vulkan3DPitchDegrees = valueOr(value, "vulkan3DPitchDegrees", 0.0);
        keyframe.vulkan3DRollDegrees = valueOr(value, "vulkan3DRollDegrees", 0.0);
        keyframe.vulkan3DDepth = valueOr(value, "vulkan3DDepth", 0.0);
        keyframe.vulkan3DScale = valueOr(value, "vulkan3DScale", 1.0);
        keyframes->push_back(std::move(keyframe));
    }
}

void parseTranscriptOverlay(const json& value, jcut::EditorTranscriptOverlayState* overlay)
{
    if (!value.is_object() || !overlay) {
        return;
    }
    overlay->enabled = valueOr(value, "enabled", overlay->enabled);
    overlay->showBackground = valueOr(value, "showBackground", overlay->showBackground);
    overlay->backgroundOpacity = valueOr(value, "backgroundOpacity", overlay->backgroundOpacity);
    overlay->backgroundCornerRadius = valueOr(value, "backgroundCornerRadius", overlay->backgroundCornerRadius);
    overlay->backgroundPadding = valueOr(value, "backgroundPadding", overlay->backgroundPadding);
    overlay->backgroundFrameEnabled = valueOr(value, "backgroundFrameEnabled", overlay->backgroundFrameEnabled);
    overlay->backgroundFrameColor = stringOr(value, "backgroundFrameColor", overlay->backgroundFrameColor);
    overlay->backgroundFrameOpacity = valueOr(value, "backgroundFrameOpacity", overlay->backgroundFrameOpacity);
    overlay->backgroundFrameWidth = valueOr(value, "backgroundFrameWidth", overlay->backgroundFrameWidth);
    overlay->backgroundFrameGap = valueOr(value, "backgroundFrameGap", overlay->backgroundFrameGap);
    overlay->showShadow = valueOr(value, "showShadow", overlay->showShadow);
    overlay->shadowColor = stringOr(value, "shadowColor", overlay->shadowColor);
    overlay->shadowOpacity = valueOr(value, "shadowOpacity", overlay->shadowOpacity);
    overlay->shadowOffsetX = valueOr(value, "shadowOffsetX", overlay->shadowOffsetX);
    overlay->shadowOffsetY = valueOr(value, "shadowOffsetY", overlay->shadowOffsetY);
    overlay->textOutlineEnabled = valueOr(value, "textOutlineEnabled", overlay->textOutlineEnabled);
    overlay->textOutlineWidth = valueOr(value, "textOutlineWidth", overlay->textOutlineWidth);
    overlay->textOutlineColor = stringOr(value, "textOutlineColor", overlay->textOutlineColor);
    overlay->textOutlineOpacity = valueOr(value, "textOutlineOpacity", overlay->textOutlineOpacity);
    overlay->textExtrudeMode = stringOr(value, "textExtrudeMode", overlay->textExtrudeMode);
    overlay->textExtrudeDepth = valueOr(value, "textExtrudeDepth", overlay->textExtrudeDepth);
    overlay->textExtrudeBevelScale = valueOr(value, "textExtrudeBevelScale", overlay->textExtrudeBevelScale);
    overlay->showSpeakerTitle = valueOr(value, "showSpeakerTitle", overlay->showSpeakerTitle);
    overlay->highlightCurrentWord = valueOr(value, "highlightCurrentWord", overlay->highlightCurrentWord);
    overlay->autoScroll = valueOr(value, "autoScroll", overlay->autoScroll);
    overlay->useManualPlacement = valueOr(value, "useManualPlacement", overlay->useManualPlacement);
    overlay->translationX = valueOr(value, "translationX", overlay->translationX);
    overlay->translationY = valueOr(value, "translationY", overlay->translationY);
    overlay->boxWidth = valueOr(value, "boxWidth", overlay->boxWidth);
    overlay->boxHeight = valueOr(value, "boxHeight", overlay->boxHeight);
    overlay->maxLines = valueOr(value, "maxLines", overlay->maxLines);
    overlay->maxCharsPerLine = valueOr(value, "maxCharsPerLine", overlay->maxCharsPerLine);
    overlay->fontFamily = stringOr(value, "fontFamily", overlay->fontFamily);
    overlay->fontPointSize = valueOr(value, "fontPointSize", overlay->fontPointSize);
    overlay->bold = valueOr(value, "bold", overlay->bold);
    overlay->italic = valueOr(value, "italic", overlay->italic);
    overlay->textColor = stringOr(value, "textColor", overlay->textColor);
    overlay->textOpacity = valueOr(value, "textOpacity", overlay->textOpacity);
    overlay->backgroundColor = stringOr(value, "backgroundColor", overlay->backgroundColor);
    overlay->highlightColor = stringOr(value, "highlightColor", overlay->highlightColor);
    overlay->highlightTextColor = stringOr(value, "highlightTextColor", overlay->highlightTextColor);
}

void parseExtendedClip(const json& value, jcut::EditorClip* clip)
{
    if (!value.is_object() || !clip) {
        return;
    }
    clip->persistentId = stringOr(value, "persistentId", stringOr(value, "id"));
    clip->clipRole = jcut::editorClipRoleForStorage(
        stringOr(value, "clipRole", "media"));
    clip->linkedSourceClipId = jcut::trimmedEditorClipId(
        stringOr(value, "linkedSourceClipId"));
    clip->generatedFromMaskId = stringOr(value, "generatedFromMaskId");
    clip->syncLockedToSource = valueOr(value, "syncLockedToSource", false);
    clip->sourceTransformLocked = valueOr(value, "sourceTransformLocked", false);
    clip->zLevel = valueOr(
        value, "zLevel", std::numeric_limits<int>::min());
    clip->zLevelUserSet = valueOr(value, "zLevelUserSet", false);
    clip->proxyPath = stringOr(value, "proxyPath");
    clip->useProxy = valueOr(value, "useProxy", true);
    clip->mediaKind = stringOr(value, "mediaKind", clipKindFromState(value));
    clip->sourceDurationFrames = valueOr(value, "sourceDurationFrames", std::int64_t{0});
    clip->sourceInFrame = valueOr(value, "sourceInFrame", std::int64_t{0});
    clip->sourceInSubframeSamples = valueOr(value, "sourceInSubframeSamples", std::int64_t{0});
    clip->startSubframeSamples = valueOr(value, "startSubframeSamples", std::int64_t{0});
    clip->durationSubframeSamples = valueOr(value, "durationSubframeSamples", std::int64_t{0});
    clip->sourceFps = valueOr(value, "sourceFps", 30.0);
    clip->playbackRate = valueOr(value, "playbackRate", 1.0);
    clip->videoEnabled = valueOr(value, "videoEnabled", true);
    clip->audioEnabled = valueOr(value, "audioEnabled", true);
    clip->audioPresenceKnown = valueOr(
        value, "audioPresenceKnown", value.contains("hasAudio"));
    // Older state files predate the neutral stream-presence metadata. Infer
    // obvious audio/video paths, but keep the value marked unknown so a
    // later media probe can replace the optimistic fallback. Stream presence
    // is independent of the clip's user-controlled audio enablement.
    clip->hasAudio = valueOr(
        value,
        "hasAudio",
        jcut::mediaKindMayContainAudio(clip->mediaKind, clip->sourcePath));
    clip->audioSourceMode = stringOr(value, "audioSourceMode", "embedded");
    clip->audioSourcePath = stringOr(value, "audioSourcePath");
    clip->audioSourceStatus = stringOr(value, "audioSourceStatus", "unknown");
    clip->audioStreamIndex = valueOr(value, "audioStreamIndex", -1);
    clip->transcriptActiveCutPath = stringOr(value, "transcriptActiveCutPath");
    clip->audioBusId = stringOr(value, "audioBusId");
    clip->audioGain = valueOr(value, "audioGain", 1.0);
    clip->audioPan = valueOr(value, "audioPan", 0.0);
    clip->audioSolo = valueOr(value, "audioSolo", false);
    clip->audioLinkedToVideo = valueOr(value, "audioLinkedToVideo", true);
    clip->fadeSamples = std::max(0, valueOr(value, "fadeSamples", 250));
    clip->speakerFramingEnabled =
        valueOr(value, "speakerFramingEnabled", false);
    clip->speakerFramingBakedTargetXNorm =
        valueOr(value, "speakerFramingBakedTargetXNorm", 0.5);
    clip->speakerFramingBakedTargetYNorm =
        valueOr(value, "speakerFramingBakedTargetYNorm", 0.35);
    clip->speakerFramingBakedTargetBoxNorm =
        valueOr(value, "speakerFramingBakedTargetBoxNorm", -1.0);
    clip->speakerFramingMinConfidence =
        valueOr(value, "speakerFramingMinConfidence", 0.08);
    clip->speakerFramingManualTrackId =
        valueOr(value, "speakerFramingManualTrackId", -1);
    clip->speakerFramingManualStreamId =
        stringOr(value, "speakerFramingManualStreamId");
    clip->speakerFramingCenterSmoothingFrames =
        valueOr(value, "speakerFramingCenterSmoothingFrames", 0);
    clip->speakerFramingZoomSmoothingFrames =
        valueOr(value, "speakerFramingZoomSmoothingFrames", 0);
    clip->speakerFramingSmoothingMode =
        valueOr(value, "speakerFramingSmoothingMode", 0);
    clip->speakerFramingCenterSmoothingStrength =
        valueOr(value, "speakerFramingCenterSmoothingStrength", 1.0);
    clip->speakerFramingZoomSmoothingStrength =
        valueOr(value, "speakerFramingZoomSmoothingStrength", 1.0);
    clip->speakerFramingGapHoldFrames =
        valueOr(value, "speakerFramingGapHoldFrames", 0);
    clip->speakerSectionMinimumWords = std::clamp(
        valueOr(value, "speakerSectionMinimumWords", 10),
        0,
        1000);
    clip->brightness = valueOr(value, "brightness", 0.0);
    clip->contrast = valueOr(value, "contrast", 1.0);
    clip->saturation = valueOr(value, "saturation", 1.0);
    clip->opacity = valueOr(value, "opacity", 1.0);
    clip->baseTranslationX = valueOr(value, "baseTranslationX", 0.0);
    clip->baseTranslationY = valueOr(value, "baseTranslationY", 0.0);
    clip->baseRotation = valueOr(value, "baseRotation", 0.0);
    clip->baseScaleX = valueOr(value, "baseScaleX", 1.0);
    clip->baseScaleY = valueOr(value, "baseScaleY", 1.0);
    clip->gradingPreviewEnabled = valueOr(value, "gradingPreviewEnabled", true);
    parseTransformKeyframes(value.value("transformKeyframes", json::array()), &clip->transformKeyframes);
    parseBoolKeyframes(
        value.value(
            "speakerFramingEnabledKeyframes", json::array()),
        &clip->speakerFramingEnabledKeyframes);
    parseTransformKeyframes(
        value.value("speakerFramingKeyframes", json::array()),
        &clip->speakerFramingKeyframes);
    parseTransformKeyframes(
        value.value(
            "speakerFramingTargetKeyframes", json::array()),
        &clip->speakerFramingTargetKeyframes);
    parseGradingKeyframes(value.value("gradingKeyframes", json::array()), &clip->gradingKeyframes);
    parseOpacityKeyframes(value.value("opacityKeyframes", json::array()), &clip->opacityKeyframes);
    parseTitleKeyframes(value.value("titleKeyframes", json::array()), &clip->titleKeyframes);
    parseTranscriptOverlay(value.value("transcriptOverlay", json::object()), &clip->transcriptOverlay);
    clip->locked = valueOr(value, "locked", false);
    clip->maskEnabled = valueOr(value, "maskEnabled", false);
    clip->maskFramesDir = stringOr(value, "maskFramesDir");
    clip->maskFeather = valueOr(value, "maskFeather", 0.0);
    clip->maskFeatherGamma = valueOr(value, "maskFeatherGamma", 1.0);
    clip->maskFeatherFalloff = valueOr(value, "maskFeatherFalloff", 0);
    clip->maskDilate = valueOr(value, "maskDilate", 0.0);
    clip->maskErode = valueOr(value, "maskErode", 0.0);
    clip->maskBlur = valueOr(value, "maskBlur", 0.0);
    clip->maskInvert = valueOr(value, "maskInvert", false);
    clip->maskShowOnly = valueOr(value, "maskShowOnly", false);
    clip->maskOpacity = valueOr(value, "maskOpacity", 1.0);
    clip->maskGradeEnabled = valueOr(value, "maskGradeEnabled", false);
    clip->maskGradeBrightness = valueOr(value, "maskGradeBrightness", 0.0);
    clip->maskGradeContrast = valueOr(value, "maskGradeContrast", 1.0);
    clip->maskGradeSaturation = valueOr(value, "maskGradeSaturation", 1.0);
    clip->maskGradeCurvePointsR = editorPointsOrIdentity(value, "maskGradeCurvePointsR");
    clip->maskGradeCurvePointsG = editorPointsOrIdentity(value, "maskGradeCurvePointsG");
    clip->maskGradeCurvePointsB = editorPointsOrIdentity(value, "maskGradeCurvePointsB");
    clip->maskGradeCurvePointsLuma = editorPointsOrIdentity(value, "maskGradeCurvePointsLuma");
    clip->maskGradeCurveSmoothingEnabled = valueOr(
        value, "maskGradeCurveSmoothingEnabled", true);
    clip->maskDropShadowEnabled = valueOr(value, "maskDropShadowEnabled", false);
    clip->maskDropShadowRadius = valueOr(value, "maskDropShadowRadius", 12.0);
    clip->maskDropShadowOffsetX = valueOr(value, "maskDropShadowOffsetX", 0.0);
    clip->maskDropShadowOffsetY = valueOr(value, "maskDropShadowOffsetY", 4.0);
    clip->maskDropShadowOpacity = valueOr(value, "maskDropShadowOpacity", 0.45);
    clip->maskForegroundLayerEnabled = valueOr(value, "maskForegroundLayerEnabled", false);
    clip->maskRepeatEnabled = valueOr(value, "maskRepeatEnabled", false);
    clip->maskRepeatDeltaX = valueOr(value, "maskRepeatDeltaX", 160.0);
    clip->maskRepeatDeltaY = valueOr(value, "maskRepeatDeltaY", 0.0);
    clip->effectPreset = stringOr(value, "effectPreset", "none");
    clip->effectRows = valueOr(value, "effectRows", 32);
    clip->effectSpeed = valueOr(value, "effectSpeed", 1.0);
    clip->effectScale = valueOr(value, "effectScale", 1.0);
    clip->effectAlternateDirection = valueOr(value, "effectAlternateDirection", true);
    clip->effectSkipAwareTiming = valueOr(value, "effectSkipAwareTiming", true);
    clip->differenceReferenceFrames = valueOr(value, "differenceReferenceFrames", 1);
    clip->differenceThreshold = valueOr(value, "differenceThreshold", 0.10);
    clip->differenceSoftness = valueOr(value, "differenceSoftness", 0.05);
    clip->temporalEchoCount = valueOr(value, "temporalEchoCount", 4);
    clip->temporalEchoSpacingFrames = valueOr(value, "temporalEchoSpacingFrames", 2);
    clip->temporalEchoDecay = valueOr(value, "temporalEchoDecay", 0.65);
    clip->tilingPattern = stringOr(value, "tilingPattern", "grid");
    clip->tilingSpacing = valueOr(value, "tilingSpacing", 1.0);
    clip->tilingWrap = valueOr(value, "tilingWrap", true);
    const json& polygons = value.value("correctionPolygons", json::array());
    if (polygons.is_array()) {
        for (const json& polygonValue : polygons) {
            if (!polygonValue.is_object()) {
                continue;
            }
            jcut::EditorCorrectionPolygon polygon;
            polygon.enabled = valueOr(polygonValue, "enabled", true);
            polygon.startFrame = std::max<std::int64_t>(
                0, valueOr(polygonValue, "startFrame", std::int64_t{0}));
            polygon.endFrame = valueOr(polygonValue, "endFrame", std::int64_t{-1});
            const json& points = polygonValue.value("points", json::array());
            if (points.is_array()) {
                for (const json& point : points) {
                    if (point.is_object()) {
                        polygon.pointsNormalized.push_back({
                            std::clamp(valueOr(point, "x", 0.0), 0.0, 1.0),
                            std::clamp(valueOr(point, "y", 0.0), 0.0, 1.0)
                        });
                    }
                }
            }
            clip->correctionPolygons.push_back(std::move(polygon));
        }
    }
    normalizeParsedKeyframes(&clip->transformKeyframes, clip->durationFrames);
    normalizeParsedKeyframes(
        &clip->speakerFramingEnabledKeyframes,
        clip->durationFrames);
    normalizeParsedKeyframes(
        &clip->speakerFramingKeyframes,
        clip->durationFrames);
    normalizeParsedKeyframes(
        &clip->speakerFramingTargetKeyframes,
        clip->durationFrames);
    normalizeParsedKeyframes(&clip->gradingKeyframes, clip->durationFrames);
    normalizeParsedKeyframes(&clip->opacityKeyframes, clip->durationFrames);
    normalizeParsedKeyframes(&clip->titleKeyframes, clip->durationFrames);
}

void parseExtendedTrack(const json& value, jcut::EditorTrack* track)
{
    if (!value.is_object() || !track) {
        return;
    }
    track->height = valueOr(value, "height", 72);
    track->visualMode = valueOr(value, "visualMode", valueOr(value, "visualsEnabled", true) ? 0 : 2);
    track->gradingPreviewEnabled = valueOr(value, "gradingPreviewEnabled", true);
    track->audioEnabled = valueOr(value, "audioEnabled", true);
    track->audioBusId = stringOr(value, "audioBusId");
    track->audioGain = valueOr(value, "audioGain", 1.0);
    track->audioMuted = valueOr(value, "audioMuted", false);
    track->audioSolo = valueOr(value, "audioSolo", false);
    track->audioWaveformVisible = valueOr(value, "audioWaveformVisible", true);
    track->generatedChildTrack =
        valueOr(value, "generatedChildTrack", false);
    track->parentClipId = stringOr(value, "parentClipId");
    track->childClipId = stringOr(value, "childClipId");
}

void parseAudioDynamics(const json& root,
                        bool legacyRoot,
                        jcut::audio::DynamicsSettingsCore* settings)
{
    if (!settings || !root.is_object()) return;
    const json& value = legacyRoot
        ? root
        : root.value("audioDynamics", json::object());
    const auto key = [legacyRoot](const char* coreName) {
        return legacyRoot
            ? std::string("audio") +
                static_cast<char>(std::toupper(
                    static_cast<unsigned char>(coreName[0]))) +
                std::string(coreName + 1)
            : std::string(coreName);
    };
    settings->amplifyEnabled =
        valueOr(value, key("amplifyEnabled").c_str(), false);
    settings->amplifyDb =
        valueOr(value, key("amplifyDb").c_str(), 0.0);
    settings->normalizeEnabled =
        valueOr(value, key("normalizeEnabled").c_str(), false);
    settings->normalizeTargetDb =
        valueOr(value, key("normalizeTargetDb").c_str(), -1.0);
    settings->selectiveNormalizeEnabled = valueOr(
        value, key("selectiveNormalizeEnabled").c_str(), false);
    settings->selectiveNormalizeMinSegmentSeconds = valueOr(
        value,
        key("selectiveNormalizeMinSegmentSeconds").c_str(),
        0.5);
    settings->selectiveNormalizePeakDb = valueOr(
        value, key("selectiveNormalizePeakDb").c_str(), -12.0);
    settings->selectiveNormalizePasses = valueOr(
        value, key("selectiveNormalizePasses").c_str(), 1);
    settings->selectiveNormalizeOverlayVisible = valueOr(
        value, key("selectiveNormalizeOverlayVisible").c_str(), true);
    settings->transcriptNormalizeEnabled = valueOr(
        value, key("transcriptNormalizeEnabled").c_str(), false);
    settings->peakReductionEnabled = valueOr(
        value, key("peakReductionEnabled").c_str(), false);
    settings->peakThresholdDb =
        valueOr(value, key("peakThresholdDb").c_str(), -6.0);
    settings->limiterEnabled =
        valueOr(value, key("limiterEnabled").c_str(), false);
    settings->limiterThresholdDb =
        valueOr(value, key("limiterThresholdDb").c_str(), -1.0);
    settings->compressorEnabled =
        valueOr(value, key("compressorEnabled").c_str(), false);
    settings->compressorThresholdDb =
        valueOr(value, key("compressorThresholdDb").c_str(), -18.0);
    settings->compressorRatio =
        valueOr(value, key("compressorRatio").c_str(), 3.0);
    settings->softClipEnabled =
        valueOr(value, key("softClipEnabled").c_str(), false);
    settings->stereoToMonoEnabled =
        valueOr(value, key("stereoToMonoEnabled").c_str(), false);
    settings->waveformPreviewPostProcessing = valueOr(
        value, key("waveformPreviewPostProcessing").c_str(), true);
    *settings =
        jcut::audio::normalizedDynamicsSettingsCore(*settings);
}

json audioDynamicsJson(
    const jcut::audio::DynamicsSettingsCore& requested)
{
    const auto settings =
        jcut::audio::normalizedDynamicsSettingsCore(requested);
    return {
        {"amplifyEnabled", settings.amplifyEnabled},
        {"amplifyDb", settings.amplifyDb},
        {"normalizeEnabled", settings.normalizeEnabled},
        {"normalizeTargetDb", settings.normalizeTargetDb},
        {"selectiveNormalizeEnabled",
         settings.selectiveNormalizeEnabled},
        {"selectiveNormalizeMinSegmentSeconds",
         settings.selectiveNormalizeMinSegmentSeconds},
        {"selectiveNormalizePeakDb",
         settings.selectiveNormalizePeakDb},
        {"selectiveNormalizePasses",
         settings.selectiveNormalizePasses},
        {"selectiveNormalizeOverlayVisible",
         settings.selectiveNormalizeOverlayVisible},
        {"transcriptNormalizeEnabled",
         settings.transcriptNormalizeEnabled},
        {"peakReductionEnabled", settings.peakReductionEnabled},
        {"peakThresholdDb", settings.peakThresholdDb},
        {"limiterEnabled", settings.limiterEnabled},
        {"limiterThresholdDb", settings.limiterThresholdDb},
        {"compressorEnabled", settings.compressorEnabled},
        {"compressorThresholdDb", settings.compressorThresholdDb},
        {"compressorRatio", settings.compressorRatio},
        {"softClipEnabled", settings.softClipEnabled},
        {"stereoToMonoEnabled", settings.stereoToMonoEnabled},
        {"waveformPreviewPostProcessing",
         settings.waveformPreviewPostProcessing}
    };
}

bool parseCoreDocument(const json& root, jcut::EditorDocumentCore* document, std::string* errorOut)
{
    if (!root.is_object()) {
        if (errorOut) {
            *errorOut = "document root must be an object";
        }
        return false;
    }

    document->projectName = stringOr(root, "projectName", "Untitled Project");
    document->audioTreatment = jcut::editorAudioTreatmentFromId(
        stringOr(root, "audioTreatment", "time_stretch"));
    parseAudioDynamics(root, false, &document->audioDynamics);

    const json& mediaItems = root.value("mediaItems", json::array());
    if (!mediaItems.is_array()) {
        if (errorOut) {
            *errorOut = "mediaItems must be an array";
        }
        return false;
    }
    for (const json& mediaItem : mediaItems) {
        if (!mediaItem.is_object()) {
            continue;
        }
        document->mediaItems.push_back({
            stringOr(mediaItem, "id"),
            stringOr(mediaItem, "label"),
            stringOr(mediaItem, "kind", "unknown"),
            valueOr(mediaItem,
                    "audioPresenceKnown",
                    mediaItem.contains("hasAudio")),
            valueOr(mediaItem, "hasAudio", false)
        });
    }

    const json& tracks = root.value("tracks", json::array());
    if (!tracks.is_array()) {
        if (errorOut) {
            *errorOut = "tracks must be an array";
        }
        return false;
    }
    for (const json& track : tracks) {
        if (!track.is_object()) {
            continue;
        }
        document->tracks.push_back({
            valueOr(track, "id", 0),
            stringOr(track, "label"),
            valueOr(track, "selected", false)
        });
        parseExtendedTrack(track, &document->tracks.back());
    }

    const json& clips = root.value("clips", json::array());
    if (!clips.is_array()) {
        if (errorOut) {
            *errorOut = "clips must be an array";
        }
        return false;
    }
    for (const json& clip : clips) {
        if (!clip.is_object()) {
            continue;
        }
        document->clips.push_back({
            valueOr(clip, "id", 0),
            valueOr(clip, "trackId", 0),
            stringOr(clip, "label"),
            valueOr(clip, "startFrame", 0),
            valueOr(clip, "durationFrames", 0),
            valueOr(clip, "selected", false),
            stringOr(clip, "sourcePath")
        });
        parseExtendedClip(clip, &document->clips.back());
    }

    const json& renderSyncMarkers = root.value("renderSyncMarkers", json::array());
    if (renderSyncMarkers.is_array()) {
        for (const json& marker : renderSyncMarkers) {
            if (!marker.is_object()) {
                continue;
            }
            const std::string action = stringOr(marker, "action", "duplicate");
            document->renderSyncMarkers.push_back({
                stringOr(marker, "clipId"),
                valueOr(marker, "frame", std::int64_t{0}),
                action == "skip" || action == "skip_frame",
                std::max(1, valueOr(marker, "count", 1))
            });
        }
    }
    const json& exportRanges = root.value("exportRanges", json::array());
    if (exportRanges.is_array()) {
        for (const json& range : exportRanges) {
            if (range.is_object()) {
                document->exportRanges.push_back({
                    valueOr(range, "startFrame", std::int64_t{0}),
                    valueOr(range, "endFrame", std::int64_t{0})
                });
            }
        }
    }

    const json& transport = root.value("transport", json::object());
    if (transport.is_object()) {
        document->transport.playbackActive = valueOr(transport, "playbackActive", false);
        document->transport.playbackSpeed =
            normalizedPlaybackSpeedValue(
                valueOr(transport, "playbackSpeed", 1.0f));
        document->transport.playbackLoopEnabled =
            valueOr(transport, "playbackLoopEnabled", false);
        document->transport.previewViewMode =
            stringOr(transport, "previewViewMode", "video") == "audio"
            ? "audio" : "video";
        document->transport.audioMuted =
            valueOr(transport, "audioMuted", false);
        document->transport.audioVolume = std::clamp(
            valueOr(transport, "audioVolume", 0.8f), 0.0f, 1.0f);
        document->transport.previewZoom = valueOr(transport, "previewZoom", 1.0f);
        document->transport.currentFrame = valueOr(transport, "currentFrame", 0);
    }

    const json& panels = root.value("panels", json::object());
    if (panels.is_object()) {
        document->panels.showWaveform = valueOr(panels, "showWaveform", true);
        document->panels.showTranscript = valueOr(panels, "showTranscript", true);
        document->panels.showScopes = valueOr(panels, "showScopes", false);
    }

    const json& exportRequest = root.value("exportRequest", json::object());
    if (exportRequest.is_object()) {
        document->exportRequest.outputPath = stringOr(exportRequest, "outputPath");
        document->exportRequest.outputFormat = stringOr(exportRequest, "outputFormat", "mp4");
        document->exportRequest.imageSequenceFormat =
            stringOr(exportRequest, "imageSequenceFormat");
        const json& outputSize = exportRequest.value("outputSize", json::object());
        if (outputSize.is_object()) {
            document->exportRequest.outputSize.width = valueOr(outputSize, "width", 1080);
            document->exportRequest.outputSize.height = valueOr(outputSize, "height", 1920);
        }
        document->exportRequest.outputFps = valueOr(exportRequest, "outputFps", 30.0);
        document->exportRequest.playbackSpeed = valueOr(
            exportRequest,
            "playbackSpeed",
            static_cast<double>(document->transport.playbackSpeed));
        document->exportRequest.useProxyMedia = valueOr(exportRequest, "useProxyMedia", false);
        document->exportRequest.bypassGrading = valueOr(exportRequest, "bypassGrading", false);
        document->exportRequest.correctionsEnabled =
            valueOr(exportRequest, "correctionsEnabled", true);
        document->exportRequest.createVideoFromImageSequence =
            valueOr(exportRequest, "createVideoFromImageSequence", false);
        document->exportRequest.disableParallelImageWrite =
            valueOr(exportRequest, "disableParallelImageWrite", false);
        document->exportRequest.backgroundFillEffect =
            stringOr(exportRequest, "backgroundFillEffect", "edge_stretch");
        document->exportRequest.backgroundFillOpacity =
            valueOr(exportRequest, "backgroundFillOpacity", 1.0);
        document->exportRequest.backgroundFillBrightness =
            valueOr(exportRequest, "backgroundFillBrightness", 0.0);
        document->exportRequest.backgroundFillSaturation =
            valueOr(exportRequest, "backgroundFillSaturation", 1.0);
        document->exportRequest.backgroundFillEdgePixels =
            valueOr(exportRequest, "backgroundFillEdgePixels", 1);
        document->exportRequest.backgroundFillEdgeProgressive =
            valueOr(exportRequest, "backgroundFillEdgeProgressive", false);
        document->exportRequest.backgroundFillEdgePower =
            valueOr(exportRequest, "backgroundFillEdgePower", 2.0);
        document->exportRequest.backgroundFillStretchSourceClipId =
            stringOr(exportRequest, "backgroundFillStretchSourceClipId");
        document->exportRequest.transcriptPrependMs =
            valueOr(exportRequest, "transcriptPrependMs", 150);
        document->exportRequest.transcriptPostpendMs =
            valueOr(exportRequest, "transcriptPostpendMs", 70);
        document->exportRequest.transcriptOffsetMs =
            valueOr(exportRequest, "transcriptOffsetMs", 0);
        document->exportRequest.exportStartFrame =
            valueOr(exportRequest, "exportStartFrame", std::int64_t{0});
        document->exportRequest.exportEndFrame =
            valueOr(exportRequest, "exportEndFrame", std::int64_t{0});
        document->exportRequest.clipCount = valueOr(exportRequest, "clipCount", std::size_t{0});
        document->exportRequest.trackCount = valueOr(exportRequest, "trackCount", std::size_t{0});
        document->exportRequest.renderSyncMarkerCount =
            valueOr(exportRequest, "renderSyncMarkerCount", std::size_t{0});
        document->exportRequest.exportRangeCount =
            valueOr(exportRequest, "exportRangeCount", std::size_t{0});
        const std::string outputMode = stringOr(exportRequest, "outputMode", "encoded_file");
        if (outputMode == "image_sequence") {
            document->exportRequest.outputMode = jcut::render::RenderOutputMode::ImageSequence;
        } else if (outputMode == "encoded_file_and_image_sequence") {
            document->exportRequest.outputMode =
                jcut::render::RenderOutputMode::EncodedFileAndImageSequence;
        } else {
            document->exportRequest.outputMode = jcut::render::RenderOutputMode::EncodedFile;
        }
    }
    if (document->exportRequest.imageSequenceFormat.empty()) {
        document->exportRequest.imageSequenceFormat =
            document->exportRequest.outputFormat == "png"
            ? "png"
            : "jpeg";
    }

    return true;
}

bool parseLegacyStateDocument(const json& root, jcut::EditorDocumentCore* document, std::string* errorOut)
{
    if (!root.is_object()) {
        if (errorOut) {
            *errorOut = "state root must be an object";
        }
        return false;
    }

    document->projectName = stringOr(root, "projectName", "Untitled Project");
    document->audioTreatment = jcut::editorAudioTreatmentFromId(
        stringOr(root, "playbackAudioWarpMode", "time_stretch"));
    parseAudioDynamics(root, true, &document->audioDynamics);
    document->transport.currentFrame = valueOr(root, "currentFrame", 0);
    document->transport.playbackActive = valueOr(root, "playing", false);
    document->transport.playbackSpeed =
        normalizedPlaybackSpeedValue(
            valueOr(root, "playbackSpeed", 1.0f));
    document->transport.playbackLoopEnabled =
        valueOr(root, "playbackLoopEnabled", false);
    document->transport.previewViewMode =
        stringOr(root, "previewViewMode", "video") == "audio"
        ? "audio" : "video";
    document->transport.audioMuted =
        valueOr(root, "audioMuted", false);
    document->transport.audioVolume = std::clamp(
        valueOr(root, "audioVolume", 0.8f), 0.0f, 1.0f);
    document->transport.previewZoom = valueOr(root, "timelineZoom", 1.0f);
    document->panels.showWaveform = valueOr(root, "audioWaveformVisible", true);
    document->panels.showTranscript = true;
    document->panels.showScopes = false;

    document->exportRequest.outputPath = stringOr(root, "lastRenderOutputPath");
    document->exportRequest.outputFormat = stringOr(root, "outputFormat", "mp4");
    document->exportRequest.outputSize.width = valueOr(root, "outputWidth", 1080);
    document->exportRequest.outputSize.height = valueOr(root, "outputHeight", 1920);
    document->exportRequest.outputFps = valueOr(root, "outputFps", 30.0);
    document->exportRequest.playbackSpeed = valueOr(
        root,
        "exportPlaybackSpeed",
        valueOr(root, "playbackSpeed", 1.0));
    document->exportRequest.useProxyMedia = valueOr(root, "renderUseProxies", false);
    document->exportRequest.bypassGrading = !valueOr(root, "gradingPreview", true);
    document->exportRequest.correctionsEnabled = valueOr(root, "correctionsEnabled", true);
    document->exportRequest.createVideoFromImageSequence =
        valueOr(root, "createImageSequence", false);
    document->exportRequest.imageSequenceFormat =
        stringOr(root, "imageSequenceFormat");
    document->exportRequest.backgroundFillEffect =
        stringOr(root, "backgroundFillEffect", "edge_stretch");
    document->exportRequest.backgroundFillOpacity =
        valueOr(root, "backgroundFillOpacity", 1.0);
    document->exportRequest.backgroundFillBrightness =
        valueOr(root, "backgroundFillBrightness", 0.0);
    document->exportRequest.backgroundFillSaturation =
        valueOr(root, "backgroundFillSaturation", 1.0);
    document->exportRequest.backgroundFillEdgePixels =
        valueOr(root, "backgroundFillEdgePixels", 1);
    document->exportRequest.backgroundFillEdgeProgressive =
        valueOr(root, "backgroundFillEdgeProgressive", false);
    document->exportRequest.backgroundFillEdgePower =
        valueOr(root, "backgroundFillEdgePower", 2.0);
    document->exportRequest.backgroundFillStretchSourceClipId =
        stringOr(root, "backgroundFillStretchSourceClipId");
    document->exportRequest.transcriptPrependMs =
        valueOr(root, "transcriptPrependMs", 150);
    document->exportRequest.transcriptPostpendMs =
        valueOr(root, "transcriptPostpendMs", 70);
    document->exportRequest.transcriptOffsetMs =
        valueOr(root, "transcriptOffsetMs", 0);
    document->exportRequest.exportStartFrame =
        valueOr(root, "exportStartFrame", std::int64_t{0});
    document->exportRequest.exportEndFrame =
        valueOr(root, "exportEndFrame", std::int64_t{0});

    const json& exportRanges = root.value("exportRanges", json::array());
    if (exportRanges.is_array()) {
        document->exportRequest.exportRangeCount = exportRanges.size();
        for (const json& range : exportRanges) {
            if (range.is_object()) {
                document->exportRanges.push_back({
                    valueOr(range, "startFrame", std::int64_t{0}),
                    valueOr(range, "endFrame", std::int64_t{0})
                });
            }
        }
    }

    const json& renderSyncMarkers = root.value("renderSyncMarkers", json::array());
    if (renderSyncMarkers.is_array()) {
        document->exportRequest.renderSyncMarkerCount = renderSyncMarkers.size();
        for (const json& marker : renderSyncMarkers) {
            if (!marker.is_object()) {
                continue;
            }
            const std::string action = stringOr(marker, "action", "duplicate");
            document->renderSyncMarkers.push_back({
                stringOr(marker, "clipId"),
                valueOr(marker, "frame", std::int64_t{0}),
                action == "skip" || action == "skip_frame",
                std::max(1, valueOr(marker, "count", 1))
            });
        }
    }

    const std::string selectedClipId = stringOr(root, "selectedClipId");
    const std::string selectedTranscriptActiveCutPath =
        stringOr(root, "transcriptActiveCutPath");
    const int selectedTrackIndex = valueOr(root, "selectedTrackIndex", -1);

    const json& tracks = root.value("tracks", json::array());
    if (!tracks.is_array()) {
        if (errorOut) {
            *errorOut = "tracks must be an array";
        }
        return false;
    }
    document->tracks.reserve(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const json& track = tracks.at(i);
        if (!track.is_object()) {
            continue;
        }
        document->tracks.push_back({
            static_cast<int>(i + 1),
            stringOr(track, "name", "Track " + std::to_string(i + 1)),
            static_cast<int>(i) == selectedTrackIndex
        });
        parseExtendedTrack(track, &document->tracks.back());
    }

    std::unordered_set<std::string> seenMediaIds;
    const json& persistedMediaItems = root.value("mediaItems", json::array());
    if (persistedMediaItems.is_array()) {
        for (const json& mediaItem : persistedMediaItems) {
            if (!mediaItem.is_object()) {
                continue;
            }
            const std::string id = stringOr(mediaItem, "id");
            if (id.empty() || !seenMediaIds.insert(id).second) {
                continue;
            }
            document->mediaItems.push_back({
                id,
                stringOr(mediaItem, "label", id),
                stringOr(mediaItem, "kind", "unknown"),
                valueOr(mediaItem,
                        "audioPresenceKnown",
                        mediaItem.contains("hasAudio")),
                valueOr(mediaItem, "hasAudio", false)
            });
        }
    }

    const json& timeline = root.value("timeline", json::array());
    if (!timeline.is_array()) {
        if (errorOut) {
            *errorOut = "timeline must be an array";
        }
        return false;
    }

    document->clips.reserve(timeline.size());
    int nextClipId = 1;
    for (const json& clip : timeline) {
        if (!clip.is_object()) {
            continue;
        }
        const std::string sourcePath = stringOr(clip, "filePath");
        const std::string label = stringOr(clip, "label", sourcePath.empty()
            ? std::string("clip")
            : sourcePath.substr(sourcePath.find_last_of("/\\") + 1));
        if (!sourcePath.empty() && seenMediaIds.insert(sourcePath).second) {
            document->mediaItems.push_back({
                sourcePath,
                label,
                clipKindFromState(clip),
                valueOr(clip, "audioPresenceKnown", clip.contains("hasAudio")),
                valueOr(clip, "hasAudio", false)
            });
        }
        const int trackId = std::max(1, valueOr(clip, "trackIndex", 0) + 1);
        document->clips.push_back({
            nextClipId++,
            trackId,
            label,
            valueOr(clip, "startFrame", 0),
            valueOr(clip, "durationFrames", 0),
            stringOr(clip, "id") == selectedClipId,
            sourcePath
        });
        parseExtendedClip(clip, &document->clips.back());
        if (document->clips.back().selected &&
            !selectedTranscriptActiveCutPath.empty()) {
            document->clips.back().transcriptActiveCutPath =
                selectedTranscriptActiveCutPath;
        }
    }

    if (document->tracks.empty() && !document->clips.empty()) {
        int maxTrackId = 0;
        for (const jcut::EditorClip& clip : document->clips) {
            maxTrackId = std::max(maxTrackId, clip.trackId);
        }
        for (int trackId = 1; trackId <= maxTrackId; ++trackId) {
            document->tracks.push_back({
                trackId,
                "Track " + std::to_string(trackId),
                trackId == 1
            });
        }
    }

    if (std::none_of(document->tracks.begin(), document->tracks.end(), [](const jcut::EditorTrack& track) {
            return track.selected;
        }) && !document->tracks.empty()) {
        document->tracks.front().selected = true;
    }
    if (std::none_of(document->clips.begin(), document->clips.end(), [](const jcut::EditorClip& clip) {
            return clip.selected;
        }) && !document->clips.empty()) {
        document->clips.front().selected = true;
    }

    document->exportRequest.clipCount = document->clips.size();
    document->exportRequest.trackCount = document->tracks.size();

    const std::string format = document->exportRequest.outputFormat;
    if (document->exportRequest.imageSequenceFormat.empty()) {
        // Qt persists this independent preference even while exporting a
        // video container. Canonicalize the neutral document to the same
        // default so opening the same project through either shell does not
        // produce a dirty snapshot before the first edit.
        document->exportRequest.imageSequenceFormat =
            format == "png" ? "png" : "jpeg";
    }
    if (document->exportRequest.createVideoFromImageSequence ||
        format == "png" || format == "jpg" || format == "jpeg") {
        document->exportRequest.outputMode =
            jcut::render::RenderOutputMode::EncodedFileAndImageSequence;
    } else {
        document->exportRequest.outputMode = jcut::render::RenderOutputMode::EncodedFile;
    }

    return true;
}

json transformKeyframesJson(const std::vector<jcut::EditorTransformKeyframe>& keyframes)
{
    json values = json::array();
    for (const jcut::EditorTransformKeyframe& keyframe : keyframes) {
        values.push_back({
            {"frame", keyframe.frame},
            {"title", keyframe.title},
            {"translationX", keyframe.translationX},
            {"translationY", keyframe.translationY},
            {"rotation", keyframe.rotation},
            {"scaleX", keyframe.scaleX},
            {"scaleY", keyframe.scaleY},
            {"linearInterpolation", keyframe.linearInterpolation}
        });
    }
    return values;
}

json boolKeyframesJson(
    const std::vector<jcut::EditorBoolKeyframe>& keyframes)
{
    json values = json::array();
    for (const jcut::EditorBoolKeyframe& keyframe : keyframes) {
        values.push_back({
            {"frame", keyframe.frame},
            {"enabled", keyframe.enabled}});
    }
    return values;
}

json gradingKeyframesJson(const std::vector<jcut::EditorGradingKeyframe>& keyframes)
{
    json values = json::array();
    for (const jcut::EditorGradingKeyframe& keyframe : keyframes) {
        const auto pointsJson = [](const std::vector<jcut::EditorPoint>& points) {
            json result = json::array();
            for (const jcut::EditorPoint& point : points) {
                result.push_back({{"x", point.x}, {"y", point.y}});
            }
            return result;
        };
        values.push_back({
            {"frame", keyframe.frame},
            {"brightness", keyframe.brightness},
            {"contrast", keyframe.contrast},
            {"saturation", keyframe.saturation},
            {"opacity", keyframe.opacity},
            {"linearInterpolation", keyframe.linearInterpolation},
            {"shadowsR", keyframe.shadowsR},
            {"shadowsG", keyframe.shadowsG},
            {"shadowsB", keyframe.shadowsB},
            {"midtonesR", keyframe.midtonesR},
            {"midtonesG", keyframe.midtonesG},
            {"midtonesB", keyframe.midtonesB},
            {"highlightsR", keyframe.highlightsR},
            {"highlightsG", keyframe.highlightsG},
            {"highlightsB", keyframe.highlightsB},
            {"curvePointsR", pointsJson(keyframe.curvePointsR)},
            {"curvePointsG", pointsJson(keyframe.curvePointsG)},
            {"curvePointsB", pointsJson(keyframe.curvePointsB)},
            {"curvePointsLuma", pointsJson(keyframe.curvePointsLuma)},
            {"curveThreePointLock", keyframe.curveThreePointLock},
            {"curveSmoothingEnabled", keyframe.curveSmoothingEnabled}
        });
    }
    return values;
}

json opacityKeyframesJson(const std::vector<jcut::EditorOpacityKeyframe>& keyframes)
{
    json values = json::array();
    for (const jcut::EditorOpacityKeyframe& keyframe : keyframes) {
        values.push_back({
            {"frame", keyframe.frame},
            {"opacity", keyframe.opacity},
            {"linearInterpolation", keyframe.linearInterpolation}
        });
    }
    return values;
}

json titleKeyframesJson(const std::vector<jcut::EditorTitleKeyframe>& keyframes)
{
    json values = json::array();
    for (const jcut::EditorTitleKeyframe& keyframe : keyframes) {
        values.push_back({
            {"frame", keyframe.frame},
            {"text", keyframe.text},
            {"translationX", keyframe.translationX},
            {"translationY", keyframe.translationY},
            {"fontSize", keyframe.fontSize},
            {"opacity", keyframe.opacity},
            {"fontFamily", keyframe.fontFamily},
            {"bold", keyframe.bold},
            {"italic", keyframe.italic},
            {"color", keyframe.color},
            {"linearInterpolation", keyframe.linearInterpolation},
            {"autoFitToOutput", keyframe.autoFitToOutput},
            {"logoPath", keyframe.logoPath},
            {"textMaterialStyle", keyframe.textMaterialStyle},
            {"textPatternImagePath", keyframe.textPatternImagePath},
            {"textPatternScale", keyframe.textPatternScale},
            {"dropShadowEnabled", keyframe.dropShadowEnabled},
            {"dropShadowColor", keyframe.dropShadowColor},
            {"dropShadowOpacity", keyframe.dropShadowOpacity},
            {"dropShadowOffsetX", keyframe.dropShadowOffsetX},
            {"dropShadowOffsetY", keyframe.dropShadowOffsetY},
            {"windowEnabled", keyframe.windowEnabled},
            {"windowColor", keyframe.windowColor},
            {"windowOpacity", keyframe.windowOpacity},
            {"windowPadding", keyframe.windowPadding},
            {"windowWidth", keyframe.windowWidth},
            {"windowFrameEnabled", keyframe.windowFrameEnabled},
            {"windowFrameColor", keyframe.windowFrameColor},
            {"windowFrameOpacity", keyframe.windowFrameOpacity},
            {"windowFrameWidth", keyframe.windowFrameWidth},
            {"windowFrameGap", keyframe.windowFrameGap},
            {"windowFrameMaterialStyle", keyframe.windowFrameMaterialStyle},
            {"windowFramePatternImagePath", keyframe.windowFramePatternImagePath},
            {"windowFramePatternScale", keyframe.windowFramePatternScale},
            {"vulkan3DEnabled", keyframe.vulkan3DEnabled},
            {"vulkan3DExtrudeEnabled", keyframe.vulkan3DExtrudeEnabled},
            {"textExtrudeMode", keyframe.textExtrudeMode},
            {"vulkan3DExtrudeDepth", keyframe.vulkan3DExtrudeDepth},
            {"vulkan3DBevelScale", keyframe.vulkan3DBevelScale},
            {"vulkan3DYawDegrees", keyframe.vulkan3DYawDegrees},
            {"vulkan3DPitchDegrees", keyframe.vulkan3DPitchDegrees},
            {"vulkan3DRollDegrees", keyframe.vulkan3DRollDegrees},
            {"vulkan3DDepth", keyframe.vulkan3DDepth},
            {"vulkan3DScale", keyframe.vulkan3DScale}
        });
    }
    return values;
}

json transcriptOverlayJson(const jcut::EditorTranscriptOverlayState& overlay)
{
    return {
        {"enabled", overlay.enabled},
        {"showBackground", overlay.showBackground},
        {"backgroundOpacity", overlay.backgroundOpacity},
        {"backgroundCornerRadius", overlay.backgroundCornerRadius},
        {"backgroundPadding", overlay.backgroundPadding},
        {"backgroundFrameEnabled", overlay.backgroundFrameEnabled},
        {"backgroundFrameColor", overlay.backgroundFrameColor},
        {"backgroundFrameOpacity", overlay.backgroundFrameOpacity},
        {"backgroundFrameWidth", overlay.backgroundFrameWidth},
        {"backgroundFrameGap", overlay.backgroundFrameGap},
        {"showShadow", overlay.showShadow},
        {"shadowColor", overlay.shadowColor},
        {"shadowOpacity", overlay.shadowOpacity},
        {"shadowOffsetX", overlay.shadowOffsetX},
        {"shadowOffsetY", overlay.shadowOffsetY},
        {"textOutlineEnabled", overlay.textOutlineEnabled},
        {"textOutlineWidth", overlay.textOutlineWidth},
        {"textOutlineColor", overlay.textOutlineColor},
        {"textOutlineOpacity", overlay.textOutlineOpacity},
        {"textExtrudeMode", overlay.textExtrudeMode},
        {"textExtrudeDepth", overlay.textExtrudeDepth},
        {"textExtrudeBevelScale", overlay.textExtrudeBevelScale},
        {"showSpeakerTitle", overlay.showSpeakerTitle},
        {"highlightCurrentWord", overlay.highlightCurrentWord},
        {"autoScroll", overlay.autoScroll},
        {"useManualPlacement", overlay.useManualPlacement},
        {"translationX", overlay.translationX},
        {"translationY", overlay.translationY},
        {"boxWidth", overlay.boxWidth},
        {"boxHeight", overlay.boxHeight},
        {"maxLines", overlay.maxLines},
        {"maxCharsPerLine", overlay.maxCharsPerLine},
        {"fontFamily", overlay.fontFamily},
        {"fontPointSize", overlay.fontPointSize},
        {"bold", overlay.bold},
        {"italic", overlay.italic},
        {"textColor", overlay.textColor},
        {"textOpacity", overlay.textOpacity},
        {"backgroundColor", overlay.backgroundColor},
        {"highlightColor", overlay.highlightColor},
        {"highlightTextColor", overlay.highlightTextColor}
    };
}

json mergedObject(json base, const json& update)
{
    if (!base.is_object()) {
        base = json::object();
    }
    if (!update.is_object()) {
        return base;
    }
    for (const auto& [key, value] : update.items()) {
        base[key] = value;
    }
    return base;
}

json mergeObjectsByFrame(const json& baseValues, const json& updatedValues)
{
    if (!updatedValues.is_array()) {
        return json::array();
    }

    std::vector<bool> consumed(baseValues.is_array() ? baseValues.size() : 0, false);
    std::vector<std::optional<std::size_t>> baseMatches(updatedValues.size());
    for (std::size_t updatedIndex = 0;
         updatedIndex < updatedValues.size();
         ++updatedIndex) {
        const json& updatedValue = updatedValues.at(updatedIndex);
        if (updatedValue.is_object() && updatedValue.contains("frame") && baseValues.is_array()) {
            for (std::size_t i = 0; i < baseValues.size(); ++i) {
                if (consumed[i] || !baseValues.at(i).is_object() ||
                    !baseValues.at(i).contains("frame")) {
                    continue;
                }
                if (baseValues.at(i).at("frame") == updatedValue.at("frame")) {
                    baseMatches[updatedIndex] = i;
                    consumed[i] = true;
                    break;
                }
            }
        }
    }

    // A frame edit is represented in the neutral runtime as remove-old plus
    // upsert-new. When the keyframe count is unchanged, pair the remaining
    // unmatched rows by stable order so Qt-only curve/material/mask fields
    // move with that keyframe instead of being discarded on save. Exact-frame
    // matches above always take precedence; additions/removals do not inherit
    // an unrelated legacy row.
    if (baseValues.is_array() && baseValues.size() == updatedValues.size()) {
        std::vector<std::size_t> unmatchedBaseIndices;
        std::vector<std::size_t> unmatchedUpdatedIndices;
        for (std::size_t i = 0; i < consumed.size(); ++i) {
            if (!consumed[i]) {
                unmatchedBaseIndices.push_back(i);
            }
        }
        for (std::size_t i = 0; i < baseMatches.size(); ++i) {
            if (!baseMatches[i].has_value()) {
                unmatchedUpdatedIndices.push_back(i);
            }
        }
        for (std::size_t i = 0;
             i < std::min(unmatchedBaseIndices.size(),
                          unmatchedUpdatedIndices.size());
             ++i) {
            baseMatches[unmatchedUpdatedIndices[i]] = unmatchedBaseIndices[i];
        }
    }

    json mergedValues = json::array();
    for (std::size_t updatedIndex = 0;
         updatedIndex < updatedValues.size();
         ++updatedIndex) {
        const json* matchingBase = baseMatches[updatedIndex].has_value()
            ? &baseValues.at(*baseMatches[updatedIndex])
            : nullptr;
        mergedValues.push_back(mergedObject(
            matchingBase ? *matchingBase : json::object(),
            updatedValues.at(updatedIndex)));
    }
    return mergedValues;
}

json mergeObjectsByIndex(const json& baseValues, const json& updatedValues)
{
    if (!updatedValues.is_array()) {
        return json::array();
    }

    json mergedValues = json::array();
    for (std::size_t i = 0; i < updatedValues.size(); ++i) {
        const json baseValue = baseValues.is_array() && i < baseValues.size()
            ? baseValues.at(i)
            : json::object();
        mergedValues.push_back(mergedObject(baseValue, updatedValues.at(i)));
    }
    return mergedValues;
}

void writeExtendedClipJson(json* out, const jcut::EditorClip& clip)
{
    if (!out) {
        return;
    }
    (*out)["persistentId"] = clip.persistentId;
    (*out)["clipRole"] = jcut::editorClipRoleForStorage(clip.clipRole);
    (*out)["linkedSourceClipId"] =
        jcut::trimmedEditorClipId(clip.linkedSourceClipId);
    (*out)["generatedFromMaskId"] = clip.generatedFromMaskId;
    (*out)["syncLockedToSource"] = clip.syncLockedToSource;
    (*out)["sourceTransformLocked"] = clip.sourceTransformLocked;
    (*out)["zLevel"] = clip.zLevel;
    (*out)["zLevelUserSet"] = clip.zLevelUserSet;
    (*out)["proxyPath"] = clip.proxyPath;
    (*out)["useProxy"] = clip.useProxy;
    (*out)["mediaKind"] = clip.mediaKind;
    (*out)["sourceDurationFrames"] = clip.sourceDurationFrames;
    (*out)["sourceInFrame"] = clip.sourceInFrame;
    (*out)["sourceInSubframeSamples"] = clip.sourceInSubframeSamples;
    (*out)["startSubframeSamples"] = clip.startSubframeSamples;
    (*out)["durationSubframeSamples"] = clip.durationSubframeSamples;
    (*out)["sourceFps"] = clip.sourceFps;
    (*out)["playbackRate"] = clip.playbackRate;
    (*out)["videoEnabled"] = clip.videoEnabled;
    (*out)["audioEnabled"] = clip.audioEnabled;
    (*out)["hasAudio"] = clip.hasAudio;
    (*out)["audioPresenceKnown"] = clip.audioPresenceKnown;
    (*out)["audioSourceMode"] = clip.audioSourceMode;
    (*out)["audioSourcePath"] = clip.audioSourcePath;
    (*out)["audioSourceStatus"] = clip.audioSourceStatus;
    (*out)["audioStreamIndex"] = clip.audioStreamIndex;
    (*out)["transcriptActiveCutPath"] = clip.transcriptActiveCutPath;
    (*out)["audioBusId"] = clip.audioBusId;
    (*out)["audioGain"] = clip.audioGain;
    (*out)["audioPan"] = clip.audioPan;
    (*out)["audioSolo"] = clip.audioSolo;
    (*out)["audioLinkedToVideo"] = clip.audioLinkedToVideo;
    (*out)["fadeSamples"] = std::max(0, clip.fadeSamples);
    (*out)["speakerFramingEnabled"] =
        clip.speakerFramingEnabled;
    (*out)["speakerFramingBakedTargetXNorm"] =
        clip.speakerFramingBakedTargetXNorm;
    (*out)["speakerFramingBakedTargetYNorm"] =
        clip.speakerFramingBakedTargetYNorm;
    (*out)["speakerFramingBakedTargetBoxNorm"] =
        clip.speakerFramingBakedTargetBoxNorm;
    (*out)["speakerFramingMinConfidence"] =
        clip.speakerFramingMinConfidence;
    (*out)["speakerFramingManualTrackId"] =
        clip.speakerFramingManualTrackId;
    (*out)["speakerFramingManualStreamId"] =
        clip.speakerFramingManualStreamId;
    (*out)["speakerFramingCenterSmoothingFrames"] =
        clip.speakerFramingCenterSmoothingFrames;
    (*out)["speakerFramingZoomSmoothingFrames"] =
        clip.speakerFramingZoomSmoothingFrames;
    (*out)["speakerFramingSmoothingMode"] =
        clip.speakerFramingSmoothingMode;
    (*out)["speakerFramingCenterSmoothingStrength"] =
        clip.speakerFramingCenterSmoothingStrength;
    (*out)["speakerFramingZoomSmoothingStrength"] =
        clip.speakerFramingZoomSmoothingStrength;
    (*out)["speakerFramingGapHoldFrames"] =
        clip.speakerFramingGapHoldFrames;
    (*out)["speakerSectionMinimumWords"] =
        std::clamp(clip.speakerSectionMinimumWords, 0, 1000);
    (*out)["brightness"] = clip.brightness;
    (*out)["contrast"] = clip.contrast;
    (*out)["saturation"] = clip.saturation;
    (*out)["opacity"] = clip.opacity;
    (*out)["baseTranslationX"] = clip.baseTranslationX;
    (*out)["baseTranslationY"] = clip.baseTranslationY;
    (*out)["baseRotation"] = clip.baseRotation;
    (*out)["baseScaleX"] = clip.baseScaleX;
    (*out)["baseScaleY"] = clip.baseScaleY;
    (*out)["gradingPreviewEnabled"] = clip.gradingPreviewEnabled;
    (*out)["transformKeyframes"] = mergeObjectsByFrame(
        out->value("transformKeyframes", json::array()),
        transformKeyframesJson(clip.transformKeyframes));
    (*out)["speakerFramingEnabledKeyframes"] =
        mergeObjectsByFrame(
            out->value(
                "speakerFramingEnabledKeyframes", json::array()),
            boolKeyframesJson(
                clip.speakerFramingEnabledKeyframes));
    (*out)["speakerFramingKeyframes"] =
        mergeObjectsByFrame(
            out->value(
                "speakerFramingKeyframes", json::array()),
            transformKeyframesJson(
                clip.speakerFramingKeyframes));
    (*out)["speakerFramingTargetKeyframes"] =
        mergeObjectsByFrame(
            out->value(
                "speakerFramingTargetKeyframes", json::array()),
            transformKeyframesJson(
                clip.speakerFramingTargetKeyframes));
    (*out)["gradingKeyframes"] = mergeObjectsByFrame(
        out->value("gradingKeyframes", json::array()),
        gradingKeyframesJson(clip.gradingKeyframes));
    (*out)["opacityKeyframes"] = mergeObjectsByFrame(
        out->value("opacityKeyframes", json::array()),
        opacityKeyframesJson(clip.opacityKeyframes));
    (*out)["titleKeyframes"] = mergeObjectsByFrame(
        out->value("titleKeyframes", json::array()),
        titleKeyframesJson(clip.titleKeyframes));
    (*out)["transcriptOverlay"] = mergedObject(
        out->value("transcriptOverlay", json::object()),
        transcriptOverlayJson(clip.transcriptOverlay));
    (*out)["locked"] = clip.locked;
    (*out)["maskEnabled"] = clip.maskEnabled;
    (*out)["maskFramesDir"] = clip.maskFramesDir;
    (*out)["maskFeather"] = clip.maskFeather;
    (*out)["maskFeatherGamma"] = clip.maskFeatherGamma;
    (*out)["maskFeatherFalloff"] = clip.maskFeatherFalloff;
    (*out)["maskDilate"] = clip.maskDilate;
    (*out)["maskErode"] = clip.maskErode;
    (*out)["maskBlur"] = clip.maskBlur;
    (*out)["maskInvert"] = clip.maskInvert;
    (*out)["maskShowOnly"] = clip.maskShowOnly;
    (*out)["maskOpacity"] = clip.maskOpacity;
    (*out)["maskGradeEnabled"] = clip.maskGradeEnabled;
    (*out)["maskGradeBrightness"] = clip.maskGradeBrightness;
    (*out)["maskGradeContrast"] = clip.maskGradeContrast;
    (*out)["maskGradeSaturation"] = clip.maskGradeSaturation;
    (*out)["maskGradeCurvePointsR"] = editorPointsJson(clip.maskGradeCurvePointsR);
    (*out)["maskGradeCurvePointsG"] = editorPointsJson(clip.maskGradeCurvePointsG);
    (*out)["maskGradeCurvePointsB"] = editorPointsJson(clip.maskGradeCurvePointsB);
    (*out)["maskGradeCurvePointsLuma"] = editorPointsJson(clip.maskGradeCurvePointsLuma);
    (*out)["maskGradeCurveSmoothingEnabled"] = clip.maskGradeCurveSmoothingEnabled;
    (*out)["maskDropShadowEnabled"] = clip.maskDropShadowEnabled;
    (*out)["maskDropShadowRadius"] = clip.maskDropShadowRadius;
    (*out)["maskDropShadowOffsetX"] = clip.maskDropShadowOffsetX;
    (*out)["maskDropShadowOffsetY"] = clip.maskDropShadowOffsetY;
    (*out)["maskDropShadowOpacity"] = clip.maskDropShadowOpacity;
    (*out)["maskForegroundLayerEnabled"] = clip.maskForegroundLayerEnabled;
    (*out)["maskRepeatEnabled"] = clip.maskRepeatEnabled;
    (*out)["maskRepeatDeltaX"] = clip.maskRepeatDeltaX;
    (*out)["maskRepeatDeltaY"] = clip.maskRepeatDeltaY;
    (*out)["effectPreset"] = clip.effectPreset;
    (*out)["effectRows"] = clip.effectRows;
    (*out)["effectSpeed"] = clip.effectSpeed;
    (*out)["effectScale"] = clip.effectScale;
    (*out)["effectAlternateDirection"] = clip.effectAlternateDirection;
    (*out)["effectSkipAwareTiming"] = clip.effectSkipAwareTiming;
    (*out)["differenceReferenceFrames"] = clip.differenceReferenceFrames;
    (*out)["differenceThreshold"] = clip.differenceThreshold;
    (*out)["differenceSoftness"] = clip.differenceSoftness;
    (*out)["temporalEchoCount"] = clip.temporalEchoCount;
    (*out)["temporalEchoSpacingFrames"] = clip.temporalEchoSpacingFrames;
    (*out)["temporalEchoDecay"] = clip.temporalEchoDecay;
    (*out)["tilingPattern"] = clip.tilingPattern;
    (*out)["tilingSpacing"] = clip.tilingSpacing;
    (*out)["tilingWrap"] = clip.tilingWrap;
    json polygons = json::array();
    for (const jcut::EditorCorrectionPolygon& polygon : clip.correctionPolygons) {
        json points = json::array();
        for (const jcut::EditorPoint& point : polygon.pointsNormalized) {
            points.push_back({{"x", point.x}, {"y", point.y}});
        }
        polygons.push_back({
            {"enabled", polygon.enabled},
            {"startFrame", polygon.startFrame},
            {"endFrame", polygon.endFrame},
            {"points", std::move(points)}
        });
    }
    (*out)["correctionPolygons"] = mergeObjectsByIndex(
        out->value("correctionPolygons", json::array()), polygons);
}

void writeExtendedTrackJson(json* out, const jcut::EditorTrack& track)
{
    if (!out) {
        return;
    }
    (*out)["height"] = track.height;
    (*out)["visualMode"] = track.visualMode;
    (*out)["gradingPreviewEnabled"] = track.gradingPreviewEnabled;
    (*out)["audioEnabled"] = track.audioEnabled;
    (*out)["audioBusId"] = track.audioBusId;
    (*out)["audioGain"] = track.audioGain;
    (*out)["audioMuted"] = track.audioMuted;
    (*out)["audioSolo"] = track.audioSolo;
    (*out)["audioWaveformVisible"] = track.audioWaveformVisible;
    (*out)["generatedChildTrack"] = track.generatedChildTrack;
    (*out)["parentClipId"] = track.parentClipId;
    (*out)["childClipId"] = track.childClipId;
}

json legacyClipJson(
    const jcut::EditorClip& clip,
    int trackIndex,
    const json* baseClip = nullptr)
{
    json out = (baseClip && baseClip->is_object()) ? *baseClip : json::object();
    if (!clip.persistentId.empty()) {
        out["id"] = clip.persistentId;
    } else if (!out.contains("id") || !out["id"].is_string() || out["id"].get<std::string>().empty()) {
        out["id"] = std::string("imgui-clip-") + std::to_string(clip.id);
    }
    out["label"] = clip.label;
    out["filePath"] = clip.sourcePath;
    out["trackIndex"] = std::max(0, trackIndex);
    out["startFrame"] = clip.startFrame;
    out["durationFrames"] = std::max(1, clip.durationFrames);
    if (!out.contains("mediaType") || !out["mediaType"].is_string()) {
        out["mediaType"] = clip.mediaKind.empty() ? "video" : clip.mediaKind;
    }
    writeExtendedClipJson(&out, clip);
    return out;
}

const json* findPreservedClipJson(const json& baseTimeline,
                                  const jcut::EditorClip& clip,
                                  int trackIndex,
                                  std::vector<bool>* consumed)
{
    if (!baseTimeline.is_array() || !consumed || consumed->size() != baseTimeline.size()) {
        return nullptr;
    }

    auto matchesPath = [&](const json& candidate) {
        return stringOr(candidate, "filePath") == clip.sourcePath ||
               stringOr(candidate, "proxyPath") == clip.sourcePath ||
               stringOr(candidate, "audioSourcePath") == clip.sourcePath;
    };
    auto matchesShape = [&](const json& candidate) {
        return stringOr(candidate, "label") == clip.label &&
               valueOr(candidate, "trackIndex", 0) == std::max(0, trackIndex) &&
               valueOr(candidate, "startFrame", 0) == clip.startFrame;
    };

    if (!clip.persistentId.empty()) {
        for (std::size_t i = 0; i < baseTimeline.size(); ++i) {
            if ((*consumed)[i] || !baseTimeline.at(i).is_object()) {
                continue;
            }
            if (stringOr(baseTimeline.at(i), "id") == clip.persistentId ||
                stringOr(baseTimeline.at(i), "persistentId") == clip.persistentId) {
                (*consumed)[i] = true;
                return &baseTimeline.at(i);
            }
        }
    }

    for (std::size_t i = 0; i < baseTimeline.size(); ++i) {
        if ((*consumed)[i] || !baseTimeline.at(i).is_object()) {
            continue;
        }
        if (matchesPath(baseTimeline.at(i))) {
            (*consumed)[i] = true;
            return &baseTimeline.at(i);
        }
    }
    for (std::size_t i = 0; i < baseTimeline.size(); ++i) {
        if ((*consumed)[i] || !baseTimeline.at(i).is_object()) {
            continue;
        }
        if (matchesShape(baseTimeline.at(i))) {
            (*consumed)[i] = true;
            return &baseTimeline.at(i);
        }
    }
    return nullptr;
}

} // namespace

namespace jcut {

nlohmann::json toLegacyClipJson(
    const EditorClip& clip,
    int trackIndex,
    const nlohmann::json* baseClip)
{
    return legacyClipJson(clip, trackIndex, baseClip);
}

nlohmann::json toJson(const EditorDocumentCore& document)
{
    json mediaItems = json::array();
    for (const EditorMediaItem& mediaItem : document.mediaItems) {
        mediaItems.push_back({
            {"id", mediaItem.id},
            {"label", mediaItem.label},
            {"kind", mediaItem.kind},
            {"audioPresenceKnown", mediaItem.audioPresenceKnown},
            {"hasAudio", mediaItem.hasAudio}
        });
    }

    json tracks = json::array();
    for (const EditorTrack& track : document.tracks) {
        json trackJson = {
            {"id", track.id},
            {"label", track.label},
            {"selected", track.selected}
        };
        writeExtendedTrackJson(&trackJson, track);
        tracks.push_back(std::move(trackJson));
    }

    json clips = json::array();
    for (const EditorClip& clip : document.clips) {
        json clipJson = {
            {"id", clip.id},
            {"trackId", clip.trackId},
            {"label", clip.label},
            {"startFrame", clip.startFrame},
            {"durationFrames", clip.durationFrames},
            {"selected", clip.selected},
            {"sourcePath", clip.sourcePath}
        };
        writeExtendedClipJson(&clipJson, clip);
        clips.push_back(std::move(clipJson));
    }

    json renderSyncMarkers = json::array();
    for (const EditorRenderSyncMarker& marker : document.renderSyncMarkers) {
        renderSyncMarkers.push_back({
            {"clipId", marker.clipId},
            {"frame", marker.frame},
            {"action", marker.skipFrame ? "skip" : "duplicate"},
            {"count", marker.count}
        });
    }
    json exportRanges = json::array();
    for (const EditorExportRange& range : document.exportRanges) {
        exportRanges.push_back({
            {"startFrame", range.startFrame},
            {"endFrame", range.endFrame}
        });
    }

    return {
        {"projectName", document.projectName},
        {"mediaItems", std::move(mediaItems)},
        {"tracks", std::move(tracks)},
        {"clips", std::move(clips)},
        {"renderSyncMarkers", std::move(renderSyncMarkers)},
        {"exportRanges", std::move(exportRanges)},
        {"transport", {
            {"playbackActive", document.transport.playbackActive},
            {"playbackSpeed", document.transport.playbackSpeed},
            {"playbackLoopEnabled",
             document.transport.playbackLoopEnabled},
            {"previewViewMode",
             document.transport.previewViewMode},
            {"audioMuted", document.transport.audioMuted},
            {"audioVolume", document.transport.audioVolume},
            {"previewZoom", document.transport.previewZoom},
            {"currentFrame", document.transport.currentFrame}
        }},
        {"panels", {
            {"showWaveform", document.panels.showWaveform},
            {"showTranscript", document.panels.showTranscript},
            {"showScopes", document.panels.showScopes}
        }},
        {"audioTreatment",
         std::string(jcut::editorAudioTreatmentId(document.audioTreatment))},
        {"audioDynamics", audioDynamicsJson(document.audioDynamics)},
        {"exportRequest", jcut::render::toJson(document.exportRequest)}
    };
}

nlohmann::json toLegacyStateJson(const EditorDocumentCore& document, const nlohmann::json* baseRoot)
{
    json root = (baseRoot && baseRoot->is_object()) ? *baseRoot : json::object();

    root["projectName"] = document.projectName.empty() ? "Untitled Project" : document.projectName;
    root["currentFrame"] = document.transport.currentFrame;
    root["playing"] = document.transport.playbackActive;
    root["playbackSpeed"] = document.transport.playbackSpeed;
    root["playbackLoopEnabled"] =
        document.transport.playbackLoopEnabled;
    root["previewViewMode"] =
        document.transport.previewViewMode;
    root["audioMuted"] = document.transport.audioMuted;
    root["audioVolume"] = document.transport.audioVolume;
    root["exportPlaybackSpeed"] = document.exportRequest.playbackSpeed;
    root["timelineZoom"] = document.transport.previewZoom;
    root["audioWaveformVisible"] = document.panels.showWaveform;
    root["playbackAudioWarpMode"] =
        std::string(jcut::editorAudioTreatmentId(document.audioTreatment));
    root["playbackAudioWarpModeExplicit"] = true;
    root["outputWidth"] = document.exportRequest.outputSize.width;
    root["outputHeight"] = document.exportRequest.outputSize.height;
    root["timelineFps"] = document.exportRequest.outputFps;
    root["outputFps"] = document.exportRequest.outputFps;
    root["outputFormat"] = document.exportRequest.outputFormat.empty()
        ? std::string("mp4")
        : document.exportRequest.outputFormat;
    root["lastRenderOutputPath"] = document.exportRequest.outputPath;
    root["renderUseProxies"] = document.exportRequest.useProxyMedia;
    root["createImageSequence"] = document.exportRequest.createVideoFromImageSequence;
    root["imageSequenceFormat"] = document.exportRequest.imageSequenceFormat.empty()
        ? std::string("jpeg")
        : document.exportRequest.imageSequenceFormat;
    root["gradingPreview"] = !document.exportRequest.bypassGrading;
    root["correctionsEnabled"] = document.exportRequest.correctionsEnabled;
    root["backgroundFillEffect"] = document.exportRequest.backgroundFillEffect;
    root["backgroundFillOpacity"] = document.exportRequest.backgroundFillOpacity;
    root["backgroundFillBrightness"] = document.exportRequest.backgroundFillBrightness;
    root["backgroundFillSaturation"] = document.exportRequest.backgroundFillSaturation;
    root["backgroundFillEdgePixels"] = document.exportRequest.backgroundFillEdgePixels;
    root["backgroundFillEdgeProgressive"] = document.exportRequest.backgroundFillEdgeProgressive;
    root["backgroundFillEdgePower"] = document.exportRequest.backgroundFillEdgePower;
    root["backgroundFillStretchSourceClipId"] =
        document.exportRequest.backgroundFillStretchSourceClipId;
    root["transcriptPrependMs"] = document.exportRequest.transcriptPrependMs;
    root["transcriptPostpendMs"] = document.exportRequest.transcriptPostpendMs;
    root["transcriptOffsetMs"] = document.exportRequest.transcriptOffsetMs;
    root["exportStartFrame"] = document.exportRequest.exportStartFrame;
    root["exportEndFrame"] = document.exportRequest.exportEndFrame;
    const json dynamics = audioDynamicsJson(document.audioDynamics);
    for (const auto& [name, value] : dynamics.items()) {
        std::string legacyName = "audio";
        legacyName.push_back(static_cast<char>(std::toupper(
            static_cast<unsigned char>(name.front()))));
        legacyName.append(name.substr(1));
        root[legacyName] = value;
    }

    json renderSyncMarkers = json::array();
    for (const EditorRenderSyncMarker& marker : document.renderSyncMarkers) {
        renderSyncMarkers.push_back({
            {"clipId", marker.clipId},
            {"frame", marker.frame},
            {"action", marker.skipFrame ? "skip" : "duplicate"},
            {"count", marker.count}
        });
    }
    root["renderSyncMarkers"] = std::move(renderSyncMarkers);
    json exportRanges = json::array();
    for (const EditorExportRange& range : document.exportRanges) {
        exportRanges.push_back({
            {"startFrame", range.startFrame},
            {"endFrame", range.endFrame}
        });
    }
    root["exportRanges"] = std::move(exportRanges);

    json mediaItems = json::array();
    for (const EditorMediaItem& mediaItem : document.mediaItems) {
        mediaItems.push_back({
            {"id", mediaItem.id},
            {"label", mediaItem.label},
            {"kind", mediaItem.kind},
            {"audioPresenceKnown", mediaItem.audioPresenceKnown},
            {"hasAudio", mediaItem.hasAudio}
        });
    }
    root["mediaItems"] = std::move(mediaItems);

    int selectedTrackIndex = -1;
    json tracks = json::array();
    const json baseTracks = root.value("tracks", json::array());
    for (std::size_t i = 0; i < document.tracks.size(); ++i) {
        const EditorTrack& track = document.tracks[i];
        const std::size_t preservedTrackIndex = track.id > 0
            ? static_cast<std::size_t>(track.id - 1)
            : i;
        json trackJson = (baseTracks.is_array() &&
                          preservedTrackIndex < baseTracks.size() &&
                          baseTracks.at(preservedTrackIndex).is_object())
            ? baseTracks.at(preservedTrackIndex)
            : json::object();
        trackJson["name"] = track.label.empty()
            ? std::string("Track ") + std::to_string(i + 1)
            : track.label;
        writeExtendedTrackJson(&trackJson, track);
        trackJson["visualsEnabled"] = track.visualMode != 2;
        tracks.push_back(std::move(trackJson));
        if (track.selected) {
            selectedTrackIndex = static_cast<int>(i);
        }
    }
    root["tracks"] = std::move(tracks);
    root["selectedTrackIndex"] = selectedTrackIndex;

    json timeline = json::array();
    json selectedClip = json::object();
    std::string selectedClipId;
    std::string selectedTranscriptActiveCutPath;
    const json baseTimeline = root.value("timeline", json::array());
    std::vector<bool> consumedBaseClips(baseTimeline.is_array() ? baseTimeline.size() : 0, false);
    for (const EditorClip& clip : document.clips) {
        const auto trackIt = std::find_if(
            document.tracks.begin(), document.tracks.end(),
            [&](const EditorTrack& track) { return track.id == clip.trackId; });
        if (trackIt == document.tracks.end()) {
            continue;
        }
        const int trackIndex = static_cast<int>(
            std::distance(document.tracks.begin(), trackIt));
        const json* baseClip = findPreservedClipJson(
            baseTimeline, clip, trackIndex, &consumedBaseClips);
        json clipJson = legacyClipJson(clip, trackIndex, baseClip);
        timeline.push_back(clipJson);
        if (clip.selected) {
            selectedClipId = clipJson.value("id", std::string{});
            selectedClip = clipJson;
            selectedTranscriptActiveCutPath = clip.transcriptActiveCutPath;
        }
    }
    root["timeline"] = std::move(timeline);
    root["selectedClipId"] = selectedClipId;
    root["selectedClip"] = std::move(selectedClip);
    root["selectedClipIds"] = selectedClipId.empty() ? json::array() : json::array({selectedClipId});
    root["transcriptActiveCutPath"] = selectedTranscriptActiveCutPath;

    return root;
}

std::optional<EditorDocumentCore> editorDocumentCoreFromJson(
    const nlohmann::json& root,
    std::string* errorOut)
{
    EditorDocumentCore document;
    const bool looksLikeCoreDocument = root.is_object() &&
        (root.contains("clips") ||
         root.contains("transport") || root.contains("panels") ||
         root.contains("exportRequest") ||
         (root.contains("mediaItems") && !root.contains("timeline")));
    const bool looksLikeLegacyState = root.is_object() && !looksLikeCoreDocument &&
        (root.contains("timeline") || root.contains("selectedClipId") || root.contains("tracks"));
    const bool ok = looksLikeLegacyState
        ? parseLegacyStateDocument(root, &document, errorOut)
        : parseCoreDocument(root, &document, errorOut);
    if (!ok) {
        return std::nullopt;
    }
    if (document.projectName.empty()) {
        document.projectName = "Untitled Project";
    }
    if (document.exportRequest.outputFormat.empty()) {
        document.exportRequest.outputFormat = "mp4";
    }
    if (document.exportRequest.outputSize.width <= 0) {
        document.exportRequest.outputSize.width = 1080;
    }
    if (document.exportRequest.outputSize.height <= 0) {
        document.exportRequest.outputSize.height = 1920;
    }
    if (document.exportRequest.outputFps <= 0.0) {
        document.exportRequest.outputFps = 30.0;
    }
    document.exportRequest.clipCount = document.clips.size();
    document.exportRequest.trackCount = document.tracks.size();
    document.exportRequest.renderSyncMarkerCount = document.renderSyncMarkers.size();
    document.exportRequest.exportRangeCount = document.exportRanges.size();
    return document;
}

std::optional<EditorDocumentCore> editorDocumentCoreFromJsonBytes(
    const std::string& bytes,
    std::string* errorOut)
{
    try {
        return editorDocumentCoreFromJson(json::parse(bytes), errorOut);
    } catch (const std::exception& exception) {
        if (errorOut) {
            *errorOut = exception.what();
        }
        return std::nullopt;
    }
}

std::optional<EditorDocumentCore> loadEditorDocumentCoreFromFile(
    const std::string& path,
    std::string* errorOut)
{
    std::ifstream stream(path);
    if (!stream.is_open()) {
        if (errorOut) {
            *errorOut = "failed to open document file: " + path;
        }
        return std::nullopt;
    }

    json root;
    try {
        stream >> root;
    } catch (const std::exception& exception) {
        if (errorOut) {
            *errorOut = "failed to parse document file: " + std::string(exception.what());
        }
        return std::nullopt;
    }

    return editorDocumentCoreFromJson(root, errorOut);
}

bool saveEditorDocumentCoreToFile(
    const EditorDocumentCore& document,
    const std::string& path,
    std::string* errorOut)
{
    std::ofstream stream(path, std::ios::out | std::ios::trunc);
    if (!stream.is_open()) {
        if (errorOut) {
            *errorOut = "failed to open document file for write: " + path;
        }
        return false;
    }

    try {
        stream << toJson(document).dump(2) << '\n';
    } catch (const std::exception& exception) {
        if (errorOut) {
            *errorOut = "failed to serialize document file: " + std::string(exception.what());
        }
        return false;
    }

    if (!stream.good()) {
        if (errorOut) {
            *errorOut = "failed to write document file: " + path;
        }
        return false;
    }

    return true;
}

bool saveLegacyStateDocumentToFile(
    const EditorDocumentCore& document,
    const std::string& path,
    std::string* errorOut,
    const nlohmann::json* baseRoot)
{
    std::ofstream stream(path, std::ios::out | std::ios::trunc);
    if (!stream.is_open()) {
        if (errorOut) {
            *errorOut = "failed to open document file for write: " + path;
        }
        return false;
    }

    try {
        stream << toLegacyStateJson(document, baseRoot).dump(2) << '\n';
    } catch (const std::exception& exception) {
        if (errorOut) {
            *errorOut = "failed to serialize legacy state file: " + std::string(exception.what());
        }
        return false;
    }

    if (!stream.good()) {
        if (errorOut) {
            *errorOut = "failed to write legacy state file: " + path;
        }
        return false;
    }

    return true;
}

} // namespace jcut
