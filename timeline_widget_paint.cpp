#include "timeline_widget.h"
#include "timeline_renderer.h"

void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    m_renderer->paint(&painter);
}
