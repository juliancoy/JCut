#include "editor_pane.h"

#include "preview.h"
#include "timeline_widget.h"
#include "timeline_container.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QSlider>
#include <QStackedLayout>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QSizePolicy>

EditorPane::EditorPane(QWidget *parent)
    : QWidget(parent)
{
    setStyleSheet(
        QStringLiteral(
            "QWidget { background: #0c1015; color: #edf2f7; }"
            "QPushButton, QToolButton { background: #1b2430; border: 1px solid #2e3b4a; border-radius: 7px; padding: 8px 12px; }"
            "QPushButton:hover, QToolButton:hover { background: #233142; }"
            "QSlider::groove:horizontal { background: #24303c; height: 6px; border-radius: 3px; }"
            "QSlider::handle:horizontal { background: #ff6f61; width: 14px; margin: -5px 0; border-radius: 7px; }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(0);

    auto *verticalSplitter = new QSplitter(Qt::Vertical, this);
    verticalSplitter->setObjectName(QStringLiteral("layout.editor_splitter"));
    verticalSplitter->setChildrenCollapsible(false);
    verticalSplitter->setHandleWidth(6);
    verticalSplitter->setStyleSheet(QStringLiteral(
        "QSplitter::handle { background: #1e2a36; }"
        "QSplitter::handle:hover { background: #3a5068; }"));
    layout->addWidget(verticalSplitter, 1);

    auto *previewFrame = new QFrame;
    previewFrame->setMinimumHeight(240);
    previewFrame->setFrameShape(QFrame::NoFrame);
    previewFrame->setStyleSheet(QStringLiteral(
        "QFrame { background: #05080c; border: 1px solid #202934; border-radius: 14px; }"));

    auto *previewLayout = new QVBoxLayout(previewFrame);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(0);

    m_preview = new PreviewWindow;
    m_preview->setObjectName(QStringLiteral("preview.window"));
    m_preview->setFocusPolicy(Qt::StrongFocus);
    m_preview->setMinimumSize(160, 120);
    m_preview->setOutputSize(QSize(1080, 1920));

    auto *overlay = new QWidget;
    overlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    overlay->setStyleSheet(QStringLiteral("background: transparent;"));

    auto *overlayLayout = new QVBoxLayout(overlay);
    overlayLayout->setContentsMargins(18, 14, 18, 14);
    overlayLayout->setSpacing(6);

    auto *badgeRow = new QHBoxLayout;
    badgeRow->setContentsMargins(0, 0, 0, 0);

    m_statusBadge = new QLabel;
    m_statusBadge->setObjectName(QStringLiteral("overlay.status_badge"));
    m_statusBadge->setStyleSheet(QStringLiteral(
        "QLabel { background: rgba(7, 11, 17, 0.72); color: #f2f7fb; border-radius: 10px; padding: 8px 12px; font-weight: 600; }"));
    badgeRow->addWidget(m_statusBadge, 0, Qt::AlignLeft);
    badgeRow->addStretch(1);
    overlayLayout->addLayout(badgeRow);

    overlayLayout->addStretch(1);

    m_previewInfo = new QLabel;
    m_previewInfo->setObjectName(QStringLiteral("overlay.preview_info"));
    m_previewInfo->setStyleSheet(QStringLiteral(
        "QLabel { background: rgba(7, 11, 17, 0.72); color: #dce6ef; border-radius: 10px; padding: 10px 12px; }"));
    m_previewInfo->setWordWrap(true);
    overlayLayout->addWidget(m_previewInfo, 0, Qt::AlignLeft | Qt::AlignBottom);

    auto *stack = new QStackedLayout;
    stack->setStackingMode(QStackedLayout::StackAll);
    stack->addWidget(m_preview);
    stack->addWidget(overlay);
    previewLayout->addLayout(stack);

    verticalSplitter->addWidget(previewFrame);

    // TimelineContainer with 2x2 grid layout
    m_timelineContainer = new TimelineContainer;
    m_timelineContainer->setMinimumHeight(220);
    
    // Create transport controls and add to TimelineContainer's transport area
    setupTransportControls();
    
    verticalSplitter->addWidget(m_timelineContainer);
    verticalSplitter->setStretchFactor(0, 3);
    verticalSplitter->setStretchFactor(1, 2);
    verticalSplitter->setSizes({540, 320});
}

void EditorPane::setupTransportControls()
{
    // Find the transport widget in the timeline container
    QWidget *transportWidget = m_timelineContainer->findChild<QWidget*>(QStringLiteral("timeline.transport"));
    if (!transportWidget) {
        qWarning() << "Transport widget not found in TimelineContainer";
        return;
    }
    transportWidget->setMinimumWidth(0);
    transportWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    
    // Get the transport widget's layout
    QHBoxLayout *transportLayout = qobject_cast<QHBoxLayout*>(transportWidget->layout());
    if (!transportLayout) {
        qWarning() << "Transport layout not found";
        return;
    }
    transportLayout->setSizeConstraint(QLayout::SetNoConstraint);
    transportLayout->setSpacing(4);
    
    // Clear the placeholder stretch
    QLayoutItem *item;
    while ((item = transportLayout->takeAt(0)) != nullptr) {
        delete item;
    }

    m_playButton = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), QStringLiteral("Play"));
    m_playButton->setObjectName(QStringLiteral("transport.play"));
    m_playButton->setMinimumWidth(0);
    m_playButton->setMaximumWidth(38);
    m_playButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_playButton->setToolTip(QStringLiteral("Play / Pause"));

    m_startButton = new QToolButton;
    m_endButton = new QToolButton;
    m_prevFrameButton = new QToolButton;
    m_nextFrameButton = new QToolButton;
    m_startButton->setObjectName(QStringLiteral("transport.start"));
    m_endButton->setObjectName(QStringLiteral("transport.end"));
    m_prevFrameButton->setObjectName(QStringLiteral("transport.prev_frame"));
    m_nextFrameButton->setObjectName(QStringLiteral("transport.next_frame"));
    m_startButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
    m_endButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));
    m_prevFrameButton->setText(QStringLiteral("<"));
    m_nextFrameButton->setText(QStringLiteral(">"));
    m_prevFrameButton->setToolTip(QStringLiteral("Step back 1 frame"));
    m_nextFrameButton->setToolTip(QStringLiteral("Step forward 1 frame"));
    for (QToolButton* btn : {m_startButton, m_endButton, m_prevFrameButton, m_nextFrameButton}) {
        btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
        btn->setMinimumWidth(0);
        btn->setMaximumWidth(30);
        btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    }

    m_razorButton = new QToolButton;
    m_razorButton->setObjectName(QStringLiteral("transport.razor"));
    m_razorButton->setText(QStringLiteral("R"));
    m_razorButton->setCheckable(true);
    m_razorButton->setToolTip(QStringLiteral("Razor tool (B) \u2014 click to split clips"));
    m_razorButton->setMinimumWidth(0);
    m_razorButton->setMaximumWidth(28);
    m_razorButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_razorButton->setStyleSheet(QStringLiteral(
        "QToolButton:checked { background: #3a4d63; border-color: #a0e0ff; color: #a0e0ff; }"));

    m_seekSlider = new QSlider(Qt::Horizontal);
    m_seekSlider->setObjectName(QStringLiteral("transport.seek"));
    m_seekSlider->setRange(0, 300);

    m_timecodeLabel = new QLabel;
    m_timecodeLabel->setObjectName(QStringLiteral("transport.timecode"));
    m_timecodeLabel->setMinimumWidth(64);
    m_timecodeLabel->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);

    m_playbackSpeedCombo = new QComboBox;
    m_playbackSpeedCombo->setObjectName(QStringLiteral("transport.playback_speed"));
    m_playbackSpeedCombo->addItem(QStringLiteral("10%"), 0.1);
    m_playbackSpeedCombo->addItem(QStringLiteral("25%"), 0.25);
    m_playbackSpeedCombo->addItem(QStringLiteral("50%"), 0.5);
    m_playbackSpeedCombo->addItem(QStringLiteral("75%"), 0.75);
    m_playbackSpeedCombo->addItem(QStringLiteral("100%"), 1.0);
    m_playbackSpeedCombo->addItem(QStringLiteral("150%"), 1.5);
    m_playbackSpeedCombo->addItem(QStringLiteral("200%"), 2.0);
    m_playbackSpeedCombo->addItem(QStringLiteral("300%"), 3.0);
    m_playbackSpeedCombo->setCurrentIndex(4);
    m_playbackSpeedCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_playbackSpeedCombo->setMinimumContentsLength(4);
    m_playbackSpeedCombo->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    m_playbackSpeedCombo->setToolTip(
        QStringLiteral("Timeline playback speed. Audio clock remains authoritative when audio is active."));

    m_previewModeCombo = new QComboBox;
    m_previewModeCombo->setObjectName(QStringLiteral("transport.preview_mode"));
    m_previewModeCombo->addItem(QStringLiteral("Preview: Video"), QStringLiteral("video"));
    m_previewModeCombo->addItem(QStringLiteral("Preview: Audio"), QStringLiteral("audio"));
    m_previewModeCombo->setCurrentIndex(0);
    m_previewModeCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_previewModeCombo->setMinimumContentsLength(12);
    m_previewModeCombo->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    m_previewModeCombo->setToolTip(QStringLiteral("Switch preview between video composition and audio waveform view."));

    m_audioToolsButton = new QToolButton;
    m_audioToolsButton->setObjectName(QStringLiteral("transport.audio_tools"));
    m_audioToolsButton->setText(QStringLiteral("FX"));
    m_audioToolsButton->setToolTip(QStringLiteral("Open audio normalization/limiter/compressor controls."));
    m_audioToolsButton->setMinimumWidth(0);
    m_audioToolsButton->setMaximumWidth(34);
    m_audioToolsButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    m_audioMuteButton = new QToolButton;
    m_audioMuteButton->setObjectName(QStringLiteral("transport.audio_mute"));
    m_audioMuteButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_audioMuteButton->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
    m_audioMuteButton->setToolTip(QStringLiteral("Mute / Unmute"));
    m_audioMuteButton->setMinimumWidth(0);
    m_audioMuteButton->setMaximumWidth(28);
    m_audioMuteButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    m_audioVolumeSlider = new QSlider(Qt::Horizontal);
    m_audioVolumeSlider->setObjectName(QStringLiteral("transport.audio_volume"));
    m_audioVolumeSlider->setRange(0, 100);
    m_audioVolumeSlider->setValue(80);
    m_audioVolumeSlider->setMinimumWidth(56);
    m_audioVolumeSlider->setMaximumWidth(90);
    m_audioVolumeSlider->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    m_audioNowPlayingLabel = new QLabel(QStringLiteral("Audio idle"));
    m_audioNowPlayingLabel->setObjectName(QStringLiteral("transport.audio_status"));
    m_audioNowPlayingLabel->setMinimumWidth(0);
    m_audioNowPlayingLabel->setMaximumWidth(80);
    m_audioNowPlayingLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    transportLayout->addWidget(m_startButton);
    transportLayout->addWidget(m_playButton);
    transportLayout->addWidget(m_prevFrameButton);
    transportLayout->addWidget(m_nextFrameButton);
    transportLayout->addWidget(m_endButton);
    transportLayout->addSpacing(4);
    transportLayout->addWidget(m_razorButton);
    transportLayout->addWidget(m_seekSlider, 1);
    transportLayout->addWidget(m_timecodeLabel);
    transportLayout->addWidget(m_playbackSpeedCombo);
    transportLayout->addWidget(m_previewModeCombo);
    transportLayout->addWidget(m_audioToolsButton);
    transportLayout->addWidget(m_audioMuteButton);
    transportLayout->addWidget(m_audioVolumeSlider);
    transportLayout->addWidget(m_audioNowPlayingLabel);

    // Connect signals
    connect(m_playButton, &QPushButton::clicked, this, &EditorPane::playClicked);
    connect(m_startButton, &QToolButton::clicked, this, &EditorPane::startClicked);
    connect(m_prevFrameButton, &QToolButton::clicked, this, &EditorPane::prevFrameClicked);
    connect(m_nextFrameButton, &QToolButton::clicked, this, &EditorPane::nextFrameClicked);
    connect(m_endButton, &QToolButton::clicked, this, &EditorPane::endClicked);
    connect(m_seekSlider, &QSlider::valueChanged, this, &EditorPane::seekValueChanged);
    connect(m_playbackSpeedCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        const double speed = m_playbackSpeedCombo->itemData(index).toDouble();
        emit playbackSpeedChanged(speed);
    });
    connect(m_previewModeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        emit previewModeChanged(m_previewModeCombo->itemData(index).toString());
    });
    connect(m_audioToolsButton, &QToolButton::clicked, this, &EditorPane::audioToolsClicked);
    connect(m_audioMuteButton, &QToolButton::clicked, this, &EditorPane::audioMuteClicked);
    connect(m_audioVolumeSlider, &QSlider::valueChanged, this, &EditorPane::audioVolumeChanged);

}

// Accessor to get the TimelineWidget from the container
TimelineWidget *EditorPane::timelineWidget() const
{
    return m_timelineContainer ? m_timelineContainer->timeline() : nullptr;
}
