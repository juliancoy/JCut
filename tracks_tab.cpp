#include "tracks_tab.h"

TracksTab::TracksTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
}

void TracksTab::wire()
{
    if (!m_widgets.tracksTable) {
        return;
    }
    connect(m_widgets.tracksTable, &QTableWidget::itemChanged,
            this, &TracksTab::onTableItemChanged);
}

void TracksTab::refresh()
{
    if (!m_widgets.tracksTable || !m_deps.getTracks) {
        return;
    }

    m_updating = true;
    const QVector<TimelineTrack> tracks = m_deps.getTracks();
    m_widgets.tracksTable->setRowCount(tracks.size());

    for (int row = 0; row < tracks.size(); ++row) {
        const TimelineTrack& track = tracks[row];
        const QString trackName = track.name.trimmed().isEmpty()
            ? QStringLiteral("Track %1").arg(row + 1)
            : track.name;

        auto* nameItem = new QTableWidgetItem(trackName);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);

        auto* visualItem = new QTableWidgetItem;
        const bool hasVisual = m_deps.trackHasVisualClips ? m_deps.trackHasVisualClips(row) : false;
        const TrackVisualMode visualMode =
            m_deps.trackVisualMode ? m_deps.trackVisualMode(row) : TrackVisualMode::Enabled;
        Qt::ItemFlags visualFlags = Qt::ItemIsUserCheckable | Qt::ItemIsSelectable;
        if (hasVisual) {
            visualFlags |= Qt::ItemIsEnabled;
        }
        visualItem->setFlags(visualFlags);
        visualItem->setCheckState(
            visualMode == TrackVisualMode::Hidden ? Qt::Unchecked
            : (visualMode == TrackVisualMode::ForceOpaque ? Qt::PartiallyChecked : Qt::Checked));
        if (!hasVisual) {
            visualItem->setToolTip(QStringLiteral("No visual clips on this track"));
        } else if (visualMode == TrackVisualMode::ForceOpaque) {
            visualItem->setToolTip(QStringLiteral("Force Opaque"));
        }

        auto* audioItem = new QTableWidgetItem;
        const bool hasAudio = m_deps.trackHasAudioClips ? m_deps.trackHasAudioClips(row) : false;
        const bool audioEnabled = m_deps.trackAudioEnabled ? m_deps.trackAudioEnabled(row) : false;
        Qt::ItemFlags audioFlags = Qt::ItemIsUserCheckable | Qt::ItemIsSelectable;
        if (hasAudio) {
            audioFlags |= Qt::ItemIsEnabled;
        }
        audioItem->setFlags(audioFlags);
        audioItem->setCheckState(audioEnabled ? Qt::Checked : Qt::Unchecked);
        if (!hasAudio) {
            audioItem->setToolTip(QStringLiteral("No audio clips on this track"));
        }

        m_widgets.tracksTable->setItem(row, 0, nameItem);
        m_widgets.tracksTable->setItem(row, 1, visualItem);
        m_widgets.tracksTable->setItem(row, 2, audioItem);
    }

    m_widgets.tracksTable->resizeColumnsToContents();
    m_updating = false;
}

void TracksTab::onTableItemChanged(QTableWidgetItem* item)
{
    if (m_updating || !item || !m_widgets.tracksTable) {
        return;
    }

    const int row = item->row();
    if (row < 0 || row >= m_widgets.tracksTable->rowCount()) {
        return;
    }

    const bool checked = item->checkState() == Qt::Checked;
    bool changed = false;

    if (item->column() == 1) {
        const TrackVisualMode mode =
            item->checkState() == Qt::Unchecked ? TrackVisualMode::Hidden
            : (item->checkState() == Qt::PartiallyChecked ? TrackVisualMode::ForceOpaque
                                                          : TrackVisualMode::Enabled);
        changed = m_deps.updateTrackVisualMode ? m_deps.updateTrackVisualMode(row, mode) : false;
    } else if (item->column() == 2) {
        changed = m_deps.updateTrackAudioEnabled ? m_deps.updateTrackAudioEnabled(row, checked) : false;
    }

    if (changed) {
        if (m_deps.refreshInspector) {
            m_deps.refreshInspector();
        }
        if (m_deps.scheduleSaveState) {
            m_deps.scheduleSaveState();
        }
        if (m_deps.pushHistorySnapshot) {
            m_deps.pushHistorySnapshot();
        }
    } else {
        refresh();
    }
}
