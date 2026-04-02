#pragma once

#include <QObject>
#include <QTableWidget>
#include <QCheckBox>
#include <QMenu>
#include <functional>

#include "editor_shared.h"

// Base class for keyframe-like tabs (Video Keyframe, Grading, Titles)
// Provides common functionality for syncing table to playhead, managing state,
// and handling keyframe operations (delete, context menu, selection)
class KeyframeTabBase : public QObject
{
    Q_OBJECT

public:
    struct Dependencies {
        std::function<const TimelineClip*()> getSelectedClip;
        std::function<const TimelineClip*()> getSelectedClipConst;
        std::function<bool(const QString&, const std::function<void(TimelineClip&)>&)> updateClipById;
        std::function<QString(const TimelineClip&)> getClipFilePath;
        std::function<bool(const TimelineClip&)> clipHasVisuals;
        std::function<void()> scheduleSaveState;
        std::function<void()> pushHistorySnapshot;
        std::function<void()> refreshInspector;
        std::function<void()> setPreviewTimelineClips;
        std::function<int64_t()> getCurrentTimelineFrame;
        std::function<int64_t()> getSelectedClipStartFrame;
        std::function<QString()> getSelectedClipId;
        std::function<void(int64_t)> seekToTimelineFrame;
        std::function<void(QTableWidgetItem*)> onKeyframeItemChanged;
        std::function<void()> onKeyframeSelectionChanged;
    };

    explicit KeyframeTabBase(const Dependencies& deps, QObject* parent = nullptr);
    ~KeyframeTabBase() override = default;

    // Common utility methods
    bool shouldSkipSyncToPlayhead(QTableWidget* table, QCheckBox* followCheckBox);
    int64_t calculateLocalFrame(const TimelineClip* clip) const;
    
    // Keyframe selection helpers (work with any keyframe type via frame role)
    QList<int64_t> selectedKeyframeFramesFromTable(QTableWidget* table) const;
    int nearestKeyframeIndexFromTable(QTableWidget* table, int64_t targetFrame) const;
    void applySyncedRowSelection(QTableWidget* table, int row, bool autoScroll);
    
    // Common slot implementations
    void onTableSelectionChangedBase(QTableWidget* table, QTimer* deferredSeekTimer = nullptr, 
                                     int64_t* pendingSeekFrame = nullptr);
    
    // Install common event filter and context menu on a table widget.
    // Call this from derived class wire() for each keyframe table.
    void installTableHandlers(QTableWidget* table);
    
    // Derived classes must call this to handle Delete/Backspace keys.
    // Returns true if the event was handled.
    bool handleTableKeyPress(QKeyEvent* event);
    
    // Derived classes must implement this to handle Delete key.
    // It's called by the base class event filter when Delete/Backspace is pressed.
    virtual void removeSelectedKeyframesFromCurrentTable() = 0;
    
    // Check if we should skip repainting the UI during playhead movement
    // (to avoid disrupting multi-selection and reduce flicker)
    bool shouldSkipKeyframeRepaint() const;
    
    // Context menu with standard actions
    struct ContextMenuActions {
        QAction* addAbove = nullptr;
        QAction* addBelow = nullptr;
        QAction* copyToNext = nullptr;
        QAction* copyToPlayhead = nullptr;
        QAction* deleteRows = nullptr;
    };
    ContextMenuActions buildStandardContextMenu(QMenu& menu, QTableWidget* table, 
                                                int row, const TimelineClip* clip);
    
    // Accessors
    int64_t selectedKeyframeFrame() const { return m_selectedKeyframeFrame; }
    const QSet<int64_t>& selectedKeyframeFrames() const { return m_selectedKeyframeFrames; }
    void setSelectedKeyframeFrame(int64_t frame) { 
        m_selectedKeyframeFrame = frame; 
        m_selectedKeyframeFrames = {frame};
    }

signals:
    void keyframesRemoved();
    void keyframeSelectionChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    Dependencies m_deps;
    bool m_updating = false;
    bool m_syncingTableSelection = false;
    int64_t m_selectedKeyframeFrame = -1;
    QSet<int64_t> m_selectedKeyframeFrames;
    
    // Tracks the timeline frame that was last manually selected by the user.
    // Used to prevent the table from auto-syncing (jumping) immediately after
    // the user clicks a row, which would fight their manual selection.
    int64_t m_suppressSyncForTimelineFrame = -1;
};
