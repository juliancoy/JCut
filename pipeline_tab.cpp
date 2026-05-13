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
#include <QVulkanFunctions>
#include <QVulkanInstance>
#include <QVulkanWindow>

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

VkClearValue clearColorFor(const QColor& color, float alpha = 1.0f)
{
    VkClearValue clear{};
    clear.color.float32[0] = static_cast<float>(color.redF());
    clear.color.float32[1] = static_cast<float>(color.greenF());
    clear.color.float32[2] = static_cast<float>(color.blueF());
    clear.color.float32[3] = alpha;
    return clear;
}

class PipelineStageVulkanWindow;

class PipelineStageVulkanRenderer final : public QVulkanWindowRenderer
{
public:
    explicit PipelineStageVulkanRenderer(PipelineStageVulkanWindow* window)
        : m_window(window) {}

    void initResources() override;
    void startNextFrame() override;
    void logicalDeviceLost() override {}
    void physicalDeviceLost() override {}

private:
    void clearRect(VkCommandBuffer cb, const QRect& rect, const VkClearValue& color) const;

    PipelineStageVulkanWindow* m_window = nullptr;
    QVulkanWindow* m_qwindow = nullptr;
    QVulkanDeviceFunctions* m_devFuncs = nullptr;
};

class PipelineStageVulkanWindow final : public QVulkanWindow
{
public:
    QVulkanWindowRenderer* createRenderer() override
    {
        return new PipelineStageVulkanRenderer(this);
    }

    void setSnapshots(const QVector<PreviewSurface::PipelineStageSnapshot>& snapshots)
    {
        m_snapshots = snapshots;
        requestUpdate();
    }

    void setHighlightedIndex(int index)
    {
        const int normalized = (index >= 0 && index < m_snapshots.size()) ? index : -1;
        if (m_highlightedIndex == normalized) {
            return;
        }
        m_highlightedIndex = normalized;
        requestUpdate();
    }

    const QVector<PreviewSurface::PipelineStageSnapshot>& snapshots() const { return m_snapshots; }
    int highlightedIndex() const { return m_highlightedIndex; }

private:
    QVector<PreviewSurface::PipelineStageSnapshot> m_snapshots;
    int m_highlightedIndex = -1;
};

void PipelineStageVulkanRenderer::initResources()
{
    m_qwindow = m_window;
    m_devFuncs = m_qwindow && m_qwindow->vulkanInstance()
        ? m_qwindow->vulkanInstance()->deviceFunctions(m_qwindow->device())
        : nullptr;
}

void PipelineStageVulkanRenderer::clearRect(VkCommandBuffer cb, const QRect& rect, const VkClearValue& color) const
{
    if (!m_devFuncs || rect.isEmpty()) {
        return;
    }
    VkClearAttachment attachment{};
    attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    attachment.colorAttachment = 0;
    attachment.clearValue = color;

    VkClearRect clear{};
    clear.rect.offset = { rect.x(), rect.y() };
    clear.rect.extent = {
        static_cast<uint32_t>(std::max(1, rect.width())),
        static_cast<uint32_t>(std::max(1, rect.height()))
    };
    clear.baseArrayLayer = 0;
    clear.layerCount = 1;
    m_devFuncs->vkCmdClearAttachments(cb, 1, &attachment, 1, &clear);
}

void PipelineStageVulkanRenderer::startNextFrame()
{
    if (!m_qwindow || !m_devFuncs) {
        return;
    }
    const QSize size = m_qwindow->swapChainImageSize();
    VkClearValue clears[2]{};
    clears[0] = clearColorFor(QColor(5, 8, 12));
    clears[1].depthStencil.depth = 1.0f;
    clears[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_qwindow->defaultRenderPass();
    rp.framebuffer = m_qwindow->currentFramebuffer();
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {
        static_cast<uint32_t>(std::max(1, size.width())),
        static_cast<uint32_t>(std::max(1, size.height()))
    };
    rp.clearValueCount = m_qwindow->depthStencilFormat() == VK_FORMAT_UNDEFINED ? 1u : 2u;
    rp.pClearValues = clears;

    VkCommandBuffer cb = m_qwindow->currentCommandBuffer();
    m_devFuncs->vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);

    const QVector<PreviewSurface::PipelineStageSnapshot>& snapshots = m_window->snapshots();
    const int count = snapshots.size();
    if (count <= 0) {
        clearRect(cb, QRect(12, 12, std::max(24, size.width() - 24), std::max(24, size.height() - 24)),
                  clearColorFor(QColor(16, 22, 30)));
    } else {
        const int outerMargin = 12;
        const int spacing = 8;
        const int availableWidth = std::max(1, size.width() - outerMargin * 2);
        const int cardWidth = std::max(44, (availableWidth - spacing * std::max(0, count - 1)) / count);
        const int baseTop = 28;
        const int cardHeight = std::max(88, size.height() - baseTop - 42);
        const int highlighted = m_window->highlightedIndex();
        for (int i = 0; i < count; ++i) {
            const PreviewSurface::PipelineStageSnapshot& snapshot = snapshots.at(i);
            const QColor accent = colorForKind(snapshot.kind);
            const QColor fill = backgroundForState(snapshot.state);
            const int x = outerMargin + i * (cardWidth + spacing);
            const bool active = i == highlighted || (highlighted < 0 && snapshot.active);
            const QRect bodyRect(x, active ? 18 : baseTop, cardWidth, active ? cardHeight + 10 : cardHeight);
            clearRect(cb, bodyRect, clearColorFor(fill));
            clearRect(cb, QRect(bodyRect.x(), bodyRect.y(), bodyRect.width(), 4), clearColorFor(accent));
            clearRect(cb,
                      QRect(bodyRect.x() + 4, bodyRect.y() + 10, std::max(12, bodyRect.width() - 8), bodyRect.height() - 18),
                      clearColorFor(active ? accent.lighter(135) : accent.darker(150), active ? 0.22f : 0.12f));
            if (snapshot.exact) {
                clearRect(cb, QRect(bodyRect.x() + 6, bodyRect.bottom() - 10, bodyRect.width() - 12, 4),
                          clearColorFor(QColor(111, 211, 125)));
            }
        }
    }

    m_devFuncs->vkCmdEndRenderPass(cb);
    m_qwindow->frameReady();
}

} // namespace

PipelineTab::PipelineTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
    if (m_widgets.pipelinePreviewHost && m_deps.useVulkanVisualization && m_deps.useVulkanVisualization()) {
        m_vulkanInstance = std::make_unique<QVulkanInstance>();
        m_vulkanInstance->setApiVersion(QVersionNumber(1, 1));
        if (m_vulkanInstance->create()) {
            auto* window = new PipelineStageVulkanWindow;
            window->setVulkanInstance(m_vulkanInstance.get());
            m_vulkanWindow = window;
            m_vulkanContainer = QWidget::createWindowContainer(window, m_widgets.pipelinePreviewHost);
            if (m_vulkanContainer) {
                auto* layout = new QVBoxLayout(m_widgets.pipelinePreviewHost);
                layout->setContentsMargins(0, 0, 0, 0);
                layout->setSpacing(0);
                layout->addWidget(m_vulkanContainer);
            }
        } else {
            m_vulkanInstance.reset();
        }
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
    const bool useVulkanVisualization = m_vulkanWindow;
    const int hoverRow = useVulkanVisualization
        ? m_hoverRow
        : (m_hoverPreview && m_hoverPreview->isVisible() ? m_hoverRow : -1);

    m_widgets.pipelineStageList->clear();
    m_snapshots =
        m_deps.liveSnapshots ? m_deps.liveSnapshots() : QVector<PreviewSurface::PipelineStageSnapshot>{};
    if (auto* window = static_cast<PipelineStageVulkanWindow*>(m_vulkanWindow.data())) {
        window->setSnapshots(m_snapshots);
        window->setHighlightedIndex(hoverRow);
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
        const QImage displayImage = useVulkanVisualization ? QImage() : displayImageForSnapshot(snapshot);
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
    if (auto* window = static_cast<PipelineStageVulkanWindow*>(m_vulkanWindow.data())) {
        m_hoverRow = row;
        window->setHighlightedIndex(row);
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
    if (auto* window = static_cast<PipelineStageVulkanWindow*>(m_vulkanWindow.data())) {
        window->setHighlightedIndex(-1);
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
