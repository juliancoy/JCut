#include "inspector_pane.h"

#include "editor_timeline_types.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QVBoxLayout *createTabLayout(QWidget *page)
{
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    return layout;
}

void styleSectionTitle(QLabel *label)
{
    label->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
}

void styleSectionHelp(QLabel *label)
{
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
}

struct DisclosureSection {
    QWidget* container = nullptr;
    QVBoxLayout* body = nullptr;
};

DisclosureSection createDisclosureSection(QWidget* parent,
                                          const QString& title,
                                          bool expanded = true)
{
    DisclosureSection section;
    auto* container = new QWidget(parent);
    auto* outer = new QVBoxLayout(container);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(4);

    auto* toggle = new QToolButton(container);
    toggle->setCheckable(true);
    toggle->setChecked(expanded);
    toggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toggle->setText(QStringLiteral("%1 %2").arg(expanded ? QStringLiteral("▼") : QStringLiteral("▶"), title));
    toggle->setStyleSheet(QStringLiteral(
        "QToolButton { color: #9fb3c8; font-weight: 700; border: none; text-align: left; padding: 2px 0; }"
        "QToolButton:hover { color: #d6dee8; }"));
    outer->addWidget(toggle);

    auto* content = new QWidget(container);
    content->setVisible(expanded);
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(14, 2, 0, 2);
    body->setSpacing(6);
    outer->addWidget(content);

    QObject::connect(toggle, &QToolButton::toggled, content, [toggle, title, content](bool checked) {
        toggle->setText(QStringLiteral("%1 %2").arg(checked ? QStringLiteral("▼") : QStringLiteral("▶"), title));
        content->setVisible(checked);
    });

    section.container = container;
    section.body = body;
    return section;
}

QPair<QFrame*, QVBoxLayout*> createSectionFrame(QWidget *parent, const QString& objectName)
{
    auto *frame = new QFrame(parent);
    frame->setObjectName(objectName);
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setStyleSheet(QStringLiteral(
        "QFrame#%1 {"
        "  border: 1px solid #314459;"
        "  border-radius: 10px;"
        "  background: #112033;"
        "}"
        "QFrame#%1 QLabel { color: #d8e6f5; }")
                             .arg(objectName));
    auto *frameLayout = new QVBoxLayout(frame);
    frameLayout->setContentsMargins(12, 12, 12, 12);
    frameLayout->setSpacing(8);
    return qMakePair(frame, frameLayout);
}

QPushButton *createChipButton(const QString& text, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setCheckable(true);
    button->setMinimumHeight(30);
    button->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background:#1a2e45;"
        "  color:#d8e6f5;"
        "  border:1px solid #35506c;"
        "  border-radius:15px;"
        "  padding:4px 10px;"
        "  text-align:left;"
        "}"
        "QPushButton:checked {"
        "  background:#1f5d8c;"
        "  border-color:#4ea1ff;"
        "}"
        "QPushButton:disabled {"
        "  color:#7f8b99;"
        "  border-color:#294055;"
        "  background:#152334;"
        "}"));
    return button;
}

} // namespace

QWidget *InspectorPane::buildSpeakersContinuityTab(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *facedetectionsActionGrid = new QGridLayout;
    facedetectionsActionGrid->setContentsMargins(0, 0, 0, 0);
    facedetectionsActionGrid->setHorizontalSpacing(6);
    facedetectionsActionGrid->setVerticalSpacing(6);
    m_speakerRunAutoTrackButton = new QPushButton(QStringLiteral("Generate Continuity Tracks"), page);
    m_speakerRunAutoTrackButton->setObjectName(QStringLiteral("speakers.generate_facedetections"));
    m_speakerRunAutoTrackButton->setMinimumHeight(32);
    m_speakerRunAutoTrackButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_speakerViewFacestreamButton = new QPushButton(QStringLiteral("View Continuity Artifact"), page);
    m_speakerViewFacestreamButton->setObjectName(QStringLiteral("speakers.view_facedetections"));
    m_speakerViewFacestreamButton->setMinimumHeight(32);
    m_speakerViewFacestreamButton->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_speakerFacestreamSettingsButton = new QPushButton(QStringLiteral("Continuity Track Tools"), page);
    m_speakerFacestreamSettingsButton->setObjectName(QStringLiteral("speakers.facedetections_settings"));
    m_speakerFacestreamSettingsButton->setMinimumHeight(32);
    m_speakerFacestreamSettingsButton->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_speakerRefreshTrackAvatarsButton = new QPushButton(QStringLiteral("Refresh Track Avatars"), page);
    m_speakerRefreshTrackAvatarsButton->setObjectName(QStringLiteral("speakers.refresh_track_avatars"));
    m_speakerRefreshTrackAvatarsButton->setMinimumHeight(32);
    m_speakerRefreshTrackAvatarsButton->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_speakerGuideButton = new QPushButton(QStringLiteral("Open Workflow Guide"), page);
    facedetectionsActionGrid->addWidget(m_speakerRunAutoTrackButton, 0, 0);
    facedetectionsActionGrid->addWidget(m_speakerViewFacestreamButton, 0, 1);
    facedetectionsActionGrid->addWidget(m_speakerFacestreamSettingsButton, 1, 0);
    facedetectionsActionGrid->addWidget(m_speakerRefreshTrackAvatarsButton, 1, 1);
    facedetectionsActionGrid->setColumnStretch(0, 1);
    facedetectionsActionGrid->setColumnStretch(1, 1);

    m_speakerShowFaceDetectionsBoxesCheckBox =
        new QCheckBox(QStringLiteral("Show Continuity Tracks in Preview"), page);
    m_speakerShowFaceDetectionsBoxesCheckBox->setChecked(true);
    m_speakerShowFaceDetectionsBoxesCheckBox->setToolTip(
        QStringLiteral("Draw generated continuity-track boxes for active clips in Preview."));
    m_speakerShowRawDetectionsCheckBox =
        new QCheckBox(QStringLiteral("Show Raw Detections in Preview"), page);
    m_speakerShowRawDetectionsCheckBox->setChecked(false);
    m_speakerShowRawDetectionsCheckBox->setToolTip(
        QStringLiteral("Draw raw detector observations for the current source frame in Preview."));

    auto *facedetectionsSection = new QWidget(page);
    facedetectionsSection->setObjectName(QStringLiteral("speakers.section.continuity"));
    auto *facedetectionsLayout = createTabLayout(facedetectionsSection);
    auto *facedetectionsStageTitle = new QLabel(QStringLiteral("Continuity Tracks"), facedetectionsSection);
    styleSectionTitle(facedetectionsStageTitle);
    auto *facedetectionsStageHelp = new QLabel(
        QStringLiteral("Generate clip-wide raw detections and continuity tracks, then review the resulting track set."),
        facedetectionsSection);
    styleSectionHelp(facedetectionsStageHelp);
    auto *facedetectionsPathsTitle = new QLabel(QStringLiteral("Continuity Track Results"), facedetectionsSection);
    styleSectionTitle(facedetectionsPathsTitle);
    auto *facedetectionsPathsHelp = new QLabel(
        QStringLiteral("Inspect the generated continuity tracks here. Raw detector observations are shown separately below at the current playhead frame."),
        facedetectionsSection);
    styleSectionHelp(facedetectionsPathsHelp);
    m_speakerFaceDetectionsTable = new QTableWidget(facedetectionsSection);
    m_speakerFaceDetectionsTable->setObjectName(QStringLiteral("speakers.continuity_table"));
    m_speakerFaceDetectionsTable->setColumnCount(5);
    m_speakerFaceDetectionsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Stream"),
         QStringLiteral("Track"),
         QStringLiteral("Frames"),
         QStringLiteral("Range"),
         QStringLiteral("Source")});
    m_speakerFaceDetectionsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_speakerFaceDetectionsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_speakerFaceDetectionsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_speakerFaceDetectionsTable->verticalHeader()->setVisible(false);
    m_speakerFaceDetectionsTable->horizontalHeader()->setStretchLastSection(true);
    m_speakerFaceDetectionsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_speakerFaceDetectionsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_speakerFaceDetectionsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_speakerFaceDetectionsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_speakerFaceDetectionsTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_speakerFaceDetectionsDetailsEdit = new QPlainTextEdit(facedetectionsSection);
    m_speakerFaceDetectionsDetailsEdit->setReadOnly(true);
    m_speakerFaceDetectionsDetailsEdit->setPlaceholderText(QStringLiteral("Select a continuity-track row to inspect full JSON."));
    m_speakerFaceDetectionsDetailsEdit->setMinimumHeight(160);
    auto *artifactStatusRow = new QHBoxLayout;
    artifactStatusRow->setContentsMargins(0, 0, 0, 0);
    artifactStatusRow->setSpacing(10);
    m_speakerDetectionsAvailableCheckBox =
        new QCheckBox(QStringLiteral("Raw detections available"), facedetectionsSection);
    m_speakerTracksAvailableCheckBox =
        new QCheckBox(QStringLiteral("Continuity tracks available"), facedetectionsSection);
    for (QCheckBox* checkBox : {m_speakerDetectionsAvailableCheckBox, m_speakerTracksAvailableCheckBox}) {
        checkBox->setEnabled(false);
    }
    artifactStatusRow->addWidget(m_speakerDetectionsAvailableCheckBox);
    artifactStatusRow->addWidget(m_speakerTracksAvailableCheckBox);
    artifactStatusRow->addStretch(1);
    facedetectionsLayout->addWidget(facedetectionsStageTitle);
    facedetectionsLayout->addWidget(facedetectionsStageHelp);
    facedetectionsLayout->addLayout(facedetectionsActionGrid);
    facedetectionsLayout->addWidget(facedetectionsPathsTitle);
    facedetectionsLayout->addWidget(facedetectionsPathsHelp);
    facedetectionsLayout->addLayout(artifactStatusRow);
    facedetectionsLayout->addWidget(m_speakerShowFaceDetectionsBoxesCheckBox);
    facedetectionsLayout->addWidget(m_speakerShowRawDetectionsCheckBox);
    facedetectionsLayout->addWidget(m_speakerFaceDetectionsTable, 1);
    facedetectionsLayout->addWidget(m_speakerFaceDetectionsDetailsEdit);
    auto *rawDetectionsTitle = new QLabel(QStringLiteral("Raw Detections At Current Frame"), facedetectionsSection);
    styleSectionTitle(rawDetectionsTitle);
    auto *rawDetectionsHelp = new QLabel(
        QStringLiteral("Shows raw detector observations for the selected clip at the current playhead source frame. This is separate from the continuity-track list above."),
        facedetectionsSection);
    styleSectionHelp(rawDetectionsHelp);
    m_speakerRawDetectionTable = new QTableWidget(facedetectionsSection);
    m_speakerRawDetectionTable->setColumnCount(6);
    m_speakerRawDetectionTable->setHorizontalHeaderLabels(
        {QStringLiteral("#"),
         QStringLiteral("Conf"),
         QStringLiteral("X"),
         QStringLiteral("Y"),
         QStringLiteral("W"),
         QStringLiteral("H")});
    m_speakerRawDetectionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_speakerRawDetectionTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_speakerRawDetectionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_speakerRawDetectionTable->verticalHeader()->setVisible(false);
    m_speakerRawDetectionTable->horizontalHeader()->setStretchLastSection(false);
    for (int column = 0; column < 6; ++column) {
        m_speakerRawDetectionTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    m_speakerRawDetectionDetailsEdit = new QPlainTextEdit(facedetectionsSection);
    m_speakerRawDetectionDetailsEdit->setReadOnly(true);
    m_speakerRawDetectionDetailsEdit->setPlaceholderText(
        QStringLiteral("Select a detection row to inspect full JSON."));
    m_speakerRawDetectionDetailsEdit->setMinimumHeight(120);
    facedetectionsLayout->addWidget(rawDetectionsTitle);
    facedetectionsLayout->addWidget(rawDetectionsHelp);
    facedetectionsLayout->addWidget(m_speakerRawDetectionTable);
    facedetectionsLayout->addWidget(m_speakerRawDetectionDetailsEdit);

    auto continuitySection = createSectionFrame(page, QStringLiteral("speakers_continuity_section"));
    continuitySection.second->addWidget(facedetectionsSection);

    m_speakerRefsChipLabel = new QLabel(QStringLiteral("Assigned Tracks: 0"), page);
    m_speakerPointstreamChipLabel = new QLabel(QStringLiteral("Continuity Tracks: None"), page);
    m_speakerTrackingChipButton = createChipButton(QStringLiteral("Speaker Tracking: OFF"), page);
    m_speakerStabilizeChipButton = createChipButton(QStringLiteral("Face Stabilize: OFF"), page);
    m_speakerRefsChipLabel->setStyleSheet(QStringLiteral(
        "QLabel { border: 1px solid #35506c; border-radius: 14px; padding: 4px 10px; background: #162739; }"));
    m_speakerPointstreamChipLabel->setStyleSheet(QStringLiteral(
        "QLabel { border: 1px solid #35506c; border-radius: 14px; padding: 4px 10px; background: #162739; }"));

    m_speakerFramingZoomEnabledCheckBox = new QCheckBox(QStringLiteral("Enable Target Box"), page);
    m_speakerFramingTargetXSpin = new QDoubleSpinBox(page);
    m_speakerFramingTargetYSpin = new QDoubleSpinBox(page);
    m_speakerFramingTargetBoxSpin = new QDoubleSpinBox(page);
    m_speakerSectionRotationSpin = new QDoubleSpinBox(page);
    m_speakerFramingCenterSmoothingFramesSpin = new QSpinBox(page);
    m_speakerFramingZoomSmoothingFramesSpin = new QSpinBox(page);
    m_speakerFramingSmoothingModeCombo = new QComboBox(page);
    m_speakerFramingCenterSmoothingStrengthSpin = new QDoubleSpinBox(page);
    m_speakerFramingZoomSmoothingStrengthSpin = new QDoubleSpinBox(page);
    m_speakerFramingGapHoldFramesSpin = new QSpinBox(page);
    m_speakerApplyFramingToClipCheckBox = new QCheckBox(QStringLiteral("Apply Face Stabilize To Selected Clip"), page);
    m_speakerFramingEnabledKeyframeTable = new QTableWidget(page);
    m_speakerClipFramingStatusLabel = new QLabel(QStringLiteral("Face Stabilize: OFF | 0 keys"), page);
    m_speakerFramingZoomEnabledCheckBox->setObjectName(QStringLiteral("speakers.face_stabilize.target_box_enabled"));
    m_speakerFramingTargetXSpin->setObjectName(QStringLiteral("speakers.face_stabilize.target_x"));
    m_speakerFramingTargetYSpin->setObjectName(QStringLiteral("speakers.face_stabilize.target_y"));
    m_speakerFramingTargetBoxSpin->setObjectName(QStringLiteral("speakers.face_stabilize.target_box"));
    m_speakerSectionRotationSpin->setObjectName(QStringLiteral("speakers.face_stabilize.section_rotation"));
    m_speakerFramingCenterSmoothingFramesSpin->setObjectName(QStringLiteral("speakers.face_stabilize.center_smoothing_frames"));
    m_speakerFramingZoomSmoothingFramesSpin->setObjectName(QStringLiteral("speakers.face_stabilize.zoom_jitter_smoothing_frames"));
    m_speakerFramingSmoothingModeCombo->setObjectName(QStringLiteral("speakers.face_stabilize.smoothing_mode"));
    m_speakerFramingCenterSmoothingStrengthSpin->setObjectName(QStringLiteral("speakers.face_stabilize.center_smoothing_strength"));
    m_speakerFramingZoomSmoothingStrengthSpin->setObjectName(QStringLiteral("speakers.face_stabilize.zoom_jitter_smoothing_strength"));
    m_speakerFramingGapHoldFramesSpin->setObjectName(QStringLiteral("speakers.face_stabilize.gap_hold_frames"));
    m_speakerApplyFramingToClipCheckBox->setObjectName(QStringLiteral("speakers.face_stabilize.apply_to_clip"));
    m_speakerClipFramingStatusLabel->setObjectName(QStringLiteral("speakers.face_stabilize.status"));
    for (QDoubleSpinBox *spinBox :
         {m_speakerFramingTargetXSpin, m_speakerFramingTargetYSpin, m_speakerFramingTargetBoxSpin}) {
        spinBox->setDecimals(3);
        spinBox->setRange(0.0, 1.0);
        spinBox->setSingleStep(0.01);
    }
    m_speakerFramingTargetXSpin->setValue(0.5);
    m_speakerFramingTargetYSpin->setValue(0.35);
    m_speakerFramingTargetBoxSpin->setValue(0.20);
    m_speakerSectionRotationSpin->setDecimals(1);
    m_speakerSectionRotationSpin->setRange(-180.0, 180.0);
    m_speakerSectionRotationSpin->setSingleStep(0.5);
    m_speakerSectionRotationSpin->setSuffix(QStringLiteral(" deg"));
    m_speakerSectionRotationSpin->setValue(0.0);
    m_speakerSectionRotationSpin->setToolTip(
        QStringLiteral("Rotate the selected contiguous transcript section around its active face box."));
    for (QSpinBox* spinBox : {
             m_speakerFramingCenterSmoothingFramesSpin,
             m_speakerFramingZoomSmoothingFramesSpin}) {
        spinBox->setRange(0, TimelineClip::kSpeakerFramingSmoothingMaxFrames);
        spinBox->setSingleStep(1);
        spinBox->setSuffix(QStringLiteral(" frames"));
        spinBox->setValue(0);
    }
    m_speakerFramingGapHoldFramesSpin->setRange(0, TimelineClip::kSpeakerFramingGapHoldMaxFrames);
    m_speakerFramingGapHoldFramesSpin->setSingleStep(1);
    m_speakerFramingGapHoldFramesSpin->setSuffix(QStringLiteral(" frames"));
    m_speakerFramingGapHoldFramesSpin->setValue(0);
    m_speakerFramingCenterSmoothingFramesSpin->setToolTip(
        QStringLiteral("Average the active face center over this many frames centered on the current frame."));
    m_speakerFramingZoomSmoothingFramesSpin->setToolTip(
        QStringLiteral("Stabilize zoom by smoothing small FaceDetections box-size changes over this many frames. Increase this when tiny face-box resizes cause extreme zoom pumping."));
    m_speakerFramingSmoothingModeCombo->addItem(QStringLiteral("Robust"), 0);
    m_speakerFramingSmoothingModeCombo->addItem(QStringLiteral("Responsive"), 1);
    m_speakerFramingSmoothingModeCombo->addItem(QStringLiteral("Locked Down"), 2);
    m_speakerFramingSmoothingModeCombo->setToolTip(
        QStringLiteral("Choose the robust smoothing profile. Robust rejects outliers, Responsive follows more motion, Locked Down damps more jitter."));
    for (QDoubleSpinBox* spinBox : {
             m_speakerFramingCenterSmoothingStrengthSpin,
             m_speakerFramingZoomSmoothingStrengthSpin}) {
        spinBox->setDecimals(2);
        spinBox->setRange(0.0, TimelineClip::kSpeakerFramingSmoothingStrengthMax);
        spinBox->setSingleStep(0.05);
        spinBox->setValue(1.0);
    }
    m_speakerFramingCenterSmoothingStrengthSpin->setToolTip(
        QStringLiteral("Pan/center smoothing amount only. 0.0 is raw, 1.0 is normal robust smoothing, higher values approach the stable path without overshooting."));
    m_speakerFramingZoomSmoothingStrengthSpin->setToolTip(
        QStringLiteral("Zoom jitter damping amount. 0.0 is raw, 1.0 is normal robust smoothing, higher values hold closer to the stable face-box size without overshooting."));
    m_speakerFramingGapHoldFramesSpin->setToolTip(
        QStringLiteral("Keep using a nearby assigned-track sample across missing detections up to this many frames."));
    m_speakerFramingEnabledKeyframeTable->setObjectName(QStringLiteral("speakers.face_stabilize_keyframes"));
    m_speakerFramingEnabledKeyframeTable->setColumnCount(5);
    m_speakerFramingEnabledKeyframeTable->setHorizontalHeaderLabels(
        {QStringLiteral("Frame"),
         QStringLiteral("Face Stabilize"),
         QStringLiteral("Target X"),
         QStringLiteral("Target Y"),
         QStringLiteral("Target Box")});
    m_speakerFramingEnabledKeyframeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_speakerFramingEnabledKeyframeTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_speakerFramingEnabledKeyframeTable->verticalHeader()->setVisible(false);
    m_speakerFramingEnabledKeyframeTable->horizontalHeader()->setStretchLastSection(false);
    m_speakerFramingEnabledKeyframeTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_speakerFramingEnabledKeyframeTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_speakerFramingEnabledKeyframeTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_speakerFramingEnabledKeyframeTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_speakerFramingEnabledKeyframeTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_speakerFramingEnabledKeyframeTable->setMinimumHeight(120);
    m_speakerClipFramingStatusLabel->setWordWrap(true);

    auto *chipRow = new QHBoxLayout;
    chipRow->setContentsMargins(0, 0, 0, 0);
    chipRow->setSpacing(8);
    chipRow->addWidget(m_speakerRefsChipLabel);
    chipRow->addWidget(m_speakerPointstreamChipLabel);
    chipRow->addWidget(m_speakerTrackingChipButton);
    chipRow->addWidget(m_speakerStabilizeChipButton);
    chipRow->addStretch(1);

    auto *framingTargetHelp = new QLabel(
        QStringLiteral("Set the preview target position that generated face-stabilize transforms should resolve toward."),
        page);
    styleSectionHelp(framingTargetHelp);
    auto *framingForm = new QFormLayout;
    framingForm->setContentsMargins(0, 0, 0, 0);
    framingForm->setHorizontalSpacing(8);
    framingForm->setVerticalSpacing(6);

    auto* targetXRow = new QWidget(page);
    auto* targetXLayout = new QHBoxLayout(targetXRow);
    targetXLayout->setContentsMargins(0, 0, 0, 0);
    targetXLayout->setSpacing(6);
    auto* centerTargetXButton = new QPushButton(QStringLiteral("Center"), targetXRow);
    centerTargetXButton->setToolTip(QStringLiteral("Set Target X to the horizontal center (0.5)."));
    centerTargetXButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    targetXLayout->addWidget(m_speakerFramingTargetXSpin, 1);
    targetXLayout->addWidget(centerTargetXButton, 0);

    auto* targetYRow = new QWidget(page);
    auto* targetYLayout = new QHBoxLayout(targetYRow);
    targetYLayout->setContentsMargins(0, 0, 0, 0);
    targetYLayout->setSpacing(6);
    auto* centerTargetYButton = new QPushButton(QStringLiteral("Center"), targetYRow);
    centerTargetYButton->setToolTip(QStringLiteral("Set Target Y to the vertical center (0.5)."));
    centerTargetYButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    targetYLayout->addWidget(m_speakerFramingTargetYSpin, 1);
    targetYLayout->addWidget(centerTargetYButton, 0);

    connect(centerTargetXButton, &QPushButton::clicked, page, [this]() {
        if (m_speakerFramingTargetXSpin) {
            m_speakerFramingTargetXSpin->setValue(0.5);
        }
    });
    connect(centerTargetYButton, &QPushButton::clicked, page, [this]() {
        if (m_speakerFramingTargetYSpin) {
            m_speakerFramingTargetYSpin->setValue(0.5);
        }
    });

    framingForm->addRow(QStringLiteral("Target X"), targetXRow);
    framingForm->addRow(QStringLiteral("Target Y"), targetYRow);
    framingForm->addRow(QStringLiteral("Target Box"), m_speakerFramingTargetBoxSpin);
    framingForm->addRow(QStringLiteral("Section Rotation"), m_speakerSectionRotationSpin);
    framingForm->addRow(QStringLiteral("Center Smoothing"), m_speakerFramingCenterSmoothingFramesSpin);
    framingForm->addRow(QStringLiteral("Zoom Jitter Smoothing"), m_speakerFramingZoomSmoothingFramesSpin);
    framingForm->addRow(QStringLiteral("Smoothing Mode"), m_speakerFramingSmoothingModeCombo);
    framingForm->addRow(QStringLiteral("Pan Strength"), m_speakerFramingCenterSmoothingStrengthSpin);
    framingForm->addRow(QStringLiteral("Zoom Jitter Strength"), m_speakerFramingZoomSmoothingStrengthSpin);
    framingForm->addRow(QStringLiteral("Gap Hold"), m_speakerFramingGapHoldFramesSpin);

    auto framingSection = createSectionFrame(page, QStringLiteral("speakers_framing_section"));
    auto *framingTitle = new QLabel(QStringLiteral("Framing"), page);
    styleSectionTitle(framingTitle);
    auto *framingHelp = new QLabel(
        QStringLiteral("Capture speaker references, bind clip framing, and tune face-stabilize targets. Use Zoom Jitter Smoothing when small face-box size changes cause large zoom jumps."),
        page);
    styleSectionHelp(framingHelp);
    framingSection.second->addWidget(framingTitle);
    framingSection.second->addWidget(framingHelp);
    framingSection.second->addLayout(chipRow);
    auto framingTargetDisclosure = createDisclosureSection(page, QStringLiteral("Target"), true);
    framingTargetDisclosure.body->addWidget(framingTargetHelp);
    framingTargetDisclosure.body->addWidget(m_speakerFramingZoomEnabledCheckBox);
    framingTargetDisclosure.body->addLayout(framingForm);
    framingSection.second->addWidget(framingTargetDisclosure.container);
    framingSection.second->addWidget(m_speakerApplyFramingToClipCheckBox);
    framingSection.second->addWidget(m_speakerFramingEnabledKeyframeTable);
    framingSection.second->addWidget(m_speakerClipFramingStatusLabel);

    layout->addWidget(continuitySection.first);
    layout->addWidget(framingSection.first);
    layout->addStretch(1);
    return page;
}
