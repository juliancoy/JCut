// Transcript and speaker text preparation methods.
// Included inside OffscreenVulkanRendererPrivate; do not compile separately.
  QVector<TranscriptTextInput> buildTranscriptTextInputs(
      const QSize &imageSize, const RenderRequest &request,
      const RenderFrameClock &clock,
      const QVector<TimelineClip> &orderedClips,
      QStringList *subtitleFailures = nullptr) {
    QVector<TranscriptTextInput> inputs;
    const int finalSubtitleOffsetMs =
        jcut::subtitle::finalRenderOffsetMs(
            request.transcriptOffsetMs,
            request.masterOutputSubtitleOffsetMs);
    for (const TimelineClip &clip : orderedClips) {
      if (!clip.transcriptOverlay.enabled ||
          (clip.mediaType != ClipMediaType::Audio && !clip.hasAudio) ||
          clock.timelineSample < clipTimelineStartSamples(clip) ||
          clock.timelineSample >= clipTimelineEndSamples(clip)) {
        continue;
      }
      const QString transcriptPath =
          renderTranscriptPath(clip);
      if (transcriptPath.trimmed().isEmpty()) {
        if (subtitleFailures) {
          subtitleFailures->push_back(QStringLiteral(
              "clip %1 has transcript overlay enabled but no active transcript path")
              .arg(clip.id));
        }
        continue;
      }
      QVector<TranscriptSection> sections = m_transcriptCache.value(transcriptPath);
      if (sections.isEmpty()) {
        sections = loadTranscriptSections(transcriptPath);
        m_transcriptCache.insert(transcriptPath, sections);
      }
      if (sections.isEmpty()) {
        if (subtitleFailures) {
          subtitleFailures->push_back(QStringLiteral(
              "clip %1 has transcript overlay enabled but transcript %2 has no readable sections")
              .arg(clip.id, transcriptPath));
        }
        continue;
      }
      const ClipFrameMapping mapping =
          clipFrameMappingForClock(
              clip, request.clips, clock,
              request.renderSyncMarkers);
      const TranscriptOverlayLayout layout =
          transcriptOverlayLayoutAtSourceFrame(
              clip,
              sections,
              mapping.transcriptFrame,
              TranscriptOverlayTiming{request.transcriptPrependMs,
                                      request.transcriptPostpendMs,
                                      finalSubtitleOffsetMs});
      if (layout.lines.isEmpty()) {
        continue;
      }
      const QRectF outputRect = transcriptOverlayRectInOutputSpace(
          clip, imageSize, transcriptPath, sections, mapping.transcriptFrame);
      if (outputRect.isEmpty()) {
        if (subtitleFailures) {
          subtitleFailures->push_back(QStringLiteral(
              "clip %1 produced an empty subtitle output rectangle at transcript frame %2")
              .arg(clip.id)
              .arg(mapping.transcriptFrame));
        }
        continue;
      }
      const QString speakerTitle = clip.transcriptOverlay.showSpeakerTitle
          ? transcriptSpeakerTitleForSourceFrame(
                transcriptPath,
                sections,
                mapping.transcriptFrame,
                TranscriptOverlayTiming{request.transcriptPrependMs,
                                        request.transcriptPostpendMs,
                                        finalSubtitleOffsetMs}).trimmed()
          : QString();
      const EffectiveVisualEffects effects =
          request.bypassGrading
              ? EffectiveVisualEffects{}
              : evaluateEffectiveVisualEffectsAtPosition(
                    clip,
                    request.tracks,
                    clock.timelineFramePosition,
                    request.renderSyncMarkers,
                    request.playbackTiming);
      if (effects.grading.opacity <= 0.001) {
        continue;
      }
      inputs.push_back(TranscriptTextInput{
          clip,
          layout,
          outputRect,
          speakerTitle,
          qBound<qreal>(0.0, effects.grading.opacity, 1.0)});
    }
    return inputs;
  }

  bool buildSpeakerLabelSpec(
      const RenderRequest &request,
      const RenderFrameClock &clock,
      const QVector<TimelineClip> &orderedClips,
      SpeakerLabelOverlaySpec *outSpec) {
    if (outSpec) {
      *outSpec = SpeakerLabelOverlaySpec{};
    }
    if (!request.showCurrentSpeakerName && !request.showCurrentSpeakerOrganization) {
      return false;
    }
    SpeakerLabelOverlaySpec spec;
    spec.showName = request.showCurrentSpeakerName;
    spec.showOrganization = request.showCurrentSpeakerOrganization;
    spec.nameTextScale = qBound<qreal>(0.25, request.currentSpeakerNameTextScale, 3.0);
    spec.organizationTextScale =
        qBound<qreal>(0.25, request.currentSpeakerOrganizationTextScale, 3.0);
    spec.nameVerticalPosition =
        qBound<qreal>(0.0, request.currentSpeakerNameVerticalPosition, 1.0);
    spec.organizationVerticalPosition =
        qBound<qreal>(0.0, request.currentSpeakerOrganizationVerticalPosition, 1.0);
    spec.nameColor = request.currentSpeakerNameColor;
    spec.organizationColor = request.currentSpeakerOrganizationColor;
    spec.backgroundColor = request.currentSpeakerBackgroundColor;
    spec.borderColor = request.currentSpeakerBorderColor;
    spec.backgroundCornerRadius =
        qBound<qreal>(0.0, request.currentSpeakerBackgroundCornerRadius, 128.0);
    spec.borderWidth = qBound<qreal>(0.0, request.currentSpeakerBorderWidth, 16.0);
    spec.showShadow = request.currentSpeakerShadowEnabled;
    spec.shadowColor = request.currentSpeakerShadowColor;

    for (const TimelineClip &clip : orderedClips) {
      if (clip.speakerTitleEngineActive) {
        continue;
      }
      const int64_t clipStartSample = clipTimelineStartSamples(clip);
      const int64_t clipEndSample = clipTimelineEndSamples(clip);
      if (clip.filePath.trimmed().isEmpty() ||
          clock.timelineSample < clipStartSample ||
          clock.timelineSample >= clipEndSample ||
          (!clip.hasAudio && clip.mediaType != ClipMediaType::Audio)) {
        continue;
      }
      const QString transcriptPath =
          renderTranscriptPath(clip);
      if (transcriptPath.trimmed().isEmpty()) {
        continue;
      }
      const ClipFrameMapping mapping =
          clipFrameMappingForClock(clip, clock, request.renderSyncMarkers);
      QVector<TranscriptSection> sections = m_transcriptCache.value(transcriptPath);
      if (sections.isEmpty()) {
        sections = loadTranscriptSections(transcriptPath);
        m_transcriptCache.insert(transcriptPath, sections);
      }
      const QString speakerId =
          transcriptOverlaySpeakerAtSourceFrame(
              sections,
              mapping.transcriptFrame,
              nullptr,
              TranscriptOverlayTiming{request.transcriptPrependMs,
                                      request.transcriptPostpendMs,
                                      request.transcriptOffsetMs}).trimmed();
      if (speakerId.isEmpty()) {
        continue;
      }

      QJsonDocument document;
      SpeakerProfile profile;
      profile.speakerId = speakerId;
      profile.name = speakerId;
      if (loadTranscriptJsonCached(transcriptPath, &document) && document.isObject()) {
        const QJsonObject profiles = document.object().value(QStringLiteral("speaker_profiles")).toObject();
        profile = speakerProfileFromJson(speakerId, profiles.value(speakerId).toObject());
        if (profile.speakerId.isEmpty()) {
          profile.speakerId = speakerId;
        }
        if (profile.name.trimmed().isEmpty()) {
          profile.name = speakerId;
        }
      }
      spec.name = profile.name.trimmed();
      spec.organization = profile.organization.trimmed();
      if (outSpec) {
        *outSpec = spec;
      }
      return true;
    }
    return false;
  }

  VulkanTextLayoutDebug speakerLabelLayoutDebug(
      const QSize &outputSize,
      const SpeakerLabelOverlaySpec &spec) const {
    if (!m_speakerTextRenderer || !m_speakerTextRenderer->isReady()) {
      return {};
    }
    return m_speakerTextRenderer->buildSpeakerLabelLayoutForTesting(outputSize, spec);
  }
