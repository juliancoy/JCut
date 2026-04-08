#pragma once

#include <QPainter>

class TimelineWidget;

class TimelineRenderer {
public:
    explicit TimelineRenderer(TimelineWidget* widget);
    void paint(QPainter* painter);

private:
    TimelineWidget* m_widget = nullptr;
};
