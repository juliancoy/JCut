#pragma once

#include <QWidget>

#include <array>
#include <QColor>
#include <QPointF>
#include <QVector>

class QImage;

class GradingHistogramWidget : public QWidget
{
    Q_OBJECT

public:
    enum class Channel {
        Red = 0,
        Green = 1,
        Blue = 2,
        Brightness = 3,
        Alpha = 4,
    };

    explicit GradingHistogramWidget(QWidget* parent = nullptr);

    void clearHistogram();
    void setHistogramFromImage(const QImage& image);
    void setSelectedChannel(Channel channel);
    Channel selectedChannel() const { return m_selectedChannel; }
    void setCurvePoints(const QVector<QPointF>& points);
    QVector<QPointF> curvePoints() const;
    void setThreePointLockEnabled(bool enabled);
    bool threePointLockEnabled() const { return m_threePointLockEnabled; }
    void setCurveSmoothingEnabled(bool enabled);
    bool curveSmoothingEnabled() const { return m_curveSmoothingEnabled; }
    bool hasAlphaHistogram() const { return m_hasAlphaHistogram; }
    void setChartBackgroundColor(const QColor& color);

signals:
    void curvePointsAdjusted(const QVector<QPointF>& points, bool finalized);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QRectF chartRect() const;
    QPointF pointToWidget(const QPointF& point) const;
    QPointF widgetToPoint(const QPointF& pos) const;
    int nearestPointIndex(const QPointF& pos) const;
    int segmentIndexAtX(qreal xNorm) const;
    void sortAndClampPoints();
    QVector<QPointF> lockToThreePointCurve(const QVector<QPointF>& points) const;
    const std::array<float, 256>& selectedHistogram() const;
    void emitCurveChanged(bool finalized);

    std::array<float, 256> m_histogramR{};
    std::array<float, 256> m_histogramG{};
    std::array<float, 256> m_histogramB{};
    std::array<float, 256> m_histogramA{};
    std::array<float, 256> m_histogramLuma{};
    bool m_hasHistogram = false;
    bool m_hasAlphaHistogram = false;

    Channel m_selectedChannel = Channel::Red;
    QVector<QPointF> m_points;
    bool m_threePointLockEnabled = false;
    bool m_curveSmoothingEnabled = true;
    QColor m_chartBackgroundColor = QColor(16, 22, 30, 255);

    int m_activePoint = -1;
    bool m_dragging = false;
};
