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
    bool shouldSkipSyncToPlayhead(QTableWidget* table, QCheckBox* followCheckBox) const;
    int64_t calculateLocalFrame(const TimelineClip* clip) const;
    
    // Keyframe selection helpers (work with any keyframe type via frame role)
    QList<int64_t> selectedKeyframeFramesFromTable(QTableWidget* table) const;
    int nearestKeyframeIndexFromTable(QTableWidget* table, int64_t targetFrame) const;
    
    // Common slot implementations
    void onTableSelectionChangedBase(QTableWidget* table, QTimer* deferredSeekTimer = nullptr, 
                                     int64_t* pendingSeekFrame = nullptr);
    
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
    Dependencies m_deps;
    bool m_updating = false;
    bool m_syncingTableSelection = false;
    int64_t m_selectedKeyframeFrame = -1;
    QSet<int64_t> m_selectedKeyframeFrames;
};
