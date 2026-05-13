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
    bar->setIconSize(QSize(16, 16));
    bar->setDrawBase(false);

    struct TabSpec {
        int index;
        const char* label;
        QStyle::StandardPixmap icon;
        const char* tooltip;
    };

    const TabSpec specs[] = {
        {0, "Grade", QStyle::SP_DriveDVDIcon, "Grade: clip color and grading keyframes"},
        {1, "Opacity", QStyle::SP_BrowserStop, "Opacity: clip opacity keyframes and fades"},
        {2, "Effects", QStyle::SP_DialogResetButton, "Effects: mask feathering and visual effects"},
        {3, "Corrections", QStyle::SP_DriveFDIcon, "Corrections: draw polygon erase masks for visual artifacts"},
        {4, "Titles", QStyle::SP_FileDialogListView, "Titles: text overlay keyframes"},
        {5, "Sync", QStyle::SP_BrowserReload, "Sync: render sync markers for the selected clip"},
        {6, "Keyframes", QStyle::SP_FileDialogDetailedView, "Keyframes: transform keyframes for the selected clip"},
        {7, "Transcript", QStyle::SP_FileDialogContentsView, "Transcript: transcript editing and speech filter controls"},
        {8, "Speakers", QStyle::SP_MediaVolume, "Speakers: speaker identity and on-screen location for the active cut"},
        {9, "Properties", QStyle::SP_FileDialogInfoView, "Properties: clip and track properties"},
        {10, "Clips", QStyle::SP_FileDialogListView, "Clips: timeline clip list and clip actions"},
        {11, "History", QStyle::SP_BrowserReload, "History: saved timeline snapshots"},
        {12, "Tracks", QStyle::SP_FileDialogInfoView, "Tracks: track visibility and enable state controls"},
        {13, "Preview", QStyle::SP_MediaPlay, "Preview: editor preview display controls"},
        {14, "Audio", QStyle::SP_MediaVolume, "Audio: preview audio dynamics processing controls"},
        {15, "AI Assist", QStyle::SP_FileDialogDetailedView, "AI Assist: model, auth, and AI workflow actions"},
        {16, "Access", QStyle::SP_DialogYesButton, "Access: account subscriptions, purchases, and AI usage status"},
        {17, "Output", QStyle::SP_DialogSaveButton, "Output: render settings and export"},
        {18, "Pipeline", QStyle::SP_CommandLink, "Pipeline: live preview frame status and thumbnails"},
        {19, "System", QStyle::SP_ComputerIcon, "System: playback, decoder, cache, and benchmark information"},
        {20, "Projects", QStyle::SP_DirHomeIcon, "Projects: browse, create, rename, and switch projects"},
        {21, "Preferences", QStyle::SP_FileDialogContentsView, "Preferences: app-level behavior and feature flags"},
    };

    for (const TabSpec& spec : specs) {
        m_inspectorTabs->setTabIcon(spec.index, style()->standardIcon(spec.icon));
        m_inspectorTabs->setTabText(spec.index, QString::fromUtf8(spec.label));
        bar->setTabToolTip(spec.index, QString::fromUtf8(spec.tooltip));
    }
    bar->setElideMode(Qt::ElideRight);
    QPalette palette = bar->palette();
    palette.setColor(QPalette::WindowText, Qt::white);
    palette.setColor(QPalette::ButtonText, Qt::white);
    palette.setColor(QPalette::Text, Qt::white);
    bar->setPalette(palette);

    bar->setStyleSheet(QStringLiteral(
        "QTabBar::tab {"
        " min-width: 112px;"
        " min-height: 28px;"
        " margin: 0;"
        " padding: 4px 10px;"
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
