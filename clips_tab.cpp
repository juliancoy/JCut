#include "clips_tab.h"

#include <QHeaderView>
#include <QMenu>
#include <QSignalBlocker>

ClipsTab::ClipsTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps) {
}

void ClipsTab::wire() {
    if (!m_widgets.clipsTable) {
        return;
    }
    m_widgets.clipsTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_widgets.clipsTable, &QTableWidget::customContextMenuRequested,
            this, &ClipsTab::onCustomContextMenuRequested);
    connect(m_widgets.clipsTable, &QTableWidget::itemChanged,
            this, &ClipsTab::onTableItemChanged);
}

void ClipsTab::refresh() {
    if (m_updating || !m_widgets.clipsTable || !m_deps.getClips) {
        return;
    }
    m_updating = true;

    const QVector<TimelineClip> clips = m_deps.getClips();
    const QVector<TimelineTrack> tracks = m_deps.getTracks ? m_deps.getTracks() : QVector<TimelineTrack>{};

    QSignalBlocker block(m_widgets.clipsTable);
    m_widgets.clipsTable->setRowCount(clips.size());

    for (int row = 0; row < clips.size(); ++row) {
        const TimelineClip& clip = clips[row];
        const QString trackName = (clip.trackIndex >= 0 && clip.trackIndex < tracks.size())
                                      ? tracks[clip.trackIndex].name
                                      : QStringLiteral("Track %1").arg(clip.trackIndex + 1);

        QString typeLabel;
        switch (clip.mediaType) {
            case ClipMediaType::Video:
                typeLabel = QStringLiteral("Video");
                break;
            case ClipMediaType::Image:
                typeLabel = QStringLiteral("Image");
                break;
            case ClipMediaType::Audio:
                typeLabel = QStringLiteral("Audio");
                break;
            case ClipMediaType::Title:
                typeLabel = QStringLiteral("Title");
                break;
            default:
                typeLabel = QStringLiteral("Unknown");
                break;
        }

        auto* nameItem = new QTableWidgetItem(clip.label);
        nameItem->setData(Qt::UserRole, clip.id);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);

        auto* trackItem = new QTableWidgetItem(trackName);
        trackItem->setBackground(QBrush(trackColor(clip.trackIndex)));
        trackItem->setFlags(trackItem->flags() | Qt::ItemIsEditable);

        auto* typeItem = new QTableWidgetItem(typeLabel);
        typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);

        auto* startItem = new QTableWidgetItem(QString::number(clip.startFrame));
        startItem->setFlags(startItem->flags() & ~Qt::ItemIsEditable);

        auto* durationItem = new QTableWidgetItem(QString::number(clip.durationFrames));
        durationItem->setFlags(durationItem->flags() & ~Qt::ItemIsEditable);

        auto* fileItem = new QTableWidgetItem(clip.filePath);
        fileItem->setFlags(fileItem->flags() & ~Qt::ItemIsEditable);
        fileItem->setToolTip(clip.filePath);

        m_widgets.clipsTable->setItem(row, 0, nameItem);
        m_widgets.clipsTable->setItem(row, 1, trackItem);
        m_widgets.clipsTable->setItem(row, 2, typeItem);
        m_widgets.clipsTable->setItem(row, 3, startItem);
        m_widgets.clipsTable->setItem(row, 4, durationItem);
        m_widgets.clipsTable->setItem(row, 5, fileItem);
    }

    m_widgets.clipsTable->resizeColumnsToContents();
    m_updating = false;
}

static int parseTrackNumber(const QString& trackText)
{
    const QString trimmed = trackText.trimmed();
    bool ok = false;
    int trackNumber = trimmed.toInt(&ok);
    if (!ok) {
        QString digits;
        for (const QChar ch : trimmed) {
            if (ch.isDigit()) {
                digits.append(ch);
            }
        }
        if (!digits.isEmpty()) {
            trackNumber = digits.toInt(&ok);
        }
    }
    return ok ? qMax(1, trackNumber) : -1;
}

void ClipsTab::onTableItemChanged(QTableWidgetItem* item)
{
    if (m_updating || !item || item->column() != 1 || !m_widgets.clipsTable) {
        return;
    }

    const int row = item->row();
    QTableWidgetItem* idItem = m_widgets.clipsTable->item(row, 0);
    if (!idItem) {
        return;
    }
    const QString clipId = idItem->data(Qt::UserRole).toString();
    if (clipId.isEmpty()) {
        return;
    }

    const int trackNumber = parseTrackNumber(item->text());
    if (trackNumber < 1) {
        refresh();
        return;
    }
    const int newTrackIndex = trackNumber - 1;

    const QVector<TimelineClip> clips = m_deps.getClips ? m_deps.getClips() : QVector<TimelineClip>{};
    const TimelineClip* clipped = nullptr;
    for (const TimelineClip& clip : clips) {
        if (clip.id == clipId) {
            clipped = &clip;
            break;
        }
    }
    if (!clipped) {
        refresh();
        return;
    }

    if (clipped->trackIndex == newTrackIndex) {
        refresh();
        return;
    }

    if (m_deps.updateTrackByIndex) {
        m_deps.updateTrackByIndex(newTrackIndex, [](TimelineTrack&) {});
    }

    const bool updated = m_deps.updateClipById
        ? m_deps.updateClipById(clipId, [newTrackIndex](TimelineClip& clip) {
              clip.trackIndex = newTrackIndex;
          })
        : false;

    if (updated) {
        if (m_deps.pushHistorySnapshot) {
            m_deps.pushHistorySnapshot();
        }
        if (m_deps.scheduleSaveState) {
            m_deps.scheduleSaveState();
        }
    }

    refresh();
}

void ClipsTab::onCustomContextMenuRequested(const QPoint& pos) {
    if (!m_widgets.clipsTable) {
        return;
    }
    QTableWidgetItem* item = m_widgets.clipsTable->itemAt(pos);
    if (!item) {
        return;
    }
    const int row = item->row();
    QTableWidgetItem* idItem = m_widgets.clipsTable->item(row, 0);
    if (!idItem) {
        return;
    }
    const QString clipId = idItem->data(Qt::UserRole).toString();

    QMenu menu(m_widgets.clipsTable);
    QAction* selectAction = menu.addAction(QStringLiteral("Select Clip"));
    QAction* deleteAction = menu.addAction(QStringLiteral("Delete"));

    QAction* selected = menu.exec(m_widgets.clipsTable->viewport()->mapToGlobal(pos));
    if (selected == selectAction) {
        if (m_deps.selectClipId) {
            m_deps.selectClipId(clipId);
        }
    } else if (selected == deleteAction) {
        if (m_deps.deleteClipById) {
            if (m_deps.deleteClipById(clipId)) {
                if (m_deps.pushHistorySnapshot) {
                    m_deps.pushHistorySnapshot();
                }
                if (m_deps.scheduleSaveState) {
                    m_deps.scheduleSaveState();
                }
                refresh();
            }
        }
    }
}

QColor ClipsTab::trackColor(int trackIndex) {
    static const QVector<QColor> colors = {
        QColor(QStringLiteral("#4ea1ff")),
        QColor(QStringLiteral("#6ecf73")),
        QColor(QStringLiteral("#f7b955")),
        QColor(QStringLiteral("#e86c6c")),
        QColor(QStringLiteral("#c586f0")),
        QColor(QStringLiteral("#4ecdc4")),
        QColor(QStringLiteral("#ff8c42")),
        QColor(QStringLiteral("#7fd8be")),
    };
    return colors.value(trackIndex % colors.size(), colors.first());
}
