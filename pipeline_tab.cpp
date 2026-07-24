#include "pipeline_tab.h"

#include <QIcon>
#include <QEvent>
#include <QFont>
#include <QGuiApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollBar>
#include <QVBoxLayout>

namespace {

QColor colorForKind(const QString& kind)
{
    if (kind == QStringLiteral("decoder")) return QColor(73, 166, 255);
    if (kind == QStringLiteral("shader")) return QColor(111, 211, 125);
    if (kind == QStringLiteral("composite")) return QColor(245, 181, 78);
    if (kind == QStringLiteral("surface")) return QColor(190, 137, 255);
    if (kind == QStringLiteral("mask")) return QColor(255, 104, 104);
    if (kind == QStringLiteral("effects")) return QColor(94, 221, 208);
    if (kind == QStringLiteral("transform")) return QColor(255, 214, 102);
    if (kind == QStringLiteral("selection")) return QColor(145, 170, 201);
    if (kind == QStringLiteral("mapping")) return QColor(132, 204, 22);
    return QColor(118, 142, 170);
}

QString initialsForLabel(const QString& label)
{
    const QStringList parts = label.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QString initials;
    for (const QString& part : parts) {
        if (!part.isEmpty() && part.at(0).isLetterOrNumber()) {
            initials.append(part.at(0).toUpper());
        }
        if (initials.size() >= 2) {
            break;
        }
    }
    return initials.isEmpty() ? QStringLiteral("P") : initials;
}

QPixmap fallbackPixmap(const PreviewSurface::PipelineStageSnapshot& snapshot)
{
    QPixmap pix(96, 54);
    pix.fill(QColor(9, 15, 22));
    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor accent = colorForKind(snapshot.kind);
    painter.fillRect(pix.rect().adjusted(1, 1, -1, -1), QColor(13, 24, 34));
    painter.setPen(QPen(snapshot.active ? accent : QColor(65, 78, 93), 2));
    painter.drawRect(pix.rect().adjusted(1, 1, -2, -2));

    painter.setPen(accent);
    QFont font = painter.font();
    font.setBold(true);
    font.setPointSize(13);
    painter.setFont(font);
    painter.drawText(pix.rect().adjusted(0, 6, 0, -18),
                     Qt::AlignCenter,
                     initialsForLabel(snapshot.label));

    font.setBold(false);
    font.setPointSize(7);
    painter.setFont(font);
    painter.setPen(QColor(185, 203, 224));
    painter.drawText(pix.rect().adjusted(4, 36, -4, -3),
                     Qt::AlignCenter,
                     snapshot.kind.isEmpty() ? QStringLiteral("stage") : snapshot.kind);
    return pix;
}

QImage cropUniformBorder(const QImage& source)
{
    if (source.isNull() || source.width() < 8 || source.height() < 8) {
        return source;
    }
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    const QRgb bg = image.constScanLine(0)
                       ? reinterpret_cast<const QRgb*>(image.constScanLine(0))[0]
                       : image.pixel(0, 0);
    const auto differsFromBackground = [bg](QRgb px) {
        return qAbs(qRed(px) - qRed(bg)) > 18 ||
               qAbs(qGreen(px) - qGreen(bg)) > 18 ||
               qAbs(qBlue(px) - qBlue(bg)) > 18 ||
               qAbs(qAlpha(px) - qAlpha(bg)) > 18;
    };

    int left = image.width();
    int top = image.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (!differsFromBackground(row[x])) {
                continue;
            }
            left = qMin(left, x);
            top = qMin(top, y);
            right = qMax(right, x);
            bottom = qMax(bottom, y);
        }
    }

    if (right < left || bottom < top) {
        return source;
    }
    const QRect bounds = QRect(QPoint(left, top), QPoint(right, bottom))
                             .adjusted(-8, -8, 8, 8)
                             .intersected(image.rect());
    const qreal areaRatio =
        static_cast<qreal>(bounds.width() * bounds.height()) /
        static_cast<qreal>(image.width() * image.height());
    if (areaRatio > 0.92 || bounds.width() < 16 || bounds.height() < 16) {
        return source;
    }
    return image.copy(bounds);
}

QImage displayImageForSnapshot(const PreviewSurface::PipelineStageSnapshot& snapshot)
{
    if (snapshot.image.isNull()) {
        return QImage();
    }
    if (snapshot.kind == QStringLiteral("surface") ||
        snapshot.kind == QStringLiteral("shader") ||
        snapshot.kind == QStringLiteral("composite")) {
        return cropUniformBorder(snapshot.image);
    }
    return snapshot.image;
}

QColor backgroundForState(const QString& state)
{
    if (state == QStringLiteral("ready") || state == QStringLiteral("live exact")) {
        return QColor(24, 34, 44);
    }
    if (state == QStringLiteral("approximate") || state == QStringLiteral("live approximate")) {
        return QColor(34, 30, 20);
    }
    if (state == QStringLiteral("blocked") || state == QStringLiteral("error") ||
        state == QStringLiteral("fallback")) {
        return QColor(48, 20, 20);
    }
    return QColor(16, 22, 30);
}

class PipelineStageVisualizationWidget final : public QWidget
{
public:
    using QWidget::QWidget;

    void setSnapshots(const QVector<PreviewSurface::PipelineStageSnapshot>& snapshots)
    {
        m_snapshots = snapshots;
        update();
    }

    void setHighlightedIndex(int index)
    {
        const int normalized = (index >= 0 && index < m_snapshots.size()) ? index : -1;
        if (m_highlightedIndex == normalized) {
            return;
        }
        m_highlightedIndex = normalized;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(5, 8, 12));

        const int count = m_snapshots.size();
        if (count <= 0) {
            painter.fillRect(rect().adjusted(12, 12, -12, -12), QColor(16, 22, 30));
            return;
        }

        const int outerMargin = 12;
        const int spacing = 8;
        const int availableWidth = std::max(1, width() - outerMargin * 2);
        const int cardWidth = std::max(44, (availableWidth - spacing * std::max(0, count - 1)) / count);
        const int baseTop = 28;
        const int cardHeight = std::max(88, height() - baseTop - 42);
        for (int i = 0; i < count; ++i) {
            const PreviewSurface::PipelineStageSnapshot& snapshot = m_snapshots.at(i);
            const QColor accent = colorForKind(snapshot.kind);
            const QColor fill = backgroundForState(snapshot.state);
            const int x = outerMargin + i * (cardWidth + spacing);
            const bool active = i == m_highlightedIndex ||
                                (m_highlightedIndex < 0 && snapshot.active);
            const QRect bodyRect(x, active ? 18 : baseTop, cardWidth, active ? cardHeight + 10 : cardHeight);
            painter.fillRect(bodyRect, fill);
            painter.fillRect(QRect(bodyRect.x(), bodyRect.y(), bodyRect.width(), 4), accent);
            QColor inner = active ? accent.lighter(135) : accent.darker(150);
            inner.setAlphaF(active ? 0.22 : 0.12);
            painter.fillRect(
                QRect(bodyRect.x() + 4,
                      bodyRect.y() + 10,
                      std::max(12, bodyRect.width() - 8),
                      bodyRect.height() - 18),
                inner);
            if (snapshot.exact) {
                painter.fillRect(
                    QRect(bodyRect.x() + 6, bodyRect.bottom() - 10, bodyRect.width() - 12, 4),
                    QColor(111, 211, 125));
            }
        }
    }

private:
    QVector<PreviewSurface::PipelineStageSnapshot> m_snapshots;
    int m_highlightedIndex = -1;
};

} // namespace

PipelineTab::PipelineTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
    if (m_widgets.pipelinePreviewHost) {
        m_visualizationWidget =
            new PipelineStageVisualizationWidget(m_widgets.pipelinePreviewHost);
        auto* layout = new QVBoxLayout(m_widgets.pipelinePreviewHost);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(m_visualizationWidget);
    }
    if (m_widgets.pipelineStageList && m_widgets.pipelineStageList->viewport()) {
        m_widgets.pipelineStageList->setMouseTracking(true);
        m_widgets.pipelineStageList->viewport()->setMouseTracking(true);
        m_widgets.pipelineStageList->viewport()->installEventFilter(this);
    }
    m_liveRefreshTimer.setInterval(125);
    m_liveRefreshTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_liveRefreshTimer, &QTimer::timeout, this, &PipelineTab::refreshIfVisible);
    m_liveRefreshTimer.start();
}

void PipelineTab::refresh()
{
    if (!m_widgets.pipelineStageList) {
        return;
    }

    const int scrollValue = m_widgets.pipelineStageList->verticalScrollBar()
                                ? m_widgets.pipelineStageList->verticalScrollBar()->value()
                                : 0;
    const bool useVisualization = m_visualizationWidget;
    const int hoverRow = useVisualization
        ? m_hoverRow
        : (m_hoverPreview && m_hoverPreview->isVisible() ? m_hoverRow : -1);

    m_widgets.pipelineStageList->clear();
    m_snapshots =
        m_deps.liveSnapshots ? m_deps.liveSnapshots() : QVector<PreviewSurface::PipelineStageSnapshot>{};
    if (auto* visualization =
            static_cast<PipelineStageVisualizationWidget*>(m_visualizationWidget)) {
        visualization->setSnapshots(m_snapshots);
        visualization->setHighlightedIndex(hoverRow);
    }

    if (m_snapshots.isEmpty()) {
        auto* item = new QListWidgetItem(QStringLiteral("Live Pipeline\nNo preview pipeline state available"));
        item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_widgets.pipelineStageList->addItem(item);
        return;
    }

    for (const PreviewSurface::PipelineStageSnapshot& snapshot : m_snapshots) {
        auto* item = new QListWidgetItem;
        const QString state = snapshot.active
                                  ? (!snapshot.state.isEmpty()
                                         ? snapshot.state
                                         : (snapshot.exact ? QStringLiteral("live exact")
                                                           : QStringLiteral("live approximate")))
                                  : (!snapshot.state.isEmpty() ? snapshot.state : QStringLiteral("waiting"));
        item->setText(QStringLiteral("%1\n%2 | %3").arg(snapshot.label, snapshot.detail, state));
        item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        item->setToolTip(QString());
        QFont font = item->font();
        font.setPointSize(qMax(8, font.pointSize() - 1));
        item->setFont(font);
        const QImage displayImage = useVisualization ? QImage() : displayImageForSnapshot(snapshot);
        if (!displayImage.isNull()) {
            const QPixmap pix = QPixmap::fromImage(displayImage);
            if (!pix.isNull()) {
                item->setIcon(QIcon(pix.scaled(96, 54, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            }
        } else {
            item->setIcon(QIcon(fallbackPixmap(snapshot)));
        }
        m_widgets.pipelineStageList->addItem(item);
    }

    if (m_widgets.pipelineStageList->verticalScrollBar()) {
        m_widgets.pipelineStageList->verticalScrollBar()->setValue(scrollValue);
    }
    if (hoverRow >= 0 && hoverRow < m_snapshots.size()) {
        m_hoverRow = -1;
        showHoverPreview(hoverRow);
    }
}

bool PipelineTab::eventFilter(QObject* watched, QEvent* event)
{
    if (!m_widgets.pipelineStageList || watched != m_widgets.pipelineStageList->viewport()) {
        return QObject::eventFilter(watched, event);
    }

    if (event->type() == QEvent::ToolTip) {
        return true;
    }

    if (event->type() == QEvent::MouseMove) {
        const auto* mouseEvent = static_cast<QMouseEvent*>(event);
        const QListWidgetItem* item = m_widgets.pipelineStageList->itemAt(mouseEvent->pos());
        if (!item) {
            hideHoverPreview();
            return QObject::eventFilter(watched, event);
        }
        showHoverPreview(m_widgets.pipelineStageList->row(item));
    } else if (event->type() == QEvent::Leave) {
        hideHoverPreview();
    }

    return QObject::eventFilter(watched, event);
}

void PipelineTab::showHoverPreview(int row)
{
    if (row < 0 || row >= m_snapshots.size()) {
        hideHoverPreview();
        return;
    }
    if (auto* visualization =
            static_cast<PipelineStageVisualizationWidget*>(m_visualizationWidget)) {
        m_hoverRow = row;
        visualization->setHighlightedIndex(row);
        return;
    }
    if (m_hoverRow == row && m_hoverPreview && m_hoverPreview->isVisible()) {
        return;
    }
    m_hoverRow = row;

    const PreviewSurface::PipelineStageSnapshot& snapshot = m_snapshots.at(row);
    const QImage displayImage = displayImageForSnapshot(snapshot);
    QPixmap source = displayImage.isNull()
                         ? fallbackPixmap(snapshot)
                         : QPixmap::fromImage(displayImage);
    if (source.isNull()) {
        hideHoverPreview();
        return;
    }

    if (!m_hoverPreview) {
        m_hoverPreview = new QLabel(nullptr,
                                    Qt::Tool |
                                    Qt::FramelessWindowHint |
                                    Qt::WindowDoesNotAcceptFocus |
                                    Qt::WindowTransparentForInput);
        m_hoverPreview->setObjectName(QStringLiteral("pipelineHoverPreview"));
        m_hoverPreview->setAlignment(Qt::AlignCenter);
        m_hoverPreview->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_hoverPreview->setAttribute(Qt::WA_ShowWithoutActivating, true);
        m_hoverPreview->setStyleSheet(QStringLiteral(
            "QLabel#pipelineHoverPreview { "
            "background: #05080c; color: #edf2f7; border: 1px solid #24303c; "
            "border-radius: 10px; padding: 8px; }"));
    }

    QSize targetSize(720, 460);
    if (m_widgets.pipelineStageList && m_widgets.pipelineStageList->window()) {
        const QSize windowBound = m_widgets.pipelineStageList->window()->size() - QSize(360, 260);
        targetSize.setWidth(qBound(360, windowBound.width(), 720));
        targetSize.setHeight(qBound(220, windowBound.height(), 460));
    }

    QPixmap scaled = source.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (!snapshot.label.isEmpty()) {
        QPainter painter(&scaled);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QFont font = painter.font();
        font.setBold(true);
        font.setPointSize(10);
        painter.setFont(font);
        const QFontMetrics fm(font);
        const QString label = snapshot.label;
        const int padding = 8;
        const int textWidth = qMin(scaled.width() - 16, fm.horizontalAdvance(label));
        const QRect badgeRect(8, 8, textWidth + padding * 2, fm.height() + padding);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(9, 12, 18, 210));
        painter.drawRoundedRect(badgeRect, 8, 8);
        painter.setPen(QColor(QStringLiteral("#edf2f7")));
        painter.drawText(badgeRect.adjusted(padding, 0, -padding, 0),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         fm.elidedText(label, Qt::ElideRight, textWidth));
    }

    m_hoverPreview->setPixmap(scaled);
    m_hoverPreview->resize(scaled.size() + QSize(16, 16));

    QPoint anchor = QCursor::pos() + QPoint(24, 24);
    if (QScreen* screen = QGuiApplication::screenAt(QCursor::pos())) {
        const QRect available = screen->availableGeometry().adjusted(12, 12, -12, -12);
        if (m_widgets.pipelineStageList) {
            const QRect listRect(
                m_widgets.pipelineStageList->mapToGlobal(QPoint(0, 0)),
                m_widgets.pipelineStageList->size());
            const int leftX = listRect.left() - m_hoverPreview->width() - 16;
            const int rightX = listRect.right() + 16;
            if (leftX >= available.left()) {
                anchor.setX(leftX);
            } else if (rightX + m_hoverPreview->width() <= available.right()) {
                anchor.setX(rightX);
            }
            anchor.setY(qBound(available.top(),
                               listRect.top(),
                               available.bottom() - m_hoverPreview->height()));
        }
        if (anchor.x() + m_hoverPreview->width() > available.right()) {
            anchor.setX(available.right() - m_hoverPreview->width());
        }
        if (anchor.y() + m_hoverPreview->height() > available.bottom()) {
            anchor.setY(available.bottom() - m_hoverPreview->height());
        }
        anchor.setX(qMax(available.left(), anchor.x()));
        anchor.setY(qMax(available.top(), anchor.y()));
    }

    m_hoverPreview->move(anchor);
    m_hoverPreview->show();
    m_hoverPreview->raise();
}

void PipelineTab::hideHoverPreview()
{
    m_hoverRow = -1;
    if (auto* visualization =
            static_cast<PipelineStageVisualizationWidget*>(m_visualizationWidget)) {
        visualization->setHighlightedIndex(-1);
    }
    if (m_hoverPreview) {
        m_hoverPreview->hide();
    }
}

void PipelineTab::refreshIfVisible()
{
    const bool listVisible = m_widgets.pipelineStageList &&
        m_widgets.pipelineStageList->isVisible() &&
        m_widgets.pipelineStageList->isVisibleTo(m_widgets.pipelineStageList->window());
    const bool previewVisible = !m_widgets.pipelinePreviewHost ||
        (m_widgets.pipelinePreviewHost->isVisible() &&
         m_widgets.pipelinePreviewHost->isVisibleTo(m_widgets.pipelinePreviewHost->window()));
    if (!listVisible || !previewVisible) {
        hideHoverPreview();
        return;
    }
    refresh();
}
