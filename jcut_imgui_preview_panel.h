#pragma once

void drawPreviewPanel(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    const ShellLayout layout = computeShellLayout();
    const ImGuiCond layoutCondition = shellState->resetLayoutRequested
        ? ImGuiCond_Always
        : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(layout.preview.pos, layoutCondition);
    ImGui::SetNextWindowSize(layout.preview.size, layoutCondition);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("Preview", nullptr, flags);
    const bool videoPreviewMode =
        snapshot.transport.previewViewMode != "audio";
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float controlsHeight = 96.0f;
    const float canvasHeight = std::max(180.0f, avail.y - controlsHeight);
    const ImVec2 canvasSize(avail.x, canvasHeight);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    drawList->AddRectFilled(canvasPos,
                            ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                            IM_COL32(22, 24, 28, 255),
                            6.0f);
    drawList->AddRect(canvasPos,
                      ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                      IM_COL32(70, 78, 86, 255),
                      6.0f);

    const float targetAspect = snapshot.exportRequest.outputSize.height > 0
        ? static_cast<float>(snapshot.exportRequest.outputSize.width) /
            static_cast<float>(snapshot.exportRequest.outputSize.height)
        : (9.0f / 16.0f);
    const float paddedWidth = std::max(120.0f, canvasSize.x - 32.0f);
    const float paddedHeight = std::max(120.0f, canvasSize.y - 32.0f);
    float fittedFrameWidth = paddedWidth;
    float fittedFrameHeight = fittedFrameWidth / std::max(0.1f, targetAspect);
    if (fittedFrameHeight > paddedHeight) {
        fittedFrameHeight = paddedHeight;
        fittedFrameWidth = fittedFrameHeight * targetAspect;
    }
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 canvasMax(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);
    const bool mouseInsideCanvas = videoPreviewMode &&
        ImGui::IsMouseHoveringRect(canvasPos, canvasMax);
    const jcut::EditorClip* previewTitleClip = selectedClip(snapshot);
    const bool selectedTitleIsActive = videoPreviewMode && previewTitleClip &&
        previewTitleClip->mediaKind == "title" &&
        snapshot.transport.currentFrame >= previewTitleClip->startFrame &&
        snapshot.transport.currentFrame <
            previewTitleClip->startFrame + std::max(1, previewTitleClip->durationFrames);
    const bool selectedTransformClipIsActive = videoPreviewMode && previewTitleClip &&
        previewTitleClip->mediaKind != "audio" &&
        previewTitleClip->mediaKind != "title" &&
        snapshot.transport.currentFrame >= previewTitleClip->startFrame &&
        snapshot.transport.currentFrame <
            previewTitleClip->startFrame + std::max(1, previewTitleClip->durationFrames);
    const jcut::EditorClip* correctionClip = previewTitleClip;
    const bool correctionClipIsActive = videoPreviewMode && correctionClip &&
        correctionClip->mediaKind != "audio" &&
        shellState->correctionClipId == correctionClip->id &&
        snapshot.transport.currentFrame >= correctionClip->startFrame &&
        snapshot.transport.currentFrame <
            correctionClip->startFrame + std::max(1, correctionClip->durationFrames);
    const bool correctionInteractionActive = correctionClipIsActive &&
        (shellState->correctionDrawMode || shellState->selectedCorrectionPolygon >= 0 ||
         shellState->correctionPointDragActive);
    float zoom = snapshot.transport.previewZoom;
    const float oldZoom = zoom;
    if (mouseInsideCanvas && std::abs(io.MouseWheel) > 0.001f) {
        const float nextZoom = std::clamp(
            zoom * std::pow(1.12f, io.MouseWheel),
            0.5f,
            3.0f);
        if (std::abs(nextZoom - zoom) > 0.001f) {
            const ImVec2 canvasCenter(
                canvasPos.x + canvasSize.x * 0.5f,
                canvasPos.y + canvasSize.y * 0.5f);
            const ImVec2 mouseFromContentCenter(
                io.MousePos.x - (canvasCenter.x + shellState->previewPanX),
                io.MousePos.y - (canvasCenter.y + shellState->previewPanY));
            const float scale = nextZoom / std::max(0.001f, zoom);
            shellState->previewPanX -= mouseFromContentCenter.x * (scale - 1.0f);
            shellState->previewPanY -= mouseFromContentCenter.y * (scale - 1.0f);
            zoom = nextZoom;
            applyCommand(shellState, jcut::SetPreviewZoomCommand{zoom});
        }
    }
    if (mouseInsideCanvas &&
        ((!selectedTitleIsActive && !selectedTransformClipIsActive &&
          !correctionInteractionActive &&
          ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f))) {
        shellState->previewPanX += io.MouseDelta.x;
        shellState->previewPanY += io.MouseDelta.y;
    }
    if (mouseInsideCanvas && !selectedTitleIsActive &&
        !selectedTransformClipIsActive && !correctionInteractionActive &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        shellState->previewPanX = 0.0f;
        shellState->previewPanY = 0.0f;
        if (std::abs(zoom - 1.0f) > 0.001f) {
            zoom = 1.0f;
            applyCommand(shellState, jcut::SetPreviewZoomCommand{zoom});
        }
    }
    if (std::abs(zoom - oldZoom) < 0.001f && zoom <= 1.001f) {
        shellState->previewPanX *= 0.85f;
        shellState->previewPanY *= 0.85f;
        if (std::abs(shellState->previewPanX) < 0.25f) shellState->previewPanX = 0.0f;
        if (std::abs(shellState->previewPanY) < 0.25f) shellState->previewPanY = 0.0f;
    }
    const float frameWidth = fittedFrameWidth * zoom;
    const float frameHeight = fittedFrameHeight * zoom;
    const float maxPanX = std::max(0.0f, (frameWidth - fittedFrameWidth) * 0.5f + 48.0f);
    const float maxPanY = std::max(0.0f, (frameHeight - fittedFrameHeight) * 0.5f + 48.0f);
    shellState->previewPanX = std::clamp(shellState->previewPanX, -maxPanX, maxPanX);
    shellState->previewPanY = std::clamp(shellState->previewPanY, -maxPanY, maxPanY);

    const ImVec2 frameMin(
        canvasPos.x + (canvasSize.x - frameWidth) * 0.5f + shellState->previewPanX,
        canvasPos.y + (canvasSize.y - frameHeight) * 0.5f + shellState->previewPanY);
    const ImVec2 frameMax(frameMin.x + frameWidth, frameMin.y + frameHeight);
    const bool mouseInsideProgram = ImGui::IsMouseHoveringRect(frameMin, frameMax);

    const auto correctionPointToScreen = [&](const jcut::EditorPoint& point) {
        return ImVec2(
            frameMin.x + static_cast<float>(point.x) * frameWidth,
            frameMin.y + static_cast<float>(point.y) * frameHeight);
    };
    const auto correctionPointFromScreen = [&](const ImVec2& point) {
        return jcut::EditorPoint{
            std::clamp(static_cast<double>((point.x - frameMin.x) /
                                           std::max(1.0f, frameWidth)), 0.0, 1.0),
            std::clamp(static_cast<double>((point.y - frameMin.y) /
                                           std::max(1.0f, frameHeight)), 0.0, 1.0)};
    };

    if (!correctionClipIsActive && shellState->correctionPointDragActive) {
        shellState->correctionPointDragActive = false;
        shellState->correctionPointDragPolygon = -1;
        shellState->correctionPointDragPoint = -1;
        endRuntimeHistoryTransaction(shellState);
    }
    if (correctionClipIsActive && mouseInsideProgram &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (shellState->correctionDrawMode) {
            shellState->correctionDraftPoints.push_back(
                correctionPointFromScreen(io.MousePos));
        } else {
            constexpr float kPointHitRadius = 9.0f;
            float bestDistanceSquared = kPointHitRadius * kPointHitRadius;
            int hitPolygon = -1;
            int hitPoint = -1;
            for (std::size_t polygonIndex = 0;
                 polygonIndex < correctionClip->correctionPolygons.size();
                 ++polygonIndex) {
                const auto& points = correctionClip->correctionPolygons[polygonIndex].pointsNormalized;
                for (std::size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex) {
                    const ImVec2 screenPoint = correctionPointToScreen(points[pointIndex]);
                    const float deltaX = io.MousePos.x - screenPoint.x;
                    const float deltaY = io.MousePos.y - screenPoint.y;
                    const float distanceSquared = deltaX * deltaX + deltaY * deltaY;
                    if (distanceSquared <= bestDistanceSquared) {
                        bestDistanceSquared = distanceSquared;
                        hitPolygon = static_cast<int>(polygonIndex);
                        hitPoint = static_cast<int>(pointIndex);
                    }
                }
            }
            if (hitPolygon >= 0) {
                shellState->selectedCorrectionPolygon = hitPolygon;
                shellState->correctionPointDragActive = true;
                shellState->correctionPointDragPolygon = hitPolygon;
                shellState->correctionPointDragPoint = hitPoint;
                shellState->correctionPointDragPolygons = correctionClip->correctionPolygons;
                beginRuntimeHistoryTransaction(shellState);
            }
        }
    }
    if (shellState->correctionPointDragActive && correctionClipIsActive &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const int polygonIndex = shellState->correctionPointDragPolygon;
        const int pointIndex = shellState->correctionPointDragPoint;
        if (polygonIndex >= 0 &&
            polygonIndex < static_cast<int>(shellState->correctionPointDragPolygons.size()) &&
            pointIndex >= 0 && pointIndex < static_cast<int>(
                shellState->correctionPointDragPolygons[polygonIndex].pointsNormalized.size())) {
            shellState->correctionPointDragPolygons[polygonIndex].pointsNormalized[pointIndex] =
                correctionPointFromScreen(io.MousePos);
            applyCommand(shellState, jcut::SetClipCorrectionPolygonsCommand{
                correctionClip->id, shellState->correctionPointDragPolygons});
        }
    }
    if (shellState->correctionPointDragActive &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        shellState->correctionPointDragActive = false;
        shellState->correctionPointDragPolygon = -1;
        shellState->correctionPointDragPoint = -1;
        endRuntimeHistoryTransaction(shellState);
    }

    jcut::EditorTransformKeyframe selectedPreviewTransform;
    ImVec2 transformBoundsMin{};
    ImVec2 transformBoundsMax{};
    ImVec2 transformRightHandleMin{};
    ImVec2 transformRightHandleMax{};
    ImVec2 transformBottomHandleMin{};
    ImVec2 transformBottomHandleMax{};
    ImVec2 transformCornerHandleMin{};
    ImVec2 transformCornerHandleMax{};
    ImVec2 transformRotationHandleMin{};
    ImVec2 transformRotationHandleMax{};
    ImVec2 transformRotationHandleCenter{};
    if (selectedTransformClipIsActive) {
        selectedPreviewTransform = shellState->previewTransformDragMode !=
                PreviewTransformDragMode::None
            ? shellState->previewTransformDragValue
            : jcut::evaluateEditorClipTransformAtLocalFrame(
                  *previewTitleClip,
                  snapshot.transport.currentFrame - previewTitleClip->startFrame);
        const double radians = selectedPreviewTransform.rotation *
            3.14159265358979323846 / 180.0;
        const float scaledWidth = frameWidth * static_cast<float>(
            std::abs(selectedPreviewTransform.scaleX));
        const float scaledHeight = frameHeight * static_cast<float>(
            std::abs(selectedPreviewTransform.scaleY));
        const float boundsWidth = std::abs(static_cast<float>(std::cos(radians))) * scaledWidth +
            std::abs(static_cast<float>(std::sin(radians))) * scaledHeight;
        const float boundsHeight = std::abs(static_cast<float>(std::sin(radians))) * scaledWidth +
            std::abs(static_cast<float>(std::cos(radians))) * scaledHeight;
        const float outputWidth = static_cast<float>(
            std::max(1, snapshot.exportRequest.outputSize.width));
        const float outputHeight = static_cast<float>(
            std::max(1, snapshot.exportRequest.outputSize.height));
        const ImVec2 center(
            (frameMin.x + frameMax.x) * 0.5f +
                static_cast<float>(selectedPreviewTransform.translationX) *
                    frameWidth / outputWidth,
            (frameMin.y + frameMax.y) * 0.5f +
                static_cast<float>(selectedPreviewTransform.translationY) *
                    frameHeight / outputHeight);
        transformBoundsMin = {center.x - boundsWidth * 0.5f,
                              center.y - boundsHeight * 0.5f};
        transformBoundsMax = {center.x + boundsWidth * 0.5f,
                              center.y + boundsHeight * 0.5f};
        constexpr float kHandleRadius = 7.0f;
        transformRightHandleMin = {
            transformBoundsMax.x - kHandleRadius,
            center.y - kHandleRadius};
        transformRightHandleMax = {
            transformBoundsMax.x + kHandleRadius,
            center.y + kHandleRadius};
        transformBottomHandleMin = {
            center.x - kHandleRadius,
            transformBoundsMax.y - kHandleRadius};
        transformBottomHandleMax = {
            center.x + kHandleRadius,
            transformBoundsMax.y + kHandleRadius};
        transformCornerHandleMin = {
            transformBoundsMax.x - kHandleRadius,
            transformBoundsMax.y - kHandleRadius};
        transformCornerHandleMax = {
            transformBoundsMax.x + kHandleRadius,
            transformBoundsMax.y + kHandleRadius};
        transformRotationHandleCenter = {
            std::clamp(
                center.x,
                frameMin.x + kHandleRadius,
                frameMax.x - kHandleRadius),
            transformBoundsMin.y - 26.0f};
        if (transformRotationHandleCenter.y - kHandleRadius < frameMin.y) {
            transformRotationHandleCenter.y =
                transformBoundsMin.y + 26.0f;
        }
        transformRotationHandleCenter.y = std::clamp(
            transformRotationHandleCenter.y,
            frameMin.y + kHandleRadius,
            frameMax.y - kHandleRadius);
        transformRotationHandleMin = {
            transformRotationHandleCenter.x - kHandleRadius,
            transformRotationHandleCenter.y - kHandleRadius};
        transformRotationHandleMax = {
            transformRotationHandleCenter.x + kHandleRadius,
            transformRotationHandleCenter.y + kHandleRadius};
    }
    const auto pointInRect = [](const ImVec2& point, const ImVec2& minimum,
                                const ImVec2& maximum) {
        return point.x >= minimum.x && point.x <= maximum.x &&
            point.y >= minimum.y && point.y <= maximum.y;
    };
    if ((!selectedTransformClipIsActive ||
         shellState->previewTransformDragClipId != previewTitleClip->id) &&
        shellState->previewTransformDragMode != PreviewTransformDragMode::None) {
        shellState->previewTransformDragMode = PreviewTransformDragMode::None;
        shellState->previewTransformDragClipId = -1;
        endRuntimeHistoryTransaction(shellState);
    }
    if (selectedTransformClipIsActive && !correctionInteractionActive &&
        mouseInsideProgram && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        PreviewTransformDragMode dragMode = PreviewTransformDragMode::None;
        if (pointInRect(
                io.MousePos,
                transformRotationHandleMin,
                transformRotationHandleMax)) {
            dragMode = PreviewTransformDragMode::Rotate;
        } else if (pointInRect(io.MousePos, transformCornerHandleMin, transformCornerHandleMax)) {
            dragMode = PreviewTransformDragMode::ResizeBoth;
        } else if (pointInRect(io.MousePos, transformRightHandleMin, transformRightHandleMax)) {
            dragMode = PreviewTransformDragMode::ResizeX;
        } else if (pointInRect(io.MousePos, transformBottomHandleMin, transformBottomHandleMax)) {
            dragMode = PreviewTransformDragMode::ResizeY;
        } else if (pointInRect(io.MousePos, transformBoundsMin, transformBoundsMax)) {
            dragMode = PreviewTransformDragMode::Move;
        }
        if (dragMode != PreviewTransformDragMode::None) {
            shellState->previewTransformDragMode = dragMode;
            shellState->previewTransformDragClipId = previewTitleClip->id;
            shellState->previewTransformDragOriginMouse = io.MousePos;
            shellState->previewTransformDragOriginBoundsMin = transformBoundsMin;
            shellState->previewTransformDragOriginBoundsMax = transformBoundsMax;
            shellState->previewTransformDragValue = selectedPreviewTransform;
            beginRuntimeHistoryTransaction(shellState);
        }
    }
    if (shellState->previewTransformDragMode != PreviewTransformDragMode::None &&
        selectedTransformClipIsActive && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float deltaX = io.MousePos.x - shellState->previewTransformDragOriginMouse.x;
        const float deltaY = io.MousePos.y - shellState->previewTransformDragOriginMouse.y;
        if (std::abs(deltaX) > 0.0001f || std::abs(deltaY) > 0.0001f) {
            jcut::EditorTransformKeyframe next = shellState->previewTransformDragValue;
            const float outputWidth = static_cast<float>(
                std::max(1, snapshot.exportRequest.outputSize.width));
            const float outputHeight = static_cast<float>(
                std::max(1, snapshot.exportRequest.outputSize.height));
            const double previewScaleX = frameWidth / outputWidth;
            const double previewScaleY = frameHeight / outputHeight;
            if (shellState->previewTransformDragMode == PreviewTransformDragMode::Move) {
                next.translationX += deltaX / std::max(0.0001, previewScaleX);
                next.translationY += deltaY / std::max(0.0001, previewScaleY);
            } else if (
                shellState->previewTransformDragMode ==
                PreviewTransformDragMode::Rotate) {
                const jcut::preview::PointD center{
                    (shellState->previewTransformDragOriginBoundsMin.x +
                     shellState->previewTransformDragOriginBoundsMax.x) *
                        0.5,
                    (shellState->previewTransformDragOriginBoundsMin.y +
                     shellState->previewTransformDragOriginBoundsMax.y) *
                        0.5};
                next.rotation = jcut::preview::rotationForPointerDrag(
                    next.rotation,
                    center,
                    {shellState->previewTransformDragOriginMouse.x,
                     shellState->previewTransformDragOriginMouse.y},
                    {io.MousePos.x, io.MousePos.y},
                    io.KeyShift ? 15.0 : 0.0);
            } else {
                const double originWidth = std::max(
                    1.0f, shellState->previewTransformDragOriginBoundsMax.x -
                              shellState->previewTransformDragOriginBoundsMin.x);
                const double originHeight = std::max(
                    1.0f, shellState->previewTransformDragOriginBoundsMax.y -
                              shellState->previewTransformDragOriginBoundsMin.y);
                double factorX = 1.0 + deltaX / originWidth;
                double factorY = 1.0 + deltaY / originHeight;
                jcut::preview::ResizeAnchor anchor = jcut::preview::ResizeAnchor::Center;
                if (shellState->previewTransformDragMode == PreviewTransformDragMode::ResizeX) {
                    factorY = 1.0;
                    anchor = jcut::preview::ResizeAnchor::Left;
                } else if (shellState->previewTransformDragMode == PreviewTransformDragMode::ResizeY) {
                    factorX = 1.0;
                    anchor = jcut::preview::ResizeAnchor::Top;
                } else {
                    const double uniformFactor =
                        std::abs(factorX) >= std::abs(factorY) ? factorX : factorY;
                    factorX = uniformFactor;
                    factorY = uniformFactor;
                    anchor = jcut::preview::ResizeAnchor::TopLeft;
                }
                const auto boundedScale = [](double value) {
                    if (std::abs(value) >= 0.01) return value;
                    return value < 0.0 ? -0.01 : 0.01;
                };
                next.scaleX = boundedScale(next.scaleX * factorX);
                next.scaleY = boundedScale(next.scaleY * factorY);
                const jcut::preview::PointD translation =
                    jcut::preview::translationForAnchoredResize(
                        {shellState->previewTransformDragValue.translationX,
                         shellState->previewTransformDragValue.translationY},
                        {shellState->previewTransformDragValue.scaleX,
                         shellState->previewTransformDragValue.scaleY},
                        {next.scaleX, next.scaleY},
                        {shellState->previewTransformDragOriginBoundsMin.x,
                         shellState->previewTransformDragOriginBoundsMin.y,
                         originWidth, originHeight},
                        anchor,
                        {previewScaleX, previewScaleY});
                next.translationX = translation.x;
                next.translationY = translation.y;
            }
            next.frame = snapshot.transport.currentFrame - previewTitleClip->startFrame;
            shellState->previewTransformDragValue = next;
            shellState->previewTransformDragOriginMouse = io.MousePos;
            shellState->previewTransformDragOriginBoundsMin = transformBoundsMin;
            shellState->previewTransformDragOriginBoundsMax = transformBoundsMax;
            applyCommand(shellState, jcut::CommitPreviewTransformCommand{
                previewTitleClip->id, next.frame, next.translationX, next.translationY,
                next.rotation, next.scaleX, next.scaleY});
        }
    }
    if (shellState->previewTransformDragMode != PreviewTransformDragMode::None &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        shellState->previewTransformDragMode = PreviewTransformDragMode::None;
        shellState->previewTransformDragClipId = -1;
        endRuntimeHistoryTransaction(shellState);
    }

    if ((!selectedTitleIsActive ||
         shellState->previewTitleDragClipId != previewTitleClip->id) &&
        shellState->previewTitleDragActive) {
        shellState->previewTitleDragActive = false;
        shellState->previewTitleDragClipId = -1;
        endRuntimeHistoryTransaction(shellState);
    }
    if (selectedTitleIsActive && !correctionInteractionActive &&
        !shellState->correctionPointDragActive && mouseInsideProgram &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        shellState->previewTitleDragActive = true;
        shellState->previewTitleDragClipId = previewTitleClip->id;
        shellState->previewTitleDragKeyframe =
            jcut::evaluateEditorClipTitleAtLocalFrame(
                *previewTitleClip,
                snapshot.transport.currentFrame - previewTitleClip->startFrame);
        shellState->previewTitleDragKeyframe.frame =
            snapshot.transport.currentFrame - previewTitleClip->startFrame;
        beginRuntimeHistoryTransaction(shellState);
    }
    if (shellState->previewTitleDragActive && selectedTitleIsActive &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const double outputWidth = std::max(1, snapshot.exportRequest.outputSize.width);
        const double outputHeight = std::max(1, snapshot.exportRequest.outputSize.height);
        const double deltaX = io.MouseDelta.x * outputWidth /
            std::max(1.0f, frameWidth);
        const double deltaY = io.MouseDelta.y * outputHeight /
            std::max(1.0f, frameHeight);
        if (std::abs(deltaX) > 0.0001 || std::abs(deltaY) > 0.0001) {
            shellState->previewTitleDragKeyframe.translationX += deltaX;
            shellState->previewTitleDragKeyframe.translationY += deltaY;
            shellState->titleDraftClipId = previewTitleClip->id;
            shellState->titleDraft = shellState->previewTitleDragKeyframe;
            applyCommand(shellState, jcut::UpsertTitleKeyframeCommand{
                previewTitleClip->id,
                shellState->previewTitleDragKeyframe});
        }
    }
    if (shellState->previewTitleDragActive &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        shellState->previewTitleDragActive = false;
        shellState->previewTitleDragClipId = -1;
        endRuntimeHistoryTransaction(shellState);
    }

    drawList->PushClipRect(canvasPos, canvasMax, true);
    drawList->AddRectFilled(
        frameMin,
        frameMax,
        videoPreviewMode
            ? IM_COL32(12, 14, 18, 255)
            : IM_COL32(42, 46, 54, 255),
        4.0f);
    if (videoPreviewMode && shellState->previewTextureId != 0) {
        if (shellState->
                previewHardwarePresentationTransformValid) {
            const jcut::EditorTransformKeyframe& transform =
                shellState->
                    previewHardwarePresentationTransform;
            const jcut::preview::RectD outputRect{
                frameMin.x, frameMin.y, frameWidth, frameHeight};
            const jcut::preview::PointD outputSize{
                static_cast<double>(std::max(
                    1, snapshot.exportRequest.outputSize.width)),
                static_cast<double>(std::max(
                    1, snapshot.exportRequest.outputSize.height))};
            const jcut::preview::RectD imageRect =
                jcut::preview::fittedPresentationRect(
                    outputRect,
                    outputSize,
                    {
                        static_cast<double>(std::max(
                            1,
                            shellState->
                                previewHardwareSourceSize.width)),
                        static_cast<double>(std::max(
                            1,
                            shellState->
                                previewHardwareSourceSize.height))});
            const auto quad =
                jcut::preview::transformedPresentationQuad(
                    imageRect,
                    outputRect,
                    outputSize,
                    {
                        transform.translationX,
                        transform.translationY},
                    {transform.scaleX, transform.scaleY},
                    transform.rotation);
            const auto screenPoint = [](const auto& point) {
                return ImVec2(
                    static_cast<float>(point.x),
                    static_cast<float>(point.y));
            };
            const ImU32 tint = IM_COL32(
                255, 255, 255,
                static_cast<int>(std::lround(
                    shellState->
                        previewHardwarePresentationOpacity *
                    255.0)));
            drawList->AddImageQuad(
                shellState->previewTextureId,
                screenPoint(quad[0]),
                screenPoint(quad[1]),
                screenPoint(quad[2]),
                screenPoint(quad[3]),
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 0.0f),
                ImVec2(1.0f, 1.0f),
                ImVec2(0.0f, 1.0f),
                tint);
        } else {
            drawList->AddImage(
                shellState->previewTextureId,
                frameMin,
                frameMax,
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f));
        }
        if (shellState->previewOverlayTextureId != 0) {
            const float outputWidth = static_cast<float>(
                std::max(
                    1,
                    snapshot.exportRequest.outputSize.width));
            const float outputHeight = static_cast<float>(
                std::max(
                    1,
                    snapshot.exportRequest.outputSize.height));
            const ImVec2 overlayMin{
                frameMin.x +
                    frameWidth *
                    static_cast<float>(
                        shellState->previewOverlayX) /
                    outputWidth,
                frameMin.y +
                    frameHeight *
                    static_cast<float>(
                        shellState->previewOverlayY) /
                    outputHeight};
            const ImVec2 overlayMax{
                overlayMin.x +
                    frameWidth *
                    static_cast<float>(
                        shellState->
                            previewOverlaySize.width) /
                    outputWidth,
                overlayMin.y +
                    frameHeight *
                    static_cast<float>(
                        shellState->
                            previewOverlaySize.height) /
                    outputHeight};
            drawList->AddImage(
                shellState->previewOverlayTextureId,
                overlayMin,
                overlayMax,
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f));
        }
    } else if (!videoPreviewMode) {
        const jcut::ImGuiAudioStatus audioStatus =
            shellState->audioRuntime.status();
        const float waveformCenterY =
            (frameMin.y + frameMax.y) * 0.5f;
        const float waveformHalfHeight =
            frameHeight * 0.38f;
        drawList->AddLine(
            ImVec2(frameMin.x + 10.0f, waveformCenterY),
            ImVec2(frameMax.x - 10.0f, waveformCenterY),
            IM_COL32(72, 82, 94, 255));
        for (std::size_t point = 0;
             point < audioStatus.recentWaveform.size();
             ++point) {
            const float x = frameMin.x + 10.0f +
                (frameWidth - 20.0f) *
                    static_cast<float>(point) /
                    static_cast<float>(
                        audioStatus.recentWaveform.size() - 1);
            const float amplitude =
                audioStatus.recentWaveform[point] *
                waveformHalfHeight;
            drawList->AddLine(
                ImVec2(x, waveformCenterY - amplitude),
                ImVec2(x, waveformCenterY + amplitude),
                IM_COL32(92, 210, 178, 230));
        }
    }
    drawList->AddRect(frameMin, frameMax, IM_COL32(160, 110, 56, 255), 4.0f, 0, 2.0f);
    const float inset = std::clamp(0.12f / std::max(0.5f, zoom), 0.04f, 0.18f);
    const ImVec2 safeMin(frameMin.x + frameWidth * inset, frameMin.y + frameHeight * 0.08f);
    const ImVec2 safeMax(frameMax.x - frameWidth * inset, frameMax.y - frameHeight * 0.08f);
    drawList->AddRect(safeMin, safeMax, IM_COL32(236, 160, 74, 255), 4.0f, 0, 2.0f);
    drawList->AddText(ImVec2(frameMin.x + 14.0f, frameMin.y + 12.0f),
                      IM_COL32(242, 242, 242, 255),
                      videoPreviewMode ? "Program" : "Audio Preview");
    std::string previewDetail =
        "Frame " + std::to_string(snapshot.transport.currentFrame);
    if (!videoPreviewMode) {
        const jcut::ImGuiAudioStatus audioStatus =
            shellState->audioRuntime.status();
        previewDetail += " | " + audioStatus.message;
    }
    {
        std::lock_guard<std::mutex> lock(shellState->previewMutex);
        if (!shellState->previewResult.message.empty()) {
            previewDetail += " | " + shellState->previewResult.message;
        }
    }
    drawList->AddText(ImVec2(frameMin.x + 14.0f, frameMin.y + 34.0f),
                      IM_COL32(180, 188, 198, 255),
                      previewDetail.c_str());
    const std::string previewFaceClipIdentity = previewTitleClip
        ? (previewTitleClip->persistentId.empty()
            ? std::to_string(previewTitleClip->id)
            : previewTitleClip->persistentId)
        : std::string{};
    const std::string previewFaceContextSuffix =
        "::" + previewFaceClipIdentity;
    if (previewTitleClip &&
        shellState->transcriptCache.faceArtifactContext.ends_with(
            previewFaceContextSuffix) &&
        shellState->transcriptCache.selectedFaceTrackIds.size() == 1) {
        const int selectedTrackId =
            shellState->transcriptCache.selectedFaceTrackIds.front();
        const auto selectedTrack = std::find_if(
            shellState->transcriptCache.faceInspection.tracks.begin(),
            shellState->transcriptCache.faceInspection.tracks.end(),
            [&](const auto& track) {
                return track.trackId == selectedTrackId;
            });
        if (selectedTrack !=
            shellState->transcriptCache.faceInspection.tracks.end()) {
            const float centerX = frameMin.x +
                static_cast<float>(selectedTrack->x) * frameWidth;
            const float centerY = frameMin.y +
                static_cast<float>(selectedTrack->y) * frameHeight;
            const float boxSize = static_cast<float>(
                std::clamp(selectedTrack->box, 0.01, 1.0));
            const float halfWidth = boxSize * frameWidth * 0.5f;
            const float halfHeight = boxSize * frameHeight * 0.5f;
            const ImVec2 faceMin(
                std::max(frameMin.x, centerX - halfWidth),
                std::max(frameMin.y, centerY - halfHeight));
            const ImVec2 faceMax(
                std::min(frameMax.x, centerX + halfWidth),
                std::min(frameMax.y, centerY + halfHeight));
            drawList->AddRect(
                faceMin, faceMax, IM_COL32(92, 230, 150, 255),
                3.0f, 0, 3.0f);
            const std::string referenceLabel =
                "Face track " + std::to_string(selectedTrackId);
            drawList->AddText(
                ImVec2(faceMin.x, std::max(frameMin.y, faceMin.y - 20.0f)),
                IM_COL32(92, 230, 150, 255),
                referenceLabel.c_str());
        }
    }
    if (selectedTransformClipIsActive && !correctionInteractionActive) {
        const ImU32 outlineColor = shellState->previewTransformDragMode !=
                PreviewTransformDragMode::None
            ? IM_COL32(255, 196, 92, 255)
            : IM_COL32(92, 196, 255, 235);
        drawList->AddRect(transformBoundsMin, transformBoundsMax,
                          outlineColor, 2.0f, 0, 2.0f);
        const auto drawHandle = [&](const ImVec2& minimum, const ImVec2& maximum) {
            drawList->AddRectFilled(minimum, maximum, IM_COL32(24, 28, 34, 245), 2.0f);
            drawList->AddRect(minimum, maximum, outlineColor, 2.0f, 0, 2.0f);
        };
        drawHandle(transformRightHandleMin, transformRightHandleMax);
        drawHandle(transformBottomHandleMin, transformBottomHandleMax);
        drawHandle(transformCornerHandleMin, transformCornerHandleMax);
        drawList->AddLine(
            ImVec2(
                (transformBoundsMin.x + transformBoundsMax.x) * 0.5f,
                transformBoundsMin.y),
            transformRotationHandleCenter,
            outlineColor,
            2.0f);
        drawList->AddCircleFilled(
            transformRotationHandleCenter,
            7.0f,
            IM_COL32(24, 28, 34, 245));
        drawList->AddCircle(
            transformRotationHandleCenter,
            7.0f,
            outlineColor,
            0,
            2.0f);
        if (mouseInsideProgram) {
            if (pointInRect(
                    io.MousePos,
                    transformRotationHandleMin,
                    transformRotationHandleMax)) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            } else if (pointInRect(io.MousePos, transformCornerHandleMin, transformCornerHandleMax)) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
            } else if (pointInRect(io.MousePos, transformRightHandleMin, transformRightHandleMax)) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            } else if (pointInRect(io.MousePos, transformBottomHandleMin, transformBottomHandleMax)) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            } else if (pointInRect(io.MousePos, transformBoundsMin, transformBoundsMax)) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }
        }
    }
    if (correctionClipIsActive) {
        const std::int64_t localFrame =
            snapshot.transport.currentFrame - correctionClip->startFrame;
        for (std::size_t polygonIndex = 0;
             polygonIndex < correctionClip->correctionPolygons.size();
             ++polygonIndex) {
            const auto& polygon = correctionClip->correctionPolygons[polygonIndex];
            if (polygon.pointsNormalized.empty()) {
                continue;
            }
            const bool inRange = polygon.enabled && localFrame >= polygon.startFrame &&
                (polygon.endFrame < 0 || localFrame <= polygon.endFrame);
            const bool selected = shellState->selectedCorrectionPolygon ==
                static_cast<int>(polygonIndex);
            const ImU32 lineColor = selected
                ? IM_COL32(255, 196, 72, 255)
                : (inRange ? IM_COL32(255, 92, 92, 235) : IM_COL32(130, 136, 146, 180));
            for (std::size_t pointIndex = 0;
                 pointIndex < polygon.pointsNormalized.size();
                 ++pointIndex) {
                const ImVec2 point = correctionPointToScreen(
                    polygon.pointsNormalized[pointIndex]);
                if (polygon.pointsNormalized.size() > 1) {
                    const ImVec2 next = correctionPointToScreen(
                        polygon.pointsNormalized[(pointIndex + 1) %
                                                 polygon.pointsNormalized.size()]);
                    drawList->AddLine(point, next, lineColor, selected ? 2.5f : 1.5f);
                }
                drawList->AddCircleFilled(point, selected ? 5.0f : 3.5f,
                                          IM_COL32(24, 26, 30, 245));
                drawList->AddCircle(point, selected ? 5.0f : 3.5f, lineColor, 0, 2.0f);
            }
        }
        for (std::size_t pointIndex = 0;
             pointIndex < shellState->correctionDraftPoints.size();
             ++pointIndex) {
            const ImVec2 point = correctionPointToScreen(
                shellState->correctionDraftPoints[pointIndex]);
            if (pointIndex > 0) {
                drawList->AddLine(
                    correctionPointToScreen(shellState->correctionDraftPoints[pointIndex - 1]),
                    point, IM_COL32(92, 220, 255, 255), 2.0f);
            }
            drawList->AddCircleFilled(point, 4.5f, IM_COL32(92, 220, 255, 255));
        }
        if (mouseInsideProgram) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }
    if (selectedTitleIsActive) {
        const jcut::EditorTitleKeyframe title =
            shellState->previewTitleDragActive
            ? shellState->previewTitleDragKeyframe
            : jcut::evaluateEditorClipTitleAtLocalFrame(
                  *previewTitleClip,
                  snapshot.transport.currentFrame - previewTitleClip->startFrame);
        std::size_t maximumLineCharacters = 0;
        std::size_t lineCharacters = 0;
        int lineCount = 1;
        for (const char character : title.text) {
            if (character == '\n') {
                maximumLineCharacters = std::max(maximumLineCharacters, lineCharacters);
                lineCharacters = 0;
                ++lineCount;
            } else if ((static_cast<unsigned char>(character) & 0xc0) != 0x80) {
                ++lineCharacters;
            }
        }
        maximumLineCharacters = std::max(maximumLineCharacters, lineCharacters);
        const float outputWidth = static_cast<float>(
            std::max(1, snapshot.exportRequest.outputSize.width));
        const float outputHeight = static_cast<float>(
            std::max(1, snapshot.exportRequest.outputSize.height));
        const float titleCenterX = frameMin.x + frameWidth * 0.5f +
            static_cast<float>(title.translationX) * frameWidth / outputWidth;
        const float titleCenterY = frameMin.y + frameHeight * 0.5f +
            static_cast<float>(title.translationY) * frameHeight / outputHeight;
        const float renderedFontHeight = std::max(
            12.0f, static_cast<float>(title.fontSize) * frameHeight / outputHeight);
        const float boundsWidth = std::max(
            48.0f, static_cast<float>(maximumLineCharacters) * renderedFontHeight * 0.64f + 16.0f);
        const float boundsHeight = std::max(
            28.0f, renderedFontHeight * static_cast<float>(lineCount) * 1.25f + 12.0f);
        const ImVec2 titleBoundsMin(
            titleCenterX - boundsWidth * 0.5f,
            titleCenterY - boundsHeight * 0.5f);
        const ImVec2 titleBoundsMax(
            titleCenterX + boundsWidth * 0.5f,
            titleCenterY + boundsHeight * 0.5f);
        drawList->AddRect(
            titleBoundsMin, titleBoundsMax,
            shellState->previewTitleDragActive
                ? IM_COL32(255, 196, 92, 255)
                : IM_COL32(92, 196, 255, 230),
            2.0f, 0, 2.0f);
        drawList->AddCircleFilled(
            ImVec2(titleCenterX, titleCenterY), 3.5f,
            IM_COL32(255, 255, 255, 240));
        if (mouseInsideProgram && !correctionInteractionActive) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
    }
    drawList->PopClipRect();

    ImGui::Dummy(canvasSize);
    ImGui::Separator();
    const int transportEndFrame = [&]() {
        int endFrame = 0;
        for (const jcut::EditorClip& clip : snapshot.clips) {
            endFrame = std::max(
                endFrame, clip.startFrame + clip.durationFrames);
        }
        return endFrame;
    }();
    if (ImGui::Button("|<")) {
        applyCommand(shellState, jcut::SeekToFrameCommand{0});
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Go to start");
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("StepBack", ImGuiDir_Left)) {
        applyCommand(shellState, jcut::StepFrameCommand{-1});
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Previous playable frame");
    }
    ImGui::SameLine();
    if (ImGui::Button(snapshot.transport.playbackActive ? "Pause" : "Play")) {
        applyCommand(shellState, jcut::TogglePlaybackCommand{});
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("StepForward", ImGuiDir_Right)) {
        applyCommand(shellState, jcut::StepFrameCommand{1});
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Next playable frame");
    }
    ImGui::SameLine();
    if (ImGui::Button(">|")) {
        applyCommand(
            shellState, jcut::SeekToFrameCommand{transportEndFrame});
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Go to end");
    }
    constexpr std::array<float, 9> transportSpeeds = {
        0.1f, 0.25f, 0.5f, 0.75f, 1.0f,
        1.25f, 1.5f, 2.0f, 3.0f};
    constexpr std::array<const char*, 9> transportSpeedLabels = {
        "10%", "25%", "50%", "75%", "100%",
        "125%", "150%", "200%", "300%"};
    int speedIndex = 0;
    float nearestSpeedDistance =
        std::numeric_limits<float>::max();
    for (int index = 0;
         index < static_cast<int>(transportSpeeds.size());
         ++index) {
        const float distance = std::abs(
            snapshot.transport.playbackSpeed -
            transportSpeeds[static_cast<std::size_t>(index)]);
        if (distance < nearestSpeedDistance) {
            speedIndex = index;
            nearestSpeedDistance = distance;
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(76.0f);
    if (ImGui::Combo(
            "Speed",
            &speedIndex,
            transportSpeedLabels.data(),
            static_cast<int>(transportSpeedLabels.size()))) {
        applyCommand(
            shellState,
            jcut::SetPlaybackSpeedCommand{
                transportSpeeds[static_cast<std::size_t>(speedIndex)]});
    }
    bool playbackLoopEnabled =
        snapshot.transport.playbackLoopEnabled;
    ImGui::SameLine();
    if (ImGui::Checkbox("Loop", &playbackLoopEnabled)) {
        applyCommand(
            shellState,
            jcut::SetPlaybackLoopEnabledCommand{
                playbackLoopEnabled});
    }
    constexpr std::array<const char*, 2> previewModeLabels = {
        "Video", "Audio"};
    int previewModeIndex = videoPreviewMode ? 0 : 1;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(82.0f);
    if (ImGui::Combo(
            "View",
            &previewModeIndex,
            previewModeLabels.data(),
            static_cast<int>(previewModeLabels.size()))) {
        applyCommand(
            shellState,
            jcut::SetPreviewViewModeCommand{
                previewModeIndex == 1 ? "audio" : "video"});
    }
    bool transportAudioMuted =
        snapshot.transport.audioMuted;
    ImGui::SameLine();
    if (ImGui::Checkbox("Mute", &transportAudioMuted)) {
        applyCommand(
            shellState,
            jcut::SetTransportAudioCommand{
                transportAudioMuted,
                snapshot.transport.audioVolume});
    }
    float transportAudioVolume =
        snapshot.transport.audioVolume;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::SliderFloat(
            "Volume",
            &transportAudioVolume,
            0.0f,
            1.0f,
            "%.2f",
            ImGuiSliderFlags_None)) {
        applyCommand(
            shellState,
            jcut::SetTransportAudioCommand{
                snapshot.transport.audioMuted,
                transportAudioVolume});
    }
    float zoomValue = snapshot.transport.previewZoom;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat("Zoom", &zoomValue, 0.5f, 3.0f, "%.2fx")) {
        applyCommand(shellState, jcut::SetPreviewZoomCommand{zoomValue});
    }
    int currentFrame = snapshot.transport.currentFrame;
    if (ImGui::InputInt("Frame", &currentFrame)) {
        applyCommand(shellState, jcut::SeekToFrameCommand{currentFrame});
    }
    ImGui::End();
}
