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
    m_gridLayout = new QGridLayout(this);
    m_gridLayout->setContentsMargins(0, 0, 0, 0);
    m_gridLayout->setSpacing(0);
    m_gridLayout->setColumnStretch(0, 0);  // Sidebar column: fixed width
    m_gridLayout->setColumnStretch(1, 1);  // Timeline column: stretch
    m_gridLayout->setRowStretch(0, 0);     // Transport row: fixed height
    m_gridLayout->setRowStretch(1, 1);     // Content row: stretch

    // === TOP LEFT: Placeholder (track column header area) ===
    m_topLeftPlaceholder = new QWidget(this);
    m_topLeftPlaceholder->setObjectName(QStringLiteral("timeline.placeholder"));
    m_topLeftPlaceholder->setFixedWidth(TrackSidebar::kTrackColumnWidth);
    m_topLeftPlaceholder->setFixedHeight(44);
    m_topLeftPlaceholder->setStyleSheet(QStringLiteral(
        "QWidget { background: #14181e; border-bottom: 1px solid #2a3542; }"));
    
    // Add a label to the placeholder
    auto *placeholderLayout = new QHBoxLayout(m_topLeftPlaceholder);
    placeholderLayout->setContentsMargins(8, 4, 8, 4);
    auto *placeholderLabel = new QLabel(QStringLiteral("Tracks"));
    placeholderLabel->setStyleSheet(QStringLiteral("color: #7f8a99; font-weight: bold; font-size: 11px;"));
    placeholderLayout->addWidget(placeholderLabel);
    placeholderLayout->addStretch();
    
    m_gridLayout->addWidget(m_topLeftPlaceholder, 0, 0);

    // === TOP RIGHT: Transport controls ===
    m_transportWidget = new QWidget(this);
    m_transportWidget->setObjectName(QStringLiteral("timeline.transport"));
    m_transportWidget->setFixedHeight(44);
    m_transportWidget->setStyleSheet(QStringLiteral(
        "QWidget { background: #14181e; border-bottom: 1px solid #2a3542; }"));
    
    // Create layout for transport - EditorPane will add buttons to this
    auto *transportLayout = new QHBoxLayout(m_transportWidget);
    transportLayout->setContentsMargins(8, 4, 8, 4);
    transportLayout->setSpacing(6);
    transportLayout->addStretch();  // Placeholder stretch
    
    m_gridLayout->addWidget(m_transportWidget, 0, 1);

    // === BOTTOM LEFT: Track Sidebar ===
    m_trackSidebar = new TrackSidebar(this);
    m_trackSidebar->setObjectName(QStringLiteral("timeline.track_sidebar"));
    // Align top so it doesn't stretch vertically
    m_gridLayout->addWidget(m_trackSidebar, 1, 0, Qt::AlignTop);

    // === BOTTOM RIGHT: Timeline Widget ===
    m_timeline = new TimelineWidget(this);
    m_timeline->setObjectName(QStringLiteral("timeline.widget"));
    m_gridLayout->addWidget(m_timeline, 1, 1);

    // Initial sync
    QTimer::singleShot(0, this, &TimelineContainer::syncTracksFromTimeline);
}

void TimelineContainer::connectSignals()
{
    // Connect sidebar signals to timeline slots
    connect(m_trackSidebar, &TrackSidebar::trackVisualToggled,
            this, &TimelineContainer::onTrackVisualToggled);
    connect(m_trackSidebar, &TrackSidebar::trackAudioToggled,
            this, &TimelineContainer::onTrackAudioToggled);
    connect(m_trackSidebar, &TrackSidebar::trackSelected,
            this, &TimelineContainer::onTrackSelected);
    
    // Use std::function callbacks from timeline since it doesn't use Qt signals
    if (m_timeline) {
        m_timeline->clipsChanged = [this]() {
            syncTracksFromTimeline();
        };
        m_timeline->selectionChanged = [this]() {
            m_trackSidebar->setSelectedTrack(m_timeline->selectedTrackIndex());
        };
    }
}

void TimelineContainer::syncTracksFromTimeline()
{
    if (!m_timeline || !m_trackSidebar)
        return;

    // Get track info from timeline and sync to sidebar
    QVector<TrackInfo> tracks;
    int trackCount = m_timeline->tracks().size();
    
    for (int i = 0; i < trackCount; ++i) {
        const auto &timelineTrack = m_timeline->tracks()[i];
        TrackInfo info;
        info.name = timelineTrack.name;
        info.visualEnabled = timelineTrack.visualEnabled;
        info.audioEnabled = timelineTrack.audioEnabled;
        info.hasVisual = m_timeline->trackHasVisualClips(i);
        info.hasAudio = m_timeline->trackHasAudioClips(i);
        tracks.append(info);
    }
    
    m_trackSidebar->setTracks(tracks);
    m_trackSidebar->setSelectedTrack(m_timeline->selectedTrackIndex());
    m_trackSidebar->updateGeometry();
    m_trackSidebar->update();
}

void TimelineContainer::onTrackVisualToggled(int trackIndex, bool enabled)
{
    if (m_timeline) {
        m_timeline->setTrackVisualEnabled(trackIndex, enabled);
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

// Accessors for transport controls (these will return nullptr for now, 
// EditorPane will create the actual transport controls and add them to m_transportWidget)
QPushButton *TimelineContainer::playButton() const
{
    // Find the play button in the transport widget
    if (!m_transportWidget)
        return nullptr;
    return m_transportWidget->findChild<QPushButton*>(QStringLiteral("transport.play"));
}

QToolButton *TimelineContainer::startButton() const
{
    if (!m_transportWidget)
        return nullptr;
    return m_transportWidget->findChild<QToolButton*>(QStringLiteral("transport.start"));
}

QToolButton *TimelineContainer::endButton() const
{
    if (!m_transportWidget)
        return nullptr;
    return m_transportWidget->findChild<QToolButton*>(QStringLiteral("transport.end"));
}

QSlider *TimelineContainer::seekSlider() const
{
    if (!m_transportWidget)
        return nullptr;
    return m_transportWidget->findChild<QSlider*>(QStringLiteral("transport.seek"));
}

QLabel *TimelineContainer::timecodeLabel() const
{
    if (!m_transportWidget)
        return nullptr;
    return m_transportWidget->findChild<QLabel*>(QStringLiteral("transport.timecode"));
}
