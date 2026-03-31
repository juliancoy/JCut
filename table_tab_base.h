#pragma once

#include <QObject>
#include <QTableWidget>
#include <QCheckBox>
#include <QTimer>
#include <functional>

#include "editor_shared.h"

/**
 * Base class for table-based inspector tabs (Transcript, Keyframe, Grading, Titles)
 * Provides common functionality:
 * - Deferred seek mechanism (avoid seek on double-click)
 * - Refresh skip when items selected (prevents playhead reset)
 * - Table sync to playhead with focus awareness
 * - Common dependencies pattern
 */
class TableTabBase : public QObject
{
    Q_OBJECT

public:
    struct Dependencies {
        std::function<const TimelineClip*()> getSelectedClip;
        std::function<void()> scheduleSaveState;
        std::function<void()> pushHistorySnapshot;
        std::function<void()> refreshInspector;
        std::function<int64_t()> getCurrentTimelineFrame;
        std::function<void(int64_t)> seekToTimelineFrame;
    };

    explicit TableTabBase(const Dependencies& deps, QObject* parent = nullptr);
    ~TableTabBase() override = default;

    // Common utilities
    bool shouldSkipRefresh(QTableWidget* table) const;
    bool shouldSkipSyncToPlayhead(QTableWidget* table, QCheckBox* followCheckBox) const;
    
    // Deferred seek - call from itemClicked, stop timer in itemDoubleClicked
    void requestDeferredSeek(int64_t timelineFrame);
    void cancelDeferredSeek();
    bool hasPendingSeek() const { return m_pendingSeekTimelineFrame >= 0; }

protected:
    Dependencies m_deps;
    bool m_updating = false;
    
    // Deferred seek state
    QTimer m_deferredSeekTimer;
    int64_t m_pendingSeekTimelineFrame = -1;
    
    // Track frame where user manually selected to prevent immediate re-sync
    int64_t m_suppressSyncForTimelineFrame = -1;
};
