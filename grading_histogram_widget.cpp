#include "grading_histogram_widget.h"
#include "editor_shared.h"

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
constexpr qreal kHandleRadius = 4.5;
constexpr qreal kHandleHitRadius = 10.0;

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

qreal catmullRom(qreal p0, qreal p1, qreal p2, qreal p3, qreal t) {
    const qreal t2 = t * t;
    const qreal t3 = t2 * t;
    return 0.5 * ((2.0 * p1) +
                  (-p0 + p2) * t +
                  (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                  (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
}

} // namespace

GradingHistogramWidget::GradingHistogramWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(170);
    setMouseTracking(true);
    m_points = defaultGradingCurvePoints();
}

void GradingHistogramWidget::clearHistogram()
{
    m_hasHistogram = false;
    m_hasAlphaHistogram = false;
    m_histogramR.fill(0.0f);
    m_histogramG.fill(0.0f);
    m_histogramB.fill(0.0f);
    m_histogramA.fill(0.0f);
    m_histogramLuma.fill(0.0f);
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
    std::array<uint32_t, kBins> aBins{};
    std::array<uint32_t, kBins> lumaBins{};
    m_hasAlphaHistogram = image.hasAlphaChannel();

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
            const uchar a = row[idx + 3];
            ++rBins[static_cast<size_t>(r)];
            ++gBins[static_cast<size_t>(g)];
            ++bBins[static_cast<size_t>(b)];
            ++aBins[static_cast<size_t>(a)];
            const int luma = qBound(0, qRound((0.2126 * r) + (0.7152 * g) + (0.0722 * b)), 255);
            ++lumaBins[static_cast<size_t>(luma)];
        }
    }

    const uint32_t rMax = *std::max_element(rBins.begin(), rBins.end());
    const uint32_t gMax = *std::max_element(gBins.begin(), gBins.end());
    const uint32_t bMax = *std::max_element(bBins.begin(), bBins.end());
    const uint32_t aMax = *std::max_element(aBins.begin(), aBins.end());
    const uint32_t lumaMax = *std::max_element(lumaBins.begin(), lumaBins.end());

    for (int i = 0; i < kBins; ++i) {
        m_histogramR[static_cast<size_t>(i)] =
            normalizeHistogramBin(rBins[static_cast<size_t>(i)], rMax);
        m_histogramG[static_cast<size_t>(i)] =
            normalizeHistogramBin(gBins[static_cast<size_t>(i)], gMax);
        m_histogramB[static_cast<size_t>(i)] =
            normalizeHistogramBin(bBins[static_cast<size_t>(i)], bMax);
        m_histogramA[static_cast<size_t>(i)] =
            normalizeHistogramBin(aBins[static_cast<size_t>(i)], aMax);
        m_histogramLuma[static_cast<size_t>(i)] =
            normalizeHistogramBin(lumaBins[static_cast<size_t>(i)], lumaMax);
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

void GradingHistogramWidget::setCurvePoints(const QVector<QPointF>& points)
{
    if (m_threePointLockEnabled) {
        m_points = lockToThreePointCurve(points);
    } else {
        m_points = sanitizeGradingCurvePoints(points);
    }
    update();
}

QVector<QPointF> GradingHistogramWidget::curvePoints() const
{
    if (m_threePointLockEnabled) {
        return lockToThreePointCurve(m_points);
    }
    return sanitizeGradingCurvePoints(m_points);
}

void GradingHistogramWidget::setThreePointLockEnabled(bool enabled)
{
    if (m_threePointLockEnabled == enabled) {
        return;
    }
    m_threePointLockEnabled = enabled;
    m_points = m_threePointLockEnabled ? lockToThreePointCurve(m_points)
                                       : sanitizeGradingCurvePoints(m_points);
    m_activePoint = -1;
    m_dragging = false;
    update();
}

void GradingHistogramWidget::setCurveSmoothingEnabled(bool enabled)
{
    if (m_curveSmoothingEnabled == enabled) {
        return;
    }
    m_curveSmoothingEnabled = enabled;
    if (m_threePointLockEnabled) {
        m_points = lockToThreePointCurve(m_points);
    }
    update();
}

void GradingHistogramWidget::setChartBackgroundColor(const QColor& color)
{
    const QColor normalized = color.isValid() ? color : QColor(16, 22, 30, 255);
    if (m_chartBackgroundColor == normalized) {
        return;
    }
    m_chartBackgroundColor = normalized;
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

QPointF GradingHistogramWidget::pointToWidget(const QPointF& point) const
{
    const QRectF rect = chartRect();
    const qreal x = rect.left() + (qBound<qreal>(0.0, point.x(), 1.0) * rect.width());
    // Display curve in delta space: zero adjustment (y == x) is centered, not diagonal.
    const qreal delta = qBound<qreal>(-1.0, point.y() - point.x(), 1.0);
    const qreal displayNorm = qBound<qreal>(0.0, (delta * 0.5) + 0.5, 1.0);
    const qreal y = rect.bottom() - (displayNorm * rect.height());
    return QPointF(x, y);
}

QPointF GradingHistogramWidget::widgetToPoint(const QPointF& pos) const
{
    const QRectF rect = chartRect();
    const qreal xNorm = rect.width() <= 0.0
                            ? 0.0
                            : qBound<qreal>(0.0, (pos.x() - rect.left()) / rect.width(), 1.0);
    const qreal displayNorm = rect.height() <= 0.0
                                  ? 0.5
                                  : qBound<qreal>(0.0, (rect.bottom() - pos.y()) / rect.height(), 1.0);
    const qreal delta = (displayNorm - 0.5) * 2.0;
    const qreal yNorm = qBound<qreal>(0.0, xNorm + delta, 1.0);
    return QPointF(xNorm, yNorm);
}

void GradingHistogramWidget::sortAndClampPoints()
{
    if (m_threePointLockEnabled) {
        m_points = lockToThreePointCurve(m_points);
    } else {
        m_points = sanitizeGradingCurvePoints(m_points);
    }
}

QVector<QPointF> GradingHistogramWidget::lockToThreePointCurve(const QVector<QPointF>& points) const
{
    const QVector<QPointF> sanitized = sanitizeGradingCurvePoints(points);
    const qreal midY = sampleGradingCurveAt(sanitized, 0.5, m_curveSmoothingEnabled);
    return {QPointF(0.0, 0.0), QPointF(0.5, qBound<qreal>(0.0, midY, 1.0)), QPointF(1.0, 1.0)};
}

const std::array<float, 256>& GradingHistogramWidget::selectedHistogram() const
{
    if (m_selectedChannel == Channel::Red) {
        return m_histogramR;
    }
    if (m_selectedChannel == Channel::Green) {
        return m_histogramG;
    }
    if (m_selectedChannel == Channel::Blue) {
        return m_histogramB;
    }
    if (m_selectedChannel == Channel::Alpha) {
        return m_histogramA;
    }
    return m_histogramLuma;
}

int GradingHistogramWidget::nearestPointIndex(const QPointF& pos) const
{
    const QVector<QPointF> points = sanitizeGradingCurvePoints(m_points);
    int nearest = -1;
    qreal bestDistance = kHandleHitRadius * kHandleHitRadius;
    for (int i = 0; i < points.size(); ++i) {
        const QPointF widgetPoint = pointToWidget(points.at(i));
        const qreal dx = widgetPoint.x() - pos.x();
        const qreal dy = widgetPoint.y() - pos.y();
        const qreal dist2 = (dx * dx) + (dy * dy);
        if (dist2 <= bestDistance) {
            bestDistance = dist2;
            nearest = i;
        }
    }
    return nearest;
}

int GradingHistogramWidget::segmentIndexAtX(qreal xNorm) const
{
    const QVector<QPointF> points = sanitizeGradingCurvePoints(m_points);
    for (int i = 1; i < points.size(); ++i) {
        if (xNorm <= points.at(i).x()) {
            return i - 1;
        }
    }
    return qMax(0, points.size() - 2);
}

void GradingHistogramWidget::emitCurveChanged(bool finalized)
{
    if (m_threePointLockEnabled) {
        emit curvePointsAdjusted(lockToThreePointCurve(m_points), finalized);
    } else {
        emit curvePointsAdjusted(sanitizeGradingCurvePoints(m_points), finalized);
    }
}

void GradingHistogramWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF rect = chartRect();
    painter.fillRect(rect.adjusted(-1, -1, 1, 1), m_chartBackgroundColor);
    painter.setPen(QPen(QColor(52, 66, 82, 220), 1.0));
    painter.drawRect(rect);

    painter.setPen(QPen(QColor(40, 52, 66, 180), 1.0, Qt::DashLine));
    for (int i = 1; i < 4; ++i) {
        const qreal x = rect.left() + (rect.width() * i / 4.0);
        const qreal y = rect.top() + (rect.height() * i / 4.0);
        painter.drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
        painter.drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
    }
    // Flat reference bar: 0 adjustment (passthrough).
    const qreal midY = rect.center().y();
    painter.setPen(QPen(QColor(160, 170, 182, 155), 1.2));
    painter.drawLine(QPointF(rect.left(), midY), QPointF(rect.right(), midY));
    painter.setPen(QPen(QColor(188, 198, 210, 170), 1.0));
    painter.drawText(QRectF(rect.left() + 6.0, rect.top() + 2.0, 170.0, 14.0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("0 = passthrough"));

    if (m_hasHistogram) {
        QColor histogramFillColor(130, 170, 255, 56);
        QColor histogramStrokeColor(150, 200, 255, 122);
        if (m_selectedChannel == Channel::Red) {
            histogramFillColor = QColor(255, 110, 110, 60);
            histogramStrokeColor = QColor(255, 150, 150, 126);
        } else if (m_selectedChannel == Channel::Green) {
            histogramFillColor = QColor(120, 238, 150, 60);
            histogramStrokeColor = QColor(165, 248, 188, 126);
        } else if (m_selectedChannel == Channel::Blue) {
            histogramFillColor = QColor(120, 168, 255, 60);
            histogramStrokeColor = QColor(160, 196, 255, 126);
        } else if (m_selectedChannel == Channel::Alpha) {
            histogramFillColor = QColor(196, 196, 196, 60);
            histogramStrokeColor = QColor(224, 224, 224, 126);
        } else if (m_selectedChannel == Channel::Brightness) {
            histogramFillColor = QColor(255, 210, 115, 58);
            histogramStrokeColor = QColor(255, 224, 155, 122);
        }
        const QPainterPath path = histogramPath(rect, selectedHistogram());
        painter.fillPath(path, histogramFillColor);
        painter.setPen(QPen(histogramStrokeColor, 1.0));
        painter.drawPath(path);
    }

    QColor curveColor(120, 168, 255);
    if (m_selectedChannel == Channel::Red) {
        curveColor = QColor(255, 105, 105);
    } else if (m_selectedChannel == Channel::Green) {
        curveColor = QColor(110, 236, 142);
    } else if (m_selectedChannel == Channel::Alpha) {
        curveColor = QColor(212, 212, 212);
    } else if (m_selectedChannel == Channel::Brightness) {
        curveColor = QColor(255, 210, 115);
    }

    const QVector<QPointF> points = m_threePointLockEnabled ? lockToThreePointCurve(m_points)
                                                             : sanitizeGradingCurvePoints(m_points);

    QPainterPath curvePath;
    if (!points.isEmpty()) {
        curvePath.moveTo(pointToWidget(points.constFirst()));
        if (!m_curveSmoothingEnabled) {
            for (int i = 1; i < points.size(); ++i) {
                curvePath.lineTo(pointToWidget(points.at(i)));
            }
        } else {
            const int stepsPerSegment = 20;
            for (int i = 0; i < points.size() - 1; ++i) {
                const QPointF p0 = points.at(qMax(0, i - 1));
                const QPointF p1 = points.at(i);
                const QPointF p2 = points.at(i + 1);
                const QPointF p3 = points.at(qMin(points.size() - 1, i + 2));
                for (int s = 1; s <= stepsPerSegment; ++s) {
                    const qreal t = static_cast<qreal>(s) / static_cast<qreal>(stepsPerSegment);
                    const qreal x = catmullRom(p0.x(), p1.x(), p2.x(), p3.x(), t);
                    const qreal y = catmullRom(p0.y(), p1.y(), p2.y(), p3.y(), t);
                    curvePath.lineTo(pointToWidget(QPointF(qBound<qreal>(0.0, x, 1.0),
                                                           qBound<qreal>(0.0, y, 1.0))));
                }
            }
        }
    }

    painter.setPen(QPen(QColor(245, 250, 255, 48), 3.5));
    painter.drawPath(curvePath);
    painter.setPen(QPen(curveColor, 2.0));
    painter.drawPath(curvePath);

    for (int i = 0; i < points.size(); ++i) {
        const QPointF hp = pointToWidget(points.at(i));
        const bool active = (m_activePoint == i && m_dragging);
        painter.setBrush(active ? curveColor.lighter(125) : curveColor);
        painter.setPen(QPen(QColor(240, 246, 252, 200), 1.2));
        painter.drawEllipse(hp, kHandleRadius + (active ? 1.2 : 0.0), kHandleRadius + (active ? 1.2 : 0.0));
    }
}

void GradingHistogramWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::RightButton) {
        if (m_threePointLockEnabled) {
            event->accept();
            return;
        }
        const int hit = nearestPointIndex(event->position());
        if (hit > 0 && hit < m_points.size() - 1) {
            m_points.removeAt(hit);
            sortAndClampPoints();
            emitCurveChanged(true);
            update();
            event->accept();
            return;
        }
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_activePoint = nearestPointIndex(event->position());
    if (m_activePoint >= 0) {
        if (m_threePointLockEnabled && m_activePoint != 1) {
            m_activePoint = -1;
            event->accept();
            return;
        }
        m_dragging = true;
        event->accept();
        return;
    }
    if (m_threePointLockEnabled) {
        event->accept();
        return;
    }
    const QPointF pointNorm = widgetToPoint(event->position());
    const int insertAfter = segmentIndexAtX(pointNorm.x());
    m_points.insert(insertAfter + 1, pointNorm);
    sortAndClampPoints();
    m_activePoint = nearestPointIndex(event->position());
    m_dragging = (m_activePoint >= 0);
    emitCurveChanged(false);
    update();
    event->accept();
}

void GradingHistogramWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging && m_activePoint >= 0 && m_activePoint < m_points.size()) {
        QPointF pointNorm = widgetToPoint(event->position());
        if (m_threePointLockEnabled) {
            pointNorm.setX(0.5);
            pointNorm.setY(qBound<qreal>(0.0, pointNorm.y(), 1.0));
        } else if (m_activePoint == 0) {
            pointNorm.setX(0.0);
            pointNorm.setY(0.0);
        } else if (m_activePoint == m_points.size() - 1) {
            pointNorm.setX(1.0);
            pointNorm.setY(1.0);
        } else {
            const qreal prevX = m_points.at(m_activePoint - 1).x() + 0.001;
            const qreal nextX = m_points.at(m_activePoint + 1).x() - 0.001;
            pointNorm.setX(qBound(prevX, pointNorm.x(), nextX));
            pointNorm.setY(qBound<qreal>(0.0, pointNorm.y(), 1.0));
        }
        m_points[m_activePoint] = pointNorm;
        sortAndClampPoints();
        emitCurveChanged(false);
        update();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void GradingHistogramWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragging && event->button() == Qt::LeftButton && m_activePoint >= 0) {
        emitCurveChanged(true);
        m_dragging = false;
        m_activePoint = -1;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void GradingHistogramWidget::leaveEvent(QEvent* event)
{
    Q_UNUSED(event);
    if (!m_dragging) {
        m_activePoint = -1;
        update();
    }
}
