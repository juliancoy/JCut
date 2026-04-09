#include "timeline_container.h"

#include "timeline_widget.h"
#include "track_sidebar.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QSlider>
#include <QTimer>
#include <QStyle>

TimelineContainer::TimelineContainer(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("timeline.container"));
    setupLayout();
    connectSignals();
}

void TimelineContainer::setupLayout()
{
    m_bottomSplitter = nullptr; // no splitter; use a real 2x2 grid

    m_gridLayout = new QGridLayout(this);
    m_gridLayout->setContentsMargins(0, 0, 0, 0);
    m_gridLayout->setHorizontalSpacing(0);
    m_gridLayout->setVerticalSpacing(0);

    // A true grid:
    // row 0 col 0 = track header
    // row 0 col 1 = transport
    // row 1 col 0 = track sidebar
    // row 1 col 1 = timeline
    m_gridLayout->setColumnMinimumWidth(0, TrackSidebar::kTrackColumnWidth);
    m_gridLayout->setColumnStretch(0, 0);
    m_gridLayout->setColumnStretch(1, 1);
    m_gridLayout->setRowStretch(0, 0);
    m_gridLayout->setRowStretch(1, 1);

    // === TOP LEFT: Track column header ===
    m_topLeftPlaceholder = new QWidget(this);
    m_topLeftPlaceholder->setObjectName(QStringLiteral("timeline.placeholder"));
    m_topLeftPlaceholder->setFixedWidth(TrackSidebar::kTrackColumnWidth);
    m_topLeftPlaceholder->setFixedHeight(44);
    m_topLeftPlaceholder->setStyleSheet(QStringLiteral(
        "QWidget { background: #14181e; border-bottom: 1px solid #2a3542; border-right: 1px solid #2a3542; }"));

    auto *placeholderLayout = new QHBoxLayout(m_topLeftPlaceholder);
    placeholderLayout->setContentsMargins(8, 4, 8, 4);

    auto *placeholderLabel = new QLabel(QStringLiteral("Tracks"), m_topLeftPlaceholder);
    placeholderLabel->setStyleSheet(QStringLiteral(
        "color: #7f8a99; font-weight: bold; font-size: 11px;"));
    placeholderLayout->addWidget(placeholderLabel);
    placeholderLayout->addStretch();

    m_gridLayout->addWidget(m_topLeftPlaceholder, 0, 0);

    // === TOP RIGHT: Transport controls ===
    m_transportWidget = new QWidget(this);
    m_transportWidget->setObjectName(QStringLiteral("timeline.transport"));
    m_transportWidget->setFixedHeight(44);
    m_transportWidget->setStyleSheet(QStringLiteral(
        "QWidget { background: #14181e; border-bottom: 1px solid #2a3542; }"));

    auto *transportLayout = new QHBoxLayout(m_transportWidget);
    transportLayout->setContentsMargins(8, 4, 8, 4);
    transportLayout->setSpacing(6);
    transportLayout->addStretch();

    m_gridLayout->addWidget(m_transportWidget, 0, 1);

    // === BOTTOM LEFT: Track Sidebar ===
    m_trackSidebar = new TrackSidebar(this);
    m_trackSidebar->setObjectName(QStringLiteral("timeline.track_sidebar"));
    m_trackSidebar->setFixedWidth(TrackSidebar::kTrackColumnWidth);
    m_trackSidebar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_gridLayout->addWidget(m_trackSidebar, 1, 0);

    // === BOTTOM RIGHT: Timeline Widget ===
    m_timeline = new TimelineWidget(this);
    m_timeline->setObjectName(QStringLiteral("timeline.widget"));
    m_timeline->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_gridLayout->addWidget(m_timeline, 1, 1);

    QTimer::singleShot(0, this, &TimelineContainer::syncTracksFromTimeline);
}

void TimelineContainer::connectSignals()
{
    connect(m_trackSidebar, &TrackSidebar::trackVisualModeChanged,
            this, &TimelineContainer::onTrackVisualModeChanged);
    connect(m_trackSidebar, &TrackSidebar::trackAudioToggled,
            this, &TimelineContainer::onTrackAudioToggled);
    connect(m_trackSidebar, &TrackSidebar::trackSelected,
            this, &TimelineContainer::onTrackSelected);
    connect(m_trackSidebar, &TrackSidebar::trackMoveUpRequested,
            this, &TimelineContainer::onTrackMoveUp);
    connect(m_trackSidebar, &TrackSidebar::trackMoveDownRequested,
            this, &TimelineContainer::onTrackMoveDown);
    connect(m_trackSidebar, &TrackSidebar::trackDropped,
            this, &TimelineContainer::onTrackDropped);
    connect(m_trackSidebar, &TrackSidebar::trackRenameRequested,
            this, &TimelineContainer::onTrackRenameRequested);
    connect(m_trackSidebar, &TrackSidebar::trackDeleteRequested,
            this, &TimelineContainer::onTrackDeleteRequested);
    connect(m_trackSidebar, &TrackSidebar::wheelAdjusted,
            this, &TimelineContainer::onTrackSidebarWheelAdjusted);
    connect(m_trackSidebar, &TrackSidebar::widthResizeRequested,
            this, &TimelineContainer::onTrackSidebarWidthResizeRequested);

    if (m_timeline) {
        m_timeline->clipsChanged = [this]() {
            syncTracksFromTimeline();
        };
        m_timeline->trackLayoutChanged = [this]() {
            syncTracksFromTimeline();
        };
        m_timeline->selectionChanged = [this]() {
            if (m_trackSidebar) {
                m_trackSidebar->setSelectedTrack(m_timeline->selectedTrackIndex());
            }
        };
    }
}

void TimelineContainer::syncTracksFromTimeline()
{
    if (!m_timeline || !m_trackSidebar) {
        return;
    }

    QVector<TrackInfo> tracks;
    const int trackCount = m_timeline->tracks().size();
    tracks.reserve(trackCount);

    for (int i = 0; i < trackCount; ++i) {
        const auto &timelineTrack = m_timeline->tracks()[i];
        TrackInfo info;
        info.name = timelineTrack.name;
        info.visualMode = timelineTrack.visualMode;
        info.audioEnabled = timelineTrack.audioEnabled;
        info.hasVisual = m_timeline->trackHasVisualClips(i);
        info.hasAudio = m_timeline->trackHasAudioClips(i);
        info.top = m_timeline->trackTop(i);
        info.height = m_timeline->trackHeight(i);
        tracks.append(info);
    }

    m_trackSidebar->setTracks(tracks);
    m_trackSidebar->setSelectedTrack(m_timeline->selectedTrackIndex());
    m_trackSidebar->updateGeometry();
    m_trackSidebar->update();
}

void TimelineContainer::onTrackVisualModeChanged(int trackIndex, int mode)
{
    if (m_timeline) {
        m_timeline->setTrackVisualMode(trackIndex, static_cast<TrackVisualMode>(mode));
        m_timeline->update();
    }
}

void TimelineContainer::onTrackAudioToggled(int trackIndex, bool enabled)
{
    if (m_timeline) {
        m_timeline->setTrackAudioEnabled(trackIndex, enabled);
        m_timeline->update();
    }
}

void TimelineContainer::onTrackSelected(int trackIndex)
{
    if (m_timeline) {
        m_timeline->setSelectedTrackIndex(trackIndex);
        m_timeline->update();
    }
}

void TimelineContainer::onTrackMoveUp(int trackIndex)
{
    if (m_timeline) {
        m_timeline->moveTrackUp(trackIndex);
    }
}

void TimelineContainer::onTrackMoveDown(int trackIndex)
{
    if (m_timeline) {
        m_timeline->moveTrackDown(trackIndex);
    }
}

void TimelineContainer::onTrackDropped(int fromIndex, int toIndex)
{
    if (m_timeline) {
        m_timeline->moveTrack(fromIndex, toIndex);
    }
}

void TimelineContainer::onTrackRenameRequested(int trackIndex)
{
    if (m_timeline) {
        m_timeline->renameTrack(trackIndex);
    }
}

void TimelineContainer::onTrackDeleteRequested(int trackIndex)
{
    if (m_timeline) {
        m_timeline->deleteTrack(trackIndex);
    }
}

void TimelineContainer::onTrackSidebarWheelAdjusted(int steps, Qt::KeyboardModifiers modifiers)
{
    if (m_timeline) {
        m_timeline->handleSidebarWheelSteps(steps, modifiers);
    }
}

void TimelineContainer::onTrackSidebarWidthResizeRequested(int width)
{
    const int boundedWidth = qMax(TrackSidebar::kTrackColumnWidth, width);
    if (m_topLeftPlaceholder) {
        m_topLeftPlaceholder->setFixedWidth(boundedWidth);
    }
    if (m_trackSidebar) {
        m_trackSidebar->setFixedWidth(boundedWidth);
    }
    if (m_gridLayout) {
        m_gridLayout->setColumnMinimumWidth(0, boundedWidth);
    }
    updateGeometry();
}

QPushButton *TimelineContainer::playButton() const
{
    if (!m_transportWidget) {
        return nullptr;
    }
    return m_transportWidget->findChild<QPushButton*>(QStringLiteral("transport.play"));
}

QToolButton *TimelineContainer::startButton() const
{
    if (!m_transportWidget) {
        return nullptr;
    }
    return m_transportWidget->findChild<QToolButton*>(QStringLiteral("transport.start"));
}

QToolButton *TimelineContainer::endButton() const
{
    if (!m_transportWidget) {
        return nullptr;
    }
    return m_transportWidget->findChild<QToolButton*>(QStringLiteral("transport.end"));
}

QSlider *TimelineContainer::seekSlider() const
{
    if (!m_transportWidget) {
        return nullptr;
    }
    return m_transportWidget->findChild<QSlider*>(QStringLiteral("transport.seek"));
}

QLabel *TimelineContainer::timecodeLabel() const
{
    if (!m_transportWidget) {
        return nullptr;
    }
    return m_transportWidget->findChild<QLabel*>(QStringLiteral("transport.timecode"));
}
