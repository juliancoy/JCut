#pragma once

#include <QWidget>

#include <array>

class QImage;

class GradingHistogramWidget : public QWidget
{
    Q_OBJECT

public:
    enum class Channel {
        Red = 0,
        Green = 1,
        Blue = 2,
    };

    explicit GradingHistogramWidget(QWidget* parent = nullptr);

    void clearHistogram();
    void setHistogramFromImage(const QImage& image);
    void setSelectedChannel(Channel channel);
    Channel selectedChannel() const { return m_selectedChannel; }
    void setCurveValues(double shadows, double midtones, double highlights);

signals:
    void curveAdjusted(double shadows, double midtones, double highlights, bool finalized);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QRectF chartRect() const;
    QPointF handlePoint(int index) const;
    int nearestHandleIndex(const QPointF& pos) const;
    double yToNormalizedValue(qreal y) const;
    qreal normalizedValueToY(double value) const;
    double parameterToNormalizedValue(int index, double parameter) const;
    double normalizedValueToParameter(int index, double normalizedValue) const;
    void updateParameterFromDrag(int handleIndex, const QPointF& pos, bool finalized);

    std::array<float, 256> m_histogramR{};
    std::array<float, 256> m_histogramG{};
    std::array<float, 256> m_histogramB{};
    bool m_hasHistogram = false;

    Channel m_selectedChannel = Channel::Red;
    double m_shadows = 0.0;
    double m_midtones = 0.0;
    double m_highlights = 0.0;

    int m_activeHandle = -1;
    bool m_dragging = false;
};

