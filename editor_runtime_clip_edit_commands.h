#pragma once

template <typename T>
std::optional<CommandResult> EditorRuntime::dispatchClipEditCommand(
    const T& typedCommand)
{
    if constexpr (std::is_same_v<T, SetClipGradingCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    clip->brightness = std::clamp(typedCommand.brightness, -10.0, 10.0);
                    clip->contrast = std::clamp(typedCommand.contrast, -10.0, 10.0);
                    clip->saturation = std::clamp(typedCommand.saturation, -10.0, 10.0);
                    clip->gradingPreviewEnabled = typedCommand.previewEnabled;
                    return CommandResult{true, "clip grading updated"};
                } else if constexpr (std::is_same_v<T, ResetClipGradingCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    clip->brightness = 0.0;
                    clip->contrast = 1.0;
                    clip->saturation = 1.0;
                    clip->opacity = 1.0;
                    clip->gradingKeyframes.clear();
                    clip->opacityKeyframes.clear();
                    // Qt normalization materializes one neutral base key for each
                    // visual channel after clearing. Audio-only clips keep the
                    // channels empty.
                    if (editorClipHasVisuals(*clip)) {
                        clip->gradingKeyframes.push_back(EditorGradingKeyframe{});
                        clip->opacityKeyframes.push_back(EditorOpacityKeyframe{});
                    }
                    return CommandResult{true, "clip grading reset"};
                } else if constexpr (std::is_same_v<T, UpsertGradingKeyframeCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    EditorGradingKeyframe keyframe = typedCommand.keyframe;
                    keyframe.frame = std::clamp<std::int64_t>(
                        keyframe.frame, 0, std::max(0, clip->durationFrames - 1));
                    keyframe.brightness = std::clamp(keyframe.brightness, -10.0, 10.0);
                    keyframe.contrast = std::clamp(keyframe.contrast, -10.0, 10.0);
                    keyframe.saturation = std::clamp(keyframe.saturation, -10.0, 10.0);
                    keyframe.opacity = std::clamp(keyframe.opacity, 0.0, 1.0);
                    keyframe.shadowsR = std::clamp(keyframe.shadowsR, -2.0, 2.0);
                    keyframe.shadowsG = std::clamp(keyframe.shadowsG, -2.0, 2.0);
                    keyframe.shadowsB = std::clamp(keyframe.shadowsB, -2.0, 2.0);
                    keyframe.midtonesR = std::clamp(keyframe.midtonesR, -2.0, 2.0);
                    keyframe.midtonesG = std::clamp(keyframe.midtonesG, -2.0, 2.0);
                    keyframe.midtonesB = std::clamp(keyframe.midtonesB, -2.0, 2.0);
                    keyframe.highlightsR = std::clamp(keyframe.highlightsR, -2.0, 2.0);
                    keyframe.highlightsG = std::clamp(keyframe.highlightsG, -2.0, 2.0);
                    keyframe.highlightsB = std::clamp(keyframe.highlightsB, -2.0, 2.0);
                    keyframe.curvePointsR =
                        sanitizeEditorGradingCurve(keyframe.curvePointsR);
                    keyframe.curvePointsG =
                        sanitizeEditorGradingCurve(keyframe.curvePointsG);
                    keyframe.curvePointsB =
                        sanitizeEditorGradingCurve(keyframe.curvePointsB);
                    keyframe.curvePointsLuma =
                        sanitizeEditorGradingCurve(keyframe.curvePointsLuma);
                    if (keyframe.curveThreePointLock) {
                        synchronizeEditorThreePointGradingCurves(&keyframe);
                    }
                    upsertKeyframe(&clip->gradingKeyframes, std::move(keyframe));
                    return CommandResult{true, "grading keyframe updated"};
                } else if constexpr (std::is_same_v<T, SetClipOpacityCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    clip->opacity = std::clamp(typedCommand.opacity, 0.0, 1.0);
                    return CommandResult{true, "clip opacity updated"};
                } else if constexpr (std::is_same_v<T, UpsertOpacityKeyframeCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    EditorOpacityKeyframe keyframe = typedCommand.keyframe;
                    keyframe.frame = std::clamp<std::int64_t>(
                        keyframe.frame, 0, std::max(0, clip->durationFrames - 1));
                    keyframe.opacity = std::clamp(keyframe.opacity, 0.0, 1.0);
                    upsertKeyframe(&clip->opacityKeyframes, std::move(keyframe));
                    return CommandResult{true, "opacity keyframe updated"};
                } else if constexpr (std::is_same_v<T, RemoveClipKeyframeCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    bool removed = false;
                    switch (typedCommand.channel) {
                    case EditorKeyframeChannel::Grading:
                        removed = removeKeyframeAtFrame(
                            &clip->gradingKeyframes, typedCommand.frame);
                        break;
                    case EditorKeyframeChannel::Opacity:
                        removed = removeKeyframeAtFrame(
                            &clip->opacityKeyframes, typedCommand.frame);
                        break;
                    case EditorKeyframeChannel::Transform:
                        removed = removeKeyframeAtFrame(
                            &clip->transformKeyframes, typedCommand.frame);
                        break;
                    case EditorKeyframeChannel::EffectEnabled:
                        removed = removeKeyframeAtFrame(
                            &clip->effectEnabledKeyframes, typedCommand.frame);
                        break;
                    case EditorKeyframeChannel::EffectParameters:
                        removed = removeKeyframeAtFrame(
                            &clip->effectParameterKeyframes, typedCommand.frame);
                        break;
                    case EditorKeyframeChannel::SpeakerFramingEnabled:
                        removed = removeKeyframeAtFrame(
                            &clip->speakerFramingEnabledKeyframes,
                            typedCommand.frame);
                        break;
                    case EditorKeyframeChannel::SpeakerFraming:
                        removed = removeKeyframeAtFrame(
                            &clip->speakerFramingKeyframes,
                            typedCommand.frame);
                        break;
                    case EditorKeyframeChannel::SpeakerFramingTarget:
                        removed = removeKeyframeAtFrame(
                            &clip->speakerFramingTargetKeyframes,
                            typedCommand.frame);
                        break;
                    }
                    return removed
                        ? CommandResult{true, "clip keyframe removed"}
                        : CommandResult{false, "clip keyframe not found"};
                } else if constexpr (std::is_same_v<T, SetClipTransformCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    clip->baseTranslationX = typedCommand.translationX;
                    clip->baseTranslationY = typedCommand.translationY;
                    clip->baseRotation = std::clamp(typedCommand.rotation, -360.0, 360.0);
                    clip->baseScaleX = normalizedScale(typedCommand.scaleX);
                    clip->baseScaleY = normalizedScale(typedCommand.scaleY);
                    return CommandResult{true, "clip transform updated"};
                } else if constexpr (
                    std::is_same_v<T, SetClipSourceTransformLockedCommand>) {
                    EditorClip* clip = findClip(
                        &m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    if (clip->locked) {
                        return CommandResult{false, "clip is locked"};
                    }
                    if (!editorClipHasVisuals(*clip)) {
                        return CommandResult{false, "clip has no visual transform"};
                    }
                    if (typedCommand.locked &&
                        trimmedEditorClipId(
                            clip->linkedSourceClipId).empty()) {
                        return CommandResult{false, "clip has no linked source"};
                    }
                    clip->sourceTransformLocked =
                        typedCommand.locked;
                    return CommandResult{
                        true,
                        typedCommand.locked
                            ? "clip transform locked to source"
                            : "clip source transform unlocked"};
                } else if constexpr (
                    std::is_same_v<T, SetClipSpeakerFramingCommand>) {
                    EditorClip* clip = findClip(
                        &m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    if (clip->locked) {
                        return CommandResult{false, "clip is locked"};
                    }
                    if (!editorClipHasVisuals(*clip)) {
                        return CommandResult{false, "clip has no visual framing"};
                    }
                    clip->speakerFramingEnabled = typedCommand.enabled;
                    clip->speakerFramingBakedTargetXNorm =
                        std::clamp(
                            typedCommand.bakedTargetXNorm, 0.0, 1.0);
                    clip->speakerFramingBakedTargetYNorm =
                        std::clamp(
                            typedCommand.bakedTargetYNorm, 0.0, 1.0);
                    clip->speakerFramingBakedTargetBoxNorm =
                        std::clamp(
                            typedCommand.bakedTargetBoxNorm, -1.0, 1.0);
                    clip->speakerFramingMinConfidence =
                        std::clamp(
                            typedCommand.minConfidence, 0.0, 1.0);
                    clip->speakerFramingManualTrackId =
                        std::max(-1, typedCommand.manualTrackId);
                    clip->speakerFramingManualStreamId =
                        trimmed(typedCommand.manualStreamId);
                    clip->speakerFramingCenterSmoothingFrames =
                        std::clamp(
                            typedCommand.centerSmoothingFrames, 0, 500);
                    clip->speakerFramingZoomSmoothingFrames =
                        std::clamp(
                            typedCommand.zoomSmoothingFrames, 0, 500);
                    clip->speakerFramingSmoothingMode =
                        std::clamp(typedCommand.smoothingMode, 0, 2);
                    clip->speakerFramingCenterSmoothingStrength =
                        std::clamp(
                            typedCommand.centerSmoothingStrength,
                            0.0,
                            5.0);
                    clip->speakerFramingZoomSmoothingStrength =
                        std::clamp(
                            typedCommand.zoomSmoothingStrength,
                            0.0,
                            5.0);
                    clip->speakerFramingGapHoldFrames =
                        std::clamp(
                            typedCommand.gapHoldFrames, 0, 240);
                    return CommandResult{true, "speaker framing settings updated"};
                } else if constexpr (
                    std::is_same_v<
                        T,
                        SetClipSpeakerSectionMinimumWordsCommand>) {
                    EditorClip* clip = findClip(
                        &m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    const int minimumWords =
                        std::clamp(typedCommand.minimumWords, 0, 1000);
                    if (clip->speakerSectionMinimumWords == minimumWords) {
                        return CommandResult{
                            false,
                            "speaker section minimum is unchanged"};
                    }
                    clip->speakerSectionMinimumWords = minimumWords;
                    return CommandResult{
                        true,
                        "speaker section minimum updated"};
                } else if constexpr (
                    std::is_same_v<
                        T,
                        UpsertSpeakerFramingEnabledKeyframeCommand>) {
                    EditorClip* clip = findClip(
                        &m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    EditorBoolKeyframe keyframe = typedCommand.keyframe;
                    keyframe.frame = std::clamp<std::int64_t>(
                        keyframe.frame,
                        0,
                        std::max(0, clip->durationFrames - 1));
                    upsertKeyframe(
                        &clip->speakerFramingEnabledKeyframes,
                        std::move(keyframe));
                    return CommandResult{
                        true,
                        "speaker framing enable keyframe updated"};
                } else if constexpr (
                    std::is_same_v<
                        T,
                        UpsertSpeakerFramingKeyframeCommand>) {
                    EditorClip* clip = findClip(
                        &m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    EditorTransformKeyframe keyframe =
                        typedCommand.keyframe;
                    keyframe.frame = std::clamp<std::int64_t>(
                        keyframe.frame,
                        0,
                        std::max(0, clip->durationFrames - 1));
                    keyframe.rotation =
                        std::clamp(keyframe.rotation, -360.0, 360.0);
                    keyframe.scaleX = normalizedScale(keyframe.scaleX);
                    keyframe.scaleY = normalizedScale(keyframe.scaleY);
                    upsertKeyframe(
                        &clip->speakerFramingKeyframes,
                        std::move(keyframe));
                    return CommandResult{true, "speaker framing keyframe updated"};
                } else if constexpr (
                    std::is_same_v<
                        T,
                        UpsertSpeakerFramingTargetKeyframeCommand>) {
                    EditorClip* clip = findClip(
                        &m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    EditorTransformKeyframe keyframe =
                        typedCommand.keyframe;
                    keyframe.frame = std::clamp<std::int64_t>(
                        keyframe.frame,
                        0,
                        std::max(0, clip->durationFrames - 1));
                    keyframe.translationX =
                        std::clamp(keyframe.translationX, 0.0, 1.0);
                    keyframe.translationY =
                        std::clamp(keyframe.translationY, 0.0, 1.0);
                    keyframe.rotation = 0.0;
                    keyframe.scaleX =
                        std::clamp(keyframe.scaleX, -1.0, 1.0);
                    keyframe.scaleY = keyframe.scaleX;
                    upsertKeyframe(
                        &clip->speakerFramingTargetKeyframes,
                        std::move(keyframe));
                    return CommandResult{
                        true,
                        "speaker framing target keyframe updated"};
                } else if constexpr (std::is_same_v<T, UpsertTransformKeyframeCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    EditorTransformKeyframe keyframe = typedCommand.keyframe;
                    keyframe.frame = std::clamp<std::int64_t>(
                        keyframe.frame, 0, std::max(0, clip->durationFrames - 1));
                    keyframe.rotation = std::clamp(keyframe.rotation, -360.0, 360.0);
                    keyframe.scaleX = normalizedScale(keyframe.scaleX);
                    keyframe.scaleY = normalizedScale(keyframe.scaleY);
                    upsertKeyframe(&clip->transformKeyframes, std::move(keyframe));
                    return CommandResult{true, "transform keyframe updated"};
                } else if constexpr (std::is_same_v<T, CommitPreviewTransformCommand>) {
                    EditorClip* requestedClip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!requestedClip) {
                        return CommandResult{false, "clip not found"};
                    }
                    EditorClip* clip = requestedClip;
                    if (canonicalEditorClipRole(requestedClip->clipRole) == "mask_matte") {
                        const std::string ownerId =
                            trimmedEditorClipId(requestedClip->linkedSourceClipId);
                        const auto owner = std::find_if(
                            m_document.clips.begin(), m_document.clips.end(),
                            [&](const EditorClip& candidate) {
                                return trimmedEditorClipId(candidate.persistentId) == ownerId &&
                                    canonicalEditorClipRole(candidate.clipRole) != "mask_matte" &&
                                    candidate.mediaKind == "video";
                            });
                        if (owner == m_document.clips.end()) {
                            return CommandResult{false, "mask matte transform owner not found"};
                        }
                        clip = &*owner;
                    }
                    if (!editorClipHasVisuals(*clip)) {
                        return CommandResult{false, "clip has no visual transform"};
                    }
                    const std::int64_t localFrame = std::clamp<std::int64_t>(
                        typedCommand.localFrame, 0,
                        std::max(0, clip->durationFrames - 1));
                    if (localFrame > 0 && std::none_of(
                            clip->transformKeyframes.begin(), clip->transformKeyframes.end(),
                            [](const EditorTransformKeyframe& keyframe) {
                                return keyframe.frame == 0;
                            })) {
                        clip->transformKeyframes.push_back(EditorTransformKeyframe{});
                    }
                    EditorTransformKeyframe keyframe;
                    keyframe.frame = localFrame;
                    keyframe.translationX = typedCommand.translationX - clip->baseTranslationX;
                    keyframe.translationY = typedCommand.translationY - clip->baseTranslationY;
                    keyframe.rotation = std::clamp(
                        typedCommand.rotation - clip->baseRotation, -360.0, 360.0);
                    keyframe.scaleX = normalizedScale(
                        normalizedScale(typedCommand.scaleX) /
                        normalizedScale(clip->baseScaleX));
                    keyframe.scaleY = normalizedScale(
                        normalizedScale(typedCommand.scaleY) /
                        normalizedScale(clip->baseScaleY));
                    for (const EditorTransformKeyframe& existing : clip->transformKeyframes) {
                        if (existing.frame > localFrame) {
                            keyframe.linearInterpolation = existing.linearInterpolation;
                            break;
                        }
                    }
                    upsertKeyframe(&clip->transformKeyframes, std::move(keyframe));
                    normalizeMaskMatteParentCaches(&m_document);
                    return CommandResult{true, "preview transform committed"};
                } else if constexpr (std::is_same_v<T, SetClipMaskEffectCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    clip->maskEnabled = typedCommand.maskEnabled;
                    clip->maskFeather = std::clamp(typedCommand.feather, 0.0, 512.0);
                    clip->maskFeatherGamma = std::clamp(typedCommand.featherGamma, 0.1, 8.0);
                    clip->maskFeatherFalloff = std::clamp(typedCommand.featherFalloff, 0, 5);
                    clip->maskEdgeGrayAmount =
                        std::clamp(typedCommand.edgeGrayAmount, 0.0, 1.0);
                    clip->maskEdgeGrayWidth =
                        std::clamp(typedCommand.edgeGrayWidth, 0.001, 2.0);
                    clip->maskEdgeGrayGamma =
                        std::clamp(typedCommand.edgeGrayGamma, 0.1, 8.0);
                    clip->maskForegroundLayerEnabled = typedCommand.foregroundLayerEnabled;
                    clip->maskRepeatEnabled = typedCommand.repeatEnabled;
                    clip->maskRepeatDeltaX = typedCommand.repeatDeltaX;
                    clip->maskRepeatDeltaY = typedCommand.repeatDeltaY;
                    clip->edgeFillEffect =
                        typedCommand.edgeFillEffect.empty() ? "none" : typedCommand.edgeFillEffect;
                    clip->edgeFillPixels = std::clamp(typedCommand.edgeFillPixels, 1, 512);
                    clip->edgeFillPower = std::clamp(typedCommand.edgeFillPower, 0.25, 8.0);
                    clip->edgeFillOpacity = std::clamp(typedCommand.edgeFillOpacity, 0.0, 1.0);
                    clip->edgeFillBrightness = std::clamp(typedCommand.edgeFillBrightness, -1.0, 1.0);
                    clip->edgeFillSaturation = std::clamp(typedCommand.edgeFillSaturation, 0.0, 3.0);
                    clip->effectPreset = typedCommand.effectPreset.empty() ? "none" : typedCommand.effectPreset;
                    clip->effectRows = std::clamp(
                        typedCommand.effectRows,
                        kEditorEffectMinRows,
                        editorEffectMaxRowsForPreset(clip->effectPreset));
                    clip->effectSpeed = std::clamp(
                        typedCommand.effectSpeed,
                        kEditorEffectMinSpeed,
                        kEditorEffectMaxSpeed);
                    clip->effectScale = std::clamp(
                        typedCommand.effectScale,
                        kEditorEffectMinScale,
                        kEditorEffectMaxScale);
                    clip->effectAlternateDirection = typedCommand.alternateDirection;
                    clip->effectSkipAwareTiming = typedCommand.skipAwareTiming;
                    clip->differenceReferenceFrames = std::clamp(
                        typedCommand.differenceReferenceFrames, 1, 300);
                    clip->differenceThreshold = std::clamp(
                        typedCommand.differenceThreshold, 0.0, 1.0);
                    clip->differenceSoftness = std::clamp(
                        typedCommand.differenceSoftness, 0.0, 1.0);
                    clip->temporalEchoCount = std::clamp(
                        typedCommand.temporalEchoCount, 1, 12);
                    clip->temporalEchoSpacingFrames = std::clamp(
                        typedCommand.temporalEchoSpacingFrames, 1, 120);
                    clip->temporalEchoDecay = std::clamp(
                        typedCommand.temporalEchoDecay, 0.0, 1.0);
                    static constexpr std::array<std::string_view, 6> kTilingPatterns = {
                        "grid", "encircle", "spiral_xy", "spiral_xz", "spiral_yz", "diamond"};
                    clip->tilingPattern = std::find(
                        kTilingPatterns.begin(), kTilingPatterns.end(), typedCommand.tilingPattern) !=
                            kTilingPatterns.end()
                        ? typedCommand.tilingPattern
                        : "grid";
                    clip->tilingSpacing = std::clamp(typedCommand.tilingSpacing, 0.1, 8.0);
                    clip->tilingWrap = typedCommand.tilingWrap;
                    clip->effectEnabled = typedCommand.effectEnabled;
                    clip->effectModulationMode =
                        typedCommand.effectModulationMode == "lfo" ||
                        typedCommand.effectModulationMode == "steady_increase"
                            ? typedCommand.effectModulationMode
                            : "none";
                    clip->effectModulationTarget =
                        typedCommand.effectModulationTarget == "rows" ||
                        typedCommand.effectModulationTarget == "speed" ||
                        typedCommand.effectModulationTarget == "spacing"
                            ? typedCommand.effectModulationTarget
                            : "scale";
                    clip->effectModulationAmount = std::clamp(
                        typedCommand.effectModulationAmount, -512.0, 512.0);
                    clip->effectModulationRate = std::clamp(
                        typedCommand.effectModulationRate, 0.0, 20.0);
                    clip->effectModulationPhaseDegrees = std::clamp(
                        typedCommand.effectModulationPhaseDegrees,
                        -360.0,
                        360.0);
                    return CommandResult{true, "clip mask and effect updated"};
                } else if constexpr (
                    std::is_same_v<T, UpsertEffectEnabledKeyframeCommand>) {
                    EditorClip* clip =
                        findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) return CommandResult{false, "clip not found"};
                    EditorBoolKeyframe keyframe = typedCommand.keyframe;
                    keyframe.frame = std::clamp<std::int64_t>(
                        keyframe.frame,
                        0,
                        std::max(0, clip->durationFrames - 1));
                    upsertKeyframe(
                        &clip->effectEnabledKeyframes,
                        std::move(keyframe));
                    return CommandResult{true, "effect enabled keyframe updated"};
                } else if constexpr (
                    std::is_same_v<T, UpsertEffectParameterKeyframeCommand>) {
                    EditorClip* clip =
                        findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) return CommandResult{false, "clip not found"};
                    EditorEffectParameterKeyframe keyframe = typedCommand.keyframe;
                    keyframe.frame = std::clamp<std::int64_t>(
                        keyframe.frame,
                        0,
                        std::max(0, clip->durationFrames - 1));
                    keyframe.effectRows =
                        std::clamp(keyframe.effectRows, 1, 512);
                    keyframe.effectSpeed =
                        std::clamp(keyframe.effectSpeed, -8.0, 8.0);
                    keyframe.effectScale =
                        std::clamp(keyframe.effectScale, 0.1, 8.0);
                    keyframe.differenceReferenceFrames =
                        std::clamp(keyframe.differenceReferenceFrames, 1, 300);
                    keyframe.differenceThreshold =
                        std::clamp(keyframe.differenceThreshold, 0.0, 1.0);
                    keyframe.differenceSoftness =
                        std::clamp(keyframe.differenceSoftness, 0.0, 1.0);
                    keyframe.temporalEchoCount =
                        std::clamp(keyframe.temporalEchoCount, 1, 12);
                    keyframe.temporalEchoSpacingFrames =
                        std::clamp(keyframe.temporalEchoSpacingFrames, 1, 120);
                    keyframe.temporalEchoDecay =
                        std::clamp(keyframe.temporalEchoDecay, 0.0, 1.0);
                    keyframe.tilingSpacing =
                        std::clamp(keyframe.tilingSpacing, 0.1, 8.0);
                    static constexpr std::array<std::string_view, 7> kEffectKeyTilingPatterns = {
                        "grid", "encircle", "spiral", "spiral_xy", "spiral_xz", "spiral_yz", "diamond"};
                    if (std::find(
                            kEffectKeyTilingPatterns.begin(),
                            kEffectKeyTilingPatterns.end(),
                            keyframe.tilingPattern) == kEffectKeyTilingPatterns.end()) {
                        keyframe.tilingPattern = "grid";
                    }
                    upsertKeyframe(
                        &clip->effectParameterKeyframes,
                        std::move(keyframe));
                    return CommandResult{true, "effect parameter keyframe updated"};
                } else if constexpr (std::is_same_v<T, SetClipMaskCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    clip->maskEnabled = typedCommand.maskEnabled;
                    clip->maskFeather = std::clamp(typedCommand.feather, 0.0, 512.0);
                    clip->maskFeatherGamma = std::clamp(
                        typedCommand.featherGamma, 0.1, 8.0);
                    clip->maskFeatherFalloff = std::clamp(
                        typedCommand.featherFalloff, 0, 5);
                    clip->maskEdgeGrayAmount =
                        std::clamp(typedCommand.edgeGrayAmount, 0.0, 1.0);
                    clip->maskEdgeGrayWidth =
                        std::clamp(typedCommand.edgeGrayWidth, 0.001, 2.0);
                    clip->maskEdgeGrayGamma =
                        std::clamp(typedCommand.edgeGrayGamma, 0.1, 8.0);
                    clip->maskForegroundLayerEnabled =
                        typedCommand.foregroundLayerEnabled;
                    clip->maskRepeatEnabled = typedCommand.repeatEnabled;
                    clip->maskRepeatDeltaX = std::clamp(
                        typedCommand.repeatDeltaX, -100000.0, 100000.0);
                    clip->maskRepeatDeltaY = std::clamp(
                        typedCommand.repeatDeltaY, -100000.0, 100000.0);
                    clip->maskDilate = std::clamp(typedCommand.dilate, 0.0, 200.0);
                    clip->maskErode = std::clamp(typedCommand.erode, 0.0, 200.0);
                    clip->maskBlur = std::clamp(typedCommand.blur, 0.0, 200.0);
                    clip->maskTemporalStabilizeEnabled =
                        typedCommand.temporalStabilizeEnabled;
                    clip->maskTemporalStabilizeStrength = std::clamp(
                        typedCommand.temporalStabilizeStrength, 0.0, 1.0);
                    clip->maskTemporalStabilizeMotionRadius = std::clamp(
                        typedCommand.temporalStabilizeMotionRadius, 0, 32);
                    clip->maskInvert = typedCommand.invert;
                    clip->maskShowOnly = typedCommand.showOnly;
                    clip->maskOpacity = std::clamp(typedCommand.opacity, 0.0, 1.0);
                    clip->maskGradeEnabled = typedCommand.gradeEnabled;
                    clip->maskGradeBrightness = std::clamp(
                        typedCommand.gradeBrightness, -1.0, 1.0);
                    clip->maskGradeContrast = std::clamp(
                        typedCommand.gradeContrast, 0.0, 4.0);
                    clip->maskGradeSaturation = std::clamp(
                        typedCommand.gradeSaturation, 0.0, 4.0);
                    clip->maskGradeCurvePointsR = sanitizeEditorGradingCurve(
                        typedCommand.gradeCurvePointsR);
                    clip->maskGradeCurvePointsG = sanitizeEditorGradingCurve(
                        typedCommand.gradeCurvePointsG);
                    clip->maskGradeCurvePointsB = sanitizeEditorGradingCurve(
                        typedCommand.gradeCurvePointsB);
                    clip->maskGradeCurvePointsLuma = sanitizeEditorGradingCurve(
                        typedCommand.gradeCurvePointsLuma);
                    clip->maskGradeCurveSmoothingEnabled =
                        typedCommand.gradeCurveSmoothingEnabled;
                    clip->maskDropShadowEnabled = typedCommand.dropShadowEnabled;
                    clip->maskDropShadowRadius = std::clamp(
                        typedCommand.dropShadowRadius, 0.0, 200.0);
                    clip->maskDropShadowOffsetX = std::clamp(
                        typedCommand.dropShadowOffsetX, -500.0, 500.0);
                    clip->maskDropShadowOffsetY = std::clamp(
                        typedCommand.dropShadowOffsetY, -500.0, 500.0);
                    clip->maskDropShadowOpacity = std::clamp(
                        typedCommand.dropShadowOpacity, 0.0, 1.0);
                    return CommandResult{true, "clip mask updated"};
                } else if constexpr (std::is_same_v<T, MaterializeMaskMatteCommand>) {
                    EditorClip* source = findClip(&m_document.clips, typedCommand.sourceClipId);
                    if (!source || canonicalEditorClipRole(source->clipRole) != "media" ||
                        source->mediaKind != "video" || source->persistentId.empty()) {
                        return CommandResult{false, "source video clip not found"};
                    }
                    const std::string directory = trimmed(typedCommand.sidecarDirectory);
                    const std::string sidecarId = trimmed(typedCommand.sidecarId);
                    if (directory.empty() || sidecarId.empty()) {
                        return CommandResult{false, "mask sidecar is invalid"};
                    }
                    const std::string sourcePersistentId = source->persistentId;
                    const auto existing = std::find_if(
                        m_document.clips.begin(), m_document.clips.end(),
                        [&](const EditorClip& candidate) {
                            return canonicalEditorClipRole(candidate.clipRole) == "mask_matte" &&
                                trimmedEditorClipId(candidate.linkedSourceClipId) ==
                                    trimmedEditorClipId(sourcePersistentId) &&
                                (candidate.generatedFromMaskId == sidecarId ||
                                 candidate.maskFramesDir == directory);
                        });
                    if (existing != m_document.clips.end()) {
                        for (EditorClip& clip : m_document.clips) clip.selected = false;
                        existing->selected = true;
                        return CommandResult{false, "mask matte already exists"};
                    }

                    EditorClip child = *source;
                    int nextId = 1;
                    for (EditorClip& clip : m_document.clips) {
                        nextId = std::max(nextId, clip.id + 1);
                        clip.selected = false;
                    }
                    child.id = nextId;
                    child.persistentId = sourcePersistentId + "-mask-" + sidecarId;
                    int suffix = 2;
                    const auto persistentExists = [&](const std::string& id) {
                        return std::any_of(
                            m_document.clips.begin(), m_document.clips.end(),
                            [&](const EditorClip& clip) { return clip.persistentId == id; });
                    };
                    const std::string persistentBase = child.persistentId;
                    while (persistentExists(child.persistentId)) {
                        child.persistentId = persistentBase + "-" + std::to_string(suffix++);
                    }
                    child.clipRole = "mask_matte";
                    child.linkedSourceClipId = sourcePersistentId;
                    child.generatedFromMaskId = sidecarId;
                    child.syncLockedToSource = true;
                    child.sourceTransformLocked = true;
                    child.label = source->label.empty() ? "Generated Mask"
                        : source->label + " · " +
                            (typedCommand.sidecarLabel.empty()
                                 ? std::string("Generated")
                                 : typedCommand.sidecarLabel) + " Mask";
                    child.selected = true;
                    child.locked = true;
                    child.videoEnabled = true;
                    child.hasAudio = false;
                    child.audioPresenceKnown = true;
                    child.audioEnabled = false;
                    child.audioLinkedToVideo = false;
                    child.audioBusId.clear();
                    child.audioSourcePath.clear();
                    child.audioSourceStatus = "generated";
                    child.audioStreamIndex = -1;
                    child.audioGain = 1.0;
                    child.audioPan = 0.0;
                    child.audioSolo = false;
                    child.maskEnabled = true;
                    child.maskFramesDir = directory;
                    child.maskShowOnly = false;
                    child.maskForegroundLayerEnabled = false;
                    child.effectPreset = "none";
                    child.effectEnabled = true;
                    child.effectEnabledKeyframes.clear();
                    child.effectModulationMode = "none";
                    child.effectModulationTarget = "scale";
                    child.effectModulationAmount = 0.0;
                    child.effectModulationRate = 1.0;
                    child.effectModulationPhaseDegrees = 0.0;
                    child.effectRows = 32;
                    child.effectSpeed = 1.0;
                    child.effectScale = 1.0;
                    child.effectAlternateDirection = true;
                    child.effectSkipAwareTiming = true;
                    child.maskRepeatEnabled = false;
                    child.maskRepeatDeltaX = 160.0;
                    child.maskRepeatDeltaY = 0.0;
                    child.differenceReferenceFrames = 1;
                    child.differenceThreshold = 0.10;
                    child.differenceSoftness = 0.05;
                    child.temporalEchoCount = 4;
                    child.temporalEchoSpacingFrames = 2;
                    child.temporalEchoDecay = 0.65;
                    child.tilingPattern = "grid";
                    child.tilingSpacing = 1.0;
                    child.tilingWrap = true;
                    child.opacity = 1.0;
                    child.opacityKeyframes.clear();
                    child.correctionPolygons.clear();
                    child.titleKeyframes.clear();
                    child.transcriptOverlay = {};
                    child.brightness = source->maskGradeEnabled
                        ? source->maskGradeBrightness : 0.0;
                    child.contrast = source->maskGradeEnabled
                        ? source->maskGradeContrast : 1.0;
                    child.saturation = source->maskGradeEnabled
                        ? source->maskGradeSaturation : 1.0;
                    child.gradingKeyframes.clear();
                    child.maskGradeEnabled = false;
                    child.maskGradeBrightness = 0.0;
                    child.maskGradeContrast = 1.0;
                    child.maskGradeSaturation = 1.0;
                    child.maskGradeCurvePointsR = {{0.0, 0.0}, {1.0, 1.0}};
                    child.maskGradeCurvePointsG = child.maskGradeCurvePointsR;
                    child.maskGradeCurvePointsB = child.maskGradeCurvePointsR;
                    child.maskGradeCurvePointsLuma = child.maskGradeCurvePointsR;
                    child.maskGradeCurveSmoothingEnabled = true;
                    const auto effectiveZ = [](const EditorClip& clip) {
                        return clip.zLevel != std::numeric_limits<int>::min()
                            ? clip.zLevel : -std::max(0, clip.trackId - 1) * 100;
                    };
                    int nextZ = effectiveZ(*source) + 1;
                    for (const EditorClip& candidate : m_document.clips) {
                        if (canonicalEditorClipRole(candidate.clipRole) == "mask_matte" &&
                            trimmedEditorClipId(candidate.linkedSourceClipId) ==
                                trimmedEditorClipId(sourcePersistentId)) {
                            nextZ = std::max(nextZ, effectiveZ(candidate) + 1);
                        }
                    }
                    child.zLevel = nextZ;
                    child.zLevelUserSet = false;
                    m_document.clips.push_back(std::move(child));
                    return CommandResult{true, "mask matte materialized"};
                } else if constexpr (std::is_same_v<T, SetClipZLevelCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) return CommandResult{false, "clip not found"};
                    const int zLevel = typedCommand.automatic
                        ? std::numeric_limits<int>::min()
                        : std::clamp(typedCommand.zLevel, -100000, 100000);
                    const bool userSet = !typedCommand.automatic;
                    if (isTranscriptGeneratedEditorTitle(*clip)) {
                        const std::string sourceId =
                            trimmedEditorClipId(clip->linkedSourceClipId);
                        if (sourceId.empty()) {
                            return CommandResult{false, "generated title layer has no source"};
                        }
                        for (EditorClip& candidate : m_document.clips) {
                            if (isTranscriptGeneratedEditorTitle(candidate) &&
                                trimmedEditorClipId(candidate.linkedSourceClipId) ==
                                    sourceId) {
                                candidate.zLevel = zLevel;
                                candidate.zLevelUserSet = userSet;
                            }
                        }
                        return CommandResult{true, "generated title layer z level updated"};
                    }
                    clip->zLevel = zLevel;
                    clip->zLevelUserSet = userSet;
                    return CommandResult{true, "clip z level updated"};
                } else if constexpr (std::is_same_v<T, SetClipTranscriptOverlayCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    if (isTranscriptGeneratedEditorSubtitle(*clip)) {
                        return CommandResult{false,
                                "generated subtitles must be changed through their source transcript"};
                    }
                    clip->transcriptOverlay = typedCommand.overlay;
                    clip->transcriptOverlay.backgroundOpacity =
                        std::clamp(clip->transcriptOverlay.backgroundOpacity, 0.0, 1.0);
                    clip->transcriptOverlay.backgroundCornerRadius =
                        std::clamp(clip->transcriptOverlay.backgroundCornerRadius, 0.0, 128.0);
                    clip->transcriptOverlay.backgroundPadding =
                        std::clamp(clip->transcriptOverlay.backgroundPadding, 0.0, 400.0);
                    clip->transcriptOverlay.backgroundFrameOpacity =
                        std::clamp(clip->transcriptOverlay.backgroundFrameOpacity, 0.0, 1.0);
                    clip->transcriptOverlay.backgroundFrameWidth =
                        std::clamp(clip->transcriptOverlay.backgroundFrameWidth, 0.0, 120.0);
                    clip->transcriptOverlay.backgroundFrameGap =
                        std::clamp(clip->transcriptOverlay.backgroundFrameGap, 0.0, 200.0);
                    clip->transcriptOverlay.shadowOpacity =
                        std::clamp(clip->transcriptOverlay.shadowOpacity, 0.0, 1.0);
                    clip->transcriptOverlay.shadowOffsetX =
                        std::clamp(clip->transcriptOverlay.shadowOffsetX, -128.0, 128.0);
                    clip->transcriptOverlay.shadowOffsetY =
                        std::clamp(clip->transcriptOverlay.shadowOffsetY, -128.0, 128.0);
                    clip->transcriptOverlay.textOutlineWidth =
                        std::clamp(clip->transcriptOverlay.textOutlineWidth, 0.0, 24.0);
                    clip->transcriptOverlay.textOutlineOpacity =
                        std::clamp(clip->transcriptOverlay.textOutlineOpacity, 0.0, 1.0);
                    if (clip->transcriptOverlay.textExtrudeMode != "stacked_copies" &&
                        clip->transcriptOverlay.textExtrudeMode != "eroded_solid") {
                        clip->transcriptOverlay.textExtrudeMode = "none";
                    }
                    clip->transcriptOverlay.textExtrudeDepth =
                        std::clamp(clip->transcriptOverlay.textExtrudeDepth, 0.0, 2.0);
                    clip->transcriptOverlay.textExtrudeBevelScale =
                        std::clamp(clip->transcriptOverlay.textExtrudeBevelScale, 0.0, 2.0);
                    clip->transcriptOverlay.boxWidth = std::max(160.0, clip->transcriptOverlay.boxWidth);
                    clip->transcriptOverlay.boxHeight = std::max(80.0, clip->transcriptOverlay.boxHeight);
                    clip->transcriptOverlay.maxLines = std::max(1, clip->transcriptOverlay.maxLines);
                    clip->transcriptOverlay.maxCharsPerLine =
                        std::max(8, clip->transcriptOverlay.maxCharsPerLine);
                    clip->transcriptOverlay.fontPointSize =
                        std::max(12, clip->transcriptOverlay.fontPointSize);
                    clip->transcriptOverlay.textOpacity =
                        std::clamp(clip->transcriptOverlay.textOpacity, 0.0, 1.0);
                    return CommandResult{true, "transcript overlay updated"};
                } else if constexpr (std::is_same_v<T, SetClipTranscriptActiveCutCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    if (clip->transcriptActiveCutPath == typedCommand.transcriptPath) {
                        return CommandResult{false, "transcript cut unchanged"};
                    }
                    clip->transcriptActiveCutPath = typedCommand.transcriptPath;
                    return CommandResult{true, "active transcript cut updated"};
                } else if constexpr (std::is_same_v<T, UpsertTitleKeyframeCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    EditorTitleKeyframe keyframe = typedCommand.keyframe;
                    keyframe.frame = std::clamp<std::int64_t>(
                        keyframe.frame, 0, std::max(0, clip->durationFrames - 1));
                    keyframe.fontSize = std::clamp(keyframe.fontSize, 1.0, 1000.0);
                    keyframe.opacity = std::clamp(keyframe.opacity, 0.0, 1.0);
                    const auto normalizeMaterial = [](std::string value) {
                        if (value == "neon" || value == "diagonal_stripes" ||
                            value == "grid" || value == "image_pattern") return value;
                        return std::string("solid");
                    };
                    keyframe.textMaterialStyle = normalizeMaterial(
                        keyframe.textMaterialStyle);
                    keyframe.windowFrameMaterialStyle = normalizeMaterial(
                        keyframe.windowFrameMaterialStyle);
                    keyframe.textPatternScale = std::clamp(
                        keyframe.textPatternScale, 0.1, 8.0);
                    keyframe.dropShadowOpacity = std::clamp(
                        keyframe.dropShadowOpacity, 0.0, 1.0);
                    keyframe.dropShadowOffsetX = std::clamp(
                        keyframe.dropShadowOffsetX, -200.0, 200.0);
                    keyframe.dropShadowOffsetY = std::clamp(
                        keyframe.dropShadowOffsetY, -200.0, 200.0);
                    keyframe.windowOpacity = std::clamp(
                        keyframe.windowOpacity, 0.0, 1.0);
                    keyframe.windowPadding = std::clamp(
                        keyframe.windowPadding, 0.0, 400.0);
                    keyframe.windowWidth = std::clamp(
                        keyframe.windowWidth, 0.0, 10000.0);
                    keyframe.windowFrameOpacity = std::clamp(
                        keyframe.windowFrameOpacity, 0.0, 1.0);
                    keyframe.windowFrameWidth = std::clamp(
                        keyframe.windowFrameWidth, 0.0, 120.0);
                    keyframe.windowFrameGap = std::clamp(
                        keyframe.windowFrameGap, 0.0, 200.0);
                    keyframe.windowFramePatternScale = std::clamp(
                        keyframe.windowFramePatternScale, 0.1, 8.0);
                    if (keyframe.textExtrudeMode != "stacked_copies" &&
                        keyframe.textExtrudeMode != "eroded_solid") {
                        keyframe.textExtrudeMode = "none";
                    }
                    keyframe.vulkan3DExtrudeDepth = std::clamp(
                        keyframe.vulkan3DExtrudeDepth, 0.0, 2.0);
                    keyframe.vulkan3DBevelScale = std::clamp(
                        keyframe.vulkan3DBevelScale, 0.0, 2.0);
                    keyframe.vulkan3DYawDegrees = std::clamp(
                        keyframe.vulkan3DYawDegrees, -360.0, 360.0);
                    keyframe.vulkan3DPitchDegrees = std::clamp(
                        keyframe.vulkan3DPitchDegrees, -360.0, 360.0);
                    keyframe.vulkan3DRollDegrees = std::clamp(
                        keyframe.vulkan3DRollDegrees, -360.0, 360.0);
                    keyframe.vulkan3DDepth = std::clamp(
                        keyframe.vulkan3DDepth, -10.0, 10.0);
                    keyframe.vulkan3DScale = std::clamp(
                        keyframe.vulkan3DScale, 0.01, 10.0);
                    upsertKeyframe(&clip->titleKeyframes, std::move(keyframe));
                    return CommandResult{true, "title keyframe updated"};
                } else if constexpr (std::is_same_v<T, RemoveTitleKeyframeCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    const auto oldSize = clip->titleKeyframes.size();
                    clip->titleKeyframes.erase(
                        std::remove_if(clip->titleKeyframes.begin(), clip->titleKeyframes.end(),
                                       [&](const EditorTitleKeyframe& keyframe) {
                                           return keyframe.frame == typedCommand.frame;
                                       }),
                        clip->titleKeyframes.end());
                    return clip->titleKeyframes.size() == oldSize
                        ? CommandResult{false, "title keyframe not found"}
                        : CommandResult{true, "title keyframe removed"};
                } else if constexpr (std::is_same_v<T, SetClipCorrectionPolygonsCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    std::vector<EditorCorrectionPolygon> polygons = typedCommand.polygons;
                    for (EditorCorrectionPolygon& polygon : polygons) {
                        polygon.startFrame = std::max<std::int64_t>(0, polygon.startFrame);
                        if (polygon.endFrame >= 0) {
                            polygon.endFrame = std::max(polygon.startFrame, polygon.endFrame);
                        }
                        for (EditorPoint& point : polygon.pointsNormalized) {
                            point.x = std::clamp(point.x, 0.0, 1.0);
                            point.y = std::clamp(point.y, 0.0, 1.0);
                        }
                    }
                    clip->correctionPolygons = std::move(polygons);
                    return CommandResult{true, "correction polygons updated"};
                } 
    return std::nullopt;
}
