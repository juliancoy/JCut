#include "track_sidebar.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QStyleOption>
#include <QMenu>
#include <QContextMenuEvent>
#include <QWheelEvent>

namespace {
    constexpr int kDefaultTrackHeight = 44;
    constexpr int kTrackSpacing = 10;

    void drawEyeIcon(QPainter& painter, const QRect& rect, TrackVisualMode mode, bool interactive) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        const bool enabled = mode != TrackVisualMode::Hidden;
        const bool forceOpaque = mode == TrackVisualMode::ForceOpaque;
        const QColor stroke = interactive
                                  ? (enabled ? (forceOpaque ? QColor(QStringLiteral("#ffd27a"))
                                                            : QColor(QStringLiteral("#eef4fa")))
                                             : QColor(QStringLiteral("#7f8a99")))
                                  : QColor(QStringLiteral("#556170"));
        painter.setPen(QPen(stroke, 1.7));
        painter.setBrush(Qt::NoBrush);

        QPainterPath path;
        path.moveTo(rect.left() + rect.width() * 0.10, rect.center().y());
        path.quadTo(rect.center().x(), rect.top() + rect.height() * 0.08,
                    rect.right() - rect.width() * 0.10, rect.center().y());
        path.quadTo(rect.center().x(), rect.bottom() - rect.height() * 0.08,
                    rect.left() + rect.width() * 0.10, rect.center().y());
        painter.drawPath(path);
        painter.setBrush(forceOpaque ? QColor(QStringLiteral("#ffb347")) : stroke);
        painter.drawEllipse(QRectF(rect.center().x() - rect.width() * 0.10,
                                   rect.center().y() - rect.height() * 0.10,
                                   rect.width() * 0.20,
                                   rect.height() * 0.20));
        if (forceOpaque && enabled) {
            painter.setPen(QPen(QColor(QStringLiteral("#ffb347")), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(QRectF(rect.center().x() - rect.width() * 0.19,
                                       rect.center().y() - rect.height() * 0.19,
                                       rect.width() * 0.38,
                                       rect.height() * 0.38));
        }
        if (!enabled) {
            painter.setPen(QPen(QColor(QStringLiteral("#ff8c82")), 2.0));
            painter.drawLine(rect.left() + 2, rect.bottom() - 2, rect.right() - 2, rect.top() + 2);
        }
        painter.restore();
    }

    void drawSpeakerIcon(QPainter& painter, const QRect& rect, bool enabled, bool interactive) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor stroke = interactive
                                  ? (enabled ? QColor(QStringLiteral("#eef4fa"))
                                             : QColor(QStringLiteral("#7f8a99")))
                                  : QColor(QStringLiteral("#556170"));
        painter.setPen(QPen(stroke, 1.7));
        painter.setBrush(Qt::NoBrush);

        QPainterPath speaker;
        speaker.moveTo(rect.left() + rect.width() * 0.18, rect.center().y() - rect.height() * 0.18);
        speaker.lineTo(rect.left() + rect.width() * 0.36, rect.center().y() - rect.height() * 0.18);
        speaker.lineTo(rect.left() + rect.width() * 0.55, rect.top() + rect.height() * 0.18);
        speaker.lineTo(rect.left() + rect.width() * 0.55, rect.bottom() - rect.height() * 0.18);
        speaker.lineTo(rect.left() + rect.width() * 0.36, rect.center().y() + rect.height() * 0.18);
        speaker.lineTo(rect.left() + rect.width() * 0.18, rect.center().y() + rect.height() * 0.18);
        speaker.closeSubpath();
        painter.drawPath(speaker);
        if (enabled) {
            painter.drawArc(QRect(rect.left() + rect.width() * 0.45,
                                  rect.top() + rect.height() * 0.18,
                                  rect.width() * 0.28,
                                  rect.height() * 0.64),
                            -40 * 16,
                            80 * 16);
            painter.drawArc(QRect(rect.left() + rect.width() * 0.52,
                                  rect.top() + rect.height() * 0.06,
                                  rect.width() * 0.34,
                                  rect.height() * 0.88),
                            -40 * 16,
                            80 * 16);
        } else {
            painter.setPen(QPen(QColor(QStringLiteral("#ff8c82")), 2.0));
            painter.drawLine(rect.left() + rect.width() * 0.62,
                             rect.top() + rect.height() * 0.24,
                             rect.right() - 2,
                             rect.bottom() - rect.height() * 0.22);
            painter.drawLine(rect.right() - 2,
                             rect.top() + rect.height() * 0.24,
                             rect.left() + rect.width() * 0.62,
                             rect.bottom() - rect.height() * 0.22);
        }
        painter.restore();
    }
}

TrackSidebar::TrackSidebar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("timeline.track_sidebar"));
}

void TrackSidebar::setTracks(const QVector<TrackInfo> &tracks) {
    m_tracks = tracks;
    updateGeometry();
    update();
}

void TrackSidebar::setSelectedTrack(int index) {
    m_selectedTrack = index;
    update();
}

void TrackSidebar::setDraggedTrack(int index) {
    m_draggedTrack = index;
    update();
}

void TrackSidebar::setDropTarget(int index, bool inGap) {
    m_dropTarget = index;
    m_dropInGap = inGap;
    update();
}

bool TrackSidebar::isInResizeHandle(const QPoint &pos) const {
    return pos.x() >= width() - kResizeHandleWidth;
}

int TrackSidebar::dropTargetAt(const QPoint &pos) const {
    const int directHit = trackAt(pos);
    if (directHit >= 0) {
        return directHit;
    }
    if (m_tracks.isEmpty()) {
        return -1;
    }
    if (pos.y() < trackTop(0)) {
        return 0;
    }
    return m_tracks.size() - 1;
}

int TrackSidebar::trackAt(const QPoint &pos) const {
    for (int i = 0; i < m_tracks.size(); ++i) {
        if (trackLabelRect(i).contains(pos)) {
            return i;
        }
    }
    return -1;
}

QRect TrackSidebar::trackRect(int index) const {
    return trackLabelRect(index);
}

QRect TrackSidebar::trackLabelRect(int index) const {
    const int sidebarWidth = width() > 0 ? width() : kTrackColumnWidth;
    return QRect(4, trackTop(index) + 2, qMax(0, sidebarWidth - 8), qMax(0, trackHeight(index) - 4));
}

QRect TrackSidebar::trackVisualToggleRect(int index) const {
    const QRect header = trackLabelRect(index);
    return QRect(header.right() - 56, header.center().y() - 11, 20, 22);
}

QRect TrackSidebar::trackAudioToggleRect(int index) const {
    const QRect header = trackLabelRect(index);
    return QRect(header.right() - 28, header.center().y() - 11, 20, 22);
}

int TrackSidebar::trackHeight(int index) const {
    if (index >= 0 && index < m_tracks.size()) {
        return qMax(kMinTrackHeight, m_tracks[index].height);
    }
    return kDefaultTrackHeight;
}

int TrackSidebar::trackTop(int index) const {
    if (index >= 0 && index < m_tracks.size()) {
        return m_tracks[index].top;
    }

    int y = 0;
    for (int i = 0; i < index && i < m_tracks.size(); ++i) {
        y += trackHeight(i) + kTrackSpacing;
    }
    return y;
}

void TrackSidebar::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    painter.fillRect(rect(), QColor(QStringLiteral("#14181e")));

    for (int track = 0; track < m_tracks.size(); ++track) {
        const QRect labelRect = trackLabelRect(track);
        const bool dragged = track == m_draggedTrack;
        const bool target = track == m_dropTarget && !m_dropInGap;
        const bool selected = track == m_selectedTrack;

        const QColor headerFill =
            dragged ? QColor(QStringLiteral("#ff6f61"))
                    : (target ? QColor(QStringLiteral("#32465f"))
                              : (selected ? QColor(QStringLiteral("#24384d"))
                                          : QColor(QStringLiteral("#1e252d"))));
        painter.setBrush(headerFill);
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(labelRect, 8, 8);

        painter.setPen(QPen(QColor(QStringLiteral("#2a3542")), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(labelRect.adjusted(0, 0, -1, -1), 8, 8);

        if (selected) {
            painter.setPen(QPen(QColor(QStringLiteral("#7fc4ff")), 1.4));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(labelRect.adjusted(0, 0, -1, -1), 8, 8);
        }

        painter.setPen(QColor(QStringLiteral("#eef4fa")));
        QFont nameFont = painter.font();
        nameFont.setBold(true);
        painter.setFont(nameFont);

        const QRect audioRect = trackAudioToggleRect(track);
        QRect nameRect(labelRect.left() + 10,
                       labelRect.top(),
                       qMax(24, audioRect.left() - labelRect.left() - 18),
                       labelRect.height());

        const QString trackLabel = QStringLiteral("%1. %2")
                                       .arg(track + 1)
                                       .arg(m_tracks[track].name);
        painter.drawText(nameRect,
                         Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(trackLabel, Qt::ElideRight, nameRect.width()));
        painter.setFont(QFont());

        const QRect visualRect = trackVisualToggleRect(track);
        const bool hasVisual = m_tracks[track].hasVisual;
        const bool hasAudio = m_tracks[track].hasAudio;
        const TrackVisualMode visualMode = m_tracks[track].visualMode;
        const bool audioEnabled = m_tracks[track].audioEnabled;

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#141a21")));
        painter.drawRoundedRect(visualRect.adjusted(-4, -2, 4, 2), 7, 7);
        painter.drawRoundedRect(audioRect.adjusted(-4, -2, 4, 2), 7, 7);
        drawEyeIcon(painter, visualRect, visualMode, hasVisual);
        drawSpeakerIcon(painter, audioRect, audioEnabled, hasAudio);

        painter.setPen(QColor(QStringLiteral("#24303c")));
        const int dividerY = trackTop(track) + trackHeight(track);
        painter.drawLine(labelRect.left() + 6, dividerY, labelRect.right() - 6, dividerY);
    }

    if (m_dropTarget >= 0 && m_dropInGap) {
        int insertionY = -(kTrackSpacing / 2);
        if (!m_tracks.isEmpty()) {
            insertionY = trackTop(0) - (kTrackSpacing / 2);
            if (m_dropTarget >= m_tracks.size()) {
                insertionY = trackTop(m_tracks.size() - 1) + trackHeight(m_tracks.size() - 1) + (kTrackSpacing / 2);
            } else if (m_dropTarget > 0) {
                insertionY = trackTop(m_dropTarget) - (kTrackSpacing / 2);
            }
        }
        painter.setPen(QPen(QColor(QStringLiteral("#f7b955")), 2, Qt::DashLine));
        painter.drawLine(4, insertionY, width() - 4, insertionY);
    }
}

void TrackSidebar::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        if (isInResizeHandle(event->pos())) {
            m_resizingWidth = true;
            m_resizeStartX = event->globalPosition().toPoint().x();
            m_resizeStartWidth = width();
            setCursor(Qt::SizeHorCursor);
            event->accept();
            return;
        }

        m_dragStartPos = event->pos();
        m_dragging = false;

        const int track = trackAt(event->pos());
        if (track >= 0) {
            if (trackVisualToggleRect(track).contains(event->pos())) {
                switch (m_tracks[track].visualMode) {
                case TrackVisualMode::Enabled:
                    m_tracks[track].visualMode = TrackVisualMode::ForceOpaque;
                    break;
                case TrackVisualMode::ForceOpaque:
                    m_tracks[track].visualMode = TrackVisualMode::Hidden;
                    break;
                case TrackVisualMode::Hidden:
                default:
                    m_tracks[track].visualMode = TrackVisualMode::Enabled;
                    break;
                }
                update(trackVisualToggleRect(track).adjusted(-6, -4, 6, 4));
                emit trackVisualModeChanged(track, static_cast<int>(m_tracks[track].visualMode));
                event->accept();
                return;
            }
            if (trackAudioToggleRect(track).contains(event->pos())) {
                m_tracks[track].audioEnabled = !m_tracks[track].audioEnabled;
                update(trackAudioToggleRect(track).adjusted(-6, -4, 6, 4));
                emit trackAudioToggled(track, m_tracks[track].audioEnabled);
                event->accept();
                return;
            }
            emit trackSelected(track);
        }
    }
}

void TrackSidebar::mouseMoveEvent(QMouseEvent *event) {
    if (m_resizingWidth) {
        const int delta = event->globalPosition().toPoint().x() - m_resizeStartX;
        emit widthResizeRequested(qBound(kMinSidebarWidth, m_resizeStartWidth + delta, kMaxSidebarWidth));
        event->accept();
        return;
    }

    if (!(event->buttons() & Qt::LeftButton)) {
        if (isInResizeHandle(event->pos())) {
            setCursor(Qt::SizeHorCursor);
        } else {
            unsetCursor();
        }
    }

    if (event->buttons() & Qt::LeftButton) {
        if (!m_dragging && (event->pos() - m_dragStartPos).manhattanLength() > 10) {
            m_dragging = true;
            const int track = trackAt(m_dragStartPos);
            if (track >= 0) {
                setDraggedTrack(track);
                setDropTarget(track, false);
                emit trackDragStarted(track);
            }
        } else if (m_dragging) {
            const int target = dropTargetAt(event->pos());
            if (target >= 0) {
                setDropTarget(target, false);
            }
        }
    }
}

void TrackSidebar::mouseReleaseEvent(QMouseEvent *event) {
    if (event && event->button() == Qt::LeftButton && m_dragging && m_draggedTrack >= 0 && m_dropTarget >= 0) {
        emit trackDropped(m_draggedTrack, m_dropTarget);
    }
    m_resizingWidth = false;
    m_dragging = false;
    m_draggedTrack = -1;
    m_dropTarget = -1;
    m_dropInGap = false;
    unsetCursor();
    update();
}

void TrackSidebar::wheelEvent(QWheelEvent *event) {
    const QPoint numDegrees = event->angleDelta() / 8;
    const int steps = numDegrees.y() / 15;
    if (steps == 0) {
        QWidget::wheelEvent(event);
        return;
    }

    emit wheelAdjusted(steps, event->modifiers());
    event->accept();
}

QSize TrackSidebar::sizeHint() const {
    int totalHeight = 0;
    if (m_tracks.isEmpty()) {
        totalHeight = kDefaultTrackHeight + kTrackSpacing;
    } else {
        for (int i = 0; i < m_tracks.size(); ++i) {
            totalHeight += trackHeight(i) + kTrackSpacing;
        }
    }
    return QSize(kTrackColumnWidth, totalHeight);
}

QSize TrackSidebar::minimumSizeHint() const {
    return QSize(kTrackColumnWidth, kDefaultTrackHeight + kTrackSpacing);
}

void TrackSidebar::contextMenuEvent(QContextMenuEvent *event) {
    const int track = trackAt(event->pos());
    if (track < 0) {
        return;
    }

    setSelectedTrack(track);
    emit trackSelected(track);

    QMenu menu(this);

    QAction *moveUpAction = menu.addAction(QStringLiteral("Move Track Up"));
    QAction *moveDownAction = menu.addAction(QStringLiteral("Move Track Down"));

    moveUpAction->setEnabled(track > 0);
    moveDownAction->setEnabled(track < m_tracks.size() - 1);

    menu.addSeparator();

    QAction *renameAction = menu.addAction(QStringLiteral("Rename Track..."));
    QAction *deleteAction = menu.addAction(QStringLiteral("Delete Track"));
    QAction *crossfadeAction = menu.addAction(QStringLiteral("Crossfade Consecutive Clips..."));

    QAction *chosen = menu.exec(event->globalPos());
    if (!chosen) {
        return;
    }

    if (chosen == moveUpAction) {
        emit trackMoveUpRequested(track);
    } else if (chosen == moveDownAction) {
        emit trackMoveDownRequested(track);
    } else if (chosen == renameAction) {
        emit trackRenameRequested(track);
    } else if (chosen == deleteAction) {
        emit trackDeleteRequested(track);
    } else if (chosen == crossfadeAction) {
        // TODO: Implement crossfade
    }
}
