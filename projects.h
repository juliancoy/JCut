#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

class QLabel;
class QListWidget;
class QPushButton;

class ProjectsTab : public QObject
{
    Q_OBJECT

public:
    struct Widgets
    {
        QLabel* projectSectionLabel = nullptr;
        QListWidget* projectsList = nullptr;
        QPushButton* newProjectButton = nullptr;
        QPushButton* saveProjectAsButton = nullptr;
        QPushButton* renameProjectButton = nullptr;
    };

    struct Dependencies
    {
        std::function<QStringList()> availableProjectIds;
        std::function<QString()> currentProjectName;
        std::function<QString(const QString&)> projectPath;
        std::function<void(const QString&)> switchToProject;
        std::function<void()> createProject;
        std::function<void()> saveProjectAs;
        std::function<void(const QString&)> renameProject;
    };

    explicit ProjectsTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~ProjectsTab() override = default;

    void wire();
    void refresh();

private slots:
    void onProjectSelectionChanged();
    void onRenameClicked();

private:
    QString selectedProjectId() const;

    Widgets m_widgets;
    Dependencies m_deps;
    bool m_updatingList = false;
};
