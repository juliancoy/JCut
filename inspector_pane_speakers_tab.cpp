#include "inspector_pane.h"
#include "inspector_pane_tab_helpers.h"
#include "editor_shared.h"
#include "editor_effect_presets.h"
#include "speakers_table.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <QToolButton>
#include <QVBoxLayout>

using namespace jcut::inspector;

QWidget *InspectorPane::buildSpeakersTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Speakers"), page));
    m_speakersSubtabs = nullptr;
    auto *scrollArea = new QScrollArea(page);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget(scrollArea);
    content->setObjectName(QStringLiteral("speakers.combined_content"));
    content->setMinimumWidth(0);
    auto *mappingLayout = createTabLayout(content);
    auto createSectionFrame = [](QWidget *parent, const QString& objectName) {
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
    };
    auto styleSectionTitle = [](QLabel *label) {
        label->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
    };
    auto styleSectionHelp = [](QLabel *label) {
        label->setWordWrap(true);
        label->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
    };
    m_speakersInspectorClipLabel = new QLabel(QStringLiteral("No transcript cut selected"), page);
    m_speakersInspectorDetailsLabel = new QLabel(QString(), page);
    m_speakersInspectorDetailsLabel->setWordWrap(true);

    m_speakersTable = new SpeakersTable(page);
    m_speakersTable->setObjectName(QStringLiteral("speakers.roster"));
    m_speakersTable->setColumnCount(7);
    m_speakersTable->setHorizontalHeaderLabels(
        {QStringLiteral("Avatar"),
         QStringLiteral("Speaker"),
         QStringLiteral("Organization"),
         QStringLiteral("X"),
         QStringLiteral("Y"),
         QStringLiteral("Assigned Tracks"),
         QStringLiteral("+")});
    m_speakersTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_speakersTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_speakersTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                     QAbstractItemView::EditKeyPressed);
    m_speakersTable->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    m_speakersTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_speakersTable->setMinimumHeight(0);
    m_speakersTable->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_speakersTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_speakersTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_speakersTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_speakersTable->verticalHeader()->setVisible(false);
    m_speakersTable->horizontalHeader()->setStretchLastSection(false);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_speakersTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_speakerHideUnidentifiedCheckBox =
        new QCheckBox(QStringLiteral("Hide Unidentified"), page);
    m_speakerHideUnidentifiedCheckBox->setChecked(false);
    m_speakerHideUnidentifiedCheckBox->setToolTip(
        QStringLiteral("Hide speaker roster rows that do not have an identified profile name."));
    m_speakerShowContiguousSectionsCheckBox =
        new QCheckBox(QStringLiteral("Transcript Sections"), page);
    m_speakerShowContiguousSectionsCheckBox->setChecked(false);
    m_speakerShowContiguousSectionsCheckBox->hide();
    m_speakerShowContiguousSectionsCheckBox->setToolTip(
        QStringLiteral("Switch assignment rows from speakers to transcript-ordered contiguous sections."));
    m_speakerApplyTrackToAllMatchingSectionsCheckBox =
        new QCheckBox(QStringLiteral("Apply Track To All Matching Sections"), page);
    m_speakerApplyTrackToAllMatchingSectionsCheckBox->setChecked(false);
    m_speakerApplyTrackToAllMatchingSectionsCheckBox->hide();
    m_speakerApplyTrackToAllMatchingSectionsCheckBox->setToolTip(
        QStringLiteral("When enabled, a face-track click updates every matching contiguous transcript section at the track time. Off by default; otherwise only the active playhead section is updated."));
    m_speakerSectionMinimumWordsSpin = new QSpinBox(page);
    m_speakerSectionMinimumWordsSpin->setRange(0, 1000);
    m_speakerSectionMinimumWordsSpin->setValue(10);
    m_speakerSectionMinimumWordsSpin->setPrefix(QStringLiteral("Min words "));
    m_speakerSectionMinimumWordsSpin->setToolTip(
        QStringLiteral("Hide contiguous transcript sections below this word count in the table and playback."));
    m_speakerSectionMinimumWordsSpin->setMinimumWidth(128);
    m_speakerSectionMinimumWordsSpin->hide();
    m_speakerExportLongSectionsButton = new QPushButton(QStringLiteral("Export Sections"), page);
    m_speakerExportLongSectionsButton->setObjectName(QStringLiteral("speakers.export_long_sections"));
    m_speakerExportLongSectionsButton->setMinimumHeight(30);
    m_speakerExportLongSectionsButton->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_speakerExportLongSectionsButton->setEnabled(false);
    m_speakerCreateTitleClipsButton = new QPushButton(QStringLiteral("Apply lower-third fly-in"), page);
    m_speakerCreateTitleClipsButton->setObjectName(QStringLiteral("speakers.create_news_title_clips"));
    m_speakerCreateTitleClipsButton->setMinimumHeight(30);
    m_speakerCreateTitleClipsButton->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_speakerCreateTitleClipsButton->setEnabled(false);
    m_speakerCreateTitleClipsButton->setToolTip(
        QStringLiteral("Apply or refresh fly-in lower-third speaker titles on the selected source clip."));
    m_speakerOverlayCreateTitleClipsButton = new QCheckBox(QStringLiteral("Enable Animated Speaker Introductions"), page);
    m_speakerOverlayCreateTitleClipsButton->setObjectName(QStringLiteral("speakers.overlay_create_news_title_clips"));
    m_speakerOverlayCreateTitleClipsButton->setEnabled(false);
    m_speakerOverlayCreateTitleClipsButton->setToolTip(
        QStringLiteral("Generate transcript-linked title events for speaker introductions. Turn off to remove those child events."));
    m_speakerOverlayFlyInStyleCombo = new QComboBox(page);
    m_speakerOverlayFlyInStyleCombo->addItem(QStringLiteral("Slide from left"), static_cast<int>(SpeakerTitleFlyInStyle::SlideFromLeft));
    m_speakerOverlayFlyInStyleCombo->addItem(QStringLiteral("Slide from right"), static_cast<int>(SpeakerTitleFlyInStyle::SlideFromRight));
    m_speakerOverlayFlyInStyleCombo->addItem(QStringLiteral("Rise from bottom"), static_cast<int>(SpeakerTitleFlyInStyle::RiseFromBottom));
    m_speakerOverlayFlyInStyleCombo->addItem(QStringLiteral("Drop from top"), static_cast<int>(SpeakerTitleFlyInStyle::DropFromTop));
    m_speakerOverlayFlyInStyleCombo->addItem(QStringLiteral("3D wrap around speaker"), static_cast<int>(SpeakerTitleFlyInStyle::WrapAroundSpeaker));
    m_speakerOverlayFlyInStyleCombo->setToolTip(QStringLiteral("Choose how the generated speaker title flies into frame."));
    auto makeFlyInSecondsSpin = [page](double value, double min, double max, const QString& tooltip) {
        auto *spin = new QDoubleSpinBox(page);
        spin->setRange(min, max);
        spin->setDecimals(2);
        spin->setSingleStep(0.05);
        spin->setSuffix(QStringLiteral(" s"));
        spin->setValue(value);
        spin->setKeyboardTracking(false);
        spin->setToolTip(tooltip);
        return spin;
    };
    m_speakerOverlayFlyInDelaySpin = makeFlyInSecondsSpin(
        0.35,
        0.0,
        3.0,
        QStringLiteral("Delay after each speaker change before the title starts."));
    m_speakerOverlayFlyInDurationSpin = makeFlyInSecondsSpin(
        3.0,
        1.0,
        12.0,
        QStringLiteral("Total length of each speaker title fly-in."));
    m_speakerOverlayShowAtSectionEndCheckBox =
        new QCheckBox(QStringLiteral("Also show near section end"), page);
    m_speakerOverlayShowAtSectionEndCheckBox->setToolTip(
        QStringLiteral("Generate another title ending with each contiguous speaker section."));
    m_speakerOverlayRespectSpeechFilterTimingCheckBox =
        new QCheckBox(QStringLiteral("Respect Speech Filter Timing"), page);
    m_speakerOverlayRespectSpeechFilterTimingCheckBox->setChecked(true);
    m_speakerOverlayRespectSpeechFilterTimingCheckBox->setToolTip(
        QStringLiteral("Advance fly-in and fly-out animation in playable Speech Filter time, without jumping across removed gaps."));
    m_speakerOverlayCadenceSpin = makeFlyInSecondsSpin(
        0.0,
        0.0,
        3600.0,
        QStringLiteral("Repeat at this cadence from the source clip start; set to Off to disable."));
    m_speakerOverlayCadenceSpin->setSpecialValueText(QStringLiteral("Off"));
    m_speakerOverlayCadenceSpin->setSingleStep(5.0);
    m_speakerOverlayFlyInTimeSpin = makeFlyInSecondsSpin(
        0.35,
        0.10,
        2.0,
        QStringLiteral("Time spent flying in and flying out."));
    auto makeWrapSpin = [page](double value, double min, double max, const QString& tooltip) {
        auto *spin = new QDoubleSpinBox(page);
        spin->setRange(min, max);
        spin->setDecimals(2);
        spin->setSingleStep(0.05);
        spin->setValue(value);
        spin->setKeyboardTracking(false);
        spin->setToolTip(tooltip);
        return spin;
    };
    m_speakerOverlayWrapRadiusSpin = makeWrapSpin(
        1.05,
        0.24,
        1.80,
        QStringLiteral("Horizontal orbit radius around the centered speaker mask."));
    m_speakerOverlayWrapDepthSpin = makeWrapSpin(
        0.70,
        0.0,
        1.0,
        QStringLiteral("How strongly the title shrinks and fades as it passes behind the centered speaker mask."));
    auto makeWrapAngleSpin = [page](double value, double min, double max, const QString& tooltip) {
        auto *spin = new QDoubleSpinBox(page);
        spin->setRange(min, max);
        spin->setDecimals(1);
        spin->setSingleStep(5.0);
        spin->setSuffix(QStringLiteral(" deg"));
        spin->setValue(value);
        spin->setKeyboardTracking(false);
        spin->setToolTip(tooltip);
        return spin;
    };
    m_speakerOverlayWrapStartAngleSpin = makeWrapAngleSpin(
        -110.0,
        -720.0,
        720.0,
        QStringLiteral("Start angle of the 3D orbit around the centered speaker mask."));
    m_speakerOverlayWrapEndAngleSpin = makeWrapAngleSpin(
        110.0,
        -720.0,
        720.0,
        QStringLiteral("End angle of the 3D orbit around the centered speaker mask."));
    m_speakerOverlayWrapPitchSpin = makeWrapAngleSpin(
        8.0,
        -80.0,
        80.0,
        QStringLiteral("Tilt the orbit path forward or backward in 3D space."));
    m_speakerOverlayWrapRollSpin = makeWrapAngleSpin(
        0.0,
        -180.0,
        180.0,
        QStringLiteral("Roll the projected orbit path clockwise or counter-clockwise."));
    m_speakerOverlayRotationXSpin = makeWrapAngleSpin(
        0.0, -720.0, 720.0,
        QStringLiteral("Initial X-axis rotation, animated to zero as the title settles."));
    m_speakerOverlayRotationYSpin = makeWrapAngleSpin(
        0.0, -720.0, 720.0,
        QStringLiteral("Initial Y-axis rotation, animated to zero as the title settles."));
    m_speakerOverlayRotationZSpin = makeWrapAngleSpin(
        0.0, -720.0, 720.0,
        QStringLiteral("Initial Z-axis rotation, animated to zero as the title settles."));
    m_speakerOverlayTitleFontSizeSpin = new QSpinBox(page);
    m_speakerOverlayTitleFontSizeSpin->setRange(12, 220);
    m_speakerOverlayTitleFontSizeSpin->setSingleStep(2);
    m_speakerOverlayTitleFontSizeSpin->setSuffix(QStringLiteral(" px"));
    m_speakerOverlayTitleFontSizeSpin->setValue(48);
    m_speakerOverlayTitleFontSizeSpin->setToolTip(
        QStringLiteral("Set the generated speaker title text size independently from the title box width."));
    m_speakerOverlayTitleAutoFitCheckBox = new QCheckBox(
        QStringLiteral("Auto-size Title to Safe Area"), page);
    m_speakerOverlayTitleAutoFitCheckBox->setChecked(true);
    m_speakerOverlayTitleAutoFitCheckBox->setToolTip(
        QStringLiteral("Reduce long speaker titles as needed so text and 3D geometry fit within the output safe area."));
    m_speakerOverlayTitleBoxWidthSpin = new QSpinBox(page);
    m_speakerOverlayTitleBoxWidthSpin->setRange(0, 4000);
    m_speakerOverlayTitleBoxWidthSpin->setSingleStep(20);
    m_speakerOverlayTitleBoxWidthSpin->setSuffix(QStringLiteral(" px"));
    m_speakerOverlayTitleBoxWidthSpin->setSpecialValueText(QStringLiteral("Auto"));
    m_speakerOverlayTitleBoxWidthSpin->setValue(720);
    m_speakerOverlayTitleBoxWidthSpin->setToolTip(
        QStringLiteral("Set the generated speaker title background width. Auto fits the text."));
    auto makeTitleMaterialCombo = [page]() {
        auto *combo = new QComboBox(page);
        using MaterialStyle = TimelineClip::TitleKeyframe::MaterialStyle;
        combo->addItem(QStringLiteral("Solid"), static_cast<int>(MaterialStyle::Solid));
        combo->addItem(QStringLiteral("Neon glow"), static_cast<int>(MaterialStyle::Neon));
        combo->addItem(QStringLiteral("Diagonal stripes"), static_cast<int>(MaterialStyle::DiagonalStripes));
        combo->addItem(QStringLiteral("Grid"), static_cast<int>(MaterialStyle::Grid));
        combo->addItem(QStringLiteral("Image pattern"), static_cast<int>(MaterialStyle::ImagePattern));
        combo->setToolTip(QStringLiteral("Choose the procedural material style applied to generated speaker titles."));
        return combo;
    };
    m_speakerOverlayTitleTextMaterialCombo = makeTitleMaterialCombo();
    m_speakerOverlayTitleBorderMaterialCombo = makeTitleMaterialCombo();
    m_speakerOverlayTitleBorderMaterialCombo->setCurrentIndex(1);
    m_speakerOverlayTitleTextPatternPathEdit = new QLineEdit(page);
    m_speakerOverlayTitleTextPatternPathEdit->setPlaceholderText(QStringLiteral("Optional image path"));
    m_speakerOverlayTitleTextPatternPathEdit->setToolTip(
        QStringLiteral("Use this image as the title text material when Image pattern is selected."));
    m_speakerOverlayTitleBorderPatternPathEdit = new QLineEdit(page);
    m_speakerOverlayTitleBorderPatternPathEdit->setPlaceholderText(QStringLiteral("Optional image path"));
    m_speakerOverlayTitleBorderPatternPathEdit->setToolTip(
        QStringLiteral("Use this image as the border material when Image pattern is selected."));
    m_speakerOverlayTitlePatternScaleSpin = new QDoubleSpinBox(page);
    m_speakerOverlayTitlePatternScaleSpin->setRange(0.10, 8.0);
    m_speakerOverlayTitlePatternScaleSpin->setDecimals(2);
    m_speakerOverlayTitlePatternScaleSpin->setSingleStep(0.10);
    m_speakerOverlayTitlePatternScaleSpin->setValue(1.0);
    m_speakerOverlayTitlePatternScaleSpin->setKeyboardTracking(false);
    m_speakerOverlayTitlePatternScaleSpin->setToolTip(
        QStringLiteral("Scale procedural text and border patterns. Lower values make denser patterns."));
    m_speakerOverlayTitleExtrudeCheckBox = new QCheckBox(QStringLiteral("Extruded 3D Title Geometry"), page);
    m_speakerOverlayTitleExtrudeCheckBox->setChecked(false);
    m_speakerOverlayTitleExtrudeCheckBox->setToolTip(
        QStringLiteral("Mark generated wrap titles for the extruded 3D mesh pathway."));
    m_speakerOverlayTitleExtrudeModeCombo = new QComboBox(page);
    m_speakerOverlayTitleExtrudeModeCombo->addItem(
        QStringLiteral("Stacked Copies"),
        static_cast<int>(TimelineClip::TitleKeyframe::TextExtrudeMode::StackedCopies));
    m_speakerOverlayTitleExtrudeModeCombo->addItem(
        QStringLiteral("Eroded Solid"),
        static_cast<int>(TimelineClip::TitleKeyframe::TextExtrudeMode::ErodedSolid));
    m_speakerOverlayTitleExtrudeModeCombo->setCurrentIndex(1);
    m_speakerOverlayTitleExtrudeModeCombo->setToolTip(
        QStringLiteral("Choose separated text layers or a continuous eroded 3D sidewall."));
    m_speakerOverlayTitleExtrudeDepthSpin = new QDoubleSpinBox(page);
    m_speakerOverlayTitleExtrudeDepthSpin->setRange(0.02, 2.0);
    m_speakerOverlayTitleExtrudeDepthSpin->setDecimals(2);
    m_speakerOverlayTitleExtrudeDepthSpin->setSingleStep(0.02);
    m_speakerOverlayTitleExtrudeDepthSpin->setValue(0.16);
    m_speakerOverlayTitleExtrudeDepthSpin->setKeyboardTracking(false);
    m_speakerOverlayTitleBevelScaleSpin = new QDoubleSpinBox(page);
    m_speakerOverlayTitleBevelScaleSpin->setRange(0.0, 2.0);
    m_speakerOverlayTitleBevelScaleSpin->setDecimals(2);
    m_speakerOverlayTitleBevelScaleSpin->setSingleStep(0.05);
    m_speakerOverlayTitleBevelScaleSpin->setValue(0.70);
    m_speakerOverlayTitleBevelScaleSpin->setKeyboardTracking(false);
    m_speakerShowCurrentSpeakerNameCheckBox =
        new QCheckBox(QStringLiteral("Show Current Speaker Name at Bottom"), page);
    m_speakerShowCurrentSpeakerNameCheckBox->setChecked(false);
    m_speakerShowCurrentSpeakerNameCheckBox->setToolTip(
        QStringLiteral("Draw the active transcript speaker name at the bottom of the preview."));
    m_speakerShowCurrentSpeakerOrganizationCheckBox =
        new QCheckBox(QStringLiteral("Show Current Speaker Organization at Bottom"), page);
    m_speakerShowCurrentSpeakerOrganizationCheckBox->setChecked(false);
    m_speakerShowCurrentSpeakerOrganizationCheckBox->setToolTip(
        QStringLiteral("Draw the active speaker organization at the bottom of the preview when it is stored in the speaker profile."));
    m_speakerCurrentSpeakerNameTextSizeSpin = new QSpinBox(page);
    m_speakerCurrentSpeakerNameTextSizeSpin->setRange(25, 300);
    m_speakerCurrentSpeakerNameTextSizeSpin->setSingleStep(5);
    m_speakerCurrentSpeakerNameTextSizeSpin->setSuffix(QStringLiteral("%"));
    m_speakerCurrentSpeakerNameTextSizeSpin->setValue(100);
    m_speakerCurrentSpeakerNameTextSizeSpin->setToolTip(
        QStringLiteral("Scale the active speaker name drawn at the bottom of the preview."));
    m_speakerCurrentSpeakerOrganizationTextSizeSpin = new QSpinBox(page);
    m_speakerCurrentSpeakerOrganizationTextSizeSpin->setRange(25, 300);
    m_speakerCurrentSpeakerOrganizationTextSizeSpin->setSingleStep(5);
    m_speakerCurrentSpeakerOrganizationTextSizeSpin->setSuffix(QStringLiteral("%"));
    m_speakerCurrentSpeakerOrganizationTextSizeSpin->setValue(100);
    m_speakerCurrentSpeakerOrganizationTextSizeSpin->setToolTip(
        QStringLiteral("Scale the active speaker organization drawn at the bottom of the preview."));
    m_speakerCurrentSpeakerNameYPositionSpin = new QSpinBox(page);
    m_speakerCurrentSpeakerNameYPositionSpin->setRange(0, 100);
    m_speakerCurrentSpeakerNameYPositionSpin->setSingleStep(1);
    m_speakerCurrentSpeakerNameYPositionSpin->setSuffix(QStringLiteral("%"));
    m_speakerCurrentSpeakerNameYPositionSpin->setValue(86);
    m_speakerCurrentSpeakerNameYPositionSpin->setToolTip(
        QStringLiteral("Set the active speaker name vertical position in the preview. 0% is top; 100% is bottom."));
    m_speakerCurrentSpeakerOrganizationYPositionSpin = new QSpinBox(page);
    m_speakerCurrentSpeakerOrganizationYPositionSpin->setRange(0, 100);
    m_speakerCurrentSpeakerOrganizationYPositionSpin->setSingleStep(1);
    m_speakerCurrentSpeakerOrganizationYPositionSpin->setSuffix(QStringLiteral("%"));
    m_speakerCurrentSpeakerOrganizationYPositionSpin->setValue(93);
    m_speakerCurrentSpeakerOrganizationYPositionSpin->setToolTip(
        QStringLiteral("Set the active speaker organization vertical position in the preview. 0% is top; 100% is bottom."));
    auto makeSpeakerColorButton = [page](const QString& color, const QString& tooltip) {
        auto* button = new QPushButton(color, page);
        button->setMinimumHeight(24);
        button->setToolTip(tooltip);
        button->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; color: %2; "
                           "border: 1px solid #2e3b4a; border-radius: 4px; padding: 3px 8px; }")
                .arg(color,
                     QColor(color).lightness() > 128 ? QStringLiteral("#000000")
                                                     : QStringLiteral("#ffffff")));
        return button;
    };
    m_speakerCurrentSpeakerNameColorButton = makeSpeakerColorButton(
        QStringLiteral("#f4f8fc"),
        QStringLiteral("Set the active speaker name text color."));
    m_speakerCurrentSpeakerOrganizationColorButton = makeSpeakerColorButton(
        QStringLiteral("#b9d0e5"),
        QStringLiteral("Set the active speaker organization text color."));
    m_speakerCurrentSpeakerBackgroundColorButton = makeSpeakerColorButton(
        QStringLiteral("#080d14"),
        QStringLiteral("Set the active speaker label background color."));
    m_speakerCurrentSpeakerBackgroundVisibleCheckBox =
        new QCheckBox(QStringLiteral("Show Background Box"), page);
    m_speakerCurrentSpeakerBackgroundVisibleCheckBox->setChecked(true);
    m_speakerCurrentSpeakerBackgroundVisibleCheckBox->setToolTip(
        QStringLiteral("Turn off the speaker-title background and border while keeping the title text and text shadow."));
    m_speakerCurrentSpeakerBackgroundOpacitySpin = new QSpinBox(page);
    m_speakerCurrentSpeakerBackgroundOpacitySpin->setRange(0, 100);
    m_speakerCurrentSpeakerBackgroundOpacitySpin->setSuffix(QStringLiteral("%"));
    m_speakerCurrentSpeakerBackgroundOpacitySpin->setValue(75);
    m_speakerCurrentSpeakerBackgroundOpacitySpin->setToolTip(
        QStringLiteral("Set the active speaker label background opacity."));
    m_speakerCurrentSpeakerBorderColorButton = makeSpeakerColorButton(
        QStringLiteral("#e1ecf7"),
        QStringLiteral("Set the active speaker label border color."));
    m_speakerCurrentSpeakerBorderOpacitySpin = new QSpinBox(page);
    m_speakerCurrentSpeakerBorderOpacitySpin->setRange(0, 100);
    m_speakerCurrentSpeakerBorderOpacitySpin->setSuffix(QStringLiteral("%"));
    m_speakerCurrentSpeakerBorderOpacitySpin->setValue(47);
    m_speakerCurrentSpeakerBorderOpacitySpin->setToolTip(
        QStringLiteral("Set the active speaker label border opacity."));
    m_speakerCurrentSpeakerBackgroundRadiusSpin = new QSpinBox(page);
    m_speakerCurrentSpeakerBackgroundRadiusSpin->setRange(0, 128);
    m_speakerCurrentSpeakerBackgroundRadiusSpin->setSuffix(QStringLiteral(" px"));
    m_speakerCurrentSpeakerBackgroundRadiusSpin->setValue(14);
    m_speakerCurrentSpeakerBackgroundRadiusSpin->setToolTip(
        QStringLiteral("Set the active speaker label background corner radius."));
    m_speakerCurrentSpeakerBorderWidthSpin = new QSpinBox(page);
    m_speakerCurrentSpeakerBorderWidthSpin->setRange(0, 16);
    m_speakerCurrentSpeakerBorderWidthSpin->setSuffix(QStringLiteral(" px"));
    m_speakerCurrentSpeakerBorderWidthSpin->setValue(1);
    m_speakerCurrentSpeakerBorderWidthSpin->setToolTip(
        QStringLiteral("Set the active speaker label border width."));
    m_speakerCurrentSpeakerShadowCheckBox = new QCheckBox(QStringLiteral("Speaker Label Shadow"), page);
    m_speakerCurrentSpeakerShadowCheckBox->setChecked(true);
    m_speakerCurrentSpeakerShadowCheckBox->setToolTip(
        QStringLiteral("Draw the active speaker label text shadow."));
    m_speakerCurrentSpeakerShadowColorButton = makeSpeakerColorButton(
        QStringLiteral("#000000"),
        QStringLiteral("Set the active speaker label shadow color."));
    m_speakerCurrentSpeakerShadowOpacitySpin = new QSpinBox(page);
    m_speakerCurrentSpeakerShadowOpacitySpin->setRange(0, 100);
    m_speakerCurrentSpeakerShadowOpacitySpin->setSuffix(QStringLiteral("%"));
    m_speakerCurrentSpeakerShadowOpacitySpin->setValue(75);
    m_speakerCurrentSpeakerShadowOpacitySpin->setToolTip(
        QStringLiteral("Set the active speaker label shadow opacity."));
    m_speakerSectionsTable = new QTableWidget(page);
    m_speakerSectionsTable->setColumnCount(8);
    m_speakerSectionsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Avatar"),
         QStringLiteral("#"),
         QStringLiteral("Speaker"),
         QStringLiteral("Range"),
         QStringLiteral("Tracks"),
         QStringLiteral("Rotation"),
         QStringLiteral("Words"),
         QStringLiteral("Transcript")});
    m_speakerSectionsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_speakerSectionsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_speakerSectionsTable->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_speakerSectionsTable->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    m_speakerSectionsTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_speakerSectionsTable->setMinimumHeight(0);
    m_speakerSectionsTable->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_speakerSectionsTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_speakerSectionsTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_speakerSectionsTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_speakerSectionsTable->setWordWrap(false);
    m_speakerSectionsTable->setTextElideMode(Qt::ElideRight);
    m_speakerSectionsTable->verticalHeader()->setVisible(false);
    QHeaderView* sectionsHeader = m_speakerSectionsTable->horizontalHeader();
    sectionsHeader->setStretchLastSection(false);
    sectionsHeader->setMinimumSectionSize(36);
    sectionsHeader->setSectionResizeMode(QHeaderView::Interactive);
    sectionsHeader->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    sectionsHeader->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    sectionsHeader->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    sectionsHeader->setSectionResizeMode(7, QHeaderView::Stretch);
    m_speakerSectionsTable->setColumnWidth(2, 124);
    m_speakerSectionsTable->setColumnWidth(3, 116);
    m_speakerSectionsTable->setColumnWidth(4, 96);
    m_speakerSectionsTable->setColumnWidth(5, 96);
    m_speakerSectionsTable->horizontalHeaderItem(7)->setToolTip(
        QStringLiteral("Transcript excerpt; use the filter below to search visible sections."));
    m_speakerSectionsTable->setAccessibleName(QStringLiteral("Transcript sections"));

    auto *selectedSpeakerTitle = new QLabel(QStringLiteral("Selected Speaker"), page);
    styleSectionTitle(selectedSpeakerTitle);
    m_selectedSpeakerIdLabel = new QLabel(QStringLiteral("No speaker selected"), page);
    m_selectedSpeakerIdLabel->setObjectName(QStringLiteral("speakers.selected_speaker"));
    m_selectedSpeakerNameEdit = new QLineEdit(page);
    m_selectedSpeakerNameEdit->setObjectName(QStringLiteral("speakers.selected_speaker.name"));
    m_selectedSpeakerNameEdit->setPlaceholderText(QStringLiteral("Speaker name"));
    m_selectedSpeakerNameEdit->setClearButtonEnabled(true);
    m_selectedSpeakerNameEdit->setEnabled(false);
    m_selectedSpeakerOrganizationEdit = new QLineEdit(page);
    m_selectedSpeakerOrganizationEdit->setObjectName(QStringLiteral("speakers.selected_speaker.organization"));
    m_selectedSpeakerOrganizationEdit->setPlaceholderText(QStringLiteral("Organization"));
    m_selectedSpeakerOrganizationEdit->setClearButtonEnabled(true);
    m_selectedSpeakerOrganizationEdit->setEnabled(false);
    m_selectedSpeakerLogoPathEdit = new QLineEdit(page);
    m_selectedSpeakerLogoPathEdit->setObjectName(QStringLiteral("speakers.selected_speaker.logo_path"));
    m_selectedSpeakerLogoPathEdit->setPlaceholderText(QStringLiteral("Logo image path"));
    m_selectedSpeakerLogoPathEdit->setClearButtonEnabled(true);
    m_selectedSpeakerLogoPathEdit->setEnabled(false);
    auto makeSelectedSpeakerColorEdit = [page](const QString& objectName, const QString& placeholder) {
        auto *edit = new QLineEdit(page);
        edit->setObjectName(objectName);
        edit->setPlaceholderText(placeholder);
        edit->setClearButtonEnabled(true);
        edit->setMaxLength(9);
        edit->setEnabled(false);
        return edit;
    };
    m_selectedSpeakerPrimaryColorEdit = makeSelectedSpeakerColorEdit(
        QStringLiteral("speakers.selected_speaker.primary_color"),
        QStringLiteral("#f7fbff"));
    m_selectedSpeakerSecondaryColorEdit = makeSelectedSpeakerColorEdit(
        QStringLiteral("speakers.selected_speaker.secondary_color"),
        QStringLiteral("#07111d"));
    m_selectedSpeakerAccentColorEdit = makeSelectedSpeakerColorEdit(
        QStringLiteral("speakers.selected_speaker.accent_color"),
        QStringLiteral("#56c7ff"));
    m_selectedSpeakerGradingEnabledCheckBox = new QCheckBox(QStringLiteral("Enable speaker grading"), page);
    auto makeSpeakerGradeSpin = [page](double minimum, double maximum, double value, const QString& name) {
        auto* spin = new QDoubleSpinBox(page);
        spin->setObjectName(name);
        spin->setRange(minimum, maximum);
        spin->setDecimals(3);
        spin->setSingleStep(0.05);
        spin->setValue(value);
        spin->setEnabled(false);
        return spin;
    };
    m_selectedSpeakerBrightnessSpin = makeSpeakerGradeSpin(-2.0, 2.0, 0.0, QStringLiteral("speakers.selected_speaker.grading.brightness"));
    m_selectedSpeakerContrastSpin = makeSpeakerGradeSpin(0.0, 4.0, 1.0, QStringLiteral("speakers.selected_speaker.grading.contrast"));
    m_selectedSpeakerSaturationSpin = makeSpeakerGradeSpin(0.0, 4.0, 1.0, QStringLiteral("speakers.selected_speaker.grading.saturation"));
    auto *selectedSpeakerProfileForm = new QFormLayout;
    selectedSpeakerProfileForm->setContentsMargins(0, 0, 0, 0);
    selectedSpeakerProfileForm->setHorizontalSpacing(6);
    selectedSpeakerProfileForm->setVerticalSpacing(6);
    selectedSpeakerProfileForm->addRow(QStringLiteral("Name"), m_selectedSpeakerNameEdit);
    selectedSpeakerProfileForm->addRow(QStringLiteral("Organization"), m_selectedSpeakerOrganizationEdit);
    selectedSpeakerProfileForm->addRow(QStringLiteral("Logo"), m_selectedSpeakerLogoPathEdit);
    selectedSpeakerProfileForm->addRow(QStringLiteral("Primary Color"), m_selectedSpeakerPrimaryColorEdit);
    selectedSpeakerProfileForm->addRow(QStringLiteral("Secondary Color"), m_selectedSpeakerSecondaryColorEdit);
    selectedSpeakerProfileForm->addRow(QStringLiteral("Accent Color"), m_selectedSpeakerAccentColorEdit);
    selectedSpeakerProfileForm->addRow(QStringLiteral("Speaker Grade"), m_selectedSpeakerGradingEnabledCheckBox);
    selectedSpeakerProfileForm->addRow(QStringLiteral("Brightness +"), m_selectedSpeakerBrightnessSpin);
    selectedSpeakerProfileForm->addRow(QStringLiteral("Contrast ×"), m_selectedSpeakerContrastSpin);
    selectedSpeakerProfileForm->addRow(QStringLiteral("Saturation ×"), m_selectedSpeakerSaturationSpin);
    auto *selectedFaceDetectionsTitle = new QLabel(QStringLiteral("Assigned Tracks"), page);
    styleSectionTitle(selectedFaceDetectionsTitle);
    m_selectedSpeakerFaceDetectionsList = new QListWidget(page);
    m_selectedSpeakerFaceDetectionsList->setObjectName(QStringLiteral("speakers.assigned_tracks"));
    m_selectedSpeakerFaceDetectionsList->setViewMode(QListView::IconMode);
    m_selectedSpeakerFaceDetectionsList->setFlow(QListView::LeftToRight);
    m_selectedSpeakerFaceDetectionsList->setWrapping(true);
    m_selectedSpeakerFaceDetectionsList->setResizeMode(QListView::Adjust);
    m_selectedSpeakerFaceDetectionsList->setMovement(QListView::Static);
    m_selectedSpeakerFaceDetectionsList->setSpacing(8);
    m_selectedSpeakerFaceDetectionsList->setIconSize(QSize(72, 72));
    m_selectedSpeakerFaceDetectionsList->setMinimumHeight(96);
    m_selectedSpeakerFaceDetectionsList->setMaximumHeight(220);
    m_selectedSpeakerFaceDetectionsList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_selectedSpeakerFaceDetectionsList->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_selectedSpeakerFaceDetectionsList->setStyleSheet(QStringLiteral(
        "QListWidget { border: 1px solid #314459; border-radius: 8px; background: #142234; color: #d8e6f5; padding: 6px; }"
        "QListWidget::item { padding: 4px; }"
        "QListWidget::item:selected { background: #1d324a; border: 1px solid #4ea1ff; }"));
    auto *playheadFaceDetectionsTitle = new QLabel(QStringLiteral("Tracks At Playhead"), page);
    styleSectionTitle(playheadFaceDetectionsTitle);
    auto *playheadFaceDetectionsHeaderRow = new QHBoxLayout;
    playheadFaceDetectionsHeaderRow->setContentsMargins(0, 0, 0, 0);
    playheadFaceDetectionsHeaderRow->setSpacing(6);
    m_speakerShowPlayheadFaceDetectionsCheckBox =
        new QCheckBox(QStringLiteral("Show"), page);
    m_speakerShowPlayheadFaceDetectionsCheckBox->setObjectName(
        QStringLiteral("speakers.show_playhead_tracks"));
    m_speakerShowPlayheadFaceDetectionsCheckBox->setChecked(true);
    m_speakerShowPlayheadFaceDetectionsCheckBox->setToolTip(
        QStringLiteral("Show or hide the Tracks At Playhead picker without affecting continuity overlays."));
    m_speakerPlayheadFaceDetectionsList = new QListWidget(page);
    m_speakerPlayheadFaceDetectionsList->setObjectName(QStringLiteral("speakers.playhead_tracks"));
    m_speakerPlayheadFaceDetectionsList->setViewMode(QListView::IconMode);
    m_speakerPlayheadFaceDetectionsList->setFlow(QListView::LeftToRight);
    m_speakerPlayheadFaceDetectionsList->setWrapping(true);
    m_speakerPlayheadFaceDetectionsList->setResizeMode(QListView::Adjust);
    m_speakerPlayheadFaceDetectionsList->setMovement(QListView::Static);
    m_speakerPlayheadFaceDetectionsList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_speakerPlayheadFaceDetectionsList->setSpacing(8);
    m_speakerPlayheadFaceDetectionsList->setIconSize(QSize(72, 72));
    m_speakerPlayheadFaceDetectionsList->setMinimumHeight(96);
    m_speakerPlayheadFaceDetectionsList->setMaximumHeight(220);
    m_speakerPlayheadFaceDetectionsList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_speakerPlayheadFaceDetectionsList->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_speakerPlayheadFaceDetectionsList->setStyleSheet(QStringLiteral(
        "QListWidget { border: 1px solid #314459; border-radius: 8px; background: #142234; color: #d8e6f5; padding: 6px; }"
        "QListWidget::item { padding: 4px; }"
        "QListWidget::item:selected { background: #1d324a; border: 1px solid #4ea1ff; }"));
    auto *selectedActionsRow = new QHBoxLayout;
    m_selectedSpeakerPreviousSentenceButton = new QPushButton(QStringLiteral("Previous Sentence"), page);
    m_selectedSpeakerNextSentenceButton = new QPushButton(QStringLiteral("Next Sentence"), page);
    m_selectedSpeakerNextSectionButton = new QPushButton(QStringLiteral("Next Section"), page);
    m_selectedSpeakerRandomSentenceButton = new QPushButton(QStringLiteral("Random Sentence"), page);
    for (QPushButton* button :
         {m_selectedSpeakerPreviousSentenceButton,
          m_selectedSpeakerNextSentenceButton,
          m_selectedSpeakerNextSectionButton,
          m_selectedSpeakerRandomSentenceButton}) {
        button->setMinimumHeight(30);
        button->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    }
    selectedActionsRow->addWidget(m_selectedSpeakerPreviousSentenceButton);
    selectedActionsRow->addWidget(m_selectedSpeakerNextSentenceButton);
    selectedActionsRow->addWidget(m_selectedSpeakerNextSectionButton);
    selectedActionsRow->addWidget(m_selectedSpeakerRandomSentenceButton);
    m_speakerAiFindNamesButton = new QPushButton(QStringLiteral("Mine Transcript (AI)"), page);
    m_speakerPrecropFacesButton = new QPushButton(QStringLiteral("Add Selected Tracks"), page);
    m_speakerAiFindOrganizationsButton = new QPushButton(QStringLiteral("Find Organizations"), page);
    m_speakerAiCleanAssignmentsButton = new QPushButton(QStringLiteral("Clean Assignments"), page);
    m_speakerPrecropFacesButton->setObjectName(QStringLiteral("speakers.assign_facedetections"));
    m_speakerAiFindNamesButton->setObjectName(QStringLiteral("speakers.mine_transcript_ai"));
    m_speakerAiFindOrganizationsButton->setObjectName(QStringLiteral("speakers.find_organizations_ai"));
    m_speakerAiCleanAssignmentsButton->setObjectName(QStringLiteral("speakers.clean_assignments_ai"));
    for (QPushButton* button :
         {m_speakerPrecropFacesButton, m_speakerAiFindNamesButton, m_speakerAiFindOrganizationsButton, m_speakerAiCleanAssignmentsButton}) {
        button->setMinimumHeight(30);
        button->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    }
    playheadFaceDetectionsHeaderRow->addWidget(playheadFaceDetectionsTitle);
    playheadFaceDetectionsHeaderRow->addWidget(m_speakerShowPlayheadFaceDetectionsCheckBox);
    playheadFaceDetectionsHeaderRow->addStretch(1);
    playheadFaceDetectionsHeaderRow->addWidget(m_speakerPrecropFacesButton);

    auto *currentSentenceTitle = new QLabel(QStringLiteral("Current Speaker Sentence"), page);
    styleSectionTitle(currentSentenceTitle);
    m_speakerCurrentSentenceLabel = new QLabel(QStringLiteral("Select a speaker to view sentence context."), page);
    m_speakerCurrentSentenceLabel->setWordWrap(true);
    m_speakerCurrentSentenceLabel->setMinimumHeight(48);
    m_speakerCurrentSentenceLabel->setStyleSheet(QStringLiteral(
        "QLabel { border: 1px solid #314459; border-radius: 8px; background: #142234; color: #d8e6f5; padding: 8px; }"));

    m_speakerTranscriptTable = new QTableWidget(page);
    m_speakerTranscriptTable->setObjectName(QStringLiteral("speakers.embedded_transcript"));
    m_speakerTranscriptTable->setColumnCount(5);
    m_speakerTranscriptTable->setHorizontalHeaderLabels(
        {QStringLiteral("Source Start"),
         QStringLiteral("Source End"),
         QStringLiteral("Speaker"),
         QStringLiteral("Text"),
         QStringLiteral("Edits")});
    m_speakerTranscriptTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_speakerTranscriptTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_speakerTranscriptTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                              QAbstractItemView::EditKeyPressed);
    m_speakerTranscriptTable->verticalHeader()->setVisible(false);
    m_speakerTranscriptTable->horizontalHeader()->setStretchLastSection(true);
    m_speakerTranscriptTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_speakerTranscriptTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_speakerTranscriptTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_speakerTranscriptTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_speakerTranscriptTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_speakerTranscriptTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_speakerTranscriptTable->setMinimumHeight(240);

    m_speakerTrackingStatusLabel = new QLabel(QString(), page);
    m_speakerTrackingStatusLabel->setObjectName(QStringLiteral("speakers.tracking_status"));
    m_speakerTrackingStatusLabel->setWordWrap(true);
    m_speakerTrackingStatusLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));

    auto *debugTitle = new QLabel(QStringLiteral("Debug Artefacts"), page);
    styleSectionTitle(debugTitle);

    m_speakerDebugCaptureCheckBox = new QCheckBox(QStringLiteral("Enable Debug Capture"), page);
    m_speakerDebugCaptureCheckBox->setChecked(true);
    m_speakerOpenLatestDebugRunButton = new QPushButton(QStringLiteral("Open Latest Debug Run"), page);
    m_speakerExportDebugBundleButton = new QPushButton(QStringLiteral("Export Debug Bundle"), page);
    auto *debugActionsRow = new QHBoxLayout;
    debugActionsRow->setContentsMargins(0, 0, 0, 0);
    debugActionsRow->setSpacing(6);
    debugActionsRow->addWidget(m_speakerOpenLatestDebugRunButton);
    debugActionsRow->addWidget(m_speakerExportDebugBundleButton);
    debugActionsRow->addStretch(1);
    m_speakerDebugStatusLabel = new QLabel(
        QStringLiteral("Run ID: - | Last failed stage: none"),
        page);
    m_speakerDebugStatusLabel->setWordWrap(true);
    m_speakerDebugStatusLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));

    mappingLayout->addWidget(m_speakersInspectorClipLabel);
    mappingLayout->addWidget(m_speakersInspectorDetailsLabel);
    auto *selectedSpeakerPopupFrame = new QFrame(page, Qt::Popup);
    selectedSpeakerPopupFrame->setObjectName(QStringLiteral("speakers.selected_speaker_popup"));
    selectedSpeakerPopupFrame->setFrameShape(QFrame::StyledPanel);
    selectedSpeakerPopupFrame->setMinimumWidth(320);
    selectedSpeakerPopupFrame->setMaximumWidth(460);
    selectedSpeakerPopupFrame->setStyleSheet(QStringLiteral(
        "QFrame#speakers\\.selected_speaker_popup {"
        "  border: 1px solid #35506c;"
        "  border-radius: 8px;"
        "  background: #0f1824;"
        "  padding: 8px;"
        "}"));
    m_selectedSpeakerPopup = selectedSpeakerPopupFrame;
    auto *selectedSpeakerPopupLayout = new QVBoxLayout(selectedSpeakerPopupFrame);
    selectedSpeakerPopupLayout->setContentsMargins(10, 10, 10, 10);
    selectedSpeakerPopupLayout->setSpacing(8);
    auto *selectedSpeakerPage = new QWidget(selectedSpeakerPopupFrame);
    auto *selectedSpeakerPageLayout = new QVBoxLayout(selectedSpeakerPage);
    selectedSpeakerPageLayout->setContentsMargins(0, 0, 0, 0);
    selectedSpeakerPageLayout->setSpacing(6);
    selectedSpeakerPageLayout->addWidget(selectedSpeakerTitle);
    selectedSpeakerPageLayout->addWidget(m_selectedSpeakerIdLabel);
    selectedSpeakerPageLayout->addLayout(selectedSpeakerProfileForm);
    selectedSpeakerPageLayout->addWidget(selectedFaceDetectionsTitle);
    selectedSpeakerPageLayout->addWidget(m_selectedSpeakerFaceDetectionsList);
    selectedSpeakerPageLayout->addLayout(playheadFaceDetectionsHeaderRow);
    selectedSpeakerPageLayout->addWidget(m_speakerPlayheadFaceDetectionsList);
    selectedSpeakerPageLayout->addLayout(selectedActionsRow);
    selectedSpeakerPageLayout->addWidget(currentSentenceTitle);
    selectedSpeakerPageLayout->addWidget(m_speakerCurrentSentenceLabel);
    selectedSpeakerPageLayout->addWidget(m_speakerTrackingStatusLabel);
    auto *selectedSpeakerScroll = new QScrollArea(selectedSpeakerPopupFrame);
    selectedSpeakerScroll->setWidgetResizable(true);
    selectedSpeakerScroll->setFrameShape(QFrame::NoFrame);
    selectedSpeakerScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    selectedSpeakerScroll->setWidget(selectedSpeakerPage);
    selectedSpeakerPopupLayout->addWidget(selectedSpeakerScroll);
    m_speakerTranscriptTable->hide();
    auto *mappingContentRow = new QHBoxLayout;
    mappingContentRow->setContentsMargins(0, 0, 0, 0);
    mappingContentRow->setSpacing(8);
    auto *speakerListPanel = new QWidget(content);
    auto *speakerListLayout = new QVBoxLayout(speakerListPanel);
    speakerListLayout->setContentsMargins(0, 0, 0, 0);
    speakerListLayout->setSpacing(6);
    m_speakersSubtabs = new QTabWidget(speakerListPanel);
    auto *speakerWorkTabs = m_speakersSubtabs;
    speakerWorkTabs->setObjectName(QStringLiteral("speakers.work_tabs"));
    speakerWorkTabs->setDocumentMode(true);
    speakerWorkTabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    speakerWorkTabs->tabBar()->setUsesScrollButtons(true);
    speakerWorkTabs->tabBar()->setElideMode(Qt::ElideRight);
    speakerWorkTabs->setAccessibleName(QStringLiteral("Speaker workflow pages"));

    auto *speakerRosterPage = new QWidget(speakerWorkTabs);
    auto *speakerRosterLayout = new QVBoxLayout(speakerRosterPage);
    speakerRosterLayout->setContentsMargins(0, 0, 0, 0);
    speakerRosterLayout->setSpacing(6);
    auto *speakerRosterControlsGroup = new QGroupBox(QStringLiteral("Roster Options"), speakerRosterPage);
    speakerRosterControlsGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *speakerRosterControlsLayout = new QHBoxLayout(speakerRosterControlsGroup);
    speakerRosterControlsLayout->setContentsMargins(8, 6, 8, 6);
    speakerRosterControlsLayout->setSpacing(8);
    speakerRosterControlsLayout->addWidget(m_speakerHideUnidentifiedCheckBox);
    speakerRosterControlsLayout->addStretch(1);
    speakerRosterLayout->addWidget(m_speakersTable, 1);
    speakerRosterLayout->addWidget(speakerRosterControlsGroup);

    auto *speakerSectionsPage = new QWidget(speakerWorkTabs);
    auto *speakerSectionsLayout = new QVBoxLayout(speakerSectionsPage);
    speakerSectionsLayout->setContentsMargins(0, 0, 0, 0);
    speakerSectionsLayout->setSpacing(6);
    auto *speakerSectionsControlsGroup = new QGroupBox(QStringLiteral("Section Controls"), speakerSectionsPage);
    speakerSectionsControlsGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *speakerSectionsControlsLayout = new QGridLayout(speakerSectionsControlsGroup);
    speakerSectionsControlsLayout->setContentsMargins(8, 6, 8, 6);
    speakerSectionsControlsLayout->setHorizontalSpacing(8);
    speakerSectionsControlsLayout->setVerticalSpacing(4);
    m_speakerApplyTrackToAllMatchingSectionsCheckBox->show();
    m_speakerSectionMinimumWordsSpin->show();
    auto *speakerSectionsFilterRow = new QWidget(speakerSectionsPage);
    speakerSectionsFilterRow->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *speakerSectionsFilterLayout = new QHBoxLayout(speakerSectionsFilterRow);
    speakerSectionsFilterLayout->setContentsMargins(0, 0, 0, 0);
    speakerSectionsFilterLayout->setSpacing(8);
    auto *speakerSectionsSearchEdit = new QLineEdit(speakerSectionsFilterRow);
    speakerSectionsSearchEdit->setObjectName(QStringLiteral("speakers.sections.search"));
    speakerSectionsSearchEdit->setPlaceholderText(QStringLiteral("Filter sections…"));
    speakerSectionsSearchEdit->setClearButtonEnabled(true);
    speakerSectionsSearchEdit->setAccessibleName(QStringLiteral("Filter transcript sections"));
    speakerSectionsSearchEdit->setMinimumWidth(96);
    auto *speakerSectionsSummaryLabel = new QLabel(QStringLiteral("0 sections"), speakerSectionsFilterRow);
    speakerSectionsSummaryLabel->setObjectName(QStringLiteral("speakers.sections.summary"));
    speakerSectionsSummaryLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    speakerSectionsSummaryLabel->setMinimumWidth(64);
    speakerSectionsSummaryLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
    speakerSectionsFilterLayout->addWidget(speakerSectionsSearchEdit, 1);
    speakerSectionsFilterLayout->addWidget(speakerSectionsSummaryLabel);
    speakerSectionsLayout->addWidget(speakerSectionsFilterRow);
    m_speakerSectionsTable->setObjectName(QStringLiteral("speakers.sections_table"));
    speakerSectionsLayout->addWidget(m_speakerSectionsTable, 1);
    speakerSectionsControlsLayout->addWidget(m_speakerSectionMinimumWordsSpin, 0, 0);
    speakerSectionsControlsLayout->addWidget(m_speakerExportLongSectionsButton, 0, 1);
    speakerSectionsControlsLayout->addWidget(
        m_speakerApplyTrackToAllMatchingSectionsCheckBox, 1, 0, 1, 2);
    speakerSectionsControlsLayout->setColumnStretch(0, 1);
    speakerSectionsControlsLayout->setColumnStretch(1, 1);
    speakerSectionsLayout->addWidget(speakerSectionsControlsGroup);
    speakerSectionsLayout->setStretch(0, 0);
    speakerSectionsLayout->setStretch(1, 1);
    speakerSectionsLayout->setStretch(2, 0);
    const auto filterSpeakerSections =
        [table = m_speakerSectionsTable, speakerSectionsSearchEdit, speakerSectionsSummaryLabel]() {
            const QString needle = speakerSectionsSearchEdit->text().trimmed();
            int visibleRows = 0;
            const int totalRows = table->rowCount();
            for (int row = 0; row < totalRows; ++row) {
                bool matches = needle.isEmpty();
                for (int column = 0; !matches && column < table->columnCount(); ++column) {
                    const QTableWidgetItem* item = table->item(row, column);
                    matches = item && item->text().contains(needle, Qt::CaseInsensitive);
                }
                table->setRowHidden(row, !matches);
                visibleRows += matches ? 1 : 0;
            }
            speakerSectionsSummaryLabel->setText(
                needle.isEmpty()
                    ? QStringLiteral("%1 sections").arg(totalRows)
                    : QStringLiteral("%1 of %2").arg(visibleRows).arg(totalRows));
        };
    connect(speakerSectionsSearchEdit, &QLineEdit::textChanged, page,
            [filterSpeakerSections]() { filterSpeakerSections(); });
    connect(m_speakerSectionsTable->model(), &QAbstractItemModel::modelReset, page,
            [filterSpeakerSections]() { filterSpeakerSections(); });
    connect(m_speakerSectionsTable->model(), &QAbstractItemModel::rowsInserted, page,
            [filterSpeakerSections](const QModelIndex&, int, int) { filterSpeakerSections(); });

    auto *speakerAiPage = new QWidget(speakerWorkTabs);
    auto *speakerAiLayout = new QVBoxLayout(speakerAiPage);
    speakerAiLayout->setContentsMargins(0, 0, 0, 0);
    speakerAiLayout->setSpacing(6);
    auto *speakerAiGroup = new QGroupBox(QStringLiteral("Transcript Cleanup"), speakerAiPage);
    auto *speakerAiGroupLayout = new QGridLayout(speakerAiGroup);
    speakerAiGroupLayout->setContentsMargins(8, 6, 8, 6);
    speakerAiGroupLayout->setHorizontalSpacing(6);
    speakerAiGroupLayout->setVerticalSpacing(6);
    auto *speakerAiHelp = new QLabel(
        QStringLiteral("Mine transcript speaker names, find organizations, then clean assignments."),
        speakerAiGroup);
    styleSectionHelp(speakerAiHelp);
    speakerAiGroupLayout->addWidget(speakerAiHelp, 0, 0, 1, 3);
    speakerAiGroupLayout->addWidget(m_speakerAiFindNamesButton, 1, 0);
    speakerAiGroupLayout->addWidget(m_speakerAiFindOrganizationsButton, 1, 1);
    speakerAiGroupLayout->addWidget(m_speakerAiCleanAssignmentsButton, 1, 2);
    speakerAiGroupLayout->setColumnStretch(0, 1);
    speakerAiGroupLayout->setColumnStretch(1, 1);
    speakerAiGroupLayout->setColumnStretch(2, 1);
    speakerAiLayout->addWidget(speakerAiGroup);
    speakerAiLayout->addStretch(1);

    auto *speakerTitleTabs = new QTabWidget(m_transcriptSpeakerTitlesContainer);
    speakerTitleTabs->setObjectName(QStringLiteral("speakers.title_tabs"));
    speakerTitleTabs->setDocumentMode(true);
    speakerTitleTabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto *speakerFlyInPage = new QWidget(speakerTitleTabs);
    auto *speakerFlyInLayout = new QVBoxLayout(speakerFlyInPage);
    speakerFlyInLayout->setContentsMargins(0, 0, 0, 0);
    speakerFlyInLayout->setSpacing(6);

    auto *speakerLabelPage = new QWidget(speakerTitleTabs);
    auto *speakerLabelLayout = new QVBoxLayout(speakerLabelPage);
    speakerLabelLayout->setContentsMargins(0, 0, 0, 0);
    speakerLabelLayout->setSpacing(6);

    auto *speakerOverlayVisibilityGroup = new QGroupBox(QStringLiteral("Visible Fields"), speakerLabelPage);
    auto *speakerOverlayVisibilityLayout = new QVBoxLayout(speakerOverlayVisibilityGroup);
    speakerOverlayVisibilityLayout->setContentsMargins(8, 6, 8, 6);
    speakerOverlayVisibilityLayout->setSpacing(4);
    speakerOverlayVisibilityLayout->addWidget(m_speakerShowCurrentSpeakerNameCheckBox);
    speakerOverlayVisibilityLayout->addWidget(m_speakerShowCurrentSpeakerOrganizationCheckBox);
    speakerLabelLayout->addWidget(speakerOverlayVisibilityGroup);
    speakerLabelLayout->addStretch(1);

    auto *speakerOverlayFlyInGroup = new QGroupBox(QStringLiteral("Introduction Animation"), speakerFlyInPage);
    auto *speakerOverlayFlyInLayout = new QVBoxLayout(speakerOverlayFlyInGroup);
    speakerOverlayFlyInLayout->setContentsMargins(8, 6, 8, 6);
    speakerOverlayFlyInLayout->setSpacing(6);
    auto *speakerOverlayFlyInHelp = new QLabel(
        QStringLiteral("Automatically show an animated lower-third when each speaker is introduced."),
        speakerOverlayFlyInGroup);
    styleSectionHelp(speakerOverlayFlyInHelp);
    speakerOverlayFlyInLayout->addWidget(speakerOverlayFlyInHelp);
    auto *speakerOverlayFlyInForm = new QFormLayout;
    speakerOverlayFlyInForm->setContentsMargins(0, 0, 0, 0);
    speakerOverlayFlyInForm->setHorizontalSpacing(8);
    speakerOverlayFlyInForm->setVerticalSpacing(4);
    speakerOverlayFlyInForm->addRow(QStringLiteral("Fly Option"), m_speakerOverlayFlyInStyleCombo);
    speakerOverlayFlyInForm->addRow(QStringLiteral("Delay"), m_speakerOverlayFlyInDelaySpin);
    speakerOverlayFlyInForm->addRow(QStringLiteral("Duration"), m_speakerOverlayFlyInDurationSpin);
    speakerOverlayFlyInForm->addRow(m_speakerOverlayShowAtSectionEndCheckBox);
    speakerOverlayFlyInForm->addRow(m_speakerOverlayRespectSpeechFilterTimingCheckBox);
    speakerOverlayFlyInForm->addRow(QStringLiteral("Repeat Cadence"), m_speakerOverlayCadenceSpin);
    speakerOverlayFlyInForm->addRow(QStringLiteral("Fly Time"), m_speakerOverlayFlyInTimeSpin);
    auto *wrapRadiusLabel = new QLabel(QStringLiteral("Wrap Radius"), speakerOverlayFlyInGroup);
    auto *wrapDepthLabel = new QLabel(QStringLiteral("Wrap Depth"), speakerOverlayFlyInGroup);
    auto *wrapStartAngleLabel = new QLabel(QStringLiteral("Start Angle"), speakerOverlayFlyInGroup);
    auto *wrapEndAngleLabel = new QLabel(QStringLiteral("End Angle"), speakerOverlayFlyInGroup);
    auto *wrapPitchLabel = new QLabel(QStringLiteral("Pitch"), speakerOverlayFlyInGroup);
    auto *wrapRollLabel = new QLabel(QStringLiteral("Roll"), speakerOverlayFlyInGroup);
    speakerOverlayFlyInForm->addRow(wrapRadiusLabel, m_speakerOverlayWrapRadiusSpin);
    speakerOverlayFlyInForm->addRow(wrapDepthLabel, m_speakerOverlayWrapDepthSpin);
    speakerOverlayFlyInForm->addRow(wrapStartAngleLabel, m_speakerOverlayWrapStartAngleSpin);
    speakerOverlayFlyInForm->addRow(wrapEndAngleLabel, m_speakerOverlayWrapEndAngleSpin);
    speakerOverlayFlyInForm->addRow(wrapPitchLabel, m_speakerOverlayWrapPitchSpin);
    speakerOverlayFlyInForm->addRow(wrapRollLabel, m_speakerOverlayWrapRollSpin);
    speakerOverlayFlyInForm->addRow(QStringLiteral("Start Rotation X"), m_speakerOverlayRotationXSpin);
    speakerOverlayFlyInForm->addRow(QStringLiteral("Start Rotation Y"), m_speakerOverlayRotationYSpin);
    speakerOverlayFlyInForm->addRow(QStringLiteral("Start Rotation Z"), m_speakerOverlayRotationZSpin);
    auto syncWrapControls = [this,
                             wrapRadiusLabel,
                             wrapDepthLabel,
                             wrapStartAngleLabel,
                             wrapEndAngleLabel,
                             wrapPitchLabel,
                             wrapRollLabel]() {
        const bool wrapSelected =
            m_speakerOverlayFlyInStyleCombo &&
            m_speakerOverlayFlyInStyleCombo->currentData().toInt() ==
                static_cast<int>(SpeakerTitleFlyInStyle::WrapAroundSpeaker);
        wrapRadiusLabel->setVisible(wrapSelected);
        wrapDepthLabel->setVisible(wrapSelected);
        wrapStartAngleLabel->setVisible(wrapSelected);
        wrapEndAngleLabel->setVisible(wrapSelected);
        wrapPitchLabel->setVisible(wrapSelected);
        wrapRollLabel->setVisible(wrapSelected);
        if (m_speakerOverlayWrapRadiusSpin) {
            m_speakerOverlayWrapRadiusSpin->setVisible(wrapSelected);
        }
        if (m_speakerOverlayWrapDepthSpin) {
            m_speakerOverlayWrapDepthSpin->setVisible(wrapSelected);
        }
        if (m_speakerOverlayWrapStartAngleSpin) {
            m_speakerOverlayWrapStartAngleSpin->setVisible(wrapSelected);
        }
        if (m_speakerOverlayWrapEndAngleSpin) {
            m_speakerOverlayWrapEndAngleSpin->setVisible(wrapSelected);
        }
        if (m_speakerOverlayWrapPitchSpin) {
            m_speakerOverlayWrapPitchSpin->setVisible(wrapSelected);
        }
        if (m_speakerOverlayWrapRollSpin) {
            m_speakerOverlayWrapRollSpin->setVisible(wrapSelected);
        }
    };
    connect(m_speakerOverlayFlyInStyleCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            speakerOverlayFlyInGroup,
            [syncWrapControls](int) { syncWrapControls(); });
    syncWrapControls();
    speakerOverlayFlyInLayout->addWidget(m_speakerOverlayCreateTitleClipsButton);
    speakerOverlayFlyInLayout->addLayout(speakerOverlayFlyInForm);
    speakerFlyInLayout->addWidget(speakerOverlayFlyInGroup);
    speakerFlyInLayout->addStretch(1);

    auto *speakerStylePage = new QWidget(speakerTitleTabs);
    auto *speakerStyleLayout = new QVBoxLayout(speakerStylePage);
    speakerStyleLayout->setContentsMargins(0, 0, 0, 0);
    speakerStyleLayout->setSpacing(6);

    auto *speakerOverlayStyleGroup = new QGroupBox(QStringLiteral("Style"), speakerStylePage);
    auto *speakerOverlayStyleLayout = new QVBoxLayout(speakerOverlayStyleGroup);
    speakerOverlayStyleLayout->setContentsMargins(8, 6, 8, 6);
    speakerOverlayStyleLayout->setSpacing(4);
    auto *currentSpeakerTextSizeLayout = new QFormLayout;
    currentSpeakerTextSizeLayout->setContentsMargins(0, 0, 0, 0);
    currentSpeakerTextSizeLayout->setHorizontalSpacing(8);
    currentSpeakerTextSizeLayout->setVerticalSpacing(4);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Title Font Size"), m_speakerOverlayTitleFontSizeSpin);
    currentSpeakerTextSizeLayout->addRow(m_speakerOverlayTitleAutoFitCheckBox);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Title Box Width"), m_speakerOverlayTitleBoxWidthSpin);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Title Material"), m_speakerOverlayTitleTextMaterialCombo);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Border Material"), m_speakerOverlayTitleBorderMaterialCombo);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Text Pattern Image"), m_speakerOverlayTitleTextPatternPathEdit);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Border Pattern Image"), m_speakerOverlayTitleBorderPatternPathEdit);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Pattern Scale"), m_speakerOverlayTitlePatternScaleSpin);
    currentSpeakerTextSizeLayout->addRow(m_speakerOverlayTitleExtrudeCheckBox);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Extrude Mode"), m_speakerOverlayTitleExtrudeModeCombo);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Extrude Depth"), m_speakerOverlayTitleExtrudeDepthSpin);
    currentSpeakerTextSizeLayout->addRow(QStringLiteral("Bevel Scale"), m_speakerOverlayTitleBevelScaleSpin);
    m_speakerCurrentSpeakerBackgroundVisibleCheckBox->setText(QStringLiteral("Show Background Box"));
    currentSpeakerTextSizeLayout->addRow(m_speakerCurrentSpeakerBackgroundVisibleCheckBox);
    speakerOverlayStyleLayout->addLayout(currentSpeakerTextSizeLayout);
    speakerStyleLayout->addWidget(speakerOverlayStyleGroup);
    speakerStyleLayout->addStretch(1);
    speakerTitleTabs->addTab(speakerFlyInPage, QStringLiteral("Fly-In"));
    speakerTitleTabs->addTab(speakerLabelPage, QStringLiteral("Label"));
    speakerTitleTabs->addTab(speakerStylePage, QStringLiteral("Style"));
    if (m_transcriptSpeakerTitlesContainer && m_transcriptSpeakerTitlesContainer->layout()) {
        m_transcriptSpeakerTitlesContainer->layout()->addWidget(speakerTitleTabs);
    }

    auto *speakerContinuityPage = buildSpeakersContinuityTab(speakerWorkTabs);

    auto *speakerDebugPage = new QWidget(speakerWorkTabs);
    auto *speakerDebugLayout = createTabLayout(speakerDebugPage);
    speakerDebugPage->setObjectName(QStringLiteral("speakers_debug_section"));
    speakerDebugLayout->addWidget(debugTitle);
    speakerDebugLayout->addWidget(m_speakerDebugCaptureCheckBox);
    speakerDebugLayout->addLayout(debugActionsRow);
    speakerDebugLayout->addWidget(m_speakerDebugStatusLabel);
    speakerDebugLayout->addStretch(1);

    const int rosterTabIndex = speakerWorkTabs->addTab(speakerRosterPage, QStringLiteral("Roster"));
    const int sectionsTabIndex = speakerWorkTabs->addTab(speakerSectionsPage, QStringLiteral("Sections"));
    speakerWorkTabs->addTab(speakerAiPage, QStringLiteral("AI Cleanup"));
    speakerWorkTabs->addTab(speakerContinuityPage, QStringLiteral("Continuity Tracks"));
    speakerWorkTabs->addTab(speakerDebugPage, QStringLiteral("Debug"));
    const auto syncSectionModeFromWorkTab =
        [this, rosterTabIndex, sectionsTabIndex](int index) {
                if (!m_speakerShowContiguousSectionsCheckBox) {
                    return;
                }
                if (index == rosterTabIndex) {
                    m_speakerShowContiguousSectionsCheckBox->setChecked(false);
                } else if (index == sectionsTabIndex) {
                    m_speakerShowContiguousSectionsCheckBox->setChecked(true);
                }
        };
    connect(speakerWorkTabs,
            &QTabWidget::currentChanged,
            page,
            syncSectionModeFromWorkTab);
    connect(m_speakerShowContiguousSectionsCheckBox,
            &QCheckBox::toggled,
            speakerWorkTabs,
            [speakerWorkTabs, rosterTabIndex, sectionsTabIndex](bool showSections) {
                const int desiredIndex = showSections ? sectionsTabIndex : rosterTabIndex;
                if (speakerWorkTabs->currentIndex() == rosterTabIndex ||
                    speakerWorkTabs->currentIndex() == sectionsTabIndex) {
                    speakerWorkTabs->setCurrentIndex(desiredIndex);
                }
            });
    // addTab() selects the first page before currentChanged is connected.
    // Initialize the mode from the page that is actually active.
    syncSectionModeFromWorkTab(speakerWorkTabs->currentIndex());
    speakerListLayout->addWidget(speakerWorkTabs, 1);
    mappingContentRow->addWidget(speakerListPanel, 1);

    auto identitySection = createSectionFrame(content, QStringLiteral("speakers_identities_section"));
    identitySection.second->addLayout(mappingContentRow, 1);

    mappingLayout->addWidget(identitySection.first);
    mappingLayout->addStretch(1);

    scrollArea->setWidget(content);
    layout->addWidget(scrollArea, 1);
    return page;
}
