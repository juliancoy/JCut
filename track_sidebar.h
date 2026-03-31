#pragma once

#include <QWidget>
#include <QVector>

struct TrackInfo {
    QString name;
    bool visualEnabled = true;
    bool audioEnabled = true;
    bool hasVisual = false;
    bool hasAudio = false;
};

class TrackSidebar final : public QWidget {
    Q_OBJECT

public:
    explicit TrackSidebar(QWidget *parent = nullptr);

    void setTracks(const QVector<TrackInfo> &tracks);
    void setSelectedTrack(int index);
    void setDraggedTrack(int index);
    void setDropTarget(int index, bool inGap);

    int trackAt(const QPoint &pos) const;
    QRect trackRect(int index) const;

    static constexpr int kTrackColumnWidth = 110;

signals:
    void trackVisualToggled(int trackIndex, bool enabled);
    void trackAudioToggled(int trackIndex, bool enabled);
    void trackSelected(int trackIndex);
    void trackDragStarted(int trackIndex);
    void trackDropped(int fromIndex, int toIndex);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    QRect trackLabelRect(int index) const;
    QRect trackVisualToggleRect(int index) const;
    QRect trackAudioToggleRect(int index) const;
    int trackHeight() const;
    int trackTop(int index) const;

    QVector<TrackInfo> m_tracks;
    int m_selectedTrack = -1;
    int m_draggedTrack = -1;
    int m_dropTarget = -1;
    bool m_dropInGap = false;
    QPoint m_dragStartPos;
    bool m_dragging = false;
};
