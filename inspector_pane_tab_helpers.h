#pragma once

#include <QLabel>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace jcut::inspector {

inline QLabel* createTabHeading(const QString& text, QWidget* parent = nullptr)
{
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral(
        "QLabel { font-size: 13px; font-weight: 700; color: #8fa0b5; "
        "padding: 2px 0 6px 0; }"));
    return label;
}

inline QVBoxLayout* createTabLayout(QWidget* page)
{
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    return layout;
}

struct DisclosureSection {
    QWidget* container = nullptr;
    QVBoxLayout* body = nullptr;
};

inline DisclosureSection createDisclosureSection(QWidget* parent,
                                                  const QString& title,
                                                  bool expanded = true)
{
    DisclosureSection section;
    auto* container = new QWidget(parent);
    auto* outer = new QVBoxLayout(container);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(3);

    auto* toggle = new QToolButton(container);
    toggle->setCheckable(true);
    toggle->setChecked(expanded);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    toggle->setText(title);
    toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    toggle->setMinimumWidth(0);
    toggle->setMinimumHeight(28);
    toggle->setStyleSheet(QStringLiteral(
        "QToolButton {"
        "  color: #a9bdd2;"
        "  font-weight: 700;"
        "  border: 1px solid #1f2b38;"
        "  border-radius: 8px;"
        "  background: #111923;"
        "  text-align: left;"
        "  padding: 4px 8px;"
        "}"
        "QToolButton:hover {"
        "  color: #dce8f5;"
        "  background: #172231;"
        "  border-color: #2e4054;"
        "}"
        "QToolButton:checked {"
        "  color: #d6e7f8;"
        "  background: #182536;"
        "  border-color: #38516c;"
        "}"));
    outer->addWidget(toggle);

    auto* content = new QWidget(container);
    content->setVisible(expanded);
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(10, 5, 2, 5);
    body->setSpacing(6);
    outer->addWidget(content);

    QObject::connect(toggle, &QToolButton::toggled, content,
                     [toggle, title, content](bool checked) {
        Q_UNUSED(title);
        toggle->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
        content->setVisible(checked);
    });

    section.container = container;
    section.body = body;
    return section;
}

} // namespace jcut::inspector
