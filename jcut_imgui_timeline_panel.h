#pragma once

void drawTimelinePanel(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    const ShellLayout layout = computeShellLayout();
    const ImGuiCond layoutCondition = shellState->resetLayoutRequested
        ? ImGuiCond_Always
        : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(layout.timeline.pos, layoutCondition);
    ImGui::SetNextWindowSize(layout.timeline.size, layoutCondition);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("Timeline", nullptr, flags);
    if (!ImGui::GetDragDropPayload() &&
        shellState->timelineDragMode == TimelineDragMode::None) {
        shellState->timelineSnapIndicatorFrame = -1;
    }
    if (ImGui::RadioButton("Select", shellState->timelineToolMode == TimelineToolMode::Select)) {
        shellState->timelineToolMode = TimelineToolMode::Select;
        shellState->statusMessage = "select tool enabled";
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Razor (B)", shellState->timelineToolMode == TimelineToolMode::Razor)) {
        shellState->timelineToolMode = TimelineToolMode::Razor;
        shellState->statusMessage = "razor tool enabled";
    }
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &shellState->timelineSnappingEnabled);

    const std::int64_t timelineFps = std::max<std::int64_t>(
        1,
        snapshot.exportRequest.outputFps > 0.0
            ? static_cast<std::int64_t>(
                  std::llround(snapshot.exportRequest.outputFps))
            : 30);
    std::vector<jcut::timeline_viewport::ClipSpan> timelineClipSpans;
    timelineClipSpans.reserve(snapshot.clips.size());
    for (const jcut::EditorClip& clip : snapshot.clips) {
        timelineClipSpans.push_back({
            clip.startFrame,
            clip.durationFrames,
        });
    }
    const std::int64_t totalTimelineFrames =
        jcut::timeline_viewport::totalFrames(
            timelineClipSpans, timelineFps);
    const float estimatedContentWidth = std::max(
        1.0f,
        ImGui::GetContentRegionAvail().x -
            kTimelineLabelWidth -
            kTimelineTrackPadding -
            16.0f);
    const float minimumTimelineZoom =
        jcut::timeline_viewport::minimumPixelsPerFrame(
            estimatedContentWidth, totalTimelineFrames);
    shellState->timelinePixelsPerFrame = std::clamp(
        shellState->timelinePixelsPerFrame,
        minimumTimelineZoom,
        jcut::timeline_viewport::kMaximumPixelsPerFrame);
    shellState->timelineFrameOffset =
        jcut::timeline_viewport::clampFrameOffset(
            shellState->timelineFrameOffset,
            totalTimelineFrames,
            estimatedContentWidth,
            shellState->timelinePixelsPerFrame);

    ImGui::SameLine();
    ImGui::TextUnformatted("Zoom");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    const float oldTimelineZoom =
        shellState->timelinePixelsPerFrame;
    if (ImGui::SliderFloat(
            "##TimelineZoom",
            &shellState->timelinePixelsPerFrame,
            minimumTimelineZoom,
            jcut::timeline_viewport::kMaximumPixelsPerFrame,
            "%.3g px/f",
            ImGuiSliderFlags_Logarithmic)) {
        const std::int64_t anchorFrame =
            shellState->timelineFrameOffset +
            jcut::timeline_viewport::visibleFrameCount(
                estimatedContentWidth, oldTimelineZoom) /
                2;
        shellState->timelineFrameOffset =
            jcut::timeline_viewport::offsetKeepingFrameAtX(
                anchorFrame,
                estimatedContentWidth * 0.5f,
                shellState->timelinePixelsPerFrame,
                totalTimelineFrames,
                estimatedContentWidth);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        saveUiPreferences(*shellState);
    }
    ImGui::SameLine();
    if (ImGui::Button("Fit##Timeline")) {
        shellState->timelinePixelsPerFrame =
            jcut::timeline_viewport::fitPixelsPerFrame(
                estimatedContentWidth, totalTimelineFrames);
        shellState->timelineFrameOffset = 0;
        saveUiPreferences(*shellState);
    }

    const std::int64_t maximumTimelineOffset =
        jcut::timeline_viewport::maximumFrameOffset(
            totalTimelineFrames,
            estimatedContentWidth,
            shellState->timelinePixelsPerFrame);
    if (maximumTimelineOffset > 0) {
        ImGui::SameLine();
        ImGui::TextUnformatted("View");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(
            std::max(90.0f, ImGui::GetContentRegionAvail().x));
        const std::int64_t minimumTimelineOffset = 0;
        ImGui::SliderScalar(
            "##TimelinePan",
            ImGuiDataType_S64,
            &shellState->timelineFrameOffset,
            &minimumTimelineOffset,
            &maximumTimelineOffset,
            "%lld",
            ImGuiSliderFlags_AlwaysClamp);
    }
    ImGui::Separator();

    const ImVec2 rulerOrigin = ImGui::GetCursorScreenPos();
    const ImVec2 rulerSize(
        ImGui::GetContentRegionAvail().x,
        kTimelineRulerHeight);
    const float rulerContentLeft =
        rulerOrigin.x + kTimelineLabelWidth +
        kTimelineTrackPadding;
    const float rulerContentRight =
        rulerOrigin.x + rulerSize.x - 12.0f;
    const float rulerContentWidth = std::max(
        1.0f, rulerContentRight - rulerContentLeft);
    shellState->timelineFrameOffset =
        jcut::timeline_viewport::clampFrameOffset(
            shellState->timelineFrameOffset,
            totalTimelineFrames,
            rulerContentWidth,
            shellState->timelinePixelsPerFrame);
    ImDrawList* rulerDrawList = ImGui::GetWindowDrawList();
    rulerDrawList->AddRectFilled(
        rulerOrigin,
        ImVec2(
            rulerOrigin.x + rulerSize.x,
            rulerOrigin.y + rulerSize.y),
        IM_COL32(24, 27, 32, 255),
        4.0f);
    rulerDrawList->AddText(
        ImVec2(rulerOrigin.x + 8.0f, rulerOrigin.y + 6.0f),
        IM_COL32(174, 184, 194, 255),
        "Tracks");
    const std::int64_t rulerStep =
        jcut::timeline_viewport::rulerStepFrames(
            shellState->timelinePixelsPerFrame,
            timelineFps);
    const std::int64_t majorRulerStep =
        jcut::timeline_viewport::rulerStepFrames(
            shellState->timelinePixelsPerFrame,
            timelineFps,
            100.0f);
    const std::int64_t visibleStartFrame =
        shellState->timelineFrameOffset;
    const std::int64_t visibleEndFrame =
        visibleStartFrame +
        jcut::timeline_viewport::visibleFrameCount(
            rulerContentWidth,
            shellState->timelinePixelsPerFrame);
    const std::int64_t firstRulerFrame =
        (visibleStartFrame / rulerStep) * rulerStep;
    for (std::int64_t frame = firstRulerFrame, tick = 0;
         frame <= visibleEndFrame && tick < 1000;
         frame += rulerStep, ++tick) {
        const float x = jcut::timeline_viewport::xFromFrame(
            rulerContentLeft,
            frame,
            shellState->timelineFrameOffset,
            shellState->timelinePixelsPerFrame);
        if (x < rulerContentLeft || x > rulerContentRight) {
            continue;
        }
        const bool major = frame % majorRulerStep == 0;
        rulerDrawList->AddLine(
            ImVec2(
                x,
                rulerOrigin.y +
                    (major ? 5.0f : 14.0f)),
            ImVec2(x, rulerOrigin.y + rulerSize.y),
            major
                ? IM_COL32(150, 164, 180, 255)
                : IM_COL32(82, 94, 108, 255));
        if (major) {
            const std::string label =
                jcut::timeline_viewport::timecode(
                    frame, timelineFps);
            rulerDrawList->AddText(
                ImVec2(x + 4.0f, rulerOrigin.y + 4.0f),
                IM_COL32(196, 204, 214, 255),
                label.c_str());
        }
    }
    const float rulerPlayheadX =
        jcut::timeline_viewport::xFromFrame(
            rulerContentLeft,
            snapshot.transport.currentFrame,
            shellState->timelineFrameOffset,
            shellState->timelinePixelsPerFrame);
    if (rulerPlayheadX >= rulerContentLeft &&
        rulerPlayheadX <= rulerContentRight) {
        rulerDrawList->AddTriangleFilled(
            ImVec2(rulerPlayheadX - 5.0f, rulerOrigin.y),
            ImVec2(rulerPlayheadX + 5.0f, rulerOrigin.y),
            ImVec2(rulerPlayheadX, rulerOrigin.y + 7.0f),
            IM_COL32(255, 196, 86, 255));
    }
    ImGui::InvisibleButton("TimelineRuler", rulerSize);
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::GetIO().MousePos.x >= rulerContentLeft) {
        applyCommand(
            shellState,
            jcut::SeekToFrameCommand{
                frameFromTimelineX(
                    rulerContentLeft,
                    ImGui::GetIO().MousePos.x,
                    shellState->timelineFrameOffset,
                    shellState->timelinePixelsPerFrame)});
    }

    const ImVec2 timelineViewportSize =
        ImGui::GetContentRegionAvail();
    ImGui::BeginChild(
        "TimelineViewport",
        timelineViewportSize,
        false,
        ImGuiWindowFlags_AlwaysVerticalScrollbar);
    const ImVec2 viewportAvail = ImGui::GetContentRegionAvail();
    const float requiredCanvasHeight =
        kTimelineTopPadding +
        static_cast<float>(snapshot.tracks.size()) * kTimelineRowHeight +
        ImGui::GetStyle().WindowPadding.y;
    const ImVec2 avail(
        viewportAvail.x,
        std::max(viewportAvail.y, requiredCanvasHeight));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(18, 20, 24, 255), 6.0f);
    const float contentLeft =
        origin.x + kTimelineLabelWidth +
        kTimelineTrackPadding;
    const float contentRight =
        origin.x + avail.x - 12.0f;
    const float contentWidth = std::max(
        1.0f, contentRight - contentLeft);
    const ImGuiIO& timelineIo = ImGui::GetIO();
    if (ImGui::IsWindowHovered(
            ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        timelineIo.MouseWheel != 0.0f) {
        if (timelineIo.KeyCtrl) {
            const std::int64_t anchorFrame =
                jcut::timeline_viewport::frameFromX(
                    contentLeft,
                    timelineIo.MousePos.x,
                    shellState->timelineFrameOffset,
                    shellState->timelinePixelsPerFrame);
            shellState->timelinePixelsPerFrame = std::clamp(
                shellState->timelinePixelsPerFrame *
                    std::pow(1.15f, timelineIo.MouseWheel),
                jcut::timeline_viewport::minimumPixelsPerFrame(
                    contentWidth, totalTimelineFrames),
                jcut::timeline_viewport::kMaximumPixelsPerFrame);
            shellState->timelineFrameOffset =
                jcut::timeline_viewport::offsetKeepingFrameAtX(
                    anchorFrame,
                    timelineIo.MousePos.x - contentLeft,
                    shellState->timelinePixelsPerFrame,
                    totalTimelineFrames,
                    contentWidth);
            saveUiPreferences(*shellState);
        } else if (timelineIo.KeyShift) {
            const std::int64_t panFrames =
                std::max<std::int64_t>(
                    1,
                    jcut::timeline_viewport::visibleFrameCount(
                        contentWidth,
                        shellState->timelinePixelsPerFrame) /
                        12);
            shellState->timelineFrameOffset =
                jcut::timeline_viewport::clampFrameOffset(
                    shellState->timelineFrameOffset -
                        static_cast<std::int64_t>(
                            std::llround(
                                timelineIo.MouseWheel *
                                panFrames)),
                    totalTimelineFrames,
                    contentWidth,
                    shellState->timelinePixelsPerFrame);
        }
    }
    const std::int64_t canvasVisibleStartFrame =
        shellState->timelineFrameOffset;
    const std::int64_t canvasVisibleEndFrame =
        canvasVisibleStartFrame +
        jcut::timeline_viewport::visibleFrameCount(
            contentWidth,
            shellState->timelinePixelsPerFrame);

    constexpr std::array<ImU32, 4> trackColors = {
        IM_COL32(54, 110, 156, 255),
        IM_COL32(96, 132, 66, 255),
        IM_COL32(168, 106, 38, 255),
        IM_COL32(132, 82, 140, 255)
    };

    int hoveredClipId = 0;
    int hoveredTrackId = 0;
    int hoveredTrackVisualToggleId = 0;
    int hoveredTrackAudioToggleId = 0;
    bool hoveredTrackVisualToggleAvailable = false;
    bool hoveredTrackAudioToggleAvailable = false;
    bool hoveredClipIsMaskMatte = false;
    TimelineDragMode hoveredMode = TimelineDragMode::None;
    const jcut::EditorRenderSyncMarker* hoveredRenderSyncMarker = nullptr;
    int hoveredRenderSyncClipId = 0;
    int hoveredRenderSyncTrackId = 0;
    const ImVec2 mousePos = ImGui::GetIO().MousePos;

    const std::int64_t canvasRulerStep =
        jcut::timeline_viewport::rulerStepFrames(
            shellState->timelinePixelsPerFrame,
            timelineFps);
    const std::int64_t canvasMajorRulerStep =
        jcut::timeline_viewport::rulerStepFrames(
            shellState->timelinePixelsPerFrame,
            timelineFps,
            100.0f);
    const std::int64_t firstCanvasRulerFrame =
        (canvasVisibleStartFrame / canvasRulerStep) *
        canvasRulerStep;
    for (std::int64_t frame = firstCanvasRulerFrame, tick = 0;
         frame <= canvasVisibleEndFrame && tick < 1000;
         frame += canvasRulerStep, ++tick) {
        const float x = jcut::timeline_viewport::xFromFrame(
            contentLeft,
            frame,
            shellState->timelineFrameOffset,
            shellState->timelinePixelsPerFrame);
        if (x < contentLeft || x > contentRight) {
            continue;
        }
        const bool major =
            frame % canvasMajorRulerStep == 0;
        drawList->AddLine(
            ImVec2(x, origin.y),
            ImVec2(x, origin.y + avail.y),
            major
                ? IM_COL32(53, 61, 71, 180)
                : IM_COL32(39, 45, 53, 145));
    }

    for (std::size_t i = 0; i < snapshot.tracks.size(); ++i) {
        const jcut::EditorTrack& track = snapshot.tracks[i];
        const bool generatedChildTrack =
            jcut::isGeneratedEditorChildTrack(track);
        const std::string timelineTrackLabel = generatedChildTrack
            ? std::string(kGeneratedTrackLabelPrefix) + track.label
            : track.label;
        const float y = origin.y + kTimelineTopPadding + static_cast<float>(i) * kTimelineRowHeight;
        if (!ImGui::IsRectVisible(
                ImVec2(origin.x, y),
                ImVec2(
                    origin.x + avail.x,
                    y + kTimelineRowHeight))) {
            continue;
        }
        const jcut::EditorTrackMediaPresenceCore trackPresence =
            jcut::editorTrackMediaPresenceCore(snapshot, track.id);
        const ImVec2 visualToggleMin(
            origin.x + kTimelineLabelWidth - 44.0f, y + 4.0f);
        const ImVec2 visualToggleMax(
            origin.x + kTimelineLabelWidth - 27.0f,
            y + kTimelineClipHeight - 4.0f);
        const ImVec2 audioToggleMin(
            origin.x + kTimelineLabelWidth - 23.0f, y + 4.0f);
        const ImVec2 audioToggleMax(
            origin.x + kTimelineLabelWidth - 6.0f,
            y + kTimelineClipHeight - 4.0f);
        const ImVec4 trackLabelClipRect(
            origin.x + 8.0f,
            y,
            visualToggleMin.x - 5.0f,
            y + kTimelineClipHeight);
        drawList->AddText(
            nullptr,
            0.0f,
            ImVec2(origin.x + 10.0f, y + 6.0f),
            track.selected
                ? IM_COL32(255, 214, 140, 255)
                : (generatedChildTrack
                       ? IM_COL32(158, 168, 178, 255)
                       : IM_COL32(224, 228, 232, 255)),
            timelineTrackLabel.c_str(),
            nullptr,
            0.0f,
            &trackLabelClipRect);
        const ImU32 disabledToggleColor = IM_COL32(66, 70, 76, 255);
        const ImU32 visualToggleColor = !trackPresence.hasVisual
            ? disabledToggleColor
            : (track.visualMode == 2
                   ? IM_COL32(115, 72, 72, 255)
                   : (track.visualMode == 1
                          ? IM_COL32(191, 145, 66, 255)
                          : IM_COL32(70, 143, 102, 255)));
        const ImU32 audioToggleColor =
            !trackPresence.hasAudio || generatedChildTrack
            ? disabledToggleColor
            : (track.audioEnabled
                   ? IM_COL32(70, 143, 102, 255)
                   : IM_COL32(115, 72, 72, 255));
        drawList->AddRectFilled(
            visualToggleMin, visualToggleMax, visualToggleColor, 3.0f);
        drawList->AddRectFilled(
            audioToggleMin, audioToggleMax, audioToggleColor, 3.0f);
        drawList->AddText(
            ImVec2(visualToggleMin.x + 4.0f, visualToggleMin.y + 1.0f),
            IM_COL32(238, 240, 242, 255),
            "V");
        drawList->AddText(
            ImVec2(audioToggleMin.x + 4.0f, audioToggleMin.y + 1.0f),
            IM_COL32(238, 240, 242, 255),
            "A");
        if (ImGui::IsMouseHoveringRect(
                visualToggleMin, visualToggleMax)) {
            hoveredTrackVisualToggleId = track.id;
            hoveredTrackVisualToggleAvailable = trackPresence.hasVisual;
        }
        if (ImGui::IsMouseHoveringRect(audioToggleMin, audioToggleMax)) {
            hoveredTrackAudioToggleId = track.id;
            hoveredTrackAudioToggleAvailable =
                trackPresence.hasAudio && !generatedChildTrack;
        }
        drawList->AddRectFilled(ImVec2(contentLeft - kTimelineTrackPadding, y),
                                ImVec2(contentRight, y + kTimelineClipHeight),
                                generatedChildTrack
                                    ? kGeneratedTrackLaneColor
                                    : IM_COL32(34, 38, 44, 255),
                                4.0f);
        for (const jcut::EditorClip& clip : snapshot.clips) {
            if (clip.trackId != track.id) {
                continue;
            }
            const std::int64_t clipEndFrame =
                static_cast<std::int64_t>(clip.startFrame) +
                std::max(0, clip.durationFrames);
            if (clipEndFrame < canvasVisibleStartFrame ||
                clip.startFrame > canvasVisibleEndFrame) {
                continue;
            }
            const bool maskMatteClip =
                jcut::canonicalEditorClipRole(clip.clipRole) == "mask_matte";
            const float clipStart =
                jcut::timeline_viewport::xFromFrame(
                    contentLeft,
                    clip.startFrame,
                    shellState->timelineFrameOffset,
                    shellState->timelinePixelsPerFrame);
            const float clipWidth =
                jcut::timeline_viewport::clipPixelWidth(
                    clip.durationFrames,
                    shellState->timelinePixelsPerFrame);
            const float clipEnd = clipStart + clipWidth;
            const ImVec2 clipMin(
                std::max(contentLeft, clipStart),
                y + 2.0f);
            const ImVec2 clipMax(
                std::min(contentRight, clipEnd),
                y + kTimelineClipHeight - 2.0f);
            if (clipMax.x <= clipMin.x) {
                continue;
            }
            const ImU32 color = generatedChildTrack
                ? (clip.selected
                       ? kGeneratedTrackSelectedClipColor
                       : kGeneratedTrackClipColor)
                : (clip.selected
                       ? IM_COL32(255, 196, 86, 255)
                       : trackColors[i % trackColors.size()]);
            drawList->AddRectFilled(clipMin,
                                    clipMax,
                                    color,
                                    4.0f);
            if (snapshot.panels.showWaveform &&
                track.audioWaveformVisible &&
                clip.hasAudio &&
                clip.audioEnabled &&
                clipMax.x - clipMin.x >= 8.0f) {
                const int waveformColumns = std::clamp(
                    static_cast<int>(
                        std::floor(clipWidth - 4.0f)),
                    16,
                    512);
                std::vector<float> waveformMinimum;
                std::vector<float> waveformMaximum;
                if (shellState->audioRuntime.queryClipWaveform(
                        clip.id,
                        waveformColumns,
                        &waveformMinimum,
                        &waveformMaximum)) {
                    const float waveformCenter =
                        (clipMin.y + clipMax.y) * 0.5f;
                    const float waveformRadius =
                        std::max(
                            1.0f,
                            (clipMax.y - clipMin.y) *
                                0.38f);
                    const float waveformSpan =
                        std::max(
                            1.0f,
                            clipMax.x - clipMin.x - 4.0f);
                    const ImU32 waveformColor =
                        clip.selected
                        ? IM_COL32(70, 66, 48, 205)
                        : IM_COL32(222, 237, 245, 190);
                    for (int column = 0;
                         column < waveformColumns;
                         ++column) {
                        const float x =
                            clipMin.x + 2.0f +
                            (static_cast<float>(column) +
                             0.5f) *
                                waveformSpan /
                                waveformColumns;
                        const float minimum =
                            waveformMinimum[
                                static_cast<std::size_t>(
                                    column)];
                        const float maximum =
                            waveformMaximum[
                                static_cast<std::size_t>(
                                    column)];
                        drawList->AddLine(
                            ImVec2(
                                x,
                                waveformCenter -
                                    maximum *
                                        waveformRadius),
                            ImVec2(
                                x,
                                waveformCenter -
                                    minimum *
                                        waveformRadius),
                            waveformColor,
                            1.0f);
                    }
                }
            }
            const ImVec4 clipLabelClipRect(
                clipMin.x + 3.0f,
                clipMin.y,
                clipMax.x - (clip.locked ? 45.0f : 3.0f),
                clipMax.y);
            if (clipLabelClipRect.z > clipLabelClipRect.x) {
                drawList->AddText(
                    nullptr,
                    0.0f,
                    ImVec2(clipMin.x + 8.0f, y + 6.0f),
                    IM_COL32(245, 245, 245, 255),
                    clip.label.c_str(),
                    nullptr,
                    0.0f,
                    &clipLabelClipRect);
            }
            if (clip.locked) {
                const ImVec4 lockClipRect(
                    std::max(
                        clipMin.x,
                        clipMax.x - 45.0f),
                    clipMin.y,
                    clipMax.x - 3.0f,
                    clipMax.y);
                drawList->AddText(
                    nullptr,
                    0.0f,
                    ImVec2(std::max(clipMin.x + 8.0f, clipMax.x - 42.0f),
                           y + 6.0f),
                    IM_COL32(255, 226, 160, 255),
                    "LOCK",
                    nullptr,
                    0.0f,
                    &lockClipRect);
            }
            if (clip.selected && !clip.locked && !maskMatteClip) {
                if (clipMax.x - clipMin.x <
                    kTimelineHandleWidth * 2.0f) {
                    drawList->AddRect(
                        clipMin,
                        clipMax,
                        IM_COL32(255, 228, 160, 220),
                        3.0f,
                        0,
                        2.0f);
                } else if (clipStart >= contentLeft) {
                    drawList->AddRectFilled(
                        clipMin,
                        ImVec2(
                            clipMin.x + kTimelineHandleWidth,
                            clipMax.y),
                        IM_COL32(255, 228, 160, 180),
                        3.0f);
                }
                if (clipMax.x - clipMin.x >=
                        kTimelineHandleWidth * 2.0f &&
                    clipEnd <= contentRight) {
                    drawList->AddRectFilled(
                        ImVec2(
                            clipMax.x - kTimelineHandleWidth,
                            clipMin.y),
                        clipMax,
                        IM_COL32(255, 228, 160, 180),
                        3.0f);
                }
            }

            if (ImGui::IsMouseHoveringRect(clipMin, clipMax)) {
                hoveredClipId = clip.id;
                hoveredTrackId = track.id;
                hoveredClipIsMaskMatte = maskMatteClip;
                hoveredMode = (shellState->timelineToolMode == TimelineToolMode::Razor ||
                               clip.locked || maskMatteClip)
                    ? TimelineDragMode::None
                    : TimelineDragMode::MoveClip;
                if (shellState->timelineToolMode == TimelineToolMode::Select &&
                    !clip.locked) {
                    if (clip.selected &&
                        clipStart >= contentLeft &&
                        mousePos.x <=
                            clipStart + kTimelineHandleWidth) {
                        hoveredMode = TimelineDragMode::TrimClipStart;
                    } else if (
                        clip.selected &&
                        clipEnd <= contentRight &&
                        mousePos.x >=
                            clipEnd - kTimelineHandleWidth) {
                        hoveredMode = TimelineDragMode::TrimClipEnd;
                    }
                }
            }
        }
    }
    if (hoveredTrackVisualToggleId != 0) {
        ImGui::SetTooltip(
            hoveredTrackVisualToggleAvailable
                ? "Cycle track visual mode: Enabled / Force Opaque / Hidden"
                : "No visual clips on this track");
    } else if (hoveredTrackAudioToggleId != 0) {
        ImGui::SetTooltip(
            hoveredTrackAudioToggleAvailable
                ? "Toggle track audio"
                : "No audio clips on this track");
    }

    // Render-sync decisions belong to a persistent source clip and occupy a
    // timeline frame, so draw and hit-test them independently of selection.
    // This mirrors the Qt timeline's visible marker affordance while keeping
    // marker mutation in the shared runtime commands.
    for (const jcut::EditorRenderSyncMarker& marker :
         snapshot.renderSyncMarkers) {
        const auto clipIt = std::find_if(
            snapshot.clips.begin(), snapshot.clips.end(),
            [&](const jcut::EditorClip& clip) {
                return clip.persistentId == marker.clipId &&
                    jcut::canonicalEditorClipRole(clip.clipRole) !=
                        "mask_matte";
            });
        if (clipIt == snapshot.clips.end() ||
            marker.frame < clipIt->startFrame ||
            marker.frame >= clipIt->startFrame + clipIt->durationFrames ||
            marker.frame < canvasVisibleStartFrame ||
            marker.frame > canvasVisibleEndFrame) {
            continue;
        }
        const auto trackIt = std::find_if(
            snapshot.tracks.begin(), snapshot.tracks.end(),
            [&](const jcut::EditorTrack& track) {
                return track.id == clipIt->trackId;
            });
        if (trackIt == snapshot.tracks.end()) {
            continue;
        }
        const std::size_t trackIndex = static_cast<std::size_t>(
            std::distance(snapshot.tracks.begin(), trackIt));
        const float markerX =
            jcut::timeline_viewport::xFromFrame(
                contentLeft,
                marker.frame,
                shellState->timelineFrameOffset,
                shellState->timelinePixelsPerFrame);
        const float markerY = origin.y + kTimelineTopPadding +
            static_cast<float>(trackIndex) * kTimelineRowHeight + 2.0f;
        const ImVec2 markerMin(markerX - 3.0f, markerY);
        const ImVec2 markerMax(
            markerX + 3.0f, markerY + kTimelineClipHeight - 4.0f);
        const ImU32 markerColor = marker.skipFrame
            ? IM_COL32(255, 158, 61, 235)
            : IM_COL32(255, 91, 91, 235);
        drawList->AddRectFilled(markerMin, markerMax, markerColor, 3.0f);
        drawList->AddRect(
            markerMin, markerMax, IM_COL32(92, 45, 35, 255), 3.0f);
        if (ImGui::IsMouseHoveringRect(markerMin, markerMax)) {
            hoveredRenderSyncMarker = &marker;
            hoveredRenderSyncClipId = clipIt->id;
            hoveredRenderSyncTrackId = clipIt->trackId;
        }
    }
    if (hoveredRenderSyncMarker) {
        ImGui::SetTooltip(
            "%s %d frame%s at %lld",
            hoveredRenderSyncMarker->skipFrame ? "Skip" : "Duplicate",
            hoveredRenderSyncMarker->count,
            hoveredRenderSyncMarker->count == 1 ? "" : "s",
            static_cast<long long>(hoveredRenderSyncMarker->frame));
    }

    if (shellState->timelineToolMode == TimelineToolMode::Razor &&
        hoveredClipId != 0 && !hoveredClipIsMaskMatte) {
        const int razorFrame = frameFromTimelineX(
            contentLeft,
            mousePos.x,
            shellState->timelineFrameOffset,
            shellState->timelinePixelsPerFrame);
        const float razorX =
            jcut::timeline_viewport::xFromFrame(
                contentLeft,
                razorFrame,
                shellState->timelineFrameOffset,
                shellState->timelinePixelsPerFrame);
        drawList->AddLine(ImVec2(razorX, origin.y + 4.0f),
                          ImVec2(razorX, origin.y + avail.y - 4.0f),
                          IM_COL32(120, 220, 255, 230),
                          2.0f);
    }

    if (shellState->timelineSnapIndicatorFrame >= 0) {
        const float snapX =
            jcut::timeline_viewport::xFromFrame(
                contentLeft,
                shellState->timelineSnapIndicatorFrame,
                shellState->timelineFrameOffset,
                shellState->timelinePixelsPerFrame);
        if (snapX >= contentLeft && snapX <= contentRight) {
            drawList->AddLine(
                ImVec2(snapX, origin.y + 4.0f),
                ImVec2(snapX, origin.y + avail.y - 4.0f),
                IM_COL32(92, 232, 178, 230),
                2.0f);
        }
    }

    const float playheadX =
        jcut::timeline_viewport::xFromFrame(
            contentLeft,
            snapshot.transport.currentFrame,
            shellState->timelineFrameOffset,
            shellState->timelinePixelsPerFrame);
    if (playheadX >= contentLeft && playheadX <= contentRight) {
        drawList->AddLine(
            ImVec2(playheadX, origin.y + 6.0f),
            ImVec2(playheadX, origin.y + avail.y - 6.0f),
            IM_COL32(255, 196, 86, 255),
            2.0f);
    }

    const bool mouseInsideCanvas = ImGui::IsMouseHoveringRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y));
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouseInsideCanvas) {
        if (hoveredTrackVisualToggleId != 0) {
            if (hoveredTrackVisualToggleAvailable) {
                const auto trackIt = std::find_if(
                    snapshot.tracks.begin(), snapshot.tracks.end(),
                    [&](const jcut::EditorTrack& track) {
                        return track.id == hoveredTrackVisualToggleId;
                    });
                if (trackIt != snapshot.tracks.end()) {
                    applyCommand(shellState, jcut::SetTrackStateCommand{
                        trackIt->id,
                        (std::clamp(trackIt->visualMode, 0, 2) + 1) % 3,
                        trackIt->audioEnabled,
                        trackIt->audioGain,
                        trackIt->audioMuted,
                        trackIt->audioSolo,
                        trackIt->gradingPreviewEnabled,
                    });
                }
            }
            clearTimelineDrag(shellState);
        } else if (hoveredTrackAudioToggleId != 0) {
            if (hoveredTrackAudioToggleAvailable) {
                const auto trackIt = std::find_if(
                    snapshot.tracks.begin(), snapshot.tracks.end(),
                    [&](const jcut::EditorTrack& track) {
                        return track.id == hoveredTrackAudioToggleId;
                    });
                if (trackIt != snapshot.tracks.end()) {
                    applyCommand(shellState, jcut::SetTrackStateCommand{
                        trackIt->id,
                        trackIt->visualMode,
                        !trackIt->audioEnabled,
                        trackIt->audioGain,
                        trackIt->audioMuted,
                        trackIt->audioSolo,
                        trackIt->gradingPreviewEnabled,
                    });
                }
            }
            clearTimelineDrag(shellState);
        } else if (hoveredClipId != 0) {
            if (shellState->timelineToolMode == TimelineToolMode::Razor &&
                !hoveredClipIsMaskMatte) {
                applyCommand(shellState, jcut::SelectTrackCommand{hoveredTrackId});
                applyCommand(shellState, jcut::SelectClipCommand{hoveredClipId});
                // Clicking with the blade cuts only the clip under it.
                // Ctrl+B and the Edit menu retain explicit group-split
                // behavior for intentionally selected clips.
                applyCommand(shellState, jcut::SplitClipCommand{
                    hoveredClipId,
                    frameFromTimelineX(
                        contentLeft,
                        mousePos.x,
                        shellState->timelineFrameOffset,
                        shellState->timelinePixelsPerFrame)});
                clearTimelineDrag(shellState);
            } else {
                const ImGuiKeyChord keyMods = ImGui::GetIO().KeyMods;
                const bool toggleSelection = (keyMods & ImGuiMod_Ctrl) != 0;
                const bool additiveSelection = !toggleSelection &&
                    (keyMods & ImGuiMod_Shift) != 0;
                const bool selectionOnly = toggleSelection || additiveSelection;
                const auto hoveredIt = std::find_if(
                    snapshot.clips.begin(), snapshot.clips.end(),
                    [&](const jcut::EditorClip& clip) {
                        return clip.id == hoveredClipId;
                    });
                const bool preserveSelectedGroup = !selectionOnly &&
                    hoveredMode == TimelineDragMode::MoveClip &&
                    hoveredIt != snapshot.clips.end() && hoveredIt->selected &&
                    selectedClipCount(snapshot) > 1;
                if (!selectionOnly && hoveredMode != TimelineDragMode::Seek) {
                    beginRuntimeHistoryTransaction(shellState);
                }
                applyCommand(shellState, jcut::SelectTrackCommand{hoveredTrackId});
                if (!preserveSelectedGroup) {
                    applyCommand(shellState, jcut::SelectClipCommand{
                        hoveredClipId, additiveSelection, toggleSelection});
                }
                shellState->timelineDragMode = selectionOnly
                    ? TimelineDragMode::None
                    : hoveredMode;
                shellState->timelineDragClipId = hoveredClipId;
                shellState->timelineDragTrackId = hoveredTrackId;
                shellState->timelineDragMouseX = mousePos.x;
                shellState->timelineDragMouseY = mousePos.y;
                for (std::size_t i = 0; i < snapshot.tracks.size(); ++i) {
                    if (snapshot.tracks[i].id == hoveredTrackId) {
                        shellState->timelineDragTrackIndex = static_cast<int>(i);
                        break;
                    }
                }
                for (const jcut::EditorClip& clip : snapshot.clips) {
                    if (clip.id == hoveredClipId) {
                        shellState->timelineDragStartFrame = clip.startFrame;
                        shellState->timelineDragDurationFrames = clip.durationFrames;
                        break;
                    }
                }
            }
        } else {
            const int trackIndex = trackIndexFromTimelineY(snapshot, origin.y, mousePos.y);
            if (trackIndex >= 0) {
                applyCommand(shellState, jcut::SelectTrackCommand{snapshot.tracks[trackIndex].id});
            }
            if (mousePos.x >= contentLeft) {
                applyCommand(
                    shellState,
                    jcut::SeekToFrameCommand{
                        frameFromTimelineX(
                            contentLeft,
                            mousePos.x,
                            shellState->timelineFrameOffset,
                            shellState->timelinePixelsPerFrame)});
                shellState->timelineDragMode =
                    TimelineDragMode::Seek;
            }
        }
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        mouseInsideCanvas && hoveredRenderSyncMarker &&
        hoveredRenderSyncClipId != 0) {
        const auto ownerIt = std::find_if(
            snapshot.clips.begin(), snapshot.clips.end(),
            [&](const jcut::EditorClip& clip) {
                return clip.id == hoveredRenderSyncClipId;
            });
        if (ownerIt != snapshot.clips.end()) {
            if (!ownerIt->selected) {
                applyCommand(
                    shellState,
                    jcut::SelectTrackCommand{hoveredRenderSyncTrackId});
                applyCommand(
                    shellState,
                    jcut::SelectClipCommand{hoveredRenderSyncClipId});
            }
            applyCommand(shellState, jcut::SeekToFrameCommand{
                static_cast<int>(std::clamp<std::int64_t>(
                    hoveredRenderSyncMarker->frame,
                    0,
                    std::numeric_limits<int>::max()))});
            shellState->timelineContextClipId = ownerIt->id;
            shellState->timelineContextClipPersistentId =
                ownerIt->persistentId;
            shellState->timelineContextFrame =
                hoveredRenderSyncMarker->frame;
            shellState->timelineContextDocumentGeneration =
                shellState->documentGeneration;
            ImGui::OpenPopup("TimelineSyncMarkerContext");
        }
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
               mouseInsideCanvas) {
        const auto hoveredIt = std::find_if(
            snapshot.clips.begin(), snapshot.clips.end(),
            [&](const jcut::EditorClip& clip) {
                return clip.id == hoveredClipId;
            });
        if (hoveredIt != snapshot.clips.end() && !hoveredIt->selected) {
            applyCommand(shellState, jcut::SelectTrackCommand{hoveredTrackId});
            applyCommand(shellState, jcut::SelectClipCommand{hoveredClipId});
        }
        shellState->timelineContextClipId = hoveredClipId;
        shellState->timelineContextClipPersistentId =
            hoveredIt == snapshot.clips.end()
            ? std::string{}
            : hoveredIt->persistentId;
        shellState->timelineContextFrame = snapshot.transport.currentFrame;
        shellState->timelineContextClickFrame =
            frameFromTimelineX(
                contentLeft,
                mousePos.x,
                shellState->timelineFrameOffset,
                shellState->timelinePixelsPerFrame);
        shellState->timelineContextDocumentGeneration =
            shellState->documentGeneration;
        ImGui::OpenPopup("TimelineClipContext");
    }

    if (shellState->timelineDragMode != TimelineDragMode::None && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (shellState->timelineDragMode == TimelineDragMode::Seek) {
            applyCommand(
                shellState,
                jcut::SeekToFrameCommand{
                    frameFromTimelineX(
                        contentLeft,
                        mousePos.x,
                        shellState->timelineFrameOffset,
                        shellState->timelinePixelsPerFrame)});
        } else {
            const int deltaFrames = static_cast<int>(std::lround(
                (mousePos.x - shellState->timelineDragMouseX) /
                shellState->timelinePixelsPerFrame));
            if (shellState->timelineDragMode == TimelineDragMode::MoveClip) {
                int targetTrackId = shellState->timelineDragTrackId;
                const int hoveredTrackIndex = trackIndexFromTimelineY(snapshot, origin.y, mousePos.y);
                if (hoveredTrackIndex >= 0 &&
                    !jcut::isGeneratedEditorChildTrack(snapshot.tracks[
                        static_cast<std::size_t>(hoveredTrackIndex)])) {
                    targetTrackId = snapshot.tracks[hoveredTrackIndex].id;
                }
                const int unsnappedStart = static_cast<int>(std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(shellState->timelineDragStartFrame) + deltaFrames,
                    0,
                    std::numeric_limits<int>::max()));
                const TimelineSnapResult snap = shellState->timelineSnappingEnabled
                    ? snapTimelineMoveStart(
                          snapshot,
                          shellState->timelineDragClipId,
                          unsnappedStart,
                          shellState->timelinePixelsPerFrame)
                    : TimelineSnapResult{unsnappedStart, -1};
                shellState->timelineSnapIndicatorFrame = snap.boundaryFrame;
                applyCommand(shellState, jcut::MoveSelectedClipsCommand{
                    shellState->timelineDragClipId,
                    targetTrackId,
                    snap.frame});
            } else if (shellState->timelineDragMode == TimelineDragMode::TrimClipStart) {
                const int maximumStart = static_cast<int>(
                    std::clamp<std::int64_t>(
                        static_cast<std::int64_t>(
                            shellState->timelineDragStartFrame) +
                            shellState->timelineDragDurationFrames - 1,
                        shellState->timelineDragStartFrame,
                        std::numeric_limits<int>::max()));
                const int unsnappedStart = static_cast<int>(std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(shellState->timelineDragStartFrame) + deltaFrames,
                    0,
                    maximumStart));
                TimelineSnapResult snap = shellState->timelineSnappingEnabled
                    ? snapTimelineBoundary(
                          snapshot,
                          unsnappedStart,
                          shellState->timelinePixelsPerFrame,
                          shellState->timelineDragClipId)
                    : TimelineSnapResult{unsnappedStart, -1};
                snap.frame = std::clamp(snap.frame, 0, maximumStart);
                if (snap.frame != snap.boundaryFrame) {
                    snap.boundaryFrame = -1;
                }
                shellState->timelineSnapIndicatorFrame = snap.boundaryFrame;
                applyCommand(shellState, jcut::TrimClipStartCommand{
                    shellState->timelineDragClipId,
                    snap.frame});
            } else if (shellState->timelineDragMode == TimelineDragMode::TrimClipEnd) {
                const int minimumEnd = static_cast<int>(
                    std::min<std::int64_t>(
                        static_cast<std::int64_t>(
                            shellState->timelineDragStartFrame) + 1,
                        std::numeric_limits<int>::max()));
                const int unsnappedEnd = static_cast<int>(std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(shellState->timelineDragStartFrame) +
                        shellState->timelineDragDurationFrames + deltaFrames,
                    minimumEnd,
                    std::numeric_limits<int>::max()));
                TimelineSnapResult snap = shellState->timelineSnappingEnabled
                    ? snapTimelineBoundary(
                          snapshot,
                          unsnappedEnd,
                          shellState->timelinePixelsPerFrame,
                          shellState->timelineDragClipId)
                    : TimelineSnapResult{unsnappedEnd, -1};
                snap.frame = std::max(minimumEnd, snap.frame);
                if (snap.frame != snap.boundaryFrame) {
                    snap.boundaryFrame = -1;
                }
                shellState->timelineSnapIndicatorFrame = snap.boundaryFrame;
                applyCommand(shellState, jcut::TrimClipEndCommand{
                    shellState->timelineDragClipId,
                    snap.frame});
            }
        }
    }
    if (shellState->timelineDragMode != TimelineDragMode::None && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        clearTimelineDrag(shellState);
    }

    if (ImGui::BeginPopup("TimelineSyncMarkerContext")) {
        const auto contextIt = std::find_if(
            snapshot.clips.begin(), snapshot.clips.end(),
            [&](const jcut::EditorClip& clip) {
                return shellState->timelineContextDocumentGeneration ==
                           shellState->documentGeneration &&
                    clip.id == shellState->timelineContextClipId &&
                    clip.persistentId ==
                        shellState->timelineContextClipPersistentId;
            });
        if (contextIt == snapshot.clips.end()) {
            shellState->statusMessage =
                "render sync menu closed after document change";
            ImGui::CloseCurrentPopup();
        } else {
            drawRenderSyncContextActions(
                shellState,
                snapshot,
                *contextIt,
                shellState->timelineContextFrame);
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("TimelineClipContext")) {
        const auto contextIt = std::find_if(
            snapshot.clips.begin(), snapshot.clips.end(),
            [&](const jcut::EditorClip& clip) {
                return shellState->timelineContextDocumentGeneration ==
                           shellState->documentGeneration &&
                    clip.id == shellState->timelineContextClipId &&
                    clip.persistentId ==
                        shellState->timelineContextClipPersistentId;
            });
        const jcut::EditorClip* contextClip = contextIt == snapshot.clips.end()
            ? nullptr
            : &*contextIt;
        const bool contextDocumentValid =
            shellState->timelineContextDocumentGeneration ==
            shellState->documentGeneration;
        const bool contextClipValid =
            shellState->timelineContextClipId == 0 ||
            contextClip != nullptr;
        if (!contextDocumentValid || !contextClipValid) {
            shellState->statusMessage =
                "timeline menu closed after document change";
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::BeginMenu("Export Range")) {
            const std::int64_t playhead =
                snapshot.transport.currentFrame;
            if (ImGui::MenuItem("Set Start At Playhead")) {
                applyCommand(
                    shellState,
                    jcut::EditExportRangesCommand{
                        jcut::ExportRangeEdit::SetStartAtPlayhead,
                        playhead});
            }
            if (ImGui::MenuItem("Set End At Playhead")) {
                applyCommand(
                    shellState,
                    jcut::EditExportRangesCommand{
                        jcut::ExportRangeEdit::SetEndAtPlayhead,
                        playhead});
            }
            const bool canSplitExportRange =
                jcut::export_range::canSplitAt(
                    snapshot.exportRanges, playhead);
            if (ImGui::MenuItem(
                    "Split At Playhead",
                    nullptr,
                    false,
                    canSplitExportRange)) {
                applyCommand(
                    shellState,
                    jcut::EditExportRangesCommand{
                        jcut::ExportRangeEdit::SplitAtPlayhead,
                        playhead});
            }
            if (ImGui::MenuItem("Reset")) {
                applyCommand(
                    shellState,
                    jcut::EditExportRangesCommand{
                        jcut::ExportRangeEdit::Reset,
                        playhead});
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Create Title")) {
            const jcut::CommandResult result = applyCommand(
                shellState,
                jcut::CreateTitleClipCommand{
                    shellState->timelineContextClickFrame,
                    jcut::kEditorDefaultTitleDurationFrames});
            if (result.applied) {
                shellState->titleDraftClipId = -1;
            }
        }
        ImGui::Separator();
        ImGui::BeginDisabled(contextClip == nullptr);
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, contextClip != nullptr)) {
            applyCommand(shellState, jcut::CutSelectedClipsCommand{});
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, contextClip != nullptr)) {
            applyCommand(shellState, jcut::CopySelectedClipsCommand{});
        }
        if (ImGui::MenuItem("Paste At Playhead", "Ctrl+V")) {
            applyCommand(shellState, jcut::PasteClipsCommand{
                snapshot.transport.currentFrame, selectedTrackId(snapshot)});
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false,
                            contextClip != nullptr)) {
            applyCommand(shellState, jcut::DuplicateSelectedClipsCommand{});
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Nudge Left", "Alt+Left", false,
                            contextClip && !contextClip->locked &&
                                contextClip->startFrame > 0)) {
            applyCommand(shellState, jcut::NudgeSelectedClipCommand{-1});
        }
        if (ImGui::MenuItem("Nudge Right", "Alt+Right", false,
                            contextClip && !contextClip->locked)) {
            applyCommand(shellState, jcut::NudgeSelectedClipCommand{1});
        }
        ImGui::Separator();
        const std::size_t contextSelectionCount =
            selectedClipCount(snapshot);
        const bool groupSelection = contextSelectionCount > 1;
        const bool canSplit = selectedClipsCanSplitAtFrame(
            snapshot, snapshot.transport.currentFrame);
        if (ImGui::MenuItem(
                groupSelection
                    ? "Split Selected At Playhead"
                    : "Split At Playhead",
                "Ctrl+B",
                false,
                canSplit)) {
            if (groupSelection) {
                applyCommand(
                    shellState,
                    jcut::SplitSelectedClipsCommand{
                        snapshot.transport.currentFrame});
            } else if (contextClip) {
                applyCommand(shellState, jcut::SplitClipCommand{
                    contextClip->id,
                    snapshot.transport.currentFrame});
            }
        }
        if (ImGui::BeginMenu("Playback Speed",
                             contextClip && !contextClip->locked)) {
            constexpr std::array<double, 9> playbackRates = {
                0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0};
            for (const double playbackRate : playbackRates) {
                char label[32];
                if (std::abs(playbackRate - 1.0) <= 0.0001) {
                    std::snprintf(label, sizeof(label), "1x (Normal)");
                } else {
                    std::snprintf(label, sizeof(label), "%.3gx", playbackRate);
                }
                const bool selectedRate = contextClip &&
                    std::abs(contextClip->playbackRate - playbackRate) <= 0.0001;
                if (ImGui::MenuItem(label, nullptr, selectedRate) &&
                    contextClip) {
                    applyCommand(shellState, jcut::SetClipPlaybackRateCommand{
                        contextClip->id, playbackRate});
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Scale to Fill Preview", nullptr, false,
                            contextClip && clipCanScaleToFill(*contextClip)) &&
            contextClip) {
            scaleClipToFillPreview(shellState, snapshot, *contextClip);
        }
        const bool canLockTransformToSource =
            contextClip &&
            !contextClip->locked &&
            !contextClip->linkedSourceClipId.empty() &&
            contextClip->mediaKind != "audio";
        if (ImGui::MenuItem(
                "Lock Transform To Source",
                nullptr,
                contextClip && contextClip->sourceTransformLocked,
                canLockTransformToSource) &&
            contextClip) {
            applyCommand(
                shellState,
                jcut::SetClipSourceTransformLockedCommand{
                    contextClip->id,
                    !contextClip->sourceTransformLocked});
        }
        if (ImGui::BeginMenu("Render Sync", contextClip != nullptr)) {
            if (contextClip) {
                drawRenderSyncContextActions(
                    shellState,
                    snapshot,
                    *contextClip,
                    shellState->timelineContextFrame);
            }
            ImGui::EndMenu();
        }
        if (contextClip) {
            ImGui::Separator();
            if (ImGui::MenuItem("Copy Clip Name")) {
                ImGui::SetClipboardText(contextClip->label.c_str());
                shellState->statusMessage = "clip name copied";
            }
            const bool isTitle = contextClip->mediaKind == "title";
            if (ImGui::MenuItem(
                    "Copy title", nullptr, false, isTitle)) {
                const std::int64_t localFrame = std::clamp<std::int64_t>(
                    shellState->timelineContextFrame -
                        contextClip->startFrame,
                    0,
                    std::max(0, contextClip->durationFrames - 1));
                const jcut::EditorTitleKeyframe title =
                    jcut::evaluateEditorClipTitleAtLocalFrame(
                        *contextClip, localFrame);
                ImGui::SetClipboardText(title.text.c_str());
                shellState->statusMessage = "title text copied";
            }
            if (ImGui::MenuItem("Grading...")) {
                shellState->requestedInspectorTab = "Grade";
            }
            if (ImGui::MenuItem("Refresh")) {
                refreshClipMetadata(
                    shellState, snapshot, contextClip->id);
            }
            if (ImGui::MenuItem("Sync...")) {
                shellState->requestedInspectorTab = "Sync";
            }
            if (ImGui::BeginMenu(
                    "Generated Clips",
                    jcut::editorClipHasVisualsCore(*contextClip))) {
                if (ImGui::MenuItem("Add Mask Matte Layer")) {
                    shellState->maskSidecarContextClipId =
                        contextClip->id;
                    shellState->requestedInspectorTab = "Masks";
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(
                    "Rotoscope",
                    jcut::editorClipHasVisualsCore(*contextClip) &&
                        !isTitle)) {
                if (ImGui::MenuItem("Run SAM 3...")) {
                    shellState->promptMaskSourceClipId =
                        contextClip->id;
                    shellState->requestedInspectorTab = "Masks";
                }
                if (ImGui::MenuItem("Run BiRefNet...")) {
                    shellState->birefnetSourceClipId =
                        contextClip->id;
                    shellState->requestedInspectorTab = "Masks";
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Transcript")) {
                if (ImGui::MenuItem(
                        "Transcribe",
                        nullptr,
                        false,
                        contextClip->hasAudio)) {
                    startTranscriptionJob(
                        shellState, *contextClip);
                }
                if (ImGui::MenuItem("Open Transcript Tools")) {
                    shellState->requestedInspectorTab = "Transcript";
                }
                if (ImGui::MenuItem("Apply Speaker Title Fly-In")) {
                    shellState->requestedInspectorTab = "Speakers";
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(
                    "Proxy",
                    jcut::editorClipHasVisualsCore(*contextClip) &&
                        !isTitle)) {
                if (ImGui::MenuItem("Open Proxy Controls")) {
                    shellState->requestedInspectorTab = "Clip";
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(
                    "FaceDetections",
                    jcut::editorClipHasVisualsCore(*contextClip) &&
                        !isTitle)) {
                if (ImGui::MenuItem("Generate / Inspect...")) {
                    shellState->requestedInspectorTab = "Speakers";
                }
                if (ImGui::MenuItem("Open Job Status")) {
                    shellState->requestedInspectorTab = "Jobs";
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Properties")) {
                shellState->requestedInspectorTab = "Properties";
            }
        }
        if (ImGui::MenuItem("Reset Grading", nullptr, false,
                            contextClip != nullptr) && contextClip) {
            applyCommand(shellState,
                         jcut::ResetClipGradingCommand{contextClip->id});
        }
        if (ImGui::MenuItem("Delete Selected", "Delete", false,
                            contextClip && !contextClip->locked)) {
            deleteSelectedClips(shellState);
        }
        if (contextClip) {
            ImGui::Separator();
            const bool anySelectedUnlocked = std::any_of(
                snapshot.clips.begin(),
                snapshot.clips.end(),
                [](const jcut::EditorClip& clip) {
                    return clip.selected &&
                        jcut::canonicalEditorClipRole(clip.clipRole) !=
                            "mask_matte" &&
                        !clip.locked;
                });
            if (groupSelection &&
                ImGui::MenuItem(
                    anySelectedUnlocked
                        ? "Lock Selected"
                        : "Unlock Selected")) {
                applyCommand(
                    shellState,
                    jcut::SetSelectedClipsLockedCommand{
                        anySelectedUnlocked});
            } else if (!groupSelection &&
                       ImGui::MenuItem(
                           contextClip->locked ? "Unlock" : "Lock")) {
                applyCommand(shellState, jcut::SetClipLockedCommand{
                    contextClip->id, !contextClip->locked});
            }
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    RenderSyncMarkerDraft& renderSyncDraft =
        shellState->renderSyncMarkerDraft;
    if (std::exchange(renderSyncDraft.popupRequested, false)) {
        ImGui::OpenPopup("Render Sync Count");
    }
    if (ImGui::BeginPopupModal(
            "Render Sync Count", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto draftClipIt = std::find_if(
            snapshot.clips.begin(), snapshot.clips.end(),
            [&](const jcut::EditorClip& clip) {
                return renderSyncDraft.documentGeneration ==
                           shellState->documentGeneration &&
                    clip.id == renderSyncDraft.clipId &&
                    clip.persistentId == renderSyncDraft.clipPersistentId;
            });
        if (draftClipIt == snapshot.clips.end()) {
            shellState->statusMessage =
                "render sync edit canceled after document change";
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextUnformatted(renderSyncDraft.skipFrame
                ? "How many frames should be skipped for this clip?"
                : "How many extra frames should be duplicated for this clip?");
            ImGui::SetNextItemWidth(160.0f);
            const bool submitFromKeyboard = ImGui::InputInt(
                "Count",
                &renderSyncDraft.count,
                1,
                10,
                ImGuiInputTextFlags_EnterReturnsTrue);
            renderSyncDraft.count = std::clamp(
                renderSyncDraft.count,
                jcut::kEditorRenderSyncMinCount,
                jcut::kEditorRenderSyncMaxCount);
            if (ImGui::Button("Apply") || submitFromKeyboard) {
                applyCommand(shellState, jcut::AddRenderSyncMarkerCommand{
                    draftClipIt->id,
                    renderSyncDraft.frame,
                    renderSyncDraft.skipFrame,
                    renderSyncDraft.count});
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::InvisibleButton("TimelineCanvas", avail);
    if (ImGui::BeginDragDropTarget()) {
        const TimelineTrackDropTarget dropTarget = timelineTrackDropTarget(
            snapshot, origin.y, ImGui::GetIO().MousePos.y);
        int dropFrame = frameFromTimelineX(
            contentLeft,
            ImGui::GetIO().MousePos.x,
            shellState->timelineFrameOffset,
            shellState->timelinePixelsPerFrame);
        if (shellState->timelineSnappingEnabled) {
            const TimelineSnapResult snap = snapTimelineBoundary(
                snapshot,
                dropFrame,
                shellState->timelinePixelsPerFrame);
            dropFrame = snap.frame;
            shellState->timelineSnapIndicatorFrame = snap.boundaryFrame;
        }

        auto payloadText = [](const ImGuiPayload& payload) {
            if (!payload.Data || payload.DataSize <= 1) {
                return std::string{};
            }
            const char* bytes = static_cast<const char*>(payload.Data);
            std::size_t size = static_cast<std::size_t>(payload.DataSize);
            if (bytes[size - 1] != '\0') {
                return std::string{};
            }
            return std::string(bytes, size - 1);
        };

        constexpr ImGuiDragDropFlags acceptFlags =
            ImGuiDragDropFlags_AcceptBeforeDelivery;
        const ImGuiPayload* projectPayload = ImGui::AcceptDragDropPayload(
            kProjectMediaDragPayload, acceptFlags);
        const ImGuiPayload* filesystemPayload = ImGui::AcceptDragDropPayload(
            kFilesystemMediaDragPayload, acceptFlags);
        if (projectPayload || filesystemPayload) {
            const float dropX =
                jcut::timeline_viewport::xFromFrame(
                    contentLeft,
                    dropFrame,
                    shellState->timelineFrameOffset,
                    shellState->timelinePixelsPerFrame);
            const float trackY = origin.y + kTimelineTopPadding +
                static_cast<float>(dropTarget.trackIndex) * kTimelineRowHeight;
            if (dropTarget.insertTrack) {
                drawList->AddLine(
                    ImVec2(origin.x + 4.0f, trackY),
                    ImVec2(origin.x + avail.x - 12.0f, trackY),
                    IM_COL32(92, 232, 178, 255),
                    3.0f);
            } else {
                drawList->AddRect(
                    ImVec2(
                        contentLeft - kTimelineTrackPadding,
                        trackY),
                    ImVec2(contentRight,
                           trackY + kTimelineClipHeight),
                    IM_COL32(92, 232, 178, 230),
                    4.0f,
                    0,
                    2.0f);
            }
            drawList->AddLine(
                ImVec2(dropX, trackY - 3.0f),
                ImVec2(dropX, trackY + kTimelineClipHeight + 3.0f),
                IM_COL32(92, 232, 178, 255),
                3.0f);
        }
        if (projectPayload && projectPayload->IsDelivery()) {
            const std::string mediaId = payloadText(*projectPayload);
            const auto mediaItem = std::find_if(
                snapshot.mediaItems.begin(), snapshot.mediaItems.end(),
                [&](const jcut::EditorMediaItem& item) {
                    return item.id == mediaId;
                });
            if (mediaItem != snapshot.mediaItems.end()) {
                std::int64_t probedDurationFrames = 0;
                const jcut::ImportMediaCommand probedMedia =
                    importMediaCommandForPath(
                        mediaId,
                        mediaItem->label,
                        mediaItem->kind,
                        &probedDurationFrames);
                const int durationFrames = resolvedMediaDurationFrames(
                    0, probedDurationFrames);
                const std::string dropMediaKind = probedMedia.mediaKind;
                insertDroppedMedia(
                    shellState,
                    snapshot,
                    dropTarget,
                    dropMediaKind,
                    dropFrame,
                    durationFrames,
                    [&](int trackId) {
                        applyCommand(shellState, jcut::InsertClipFromMediaCommand{
                            mediaId, trackId, dropFrame, durationFrames});
                    });
            } else {
                shellState->statusMessage = "dropped project media is unavailable";
            }
        }
        if (filesystemPayload && filesystemPayload->IsDelivery()) {
            const fs::path mediaPath(payloadText(*filesystemPayload));
            if (isImportableMediaPath(mediaPath)) {
                const jcut::AddClipCommand addClip = addClipCommandForPath(
                    mediaPath, 0, dropFrame);
                insertDroppedMedia(
                    shellState,
                    snapshot,
                    dropTarget,
                    addClip.mediaKind,
                    dropFrame,
                    addClip.durationFrames,
                    [&](int trackId) {
                        jcut::AddClipCommand routed = addClip;
                        routed.trackId = trackId;
                        applyCommand(shellState, std::move(routed));
                    });
            } else {
                shellState->statusMessage = "dropped media is unavailable";
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();
    ImGui::End();
}
