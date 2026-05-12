#pragma once

#include <QJsonArray>
#include <QTableWidget>

class SpeakersTable final : public QTableWidget
{
    Q_OBJECT

public:
    explicit SpeakersTable(QWidget* parent = nullptr);

    QJsonArray hiddenColumnsState() const;
    void applyHiddenColumns(const QJsonArray& hiddenColumns);

signals:
    void columnVisibilityChanged();
    void avatarHoverRequested(const QString& speakerId, const QPoint& globalPos);
    void avatarHoverCleared();

private:
    void showHeaderContextMenu(const QPoint& pos);
    bool isAlwaysVisibleColumn(int column) const;
    bool viewportEvent(QEvent* event) override;

    QString m_hoveredSpeakerId;
};
