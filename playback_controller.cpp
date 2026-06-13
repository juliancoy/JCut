#include "playback_controller.h"

#include "playback_debug.h"

// NOTE:
// This file is an extracted destination for the playback logic currently living
// inside EditorWindow. The method bodies still need to be wired to the actual
// EditorWindow API or to a narrower playback context object.
//
// I am leaving the implementation as commented source because the original code
// refers directly to many EditorWindow members and helper methods. This keeps
// the extracted file immediately useful without pretending it is already fully
// integrated.
//
// Extracted methods from editor.cpp:
// - advanceFrame()
// - speechFilterPlaybackEnabled() const
// - effectivePlaybackRanges() const
// - nextPlaybackFrame(int64_t currentFrame) const

PlaybackController::PlaybackController(EditorWindow *owner)
    : m_owner(owner)
{
}

// Playback is currently implemented in EditorWindow::advanceFrame().
// The live policy there is system-clock transport: audio, video, subtitles, and
// overlays derive from monotonic transport time, while audio device position is
// drift/latency feedback only.
