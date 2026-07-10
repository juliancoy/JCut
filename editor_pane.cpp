#include "editor_pane.h"

#include "preview_surface.h"
#include "preview_surface_factory.h"
#include "timeline_widget.h"
#include "timeline_container.h"
#include "transport_icons.h"

#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QPainter>
#include <QSlider>
#include <QStackedLayout>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QSizePolicy>
#include <QVariantAnimation>
#include <QEvent>

namespace {
class TransportGlowFilter final : public QObject {
public:
    explicit TransportGlowFilter(QObject* parent = nullptr)
        : QObject(parent) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        auto* btn = qobject_cast<QAbstractButton*>(watched);
        if (!btn) {
            return QObject::eventFilter(watched, event);
        }
        switch (event->type()) {
        case QEvent::Enter:
            animateGlow(btn, 1.0);
            break;
        case QEvent::Leave:
            animateGlow(btn, 0.0);
            break;
        case QEvent::MouseButtonPress:
            animateGlow(btn, 1.35);
            break;
        case QEvent::MouseButtonRelease:
            animateGlow(btn, btn->underMouse() ? 1.0 : 0.0);
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QColor semanticGlowColor(QAbstractButton* button, qreal intensity) const
    {
        const QString name = button ? button->objectName() : QString();
        QColor base(158, 229, 255); // default cool cyan
        if (name == QStringLiteral("transport.play")) {
            base = QColor(126, 230, 155); // play = green
        } else if (name == QStringLiteral("transport.audio_mute")) {
            base = QColor(255, 192, 120); // audio mute = amber
        } else if (name == QStringLiteral("transport.audio_tools")) {
            base = QColor(200, 170, 255); // audio tools = violet accent
        } else if (name == QStringLiteral("transport.razor")) {
            base = QColor(120, 220, 255); // razor = cyan
            if (button->isChecked()) {
                base = QColor(160, 245, 255);
            }
        } else if (name == QStringLiteral("transport.start") ||
                   name == QStringLiteral("transport.prev_frame") ||
                   name == QStringLiteral("transport.next_frame") ||
                   name == QStringLiteral("transport.end")) {
            base = QColor(145, 205, 255); // timeline navigation = blue
        }
        base.setAlpha(static_cast<int>(70.0 * intensity));
        return base;
    }

    void animateGlow(QAbstractButton* button, qreal target)
    {
        if (!button) {
            return;
        }
        auto* effect = qobject_cast<QGraphicsDropShadowEffect*>(button->graphicsEffect());
        if (!effect) {
            effect = new QGraphicsDropShadowEffect(button);
            effect->setOffset(0.0, 0.0);
            effect->setBlurRadius(0.0);
            effect->setColor(QColor(158, 229, 255, 0));
            button->setGraphicsEffect(effect);
        }

        qreal start = button->property("transport_glow_intensity").toReal();
        if (!button->property("transport_glow_intensity").isValid()) {
            start = 0.0;
        }
        button->setProperty("transport_glow_intensity", start);

        auto* anim = button->findChild<QVariantAnimation*>(QStringLiteral("transportGlowAnim"));
        if (!anim) {
            anim = new QVariantAnimation(button);
            anim->setObjectName(QStringLiteral("transportGlowAnim"));
            anim->setDuration(120);
            anim->setEasingCurve(QEasingCurve::OutCubic);
            connect(anim, &QVariantAnimation::valueChanged, button, [this, button](const QVariant& value) {
                const qreal t = qBound<qreal>(0.0, value.toReal(), 1.5);
                button->setProperty("transport_glow_intensity", t);
                auto* glow = qobject_cast<QGraphicsDropShadowEffect*>(button->graphicsEffect());
                if (!glow) {
                    return;
                }
                glow->setBlurRadius(10.0 * t);
                glow->setColor(semanticGlowColor(button, t));
            });
        }
        anim->stop();
        anim->setStartValue(start);
        anim->setEndValue(target);
        anim->start();
    }
};
} // namespace

EditorPane::EditorPane(QWidget *parent)
    : QWidget(parent)
{
    editor::validateTransportIconResources();
    setStyleSheet(
        QStringLiteral(
            "QWidget { background: #0c1015; color: #edf2f7; }"
            "QPushButton, QToolButton { background: #18222d; border: 1px solid #2b3948; border-radius: 6px; padding: 5px 9px; }"
            "QPushButton:hover, QToolButton:hover { background: #223246; border-color: #4c657e; }"
            "QPushButton:pressed, QToolButton:pressed { background: #111a24; border-color: #7eaad0; }"
            "QWidget#timeline\\.transport { background: #0f151c; border-bottom: 1px solid #2a3542; }"
            "QPushButton#transport\\.play { background: #193426; border-color: #39724d; color: #dfffe8; font-weight: 700; }"
            "QPushButton#transport\\.play:hover { background: #214a33; border-color: #6bcc88; }"
            "QSlider::groove:horizontal { background: #263340; height: 5px; border-radius: 2px; }"
            "QSlider::handle:horizontal { background: #f0c36a; width: 12px; margin: -5px 0; border-radius: 6px; }"));

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
    previewFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    previewFrame->setFrameShape(QFrame::NoFrame);
    previewFrame->setStyleSheet(QStringLiteral(
        "QFrame { background: #05080c; border: 1px solid #202934; border-radius: 14px; }"));

    auto *previewLayout = new QVBoxLayout(previewFrame);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(0);

    m_preview = createPreviewSurfaceForConfiguredBackend(previewFrame);
    if (!m_preview) {
        qFatal("Vulkan preview surface creation failed and no fallback preview surface is available.");
    }
    QWidget* previewWidget = m_preview->asWidget();
    previewWidget->setObjectName(QStringLiteral("preview.window"));
    previewWidget->setFocusPolicy(Qt::StrongFocus);
    previewWidget->setMinimumSize(160, 120);
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
    m_previewInfo->setVisible(false);
    overlayLayout->addWidget(m_previewInfo, 0, Qt::AlignLeft | Qt::AlignBottom);

    auto *stack = new QStackedLayout;
    stack->setStackingMode(QStackedLayout::StackAll);
    stack->addWidget(previewWidget);
    stack->addWidget(overlay);
    previewLayout->addLayout(stack);

    verticalSplitter->addWidget(previewFrame);

    // TimelineContainer with 2x2 grid layout
    m_timelineContainer = new TimelineContainer;
    m_timelineContainer->setMinimumHeight(300);
    m_timelineContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    
    // Create transport controls and add to TimelineContainer's transport area
    setupTransportControls();
    
    verticalSplitter->addWidget(m_timelineContainer);
    verticalSplitter->setCollapsible(0, false);
    verticalSplitter->setCollapsible(1, false);
    verticalSplitter->setStretchFactor(0, 5);
    verticalSplitter->setStretchFactor(1, 4);
    verticalSplitter->setSizes({470, 390});
}

void EditorPane::setupTransportControls()
{
    // Find the transport widget in the timeline container
    QWidget *transportWidget = m_timelineContainer->findChild<QWidget*>(QStringLiteral("timeline.transport"));
    if (!transportWidget) {
        qWarning() << "Transport widget not found in TimelineContainer";
        return;
    }
    transportWidget->installEventFilter(this);
    transportWidget->setMinimumWidth(0);
    transportWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    
    // Get the transport widget's layout
    QHBoxLayout *transportLayout = qobject_cast<QHBoxLayout*>(transportWidget->layout());
    if (!transportLayout) {
        qWarning() << "Transport layout not found";
        return;
    }
    transportLayout->setSizeConstraint(QLayout::SetNoConstraint);
    transportLayout->setContentsMargins(10, 5, 10, 5);
    transportLayout->setSpacing(5);
    
    // Clear the placeholder stretch
    QLayoutItem *item;
    while ((item = transportLayout->takeAt(0)) != nullptr) {
        delete item;
    }

    m_playButton = new QPushButton(editor::transportIcon(editor::TransportIconGlyph::Play), QString());
    m_playButton->setObjectName(QStringLiteral("transport.play"));
    m_playButton->setFixedWidth(40);
    m_playButton->setFixedHeight(30);
    m_playButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_playButton->setIconSize(QSize(18, 18));
    m_playButton->setToolTip(QStringLiteral("Play / Pause (Space)"));
    m_playButton->setAccessibleName(QStringLiteral("Play Pause"));

    m_startButton = new QToolButton;
    m_endButton = new QToolButton;
    m_prevFrameButton = new QToolButton;
    m_nextFrameButton = new QToolButton;
    m_startButton->setObjectName(QStringLiteral("transport.start"));
    m_endButton->setObjectName(QStringLiteral("transport.end"));
    m_prevFrameButton->setObjectName(QStringLiteral("transport.prev_frame"));
    m_nextFrameButton->setObjectName(QStringLiteral("transport.next_frame"));
    m_startButton->setIcon(editor::transportIcon(editor::TransportIconGlyph::ToStart));
    m_endButton->setIcon(editor::transportIcon(editor::TransportIconGlyph::ToEnd));
    m_prevFrameButton->setIcon(editor::transportIcon(editor::TransportIconGlyph::StepBack));
    m_nextFrameButton->setIcon(editor::transportIcon(editor::TransportIconGlyph::StepForward));
    m_startButton->setToolTip(QStringLiteral("Go to start"));
    m_endButton->setToolTip(QStringLiteral("Go to end"));
    m_prevFrameButton->setToolTip(QStringLiteral("Previous frame"));
    m_nextFrameButton->setToolTip(QStringLiteral("Next frame"));
    m_startButton->setAccessibleName(QStringLiteral("Go To Start"));
    m_endButton->setAccessibleName(QStringLiteral("Go To End"));
    m_prevFrameButton->setAccessibleName(QStringLiteral("Previous Frame"));
    m_nextFrameButton->setAccessibleName(QStringLiteral("Next Frame"));
    for (QToolButton* btn : {m_startButton, m_endButton, m_prevFrameButton, m_nextFrameButton}) {
        btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
        btn->setFixedSize(30, 30);
        btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        btn->setIconSize(QSize(18, 18));
    }

    m_razorButton = new QToolButton;
    m_razorButton->setObjectName(QStringLiteral("transport.razor"));
    m_razorButton->setText(QStringLiteral("R"));
    m_razorButton->setCheckable(true);
    m_razorButton->setToolTip(QStringLiteral("Razor tool (B) \u2014 click to split clips"));
    m_razorButton->setFixedSize(30, 30);
    m_razorButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_razorButton->setStyleSheet(QStringLiteral(
        "QToolButton:checked { background: #3a4d63; border-color: #a0e0ff; color: #a0e0ff; }"));

    m_seekSlider = new QSlider(Qt::Horizontal);
    m_seekSlider->setObjectName(QStringLiteral("transport.seek"));
    m_seekSlider->setRange(0, 300);
    m_seekSlider->setMinimumWidth(96);

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
    m_playbackSpeedCombo->addItem(QStringLiteral("125%"), 1.25);
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
    m_audioToolsButton->setToolTip(QStringLiteral("Open the Audio inspector tab."));
    m_audioToolsButton->setFixedSize(32, 30);
    m_audioToolsButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    m_loopButton = new QToolButton;
    m_loopButton->setObjectName(QStringLiteral("transport.loop"));
    m_loopButton->setText(QStringLiteral("Loop"));
    m_loopButton->setCheckable(true);
    m_loopButton->setToolTip(QStringLiteral("Loop playback range"));
    m_loopButton->setFixedSize(44, 30);
    m_loopButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_loopButton->setStyleSheet(QStringLiteral(
        "QToolButton:checked { background: #2d455f; border-color: #9ee5ff; color: #c8f3ff; }"));

    m_audioMuteButton = new QToolButton;
    m_audioMuteButton->setObjectName(QStringLiteral("transport.audio_mute"));
    m_audioMuteButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_audioMuteButton->setIcon(editor::volumeTransportIcon(false));
    m_audioMuteButton->setIconSize(QSize(18, 18));
    m_audioMuteButton->setToolTip(QStringLiteral("Mute / Unmute"));
    m_audioMuteButton->setFixedSize(30, 30);
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
    m_audioNowPlayingLabel->setMaximumWidth(160);
    m_audioNowPlayingLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    m_zoomFitButton = new QPushButton(QStringLiteral("Zoom Fit"));
    m_zoomFitButton->setObjectName(QStringLiteral("transport.zoom_fit"));
    m_zoomFitButton->setToolTip(QStringLiteral("Fit preview to canvas and recenter"));
    m_zoomFitButton->setText(QStringLiteral("Fit"));
    m_zoomFitButton->setFixedSize(38, 30);
    m_zoomFitButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    // Common transport order: first, previous frame, play/pause, next frame, last.
    transportLayout->addWidget(m_startButton);
    transportLayout->addWidget(m_prevFrameButton);
    transportLayout->addWidget(m_playButton);
    transportLayout->addWidget(m_nextFrameButton);
    transportLayout->addWidget(m_endButton);
    transportLayout->addSpacing(4);
    transportLayout->addWidget(m_razorButton);
    transportLayout->addWidget(m_seekSlider, 1);
    transportLayout->addWidget(m_timecodeLabel);
    transportLayout->addWidget(m_playbackSpeedCombo);
    transportLayout->addWidget(m_loopButton);
    transportLayout->addWidget(m_previewModeCombo);
    transportLayout->addWidget(m_audioToolsButton);
    transportLayout->addWidget(m_audioMuteButton);
    transportLayout->addWidget(m_audioVolumeSlider);
    transportLayout->addWidget(m_audioNowPlayingLabel);
    transportLayout->addWidget(m_zoomFitButton);

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
    connect(m_zoomFitButton, &QPushButton::clicked, this, &EditorPane::zoomFitClicked);
    connect(m_audioToolsButton, &QToolButton::clicked, this, &EditorPane::audioToolsClicked);
    connect(m_loopButton, &QToolButton::toggled, this, &EditorPane::playbackLoopToggled);
    connect(m_audioMuteButton, &QToolButton::clicked, this, &EditorPane::audioMuteClicked);
    connect(m_audioVolumeSlider, &QSlider::valueChanged, this, &EditorPane::audioVolumeChanged);

    auto* glowFilter = new TransportGlowFilter(this);
    for (QAbstractButton* btn : {static_cast<QAbstractButton*>(m_startButton),
                                 static_cast<QAbstractButton*>(m_prevFrameButton),
                                 static_cast<QAbstractButton*>(m_playButton),
                                 static_cast<QAbstractButton*>(m_nextFrameButton),
                                 static_cast<QAbstractButton*>(m_endButton),
                                 static_cast<QAbstractButton*>(m_audioToolsButton),
                                 static_cast<QAbstractButton*>(m_loopButton),
                                 static_cast<QAbstractButton*>(m_audioMuteButton),
                                 static_cast<QAbstractButton*>(m_razorButton),
                                 static_cast<QAbstractButton*>(m_zoomFitButton)}) {
        if (btn) {
            btn->setAttribute(Qt::WA_Hover, true);
            btn->installEventFilter(glowFilter);
        }
    }

    updateTransportDensity(transportWidget->width());
}

bool EditorPane::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Resize &&
        watched &&
        watched->objectName() == QStringLiteral("timeline.transport")) {
        auto *transportWidget = qobject_cast<QWidget*>(watched);
        updateTransportDensity(transportWidget ? transportWidget->width() : -1);
    }
    return QWidget::eventFilter(watched, event);
}

void EditorPane::updateTransportDensity(int transportWidth)
{
    if (transportWidth < 0 && m_timelineContainer) {
        if (auto *transportWidget =
                m_timelineContainer->findChild<QWidget*>(QStringLiteral("timeline.transport"))) {
            transportWidth = transportWidget->width();
        }
    }
    if (transportWidth <= 0) {
        return;
    }

    const bool showAudioStatus = transportWidth >= 760;
    const bool showAudioTools = transportWidth >= 700;
    const bool showAudioVolume = transportWidth >= 620;
    const bool showPreviewMode = transportWidth >= 560;
    const bool showPlaybackSpeed = transportWidth >= 500;
    const bool showLoop = transportWidth >= 460;
    const bool showMute = transportWidth >= 360;
    const bool showRazor = transportWidth >= 300;
    const bool showStepButtons = transportWidth >= 240;
    const bool showEdgeButtons = transportWidth >= 210;
    const bool showTimecode = transportWidth >= 180;

    if (m_startButton) m_startButton->setVisible(showEdgeButtons);
    if (m_endButton) m_endButton->setVisible(showEdgeButtons);
    if (m_prevFrameButton) m_prevFrameButton->setVisible(showStepButtons);
    if (m_nextFrameButton) m_nextFrameButton->setVisible(showStepButtons);
    if (m_razorButton) m_razorButton->setVisible(showRazor);
    if (m_audioNowPlayingLabel) m_audioNowPlayingLabel->setVisible(showAudioStatus);
    if (m_audioToolsButton) m_audioToolsButton->setVisible(showAudioTools);
    if (m_audioVolumeSlider) m_audioVolumeSlider->setVisible(showAudioVolume);
    if (m_previewModeCombo) m_previewModeCombo->setVisible(showPreviewMode);
    if (m_playbackSpeedCombo) m_playbackSpeedCombo->setVisible(showPlaybackSpeed);
    if (m_loopButton) m_loopButton->setVisible(showLoop);
    if (m_audioMuteButton) m_audioMuteButton->setVisible(showMute);
    if (m_timecodeLabel) m_timecodeLabel->setVisible(showTimecode);

    if (m_seekSlider) {
        m_seekSlider->setMinimumWidth(transportWidth >= 420 ? 120 : (transportWidth >= 220 ? 96 : 48));
    }
}

// Accessor to get the TimelineWidget from the container
TimelineWidget *EditorPane::timelineWidget() const
{
    return m_timelineContainer ? m_timelineContainer->timeline() : nullptr;
}
