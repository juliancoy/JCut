#pragma once

#include "editor_document_core.h"

#include <string_view>

namespace jcut {

struct EditorTrackMediaPresenceCore {
    bool hasVisual = false;
    bool hasAudio = false;
};

inline bool editorMediaKindHasVisualsCore(
    std::string_view mediaKind) noexcept
{
    return mediaKind == "video" || mediaKind == "image" ||
        mediaKind == "title" || mediaKind == "graphics";
}

inline bool editorClipHasVisualsCore(
    const EditorClip& clip) noexcept
{
    if (editorMediaKindHasVisualsCore(clip.mediaKind)) {
        return true;
    }
    if (clip.mediaKind == "audio") {
        return false;
    }
    // Legacy documents can omit mediaKind while retaining videoEnabled.
    return clip.videoEnabled;
}

inline EditorTrackMediaPresenceCore editorTrackMediaPresenceCore(
    const EditorDocumentCore& document,
    int trackId) noexcept
{
    EditorTrackMediaPresenceCore result;
    for (const EditorClip& clip : document.clips) {
        if (clip.trackId != trackId) continue;
        result.hasVisual =
            result.hasVisual || editorClipHasVisualsCore(clip);
        result.hasAudio = result.hasAudio || clip.hasAudio;
        if (result.hasVisual && result.hasAudio) break;
    }
    return result;
}

} // namespace jcut
