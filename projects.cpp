#include "projects.h"

#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSignalBlocker>

ProjectsTab::ProjectsTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
}

void ProjectsTab::wire()
{
    if (m_widgets.projectsList) {
        connect(m_widgets.projectsList, &QListWidget::itemSelectionChanged,
                this, &ProjectsTab::onProjectSelectionChanged);
    }
    if (m_widgets.newProjectButton) {
        connect(m_widgets.newProjectButton, &QPushButton::clicked, this, [this]() {
            if (m_deps.createProject) {
                m_deps.createProject();
            }
        });
    }
    if (m_widgets.saveProjectAsButton) {
        connect(m_widgets.saveProjectAsButton, &QPushButton::clicked, this, [this]() {
            if (m_deps.saveProjectAs) {
                m_deps.saveProjectAs();
            }
        });
    }
    if (m_widgets.renameProjectButton) {
        connect(m_widgets.renameProjectButton, &QPushButton::clicked,
                this, &ProjectsTab::onRenameClicked);
    }
}

void ProjectsTab::refresh()
{
    if (!m_widgets.projectsList) {
        return;
    }

    const QString currentProject = m_deps.currentProjectName ? m_deps.currentProjectName() : QString();
    const QStringList projectIds = m_deps.availableProjectIds ? m_deps.availableProjectIds() : QStringList{};

    const QSignalBlocker blocker(m_widgets.projectsList);
    m_updatingList = true;
    m_widgets.projectsList->clear();

    for (const QString& projectId : projectIds) {
        auto* item = new QListWidgetItem(projectId, m_widgets.projectsList);
        item->setData(Qt::UserRole, projectId);
        if (m_deps.projectPath) {
            item->setToolTip(QDir::toNativeSeparators(m_deps.projectPath(projectId)));
        }
        if (projectId == currentProject) {
            item->setSelected(true);
        }
    }

    if (m_widgets.projectSectionLabel) {
        m_widgets.projectSectionLabel->setText(QStringLiteral("PROJECTS  %1").arg(currentProject));
    }
    if (m_widgets.renameProjectButton) {
        m_widgets.renameProjectButton->setEnabled(!selectedProjectId().isEmpty());
    }

    m_updatingList = false;
}

void ProjectsTab::onProjectSelectionChanged()
{
    const QString projectId = selectedProjectId();
    if (m_widgets.renameProjectButton) {
        m_widgets.renameProjectButton->setEnabled(!projectId.isEmpty());
    }
    if (m_updatingList || projectId.isEmpty() || !m_deps.switchToProject) {
        return;
    }
    m_deps.switchToProject(projectId);
}

void ProjectsTab::onRenameClicked()
{
    const QString projectId = selectedProjectId();
    if (projectId.isEmpty() || !m_deps.renameProject) {
        return;
    }
    m_deps.renameProject(projectId);
}

QString ProjectsTab::selectedProjectId() const
{
    if (!m_widgets.projectsList) {
        return {};
    }
    const auto items = m_widgets.projectsList->selectedItems();
    if (items.isEmpty()) {
        return {};
    }
    return items.constFirst()->data(Qt::UserRole).toString();
}
