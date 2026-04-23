#include "grading_histogram_widget.h"

#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr int kBins = 256;
constexpr int kTargetSampleCount = 180000;
constexpr qreal kHandleRadius = 5.5;
constexpr qreal kHandleHitRadius = 12.0;
constexpr double kCurveStrength = 0.35;
constexpr double kCurveScale = 0.75;

float normalizeHistogramBin(uint32_t bin, uint32_t maxBin)
{
    if (maxBin == 0u) {
        return 0.0f;
    }
    const double top = std::log1p(static_cast<double>(maxBin));
    if (top <= 0.0) {
        return 0.0f;
    }
    return static_cast<float>(std::log1p(static_cast<double>(bin)) / top);
}

QPainterPath histogramPath(const QRectF& rect, const std::array<float, kBins>& histogram)
{
    QPainterPath path;
    if (rect.width() <= 1.0 || rect.height() <= 1.0) {
        return path;
    }

    const qreal left = rect.left();
    const qreal bottom = rect.bottom();
    const qreal width = rect.width();
    const qreal height = rect.height();

    path.moveTo(left, bottom);
    for (int i = 0; i < kBins; ++i) {
        const qreal x = left + (width * static_cast<qreal>(i) / static_cast<qreal>(kBins - 1));
        const qreal y = bottom - (height * static_cast<qreal>(histogram[static_cast<size_t>(i)]));
        path.lineTo(x, y);
    }
    path.lineTo(rect.right(), bottom);
    path.closeSubpath();
    return path;
}

} // namespace

GradingHistogramWidget::GradingHistogramWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(170);
    setMouseTracking(true);
}

void GradingHistogramWidget::clearHistogram()
{
    m_hasHistogram = false;
    m_histogramR.fill(0.0f);
    m_histogramG.fill(0.0f);
    m_histogramB.fill(0.0f);
    update();
}

void GradingHistogramWidget::setHistogramFromImage(const QImage& image)
{
    if (image.isNull()) {
        clearHistogram();
        return;
    }

    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull() || rgba.width() <= 0 || rgba.height() <= 0) {
        clearHistogram();
        return;
    }

    std::array<uint32_t, kBins> rBins{};
    std::array<uint32_t, kBins> gBins{};
    std::array<uint32_t, kBins> bBins{};

    const int width = rgba.width();
    const int height = rgba.height();
    const int pixelCount = width * height;
    const int step = qMax(1, static_cast<int>(std::sqrt(
                               static_cast<double>(qMax(1, pixelCount / kTargetSampleCount)))));

    for (int y = 0; y < height; y += step) {
        const uchar* row = rgba.constScanLine(y);
        for (int x = 0; x < width; x += step) {
            const int idx = x * 4;
            const uchar r = row[idx];
            const uchar g = row[idx + 1];
            const uchar b = row[idx + 2];
            ++rBins[static_cast<size_t>(r)];
            ++gBins[static_cast<size_t>(g)];
            ++bBins[static_cast<size_t>(b)];
        }
    }

    const uint32_t rMax = *std::max_element(rBins.begin(), rBins.end());
    const uint32_t gMax = *std::max_element(gBins.begin(), gBins.end());
    const uint32_t bMax = *std::max_element(bBins.begin(), bBins.end());

    for (int i = 0; i < kBins; ++i) {
        m_histogramR[static_cast<size_t>(i)] = normalizeHistogramBin(rBins[static_cast<size_t>(i)], rMax);
        m_histogramG[static_cast<size_t>(i)] = normalizeHistogramBin(gBins[static_cast<size_t>(i)], gMax);
        m_histogramB[static_cast<size_t>(i)] = normalizeHistogramBin(bBins[static_cast<size_t>(i)], bMax);
    }
    m_hasHistogram = true;
    update();
}

void GradingHistogramWidget::setSelectedChannel(Channel channel)
{
    if (m_selectedChannel == channel) {
        return;
    }
    m_selectedChannel = channel;
    update();
}

void GradingHistogramWidget::setCurveValues(double shadows, double midtones, double highlights)
{
    m_shadows = shadows;
    m_midtones = midtones;
    m_highlights = highlights;
    update();
}

QRectF GradingHistogramWidget::chartRect() const
{
    constexpr qreal kLeft = 10.0;
    constexpr qreal kRight = 10.0;
    constexpr qreal kTop = 10.0;
    constexpr qreal kBottom = 14.0;
    return QRectF(kLeft,
                  kTop,
                  qMax<qreal>(1.0, width() - (kLeft + kRight)),
                  qMax<qreal>(1.0, height() - (kTop + kBottom)));
}

qreal GradingHistogramWidget::normalizedValueToY(double value) const
{
    const QRectF rect = chartRect();
    const double clamped = qBound(0.0, value, 1.0);
    return rect.bottom() - (rect.height() * static_cast<qreal>(clamped));
}

double GradingHistogramWidget::yToNormalizedValue(qreal y) const
{
    const QRectF rect = chartRect();
    if (rect.height() <= 0.0) {
        return 0.5;
    }
    const qreal clampedY = qBound(rect.top(), y, rect.bottom());
    return qBound(0.0, static_cast<double>((rect.bottom() - clampedY) / rect.height()), 1.0);
}

double GradingHistogramWidget::parameterToNormalizedValue(int index, double parameter) const
{
    const double base = (index == 0) ? 0.0 : ((index == 1) ? 0.5 : 1.0);
    return qBound(0.0, base + (std::tanh(parameter * kCurveScale) * kCurveStrength), 1.0);
}

double GradingHistogramWidget::normalizedValueToParameter(int index, double normalizedValue) const
{
    const double base = (index == 0) ? 0.0 : ((index == 1) ? 0.5 : 1.0);
    const double centered = qBound(-0.98, (normalizedValue - base) / kCurveStrength, 0.98);
    return qBound(-2.0, std::atanh(centered) / kCurveScale, 2.0);
}

QPointF GradingHistogramWidget::handlePoint(int index) const
{
    const QRectF rect = chartRect();
    const qreal x = (index == 0)
                        ? rect.left()
                        : ((index == 1) ? rect.center().x() : rect.right());
    const double value = (index == 0)
                             ? parameterToNormalizedValue(0, m_shadows)
                             : ((index == 1)
                                    ? parameterToNormalizedValue(1, m_midtones)
                                    : parameterToNormalizedValue(2, m_highlights));
    return QPointF(x, normalizedValueToY(value));
}

int GradingHistogramWidget::nearestHandleIndex(const QPointF& pos) const
{
    int nearest = -1;
    qreal bestDistance = kHandleHitRadius * kHandleHitRadius;
    for (int i = 0; i < 3; ++i) {
        const QPointF p = handlePoint(i);
        const qreal dx = p.x() - pos.x();
        const qreal dy = p.y() - pos.y();
        const qreal dist2 = (dx * dx) + (dy * dy);
        if (dist2 <= bestDistance) {
            bestDistance = dist2;
            nearest = i;
        }
    }
    return nearest;
}

void GradingHistogramWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF rect = chartRect();
    painter.fillRect(rect.adjusted(-1, -1, 1, 1), QColor(16, 22, 30, 255));
    painter.setPen(QPen(QColor(52, 66, 82, 220), 1.0));
    painter.drawRect(rect);

    painter.setPen(QPen(QColor(40, 52, 66, 180), 1.0, Qt::DashLine));
    painter.drawLine(QPointF(rect.left(), rect.center().y()), QPointF(rect.right(), rect.center().y()));

    if (m_hasHistogram) {
        const auto drawHist = [&painter, &rect](const std::array<float, kBins>& hist, const QColor& color) {
            QPainterPath path = histogramPath(rect, hist);
            painter.fillPath(path, QColor(color.red(), color.green(), color.blue(), 54));
            painter.setPen(QPen(color, 1.15));
            painter.drawPath(path);
        };

        drawHist(m_histogramR, QColor(255, 92, 92, m_selectedChannel == Channel::Red ? 255 : 145));
        drawHist(m_histogramG, QColor(97, 222, 127, m_selectedChannel == Channel::Green ? 255 : 145));
        drawHist(m_histogramB, QColor(108, 156, 255, m_selectedChannel == Channel::Blue ? 255 : 145));
    }

    const QColor curveColor =
        (m_selectedChannel == Channel::Red)
            ? QColor(255, 105, 105)
            : ((m_selectedChannel == Channel::Green) ? QColor(110, 236, 142) : QColor(120, 168, 255));

    const QPointF p0 = handlePoint(0);
    const QPointF p1 = handlePoint(1);
    const QPointF p2 = handlePoint(2);
    const QPointF m01((p0.x() + p1.x()) * 0.5, (p0.y() + p1.y()) * 0.5);
    const QPointF m12((p1.x() + p2.x()) * 0.5, (p1.y() + p2.y()) * 0.5);

    QPainterPath curvePath;
    curvePath.moveTo(p0);
    curvePath.quadTo(m01, p1);
    curvePath.quadTo(m12, p2);

    painter.setPen(QPen(QColor(245, 250, 255, 48), 3.5));
    painter.drawPath(curvePath);
    painter.setPen(QPen(curveColor, 2.0));
    painter.drawPath(curvePath);

    for (int i = 0; i < 3; ++i) {
        const QPointF hp = handlePoint(i);
        const bool active = (m_activeHandle == i && m_dragging);
        painter.setBrush(active ? curveColor.lighter(125) : curveColor);
        painter.setPen(QPen(QColor(240, 246, 252, 200), 1.2));
        painter.drawEllipse(hp, kHandleRadius + (active ? 1.2 : 0.0), kHandleRadius + (active ? 1.2 : 0.0));
    }
}

void GradingHistogramWidget::updateParameterFromDrag(int handleIndex, const QPointF& pos, bool finalized)
{
    const double normalized = yToNormalizedValue(pos.y());
    const double parameter = normalizedValueToParameter(handleIndex, normalized);
    if (handleIndex == 0) {
        m_shadows = parameter;
    } else if (handleIndex == 1) {
        m_midtones = parameter;
    } else if (handleIndex == 2) {
        m_highlights = parameter;
    }
    emit curveAdjusted(m_shadows, m_midtones, m_highlights, finalized);
    update();
}

void GradingHistogramWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_activeHandle = nearestHandleIndex(event->position());
    if (m_activeHandle >= 0) {
        m_dragging = true;
        updateParameterFromDrag(m_activeHandle, event->position(), false);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void GradingHistogramWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging && m_activeHandle >= 0) {
        updateParameterFromDrag(m_activeHandle, event->position(), false);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void GradingHistogramWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragging && event->button() == Qt::LeftButton && m_activeHandle >= 0) {
        updateParameterFromDrag(m_activeHandle, event->position(), true);
        m_dragging = false;
        m_activeHandle = -1;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void GradingHistogramWidget::leaveEvent(QEvent* event)
{
    Q_UNUSED(event);
    if (!m_dragging) {
        m_activeHandle = -1;
        update();
    }
}

