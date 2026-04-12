#pragma once

#include <QObject>
#include <QTableWidget>
#include <QLabel>
#include <QVector>
#include <functional>

#include "editor_shared.h"

class QJsonArray;

class HistoryTab : public QObject {
    Q_OBJECT

public:
    struct Widgets {
        QTableWidget* historyTable = nullptr;
    };

    struct Dependencies {
        std::function<QJsonArray()> getHistoryEntries;
        std::function<int()> getHistoryIndex;
        std::function<void(int)> restoreToHistoryIndex;
        std::function<void()> pushHistorySnapshot;
    };

    explicit HistoryTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~HistoryTab() override = default;

    void wire();
    void refresh();

private slots:
    void onRowDoubleClicked(int row);

private:
    Widgets m_widgets;
    Dependencies m_deps;
};