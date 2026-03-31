#include "preview_debug.h"

#include "preview.h"
#include "frame_handle.h"
#include "debug_controls.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QDebug>

namespace {
struct PlaybackStaleRunState {
    bool active = false;
    int64_t startRequestedFrame = -1;
    int64_t startSelectedFrame = -1;
    int64_t worstRequestedFrame = -1;
    int64_t worstSelectedFrame = -1;
    int64_t worstDelta = 0;
    qint64 startTraceMs = 0;
};
}

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

qint64 playbackTraceMs()
{
    static QElapsedTimer timer;
    static bool started = false;
    if (!started) {
        timer.start();
        started = true;
    }
    return timer.elapsed();
}

void playbackTrace(const QString& event, const QString& detail)
{
    if (editor::debugPlaybackLevel() < editor::DebugLogLevel::Info) {
        return;
    }
    qDebug() << "[PLAYBACK]" << playbackTraceMs() << event << "-" << detail;
}

void playbackWarnTrace(const QString& event, const QString& detail)
{
    if (!editor::debugPlaybackWarnEnabled()) {
        return;
    }
    qDebug() << "[PLAYBACK][WARN]" << playbackTraceMs() << event << "-" << detail;
}

void playbackFrameSelectionTrace(const QString& stage,
                                 const TimelineClip& clip,
                                 int64_t requestedFrame,
                                 const FrameHandle& exactFrame,
                                 const FrameHandle& selectedFrame,
                                 qreal playheadFramePosition)
{
    static QHash<QString, PlaybackStaleRunState> staleRunsByClip;

    const int64_t exactFrameNumber = exactFrame.isNull() ? -1 : exactFrame.frameNumber();
    const int64_t selectedFrameNumber = selectedFrame.isNull() ? -1 : selectedFrame.frameNumber();
    const int64_t delta = selectedFrame.isNull() ? -1 : selectedFrameNumber - requestedFrame;
    const bool singleFrame = clip.mediaType == ClipMediaType::Image;
    const bool anomaly = !singleFrame && (selectedFrame.isNull() || delta < 0 || delta > 1);

    if (editor::debugPlaybackWarnOnlyEnabled() && !anomaly) {
        auto staleIt = staleRunsByClip.find(clip.id);
        if (staleIt != staleRunsByClip.end() && staleIt->active) {
            const PlaybackStaleRunState state = staleIt.value();
            playbackWarnTrace(QStringLiteral("PreviewWindow::stale-run.end"),
                              QStringLiteral("clip=%1 file=%2 durationMs=%3 startRequested=%4 startSelected=%5 recoveredRequested=%6 recoveredSelected=%7 worstRequested=%8 worstSelected=%9 worstDelta=%10 playhead=%11")
                                  .arg(clip.id)
                                  .arg(QFileInfo(clip.filePath).fileName())
                                  .arg(playbackTraceMs() - state.startTraceMs)
                                  .arg(state.startRequestedFrame)
                                  .arg(state.startSelectedFrame)
                                  .arg(requestedFrame)
                                  .arg(selectedFrameNumber)
                                  .arg(state.worstRequestedFrame)
                                  .arg(state.worstSelectedFrame)
                                  .arg(state.worstDelta)
                                  .arg(playheadFramePosition, 0, 'f', 3));
            staleRunsByClip.remove(clip.id);
        }
        return;
    }
    if (!editor::debugPlaybackVerboseEnabled() && !editor::debugPlaybackWarnOnlyEnabled()) {
        return;
    }

    const QString detail = QStringLiteral("clip=%1 file=%2 singleFrame=%3 requested=%4 exact=%5 selected=%6 delta=%7 playhead=%8")
                               .arg(clip.id)
                               .arg(QFileInfo(clip.filePath).fileName())
                               .arg(singleFrame ? 1 : 0)
                               .arg(requestedFrame)
                               .arg(exactFrameNumber)
                               .arg(selectedFrameNumber)
                               .arg(delta)
                               .arg(playheadFramePosition, 0, 'f', 3);
    if (editor::debugPlaybackWarnOnlyEnabled()) {
        PlaybackStaleRunState& state = staleRunsByClip[clip.id];
        if (!state.active) {
            state.active = true;
            state.startRequestedFrame = requestedFrame;
            state.startSelectedFrame = selectedFrameNumber;
            state.worstRequestedFrame = requestedFrame;
            state.worstSelectedFrame = selectedFrameNumber;
            state.worstDelta = delta;
            state.startTraceMs = playbackTraceMs();
            playbackWarnTrace(QStringLiteral("PreviewWindow::stale-run.start"), detail);
            return;
        }
        if (delta < state.worstDelta) {
            state.worstRequestedFrame = requestedFrame;
            state.worstSelectedFrame = selectedFrameNumber;
            state.worstDelta = delta;
        }
        return;
    }
    playbackTrace(stage, detail);
}
