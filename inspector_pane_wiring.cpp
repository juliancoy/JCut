#include "inspector_pane.h"

#include <QHBoxLayout>
#include <QPalette>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>

void InspectorPane::setHeaderWidget(QWidget *widget)
{
    if (!m_headerLayout || !widget) {
        return;
    }

    widget->setParent(this);
    m_headerLayout->addWidget(widget, 0, Qt::AlignRight | Qt::AlignVCenter);
}

void InspectorPane::configureInspectorTabs()
{
    if (!m_inspectorTabs) {
        return;
    }

    auto *bar = m_inspectorTabs->tabBar();
    m_inspectorTabs->setTabPosition(QTabWidget::East);
    m_inspectorTabs->setDocumentMode(true);
    bar->setExpanding(false);
    bar->setUsesScrollButtons(true);
    bar->setIconSize(QSize(20, 20));
    bar->setDrawBase(false);

    struct TabSpec {
        const char* label;
        QStyle::StandardPixmap icon;
        const char* tooltip;
    };

    const TabSpec specs[] = {
        {"Grade", QStyle::SP_DriveDVDIcon, "Grade: clip color and grading keyframes"},
        {"Opacity", QStyle::SP_BrowserStop, "Opacity: clip opacity keyframes and fades"},
        {"Effects", QStyle::SP_DialogResetButton, "Effects: mask feathering and visual effects"},
        {"Masks", QStyle::SP_FileDialogDetailedView, "Masks: SAM mask source, shape filters, shadow, and masked-area grade"},
        {"Corrections", QStyle::SP_DriveFDIcon, "Corrections: draw polygon erase masks for visual artifacts"},
        {"Titles", QStyle::SP_FileDialogListView, "Titles: text overlay keyframes"},
        {"Sync", QStyle::SP_BrowserReload, "Sync: render sync markers for the selected clip"},
        {"Transform", QStyle::SP_FileDialogDetailedView, "Transform: position, scale, rotation, and keyframes for the selected clip"},
        {"Transcript", QStyle::SP_FileDialogContentsView, "Transcript: transcript editing and speech filter controls"},
        {"Speakers", QStyle::SP_MediaVolume, "Speakers: speaker identity and on-screen location for the active cut"},
        {"Properties", QStyle::SP_FileDialogInfoView, "Properties: clip and track properties"},
        {"Clips", QStyle::SP_FileDialogListView, "Clips: timeline clip list and clip actions"},
        {"History", QStyle::SP_BrowserReload, "History: saved timeline snapshots"},
        {"Tracks", QStyle::SP_FileDialogInfoView, "Tracks: track visibility and enable state controls"},
        {"Preview", QStyle::SP_MediaPlay, "Preview: editor preview display controls"},
        {"Audio", QStyle::SP_MediaVolume, "Audio: preview audio dynamics processing controls"},
        {"Jobs", QStyle::SP_ComputerIcon, "Jobs: running and recent detection processing jobs"},
        {"AI Assist", QStyle::SP_FileDialogDetailedView, "AI Assist: model, auth, and AI workflow actions"},
        {"Access", QStyle::SP_DialogYesButton, "Access: account subscriptions, purchases, and AI usage status"},
        {"Output", QStyle::SP_DialogSaveButton, "Output: render settings and export"},
        {"Pipeline", QStyle::SP_CommandLink, "Pipeline: live preview frame status and thumbnails"},
        {"System", QStyle::SP_ComputerIcon, "System: playback, decoder, cache, and benchmark information"},
        {"Projects", QStyle::SP_DirHomeIcon, "Projects: browse, create, rename, and switch projects"},
        {"Preferences", QStyle::SP_FileDialogContentsView, "Preferences: app-level behavior and feature flags"},
    };

    for (const TabSpec& spec : specs) {
        const QString label = QString::fromUtf8(spec.label);
        const int index = [&]() {
            for (int i = 0; i < m_inspectorTabs->count(); ++i) {
                if (m_inspectorTabs->tabText(i) == label) {
                    return i;
                }
            }
            return -1;
        }();
        if (index < 0) {
            continue;
        }
        m_inspectorTabs->setTabIcon(index, style()->standardIcon(spec.icon));
        bar->setTabToolTip(index, QString::fromUtf8(spec.tooltip));
    }
    bar->setElideMode(Qt::ElideRight);
    QPalette palette = bar->palette();
    palette.setColor(QPalette::WindowText, Qt::white);
    palette.setColor(QPalette::ButtonText, Qt::white);
    palette.setColor(QPalette::Text, Qt::white);
    bar->setPalette(palette);

    bar->setStyleSheet(QStringLiteral(
        "QTabBar::tab {"
        " min-width: 36px;"
        " min-height: 34px;"
        " margin: 0;"
        " padding: 4px 6px;"
        " text-align: left;"
        " color: #ffffff;"
        " }"
        "QTabBar::tab:selected {"
        " background: #1f2a36;"
        " border: 1px solid #44556a;"
        " border-radius: 6px;"
        " color: #ffffff;"
        " }"
        "QTabBar::tab:hover {"
        " background: #233142;"
        " border: 1px solid #4a5c71;"
        " color: #ffffff;"
        " }"
        "QTabBar::tab:!selected {"
        " background: #121922;"
        " border: 1px solid #2e3b4a;"
        " border-radius: 6px;"
        " }"));
}
