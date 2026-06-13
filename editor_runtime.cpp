#include "editor_runtime.h"

#include <algorithm>
#include <utility>

namespace {

constexpr float kMinPlaybackSpeed = 0.25f;
constexpr float kMaxPlaybackSpeed = 2.0f;
constexpr float kMinPreviewZoom = 0.5f;
constexpr float kMaxPreviewZoom = 3.0f;
constexpr double kDefaultTimelineFps = 30.0;

template <typename Predicate>
void selectSingle(std::vector<jcut::EditorTrack>* items, Predicate predicate)
{
    for (jcut::EditorTrack& item : *items) {
        item.selected = predicate(item);
    }
}

template <typename Predicate>
void selectSingle(std::vector<jcut::EditorClip>* items, Predicate predicate)
{
    for (jcut::EditorClip& item : *items) {
        item.selected = predicate(item);
    }
}

jcut::EditorClip* findClip(std::vector<jcut::EditorClip>* clips, int clipId)
{
    for (jcut::EditorClip& clip : *clips) {
        if (clip.id == clipId) {
            return &clip;
        }
    }
    return nullptr;
}

bool hasTrackId(const std::vector<jcut::EditorTrack>& tracks, int trackId)
{
    for (const jcut::EditorTrack& track : tracks) {
        if (track.id == trackId) {
            return true;
        }
    }
    return false;
}

const jcut::EditorMediaItem* findMediaItem(const std::vector<jcut::EditorMediaItem>& mediaItems,
                                           const std::string& mediaId)
{
    for (const jcut::EditorMediaItem& mediaItem : mediaItems) {
        if (mediaItem.id == mediaId) {
            return &mediaItem;
        }
    }
    return nullptr;
}

int nextTrackId(const std::vector<jcut::EditorTrack>& tracks)
{
    int nextId = 1;
    for (const jcut::EditorTrack& track : tracks) {
        nextId = std::max(nextId, track.id + 1);
    }
    return nextId;
}

int nextClipId(const std::vector<jcut::EditorClip>& clips)
{
    int nextId = 1;
    for (const jcut::EditorClip& clip : clips) {
        nextId = std::max(nextId, clip.id + 1);
    }
    return nextId;
}

void ensureMediaItemForClip(jcut::EditorDocumentCore* document,
                            const std::string& sourcePath,
                            const std::string& label,
                            const std::string& mediaKind)
{
    if (sourcePath.empty()) {
        return;
    }
    for (const jcut::EditorMediaItem& mediaItem : document->mediaItems) {
        if (mediaItem.id == sourcePath) {
            return;
        }
    }
    document->mediaItems.push_back({
        sourcePath,
        label.empty() ? sourcePath : label,
        mediaKind.empty() ? std::string("unknown") : mediaKind
    });
}

std::string fallbackLabelFromPath(const std::string& sourcePath)
{
    if (sourcePath.empty()) {
        return "media";
    }
    const std::size_t separator = sourcePath.find_last_of("/\\");
    if (separator == std::string::npos) {
        return sourcePath;
    }
    return sourcePath.substr(separator + 1);
}

void pruneUnusedMediaItems(jcut::EditorDocumentCore* document)
{
    std::vector<jcut::EditorMediaItem> filtered;
    filtered.reserve(document->mediaItems.size());
    for (const jcut::EditorMediaItem& mediaItem : document->mediaItems) {
        bool used = false;
        for (const jcut::EditorClip& clip : document->clips) {
            if (!clip.sourcePath.empty() && clip.sourcePath == mediaItem.id) {
                used = true;
                break;
            }
        }
        if (used || mediaItem.id.rfind("media-", 0) == 0) {
            filtered.push_back(mediaItem);
        }
    }
    document->mediaItems = std::move(filtered);
}

} // namespace

namespace jcut {

EditorRuntime EditorRuntime::createDemo()
{
    EditorRuntime runtime;
    runtime.m_document.projectName = "Demo Session";
    runtime.m_document.mediaItems = {
        {"media-1", "Interview_A_CamA.mov", "video"},
        {"media-2", "Interview_A_CamB.mov", "video"},
        {"media-3", "Broll_Street_01.mp4", "video"},
        {"media-4", "Voiceover_Main.wav", "audio"},
        {"media-5", "LowerThird_Package", "graphics"},
    };
    runtime.m_document.tracks = {
        {1, "Video A", true},
        {2, "Video B", false},
        {3, "Graphics", false},
        {4, "Audio Mix", false},
    };
    runtime.m_document.clips = {
        {1, 1, "Interview A", 0, 420, true},
        {2, 2, "Interview B", 36, 396, false},
        {3, 3, "Lower Third", 120, 96, false},
        {4, 4, "VO Main", 0, 420, false},
    };
    runtime.m_document.transport.currentFrame = 1842;
    runtime.m_document.exportRequest.outputFormat = "mp4";
    runtime.m_document.exportRequest.outputSize = {1080, 1920};
    runtime.m_document.exportRequest.outputFps = 30.0;
    runtime.m_document.exportRequest.outputMode = render::RenderOutputMode::EncodedFile;
    return runtime;
}

EditorRuntime EditorRuntime::fromDocument(EditorDocumentCore document)
{
    EditorRuntime runtime;
    runtime.m_document = std::move(document);
    return runtime;
}

EditorDocumentCore EditorRuntime::snapshot() const
{
    return m_document;
}

CommandResult EditorRuntime::execute(const EditorCommand& command)
{
    return std::visit(
        [this](const auto& typedCommand) -> CommandResult {
            using T = std::decay_t<decltype(typedCommand)>;

            if constexpr (std::is_same_v<T, TogglePlaybackCommand>) {
                m_document.transport.playbackActive = !m_document.transport.playbackActive;
                return {true, m_document.transport.playbackActive ? "playback started" : "playback paused"};
            } else if constexpr (std::is_same_v<T, SetPlaybackActiveCommand>) {
                m_document.transport.playbackActive = typedCommand.active;
                return {true, typedCommand.active ? "playback started" : "playback paused"};
            } else if constexpr (std::is_same_v<T, SetPlaybackSpeedCommand>) {
                m_document.transport.playbackSpeed =
                    std::clamp(typedCommand.speed, kMinPlaybackSpeed, kMaxPlaybackSpeed);
                return {true, "playback speed updated"};
            } else if constexpr (std::is_same_v<T, SetPreviewZoomCommand>) {
                m_document.transport.previewZoom =
                    std::clamp(typedCommand.zoom, kMinPreviewZoom, kMaxPreviewZoom);
                return {true, "preview zoom updated"};
            } else if constexpr (std::is_same_v<T, SeekToFrameCommand>) {
                m_document.transport.currentFrame =
                    std::clamp(typedCommand.frame, 0, timelineEndFrame());
                m_frameAccumulator = 0.0;
                return {true, "playhead moved"};
            } else if constexpr (std::is_same_v<T, StepFrameCommand>) {
                m_document.transport.currentFrame =
                    std::clamp(m_document.transport.currentFrame + typedCommand.delta, 0, timelineEndFrame());
                m_frameAccumulator = 0.0;
                return {true, "playhead stepped"};
            } else if constexpr (std::is_same_v<T, SetProjectNameCommand>) {
                m_document.projectName = typedCommand.name.empty()
                    ? std::string("Untitled Project")
                    : typedCommand.name;
                return {true, "project name updated"};
            } else if constexpr (std::is_same_v<T, ImportMediaCommand>) {
                if (typedCommand.sourcePath.empty()) {
                    return {false, "media path required"};
                }
                ensureMediaItemForClip(
                    &m_document,
                    typedCommand.sourcePath,
                    typedCommand.label.empty() ? fallbackLabelFromPath(typedCommand.sourcePath)
                                               : typedCommand.label,
                    typedCommand.mediaKind);
                return {true, "media imported"};
            } else if constexpr (std::is_same_v<T, AddTrackCommand>) {
                selectSingle(&m_document.tracks, [](const EditorTrack&) { return false; });
                m_document.tracks.push_back({
                    nextTrackId(m_document.tracks),
                    typedCommand.label.empty()
                        ? std::string("Track ") + std::to_string(m_document.tracks.size() + 1)
                        : typedCommand.label,
                    true
                });
                return {true, "track added"};
            } else if constexpr (std::is_same_v<T, DeleteTrackCommand>) {
                const auto oldTrackCount = m_document.tracks.size();
                m_document.tracks.erase(
                    std::remove_if(m_document.tracks.begin(), m_document.tracks.end(),
                                   [&](const EditorTrack& track) { return track.id == typedCommand.trackId; }),
                    m_document.tracks.end());
                if (m_document.tracks.size() == oldTrackCount) {
                    return {false, "track not found"};
                }
                m_document.clips.erase(
                    std::remove_if(m_document.clips.begin(), m_document.clips.end(),
                                   [&](const EditorClip& clip) { return clip.trackId == typedCommand.trackId; }),
                    m_document.clips.end());
                if (!m_document.tracks.empty() &&
                    std::none_of(m_document.tracks.begin(), m_document.tracks.end(),
                                 [](const EditorTrack& track) { return track.selected; })) {
                    m_document.tracks.front().selected = true;
                }
                if (!m_document.clips.empty() &&
                    std::none_of(m_document.clips.begin(), m_document.clips.end(),
                                 [](const EditorClip& clip) { return clip.selected; })) {
                    m_document.clips.front().selected = true;
                }
                pruneUnusedMediaItems(&m_document);
                return {true, "track deleted"};
            } else if constexpr (std::is_same_v<T, SelectTrackCommand>) {
                selectSingle(&m_document.tracks, [&](const EditorTrack& track) {
                    return track.id == typedCommand.trackId;
                });
                return {true, "track selected"};
            } else if constexpr (std::is_same_v<T, SelectClipCommand>) {
                selectSingle(&m_document.clips, [&](const EditorClip& clip) {
                    return clip.id == typedCommand.clipId;
                });
                return {true, "clip selected"};
            } else if constexpr (std::is_same_v<T, InsertClipFromMediaCommand>) {
                if (!hasTrackId(m_document.tracks, typedCommand.trackId)) {
                    return {false, "track not found"};
                }
                const EditorMediaItem* mediaItem = findMediaItem(m_document.mediaItems, typedCommand.mediaId);
                if (!mediaItem) {
                    return {false, "media not found"};
                }
                selectSingle(&m_document.clips, [](const EditorClip&) { return false; });
                const int clipId = nextClipId(m_document.clips);
                m_document.clips.push_back({
                    clipId,
                    typedCommand.trackId,
                    mediaItem->label.empty() ? std::string("Clip ") + std::to_string(clipId) : mediaItem->label,
                    std::max(0, typedCommand.startFrame),
                    std::max(1, typedCommand.durationFrames),
                    true,
                    mediaItem->id
                });
                return {true, "clip inserted"};
            } else if constexpr (std::is_same_v<T, AddClipCommand>) {
                if (!hasTrackId(m_document.tracks, typedCommand.trackId)) {
                    return {false, "track not found"};
                }
                selectSingle(&m_document.clips, [](const EditorClip&) { return false; });
                const int clipId = nextClipId(m_document.clips);
                const std::string label = typedCommand.label.empty()
                    ? std::string("Clip ") + std::to_string(clipId)
                    : typedCommand.label;
                m_document.clips.push_back({
                    clipId,
                    typedCommand.trackId,
                    label,
                    std::max(0, typedCommand.startFrame),
                    std::max(1, typedCommand.durationFrames),
                    true,
                    typedCommand.sourcePath
                });
                ensureMediaItemForClip(&m_document, typedCommand.sourcePath, label, typedCommand.mediaKind);
                return {true, "clip added"};
            } else if constexpr (std::is_same_v<T, DeleteClipCommand>) {
                const auto oldClipCount = m_document.clips.size();
                m_document.clips.erase(
                    std::remove_if(m_document.clips.begin(), m_document.clips.end(),
                                   [&](const EditorClip& clip) { return clip.id == typedCommand.clipId; }),
                    m_document.clips.end());
                if (m_document.clips.size() == oldClipCount) {
                    return {false, "clip not found"};
                }
                if (!m_document.clips.empty() &&
                    std::none_of(m_document.clips.begin(), m_document.clips.end(),
                                 [](const EditorClip& clip) { return clip.selected; })) {
                    m_document.clips.front().selected = true;
                }
                pruneUnusedMediaItems(&m_document);
                return {true, "clip deleted"};
            } else if constexpr (std::is_same_v<T, SplitClipCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                const int clipStart = clip->startFrame;
                const int clipEnd = clip->startFrame + clip->durationFrames;
                if (typedCommand.frame <= clipStart || typedCommand.frame >= clipEnd) {
                    return {false, "split frame outside clip"};
                }

                const int leadingDuration = typedCommand.frame - clipStart;
                const int trailingDuration = clipEnd - typedCommand.frame;
                if (leadingDuration < 1 || trailingDuration < 1) {
                    return {false, "split would create empty clip"};
                }

                const int newClipId = nextClipId(m_document.clips);
                EditorClip trailingClip = *clip;
                trailingClip.id = newClipId;
                trailingClip.startFrame = typedCommand.frame;
                trailingClip.durationFrames = trailingDuration;
                trailingClip.selected = true;

                clip->durationFrames = leadingDuration;
                clip->selected = false;
                m_document.clips.push_back(std::move(trailingClip));
                std::sort(m_document.clips.begin(), m_document.clips.end(),
                          [](const EditorClip& left, const EditorClip& right) {
                              if (left.trackId != right.trackId) {
                                  return left.trackId < right.trackId;
                              }
                              if (left.startFrame != right.startFrame) {
                                  return left.startFrame < right.startFrame;
                              }
                              return left.id < right.id;
                          });
                return {true, "clip split"};
            } else if constexpr (std::is_same_v<T, TrimClipStartCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                const int clipEnd = clip->startFrame + clip->durationFrames;
                if (typedCommand.startFrame < 0 || typedCommand.startFrame >= clipEnd) {
                    return {false, "trim start outside clip"};
                }
                const int nextDuration = clipEnd - typedCommand.startFrame;
                if (nextDuration < 1) {
                    return {false, "trim would create empty clip"};
                }
                clip->startFrame = typedCommand.startFrame;
                clip->durationFrames = nextDuration;
                return {true, "clip start trimmed"};
            } else if constexpr (std::is_same_v<T, TrimClipEndCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                if (typedCommand.endFrame <= clip->startFrame) {
                    return {false, "trim end outside clip"};
                }
                const int nextDuration = typedCommand.endFrame - clip->startFrame;
                if (nextDuration < 1) {
                    return {false, "trim would create empty clip"};
                }
                clip->durationFrames = nextDuration;
                return {true, "clip end trimmed"};
            } else if constexpr (std::is_same_v<T, SetClipLabelCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                clip->label = typedCommand.label.empty() ? std::string("clip") : typedCommand.label;
                return {true, "clip label updated"};
            } else if constexpr (std::is_same_v<T, MoveClipCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                if (!hasTrackId(m_document.tracks, typedCommand.trackId)) {
                    return {false, "track not found"};
                }
                clip->trackId = typedCommand.trackId;
                clip->startFrame = std::max(0, typedCommand.startFrame);
                return {true, "clip moved"};
            } else if constexpr (std::is_same_v<T, ResizeClipCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                clip->durationFrames = std::max(1, typedCommand.durationFrames);
                return {true, "clip resized"};
            } else if constexpr (std::is_same_v<T, SetWaveformVisibleCommand>) {
                m_document.panels.showWaveform = typedCommand.visible;
                return {true, "waveform visibility updated"};
            } else if constexpr (std::is_same_v<T, SetTranscriptVisibleCommand>) {
                m_document.panels.showTranscript = typedCommand.visible;
                return {true, "transcript visibility updated"};
            } else if constexpr (std::is_same_v<T, SetScopesVisibleCommand>) {
                m_document.panels.showScopes = typedCommand.visible;
                return {true, "scopes visibility updated"};
            } else if constexpr (std::is_same_v<T, SetExportSizeCommand>) {
                m_document.exportRequest.outputSize = {
                    std::max(16, typedCommand.width),
                    std::max(16, typedCommand.height)};
                return {true, "export size updated"};
            } else if constexpr (std::is_same_v<T, SetExportFpsCommand>) {
                m_document.exportRequest.outputFps = std::max(1.0, typedCommand.fps);
                return {true, "export fps updated"};
            } else if constexpr (std::is_same_v<T, SetExportOutputPathCommand>) {
                m_document.exportRequest.outputPath = typedCommand.path;
                return {true, "export output path updated"};
            } else if constexpr (std::is_same_v<T, SetExportFormatCommand>) {
                m_document.exportRequest.outputFormat = typedCommand.format.empty()
                    ? std::string("mp4")
                    : typedCommand.format;
                return {true, "export format updated"};
            } else if constexpr (std::is_same_v<T, SetExportImageSequenceFormatCommand>) {
                m_document.exportRequest.imageSequenceFormat = typedCommand.format.empty()
                    ? std::string("jpeg")
                    : typedCommand.format;
                return {true, "image sequence format updated"};
            } else if constexpr (std::is_same_v<T, SetExportUseProxyMediaCommand>) {
                m_document.exportRequest.useProxyMedia = typedCommand.enabled;
                return {true, "proxy export flag updated"};
            } else if constexpr (std::is_same_v<T, SetExportImageSequenceCommand>) {
                m_document.exportRequest.createVideoFromImageSequence = typedCommand.enabled;
                m_document.exportRequest.outputMode = typedCommand.enabled
                    ? render::RenderOutputMode::EncodedFileAndImageSequence
                    : render::RenderOutputMode::EncodedFile;
                if (typedCommand.enabled && m_document.exportRequest.imageSequenceFormat.empty()) {
                    m_document.exportRequest.imageSequenceFormat = "jpeg";
                }
                return {true, "image sequence mode updated"};
            }

            return {false, "unsupported command"};
        },
        command);
}

void EditorRuntime::tick(const TickParams& params)
{
    if (!m_document.transport.playbackActive) {
        m_frameAccumulator = 0.0;
        return;
    }

    const double deltaSeconds = std::max(0.0, params.deltaSeconds);
    if (deltaSeconds <= 0.0) {
        return;
    }

    const double fps = m_document.exportRequest.outputFps > 0.0
        ? m_document.exportRequest.outputFps
        : kDefaultTimelineFps;
    m_frameAccumulator += deltaSeconds * fps * m_document.transport.playbackSpeed;

    const int wholeFrames = static_cast<int>(m_frameAccumulator);
    if (wholeFrames <= 0) {
        return;
    }

    m_frameAccumulator -= static_cast<double>(wholeFrames);
    const int endFrame = timelineEndFrame();
    m_document.transport.currentFrame = std::min(m_document.transport.currentFrame + wholeFrames, endFrame);
    if (m_document.transport.currentFrame >= endFrame) {
        m_document.transport.currentFrame = endFrame;
        m_document.transport.playbackActive = false;
        m_frameAccumulator = 0.0;
    }
}

int EditorRuntime::timelineEndFrame() const
{
    int endFrame = 0;
    for (const EditorClip& clip : m_document.clips) {
        endFrame = std::max(endFrame, clip.startFrame + clip.durationFrames);
    }
    endFrame = std::max(endFrame, static_cast<int>(m_document.exportRequest.exportEndFrame));
    return endFrame;
}

} // namespace jcut
