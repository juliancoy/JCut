#include "titles_tab.h"
#include "editor_title_opacity_keyframe_ops.h"
#include "keyframe_table_shared.h"
#include "overlay_text_style.h"
#include "overlay_style_ui.h"

#include <QApplication>
#include <QColorDialog>
#include <QHeaderView>
#include <QMenu>
#include <QSignalBlocker>
#include <QKeyEvent>

TitlesTab::TitlesTab(const Widgets &widgets, const Dependencies &deps, QObject *parent)
    : KeyframeTabBase(deps, parent)
    , m_widgets(widgets)
{
}

void TitlesTab::wire()
{
    auto *table = m_widgets.titleKeyframeTable;
    if (table) {
        table->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(table, &QTableWidget::itemChanged, this, &TitlesTab::onTableItemChanged);
        connect(table, &QTableWidget::itemSelectionChanged, this, &TitlesTab::onTableSelectionChanged);
        connect(table, &QTableWidget::itemClicked, this, &TitlesTab::onTableItemClicked);
        connect(table, &QWidget::customContextMenuRequested, this, &TitlesTab::onTableCustomContextMenu);
        installTableHandlers(table);
    }

    auto connectSpin = [this](QDoubleSpinBox *spin) {
        if (!spin) return;
        connect(spin, &QDoubleSpinBox::editingFinished, this, &TitlesTab::applyKeyframeFromInspector);
    };
    connectSpin(m_widgets.titleXSpin);
    connectSpin(m_widgets.titleYSpin);
    connectSpin(m_widgets.titleFontSizeSpin);
    connectSpin(m_widgets.titleOpacitySpin);
    connectSpin(m_widgets.titleShadowOpacitySpin);
    connectSpin(m_widgets.titleShadowOffsetXSpin);
    connectSpin(m_widgets.titleShadowOffsetYSpin);
    connectSpin(m_widgets.titleWindowOpacitySpin);
    connectSpin(m_widgets.titleWindowPaddingSpin);
    connectSpin(m_widgets.titleWindowFrameOpacitySpin);
    connectSpin(m_widgets.titleWindowFrameWidthSpin);
    connectSpin(m_widgets.titleWindowFrameGapSpin);
    connectSpin(m_widgets.titleTextExtrudeDepthSpin);
    connectSpin(m_widgets.titleTextExtrudeBevelSpin);
    if (m_widgets.titleTextExtrudeModeCombo) {
        connect(m_widgets.titleTextExtrudeModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this](int) {
                    const bool enabled = m_widgets.titleTextExtrudeModeCombo->currentData().toInt() != 0;
                    if (m_widgets.titleTextExtrudeDepthSpin) m_widgets.titleTextExtrudeDepthSpin->setEnabled(enabled);
                    if (m_widgets.titleTextExtrudeBevelSpin) m_widgets.titleTextExtrudeBevelSpin->setEnabled(enabled);
                    if (!m_updating) applyKeyframeFromInspector();
                });
    }

    if (m_widgets.titleTextEdit) {
        // Connect textChanged signal for real-time updates
        connect(m_widgets.titleTextEdit, &QPlainTextEdit::textChanged, this, [this]() {
            if (!m_updating) applyKeyframeFromInspectorLive();
        });
        // Install event filter for Ctrl+Enter handling
        m_widgets.titleTextEdit->installEventFilter(this);
    }
    if (m_widgets.titleFontCombo) {
        connect(m_widgets.titleFontCombo, &QFontComboBox::currentFontChanged, this, [this]() {
            if (!m_updating) applyKeyframeFromInspector();
        });
    }
    if (m_widgets.titleBoldCheck) {
        connect(m_widgets.titleBoldCheck, &QCheckBox::toggled, this, [this]() {
            if (!m_updating) applyKeyframeFromInspector();
        });
    }
    if (m_widgets.titleItalicCheck) {
        connect(m_widgets.titleItalicCheck, &QCheckBox::toggled, this, [this]() {
            if (!m_updating) applyKeyframeFromInspector();
        });
    }
    if (m_widgets.titleShadowEnabledCheck) {
        connect(m_widgets.titleShadowEnabledCheck, &QCheckBox::toggled, this, [this]() {
            if (!m_updating) applyKeyframeFromInspector();
        });
    }
    if (m_widgets.titleWindowEnabledCheck) {
        connect(m_widgets.titleWindowEnabledCheck, &QCheckBox::toggled, this, [this]() {
            if (!m_updating) applyKeyframeFromInspector();
        });
    }
    if (m_widgets.titleWindowFrameEnabledCheck) {
        connect(m_widgets.titleWindowFrameEnabledCheck, &QCheckBox::toggled, this, [this]() {
            if (!m_updating) applyKeyframeFromInspector();
        });
    }
    if (m_widgets.titleColorButton) {
        connect(m_widgets.titleColorButton, &QPushButton::clicked, this, [this]() {
            const TimelineClip* clip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
            if (!clip || clip->mediaType != ClipMediaType::Title) {
                return;
            }
            QColor initial = QColor(QStringLiteral("#ffffff"));
            const int idx = selectedKeyframeIndex(*clip);
            if (idx >= 0 && idx < clip->titleKeyframes.size()) {
                initial = clip->titleKeyframes[idx].color;
            }
            QColor chosen = QColorDialog::getColor(initial, nullptr, QStringLiteral("Select Title Color"));
            if (!chosen.isValid()) {
                return;
            }
            m_selectedTitleColor = chosen;
            applyOverlayColorButtonStyle(m_widgets.titleColorButton, chosen);
            if (!m_updating) {
                applyKeyframeFromInspector();
            }
        });
    }
    if (m_widgets.titleShadowColorButton) {
        connect(m_widgets.titleShadowColorButton, &QPushButton::clicked, this, [this]() {
            const TimelineClip* clip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
            if (!clip || clip->mediaType != ClipMediaType::Title) {
                return;
            }
            QColor initial = QColor(QStringLiteral("#000000"));
            const int idx = selectedKeyframeIndex(*clip);
            if (idx >= 0 && idx < clip->titleKeyframes.size()) {
                initial = clip->titleKeyframes[idx].dropShadowColor;
            }
            QColor chosen = QColorDialog::getColor(initial, nullptr, QStringLiteral("Select Shadow Color"));
            if (!chosen.isValid()) {
                return;
            }
            m_selectedShadowColor = chosen;
            applyOverlayColorButtonStyle(m_widgets.titleShadowColorButton, chosen);
            if (!m_updating) {
                applyKeyframeFromInspector();
            }
        });
    }
    if (m_widgets.titleWindowColorButton) {
        connect(m_widgets.titleWindowColorButton, &QPushButton::clicked, this, [this]() {
            const TimelineClip* clip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
            if (!clip || clip->mediaType != ClipMediaType::Title) {
                return;
            }
            QColor initial = QColor(QStringLiteral("#000000"));
            const int idx = selectedKeyframeIndex(*clip);
            if (idx >= 0 && idx < clip->titleKeyframes.size()) {
                initial = clip->titleKeyframes[idx].windowColor;
            }
            QColor chosen = QColorDialog::getColor(initial, nullptr, QStringLiteral("Select Window Color"));
            if (!chosen.isValid()) {
                return;
            }
            m_selectedWindowColor = chosen;
            applyOverlayColorButtonStyle(m_widgets.titleWindowColorButton, chosen);
            if (!m_updating) {
                applyKeyframeFromInspector();
            }
        });
    }
    if (m_widgets.titleWindowFrameColorButton) {
        connect(m_widgets.titleWindowFrameColorButton, &QPushButton::clicked, this, [this]() {
            const TimelineClip* clip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
            if (!clip || clip->mediaType != ClipMediaType::Title) {
                return;
            }
            QColor initial = QColor(QStringLiteral("#ffffffff"));
            const int idx = selectedKeyframeIndex(*clip);
            if (idx >= 0 && idx < clip->titleKeyframes.size()) {
                initial = clip->titleKeyframes[idx].windowFrameColor;
            }
            QColor chosen = QColorDialog::getColor(initial, nullptr, QStringLiteral("Select Window Frame Color"));
            if (!chosen.isValid()) {
                return;
            }
            m_selectedWindowFrameColor = chosen;
            applyOverlayColorButtonStyle(m_widgets.titleWindowFrameColorButton, chosen);
            if (!m_updating) {
                applyKeyframeFromInspector();
            }
        });
    }

    if (m_widgets.addTitleKeyframeButton) {
        connect(m_widgets.addTitleKeyframeButton, &QPushButton::clicked, this, &TitlesTab::upsertKeyframeAtPlayhead);
    }
    if (m_widgets.removeTitleKeyframeButton) {
        connect(m_widgets.removeTitleKeyframeButton, &QPushButton::clicked, this, &TitlesTab::removeSelectedKeyframes);
    }
    if (m_widgets.centerHorizontalButton) {
        connect(m_widgets.centerHorizontalButton, &QPushButton::clicked, this, &TitlesTab::centerHorizontal);
    }
    if (m_widgets.centerVerticalButton) {
        connect(m_widgets.centerVerticalButton, &QPushButton::clicked, this, &TitlesTab::centerVertical);
    }
}

void TitlesTab::refresh()
{
    auto *table = m_widgets.titleKeyframeTable;
    
    // (Removed early return to ensure the UI updates to reflect truth when a row is modified)

    
    m_updating = true;

    const TimelineClip *clip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;

    const bool hasTitleClip = clip && clip->mediaType == ClipMediaType::Title;
    if (m_widgets.titlesInspectorClipLabel) {
        m_widgets.titlesInspectorClipLabel->setText(
            hasTitleClip ? clip->label : QStringLiteral("No title clip selected"));
    }

    if (!hasTitleClip) {
        if (table) {
            const QSignalBlocker blocker(table);
            table->clearContents();
            table->setRowCount(0);
        }
        if (m_widgets.titlesInspectorDetailsLabel) {
            m_widgets.titlesInspectorDetailsLabel->setText(
                QStringLiteral("Select a title clip to edit keyframes."));
        }
        TitleKeyframeDisplay defaults;
        updateWidgetsFromKeyframe(defaults);
        m_selectedKeyframeFrame = -1;
        m_selectedKeyframeFrames.clear();
        m_updating = false;
        return;
    }

    if (table) {
        const QSignalBlocker blocker(table);
        populateTable(*clip);
    }

    // Determine active keyframe
    const int64_t currentTimelineFrame = m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : 0;
    const int64_t localFrame = qMax<int64_t>(0, currentTimelineFrame - clip->startFrame);

    if (m_selectedKeyframeFrame < 0 || m_selectedKeyframeFrames.isEmpty()) {
        const int nearest = nearestKeyframeIndex(*clip, localFrame);
        if (nearest >= 0 && nearest < clip->titleKeyframes.size()) {
            m_selectedKeyframeFrame = clip->titleKeyframes[nearest].frame;
            m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
        }
    }

    TitleKeyframeDisplay displayed = evaluateDisplayedTitle(*clip, localFrame);
    const int selectedIndex = selectedKeyframeIndex(*clip);
    if (selectedIndex >= 0 && selectedIndex < clip->titleKeyframes.size()) {
        const auto& selected = clip->titleKeyframes[selectedIndex];
        displayed.frame = selected.frame;
        displayed.text = selected.text;
        displayed.translationX = selected.translationX;
        displayed.translationY = selected.translationY;
        displayed.fontSize = selected.fontSize;
        displayed.opacity = selected.opacity;
        displayed.fontFamily = selected.fontFamily;
        displayed.bold = selected.bold;
        displayed.italic = selected.italic;
        displayed.color = selected.color;
        displayed.dropShadowEnabled = selected.dropShadowEnabled;
        displayed.dropShadowColor = selected.dropShadowColor;
        displayed.dropShadowOpacity = selected.dropShadowOpacity;
        displayed.dropShadowOffsetX = selected.dropShadowOffsetX;
        displayed.dropShadowOffsetY = selected.dropShadowOffsetY;
        displayed.windowEnabled = selected.windowEnabled;
        displayed.windowColor = selected.windowColor;
        displayed.windowOpacity = selected.windowOpacity;
        displayed.windowPadding = selected.windowPadding;
        displayed.windowFrameEnabled = selected.windowFrameEnabled;
        displayed.windowFrameColor = selected.windowFrameColor;
        displayed.windowFrameOpacity = selected.windowFrameOpacity;
        displayed.windowFrameWidth = selected.windowFrameWidth;
        displayed.windowFrameGap = selected.windowFrameGap;
        displayed.textExtrudeMode = selected.textExtrudeMode;
        displayed.textExtrudeDepth = selected.vulkan3DExtrudeDepth;
        displayed.textExtrudeBevel = selected.vulkan3DBevelScale;
        displayed.linearInterpolation = selected.linearInterpolation;
    }
    updateWidgetsFromKeyframe(displayed);

    if (table && !m_selectedKeyframeFrames.isEmpty()) {
        const QSignalBlocker blocker(table);
        editor::restoreSelectionByFrameRole(table, m_selectedKeyframeFrames);
    }

    m_suppressSyncForTimelineFrame = -1;

    if (m_widgets.titlesInspectorDetailsLabel) {
        m_widgets.titlesInspectorDetailsLabel->setText(
            QStringLiteral("%1 title keyframes").arg(clip->titleKeyframes.size()));
    }

    m_updating = false;
    syncTableToPlayhead();
}

void TitlesTab::populateTable(const TimelineClip &clip)
{
    auto *table = m_widgets.titleKeyframeTable;
    if (!table) return;

    table->clearContents();
    table->setRowCount(clip.titleKeyframes.size());

    for (int row = 0; row < clip.titleKeyframes.size(); ++row) {
        const auto &kf = clip.titleKeyframes[row];
        auto setCell = [&](int col, const QString &text) {
            auto *item = new QTableWidgetItem(text);
            item->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(kf.frame)));
            table->setItem(row, col, item);
        };
        
        // Calculate start frame (this keyframe's frame)
        int64_t startFrame = kf.frame;
        
        // Calculate end frame (next keyframe's frame - 1, or clip duration - 1 if last keyframe)
        int64_t endFrame = clip.durationFrames - 1;
        if (row + 1 < clip.titleKeyframes.size()) {
            endFrame = clip.titleKeyframes[row + 1].frame - 1;
        }
        
        // Ensure end frame is not less than start frame
        if (endFrame < startFrame) {
            endFrame = startFrame;
        }
        
        // Set Start and End columns (editable timing boundaries)
        auto *startItem = new QTableWidgetItem(QString::number(startFrame));
        startItem->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(kf.frame)));
        table->setItem(row, 0, startItem);
        
        auto *endItem = new QTableWidgetItem(QString::number(endFrame));
        endItem->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(kf.frame)));
        table->setItem(row, 1, endItem);
        
        // Original Frame column (now column 2) - editable
        setCell(2, QString::number(kf.frame));
        setCell(3, kf.text);
        setCell(4, QString::number(kf.translationX, 'f', 1));
        setCell(5, QString::number(kf.translationY, 'f', 1));
        setCell(6, QString::number(kf.fontSize, 'f', 1));
        setCell(7, QString::number(kf.opacity, 'f', 2));

        auto *interpItem = new QTableWidgetItem(kf.linearInterpolation
            ? QStringLiteral("Linear") : QStringLiteral("Step"));
        interpItem->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(kf.frame)));
        interpItem->setFlags(interpItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 8, interpItem);
    }
}

TitlesTab::TitleKeyframeDisplay TitlesTab::evaluateDisplayedTitle(
    const TimelineClip &clip, int64_t localFrame) const
{
    TitleKeyframeDisplay display;
    if (clip.titleKeyframes.isEmpty()) return display;

    // Find the keyframe at or before localFrame
    int bestIdx = 0;
    for (int i = 0; i < clip.titleKeyframes.size(); ++i) {
        if (clip.titleKeyframes[i].frame <= localFrame) {
            bestIdx = i;
        }
    }

    const auto &kf = clip.titleKeyframes[bestIdx];
    display.frame = kf.frame;
    display.text = kf.text;
    display.translationX = kf.translationX;
    display.translationY = kf.translationY;
    display.fontSize = kf.fontSize;
    display.opacity = kf.opacity;
    display.fontFamily = kf.fontFamily;
    display.bold = kf.bold;
    display.italic = kf.italic;
    display.color = kf.color;
    display.dropShadowEnabled = kf.dropShadowEnabled;
    display.dropShadowColor = kf.dropShadowColor;
    display.dropShadowOpacity = kf.dropShadowOpacity;
    display.dropShadowOffsetX = kf.dropShadowOffsetX;
    display.dropShadowOffsetY = kf.dropShadowOffsetY;
    display.windowEnabled = kf.windowEnabled;
    display.windowColor = kf.windowColor;
    display.windowOpacity = kf.windowOpacity;
    display.windowPadding = kf.windowPadding;
    display.windowFrameEnabled = kf.windowFrameEnabled;
    display.windowFrameColor = kf.windowFrameColor;
    display.windowFrameOpacity = kf.windowFrameOpacity;
    display.windowFrameWidth = kf.windowFrameWidth;
    display.windowFrameGap = kf.windowFrameGap;
    display.textExtrudeMode = kf.textExtrudeMode;
    display.textExtrudeDepth = kf.vulkan3DExtrudeDepth;
    display.textExtrudeBevel = kf.vulkan3DBevelScale;
    display.linearInterpolation = kf.linearInterpolation;
    return display;
}

void TitlesTab::updateWidgetsFromKeyframe(const TitleKeyframeDisplay &display)
{
    auto blockAndSet = [](QDoubleSpinBox *spin, double val) {
        if (!spin) return;
        const QSignalBlocker b(spin);
        spin->setValue(val);
    };
    blockAndSet(m_widgets.titleXSpin, display.translationX);
    blockAndSet(m_widgets.titleYSpin, display.translationY);
    blockAndSet(m_widgets.titleFontSizeSpin, display.fontSize);
    blockAndSet(m_widgets.titleOpacitySpin, display.opacity);
    blockAndSet(m_widgets.titleShadowOpacitySpin, display.dropShadowOpacity);
    blockAndSet(m_widgets.titleShadowOffsetXSpin, display.dropShadowOffsetX);
    blockAndSet(m_widgets.titleShadowOffsetYSpin, display.dropShadowOffsetY);
    blockAndSet(m_widgets.titleWindowOpacitySpin, display.windowOpacity);
    blockAndSet(m_widgets.titleWindowPaddingSpin, display.windowPadding);
    blockAndSet(m_widgets.titleWindowFrameOpacitySpin, display.windowFrameOpacity);
    blockAndSet(m_widgets.titleWindowFrameWidthSpin, display.windowFrameWidth);
    blockAndSet(m_widgets.titleWindowFrameGapSpin, display.windowFrameGap);
    blockAndSet(m_widgets.titleTextExtrudeDepthSpin, display.textExtrudeDepth);
    blockAndSet(m_widgets.titleTextExtrudeBevelSpin, display.textExtrudeBevel);
    if (m_widgets.titleTextExtrudeModeCombo) {
        const QSignalBlocker b(m_widgets.titleTextExtrudeModeCombo);
        const int index = m_widgets.titleTextExtrudeModeCombo->findData(
            static_cast<int>(display.textExtrudeMode));
        m_widgets.titleTextExtrudeModeCombo->setCurrentIndex(qMax(0, index));
        const bool enabled = display.textExtrudeMode !=
            TimelineClip::TitleKeyframe::TextExtrudeMode::None;
        if (m_widgets.titleTextExtrudeDepthSpin) m_widgets.titleTextExtrudeDepthSpin->setEnabled(enabled);
        if (m_widgets.titleTextExtrudeBevelSpin) m_widgets.titleTextExtrudeBevelSpin->setEnabled(enabled);
    }

    if (m_widgets.titleTextEdit) {
        // Avoid clobbering active typing/cursor position during inspector refresh.
        if (!m_widgets.titleTextEdit->hasFocus()) {
            const QSignalBlocker b(m_widgets.titleTextEdit);
            if (m_widgets.titleTextEdit->toPlainText() != display.text) {
                m_widgets.titleTextEdit->setPlainText(display.text);
            }
        }
    }
    if (m_widgets.titleFontCombo) {
        const QSignalBlocker b(m_widgets.titleFontCombo);
        m_widgets.titleFontCombo->setCurrentFont(QFont(display.fontFamily));
    }
    if (m_widgets.titleBoldCheck) {
        const QSignalBlocker b(m_widgets.titleBoldCheck);
        m_widgets.titleBoldCheck->setChecked(display.bold);
    }
    if (m_widgets.titleItalicCheck) {
        const QSignalBlocker b(m_widgets.titleItalicCheck);
        m_widgets.titleItalicCheck->setChecked(display.italic);
    }
    if (m_widgets.titleShadowEnabledCheck) {
        const QSignalBlocker b(m_widgets.titleShadowEnabledCheck);
        m_widgets.titleShadowEnabledCheck->setChecked(display.dropShadowEnabled);
    }
    if (m_widgets.titleWindowEnabledCheck) {
        const QSignalBlocker b(m_widgets.titleWindowEnabledCheck);
        m_widgets.titleWindowEnabledCheck->setChecked(display.windowEnabled);
    }
    if (m_widgets.titleWindowFrameEnabledCheck) {
        const QSignalBlocker b(m_widgets.titleWindowFrameEnabledCheck);
        m_widgets.titleWindowFrameEnabledCheck->setChecked(display.windowFrameEnabled);
    }
    if (m_widgets.titleColorButton) {
        m_selectedTitleColor = display.color;
        applyOverlayColorButtonStyle(m_widgets.titleColorButton, display.color);
    }
    if (m_widgets.titleShadowColorButton) {
        m_selectedShadowColor = display.dropShadowColor;
        applyOverlayColorButtonStyle(m_widgets.titleShadowColorButton, display.dropShadowColor);
    }
    if (m_widgets.titleWindowColorButton) {
        m_selectedWindowColor = display.windowColor;
        applyOverlayColorButtonStyle(m_widgets.titleWindowColorButton, display.windowColor);
    }
    if (m_widgets.titleWindowFrameColorButton) {
        m_selectedWindowFrameColor = display.windowFrameColor;
        applyOverlayColorButtonStyle(m_widgets.titleWindowFrameColorButton, display.windowFrameColor);
    }
}

int64_t TitlesTab::preferredEditFrame(const TimelineClip& clip) const
{
    if (m_selectedKeyframeFrame >= 0) {
        return qBound<int64_t>(0, m_selectedKeyframeFrame, qMax<int64_t>(0, clip.durationFrames - 1));
    }
    const int64_t currentFrame = m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : 0;
    const int64_t localFrame = qBound<int64_t>(
        0,
        currentFrame - clip.startFrame,
        qMax<int64_t>(0, clip.durationFrames - 1));
    return localFrame;
}

void TitlesTab::applyKeyframeFromInspector()
{
    if (m_updating) return;
    const QString clipId = m_deps.getSelectedClipId ? m_deps.getSelectedClipId() : QString();
    const TimelineClip *clip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
    if (clipId.isEmpty() || !clip || clip->mediaType != ClipMediaType::Title) return;

    const int64_t targetFrame = preferredEditFrame(*clip);
    bool updated = false;
    if (m_deps.updateClipById) {
        m_deps.updateClipById(clipId, [&](TimelineClip &clip) {
            upsertStoredTitleKeyframe(clip, buildStoredKeyframeFromInspector(targetFrame));
            updated = true;
        });
    }
    if (!updated) {
        return;
    }
    m_selectedKeyframeFrame = targetFrame;
    m_selectedKeyframeFrames = {targetFrame};
    applyPostEditEffects();
}

void TitlesTab::applyKeyframeFromInspectorLive()
{
    if (m_updating) {
        return;
    }
    const QString clipId = m_deps.getSelectedClipId ? m_deps.getSelectedClipId() : QString();
    const TimelineClip *clip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
    if (clipId.isEmpty() || !clip || clip->mediaType != ClipMediaType::Title) {
        return;
    }

    const int64_t targetFrame = preferredEditFrame(*clip);
    bool updated = false;
    if (m_deps.updateClipById) {
        m_deps.updateClipById(clipId, [&](TimelineClip &editable) {
            upsertStoredTitleKeyframe(editable, buildStoredKeyframeFromInspector(targetFrame));
            updated = true;
        });
    }
    if (!updated) {
        return;
    }

    m_selectedKeyframeFrame = targetFrame;
    m_selectedKeyframeFrames = {targetFrame};
    applyPostEditEffects({.refreshInspector = false, .pushHistory = false});
}

TimelineClip::TitleKeyframe TitlesTab::buildStoredKeyframeFromInspector(int64_t targetFrame) const
{
    TimelineClip::TitleKeyframe keyframe;
    // The inspector intentionally exposes the controls common to ordinary and
    // generated speaker titles.  Start from the current keyframe so editing a
    // common property never resets advanced material, pattern, width, or 3D
    // properties that were authored by the speaker-title workflow.
    if (m_deps.getSelectedClipConst) {
        const TimelineClip* clip = m_deps.getSelectedClipConst();
        if (clip && !clip->titleKeyframes.isEmpty()) {
            int sourceIndex = 0;
            for (int i = 0; i < clip->titleKeyframes.size(); ++i) {
                if (clip->titleKeyframes.at(i).frame <= targetFrame) {
                    sourceIndex = i;
                }
                if (clip->titleKeyframes.at(i).frame == targetFrame) {
                    sourceIndex = i;
                    break;
                }
            }
            keyframe = clip->titleKeyframes.at(sourceIndex);
        }
    }
    keyframe.frame = targetFrame;
    keyframe.text = m_widgets.titleTextEdit ? m_widgets.titleTextEdit->toPlainText() : QString();
    keyframe.translationX = m_widgets.titleXSpin ? m_widgets.titleXSpin->value() : 0.0;
    keyframe.translationY = m_widgets.titleYSpin ? m_widgets.titleYSpin->value() : 0.0;
    keyframe.fontSize = m_widgets.titleFontSizeSpin ? m_widgets.titleFontSizeSpin->value() : 48.0;
    keyframe.opacity = m_widgets.titleOpacitySpin ? m_widgets.titleOpacitySpin->value() : 1.0;
    keyframe.fontFamily = m_widgets.titleFontCombo
        ? m_widgets.titleFontCombo->currentFont().family() : kDefaultFontFamily;
    keyframe.bold = m_widgets.titleBoldCheck ? m_widgets.titleBoldCheck->isChecked() : true;
    keyframe.italic = m_widgets.titleItalicCheck ? m_widgets.titleItalicCheck->isChecked() : false;
    keyframe.color = m_selectedTitleColor;
    keyframe.dropShadowEnabled =
        m_widgets.titleShadowEnabledCheck ? m_widgets.titleShadowEnabledCheck->isChecked() : true;
    keyframe.dropShadowColor = m_selectedShadowColor;
    keyframe.dropShadowOpacity =
        m_widgets.titleShadowOpacitySpin ? m_widgets.titleShadowOpacitySpin->value() : 0.6;
    keyframe.dropShadowOffsetX =
        m_widgets.titleShadowOffsetXSpin ? m_widgets.titleShadowOffsetXSpin->value() : 2.0;
    keyframe.dropShadowOffsetY =
        m_widgets.titleShadowOffsetYSpin ? m_widgets.titleShadowOffsetYSpin->value() : 2.0;
    keyframe.windowEnabled =
        m_widgets.titleWindowEnabledCheck ? m_widgets.titleWindowEnabledCheck->isChecked() : false;
    keyframe.windowColor = m_selectedWindowColor;
    keyframe.windowOpacity =
        m_widgets.titleWindowOpacitySpin ? m_widgets.titleWindowOpacitySpin->value() : 0.35;
    keyframe.windowPadding =
        m_widgets.titleWindowPaddingSpin ? m_widgets.titleWindowPaddingSpin->value() : 16.0;
    keyframe.windowFrameEnabled =
        m_widgets.titleWindowFrameEnabledCheck ? m_widgets.titleWindowFrameEnabledCheck->isChecked() : false;
    keyframe.windowFrameColor = m_selectedWindowFrameColor;
    keyframe.windowFrameOpacity =
        m_widgets.titleWindowFrameOpacitySpin ? m_widgets.titleWindowFrameOpacitySpin->value() : 1.0;
    keyframe.windowFrameWidth =
        m_widgets.titleWindowFrameWidthSpin ? m_widgets.titleWindowFrameWidthSpin->value() : 2.0;
    keyframe.windowFrameGap =
        m_widgets.titleWindowFrameGapSpin ? m_widgets.titleWindowFrameGapSpin->value() : 4.0;
    keyframe.textExtrudeMode = m_widgets.titleTextExtrudeModeCombo
        ? static_cast<TimelineClip::TitleKeyframe::TextExtrudeMode>(
              m_widgets.titleTextExtrudeModeCombo->currentData().toInt())
        : TimelineClip::TitleKeyframe::TextExtrudeMode::None;
    keyframe.vulkan3DExtrudeEnabled =
        keyframe.textExtrudeMode != TimelineClip::TitleKeyframe::TextExtrudeMode::None;
    keyframe.vulkan3DEnabled = keyframe.vulkan3DEnabled || keyframe.vulkan3DExtrudeEnabled;
    keyframe.vulkan3DExtrudeDepth = m_widgets.titleTextExtrudeDepthSpin
        ? m_widgets.titleTextExtrudeDepthSpin->value() : 0.16;
    keyframe.vulkan3DBevelScale = m_widgets.titleTextExtrudeBevelSpin
        ? m_widgets.titleTextExtrudeBevelSpin->value() : 0.7;
    // Keep every title source on the same bounded style contract. Advanced
    // title-only fields remain untouched by the adapter.
    applyOverlayTextStyle(overlayTextStyleFromTitle(keyframe), &keyframe);
    return keyframe;
}

void TitlesTab::upsertKeyframeAtPlayhead()
{
    const QString clipId = m_deps.getSelectedClipId ? m_deps.getSelectedClipId() : QString();
    const TimelineClip *clip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
    if (clipId.isEmpty() || !clip || clip->mediaType != ClipMediaType::Title) return;

    const int64_t currentFrame = m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : 0;
    const int64_t clipStart = m_deps.getSelectedClipStartFrame ? m_deps.getSelectedClipStartFrame() : 0;
    const int64_t localFrame = qMax<int64_t>(0, currentFrame - clipStart);

    if (m_deps.updateClipById) {
        m_deps.updateClipById(clipId, [&](TimelineClip &clip) {
            const int64_t clampedFrame = qBound<int64_t>(0, localFrame, qMax<int64_t>(0, clip.durationFrames - 1));
            if (findTitleKeyframeIndex(clip, clampedFrame) < 0) {
                upsertStoredTitleKeyframe(clip, buildStoredKeyframeFromInspector(clampedFrame));
            }
            m_selectedKeyframeFrame = clampedFrame;
            m_selectedKeyframeFrames = {clampedFrame};
        });
    }
    applyPostEditEffects();
}

void TitlesTab::removeSelectedKeyframes()
{
    if (!hasRemovableKeyframeSelection()) return;
    const QString clipId = m_deps.getSelectedClipId ? m_deps.getSelectedClipId() : QString();
    const TimelineClip *clip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
    if (clipId.isEmpty() || !clip || clip->mediaType != ClipMediaType::Title) return;

    const QSet<int64_t> framesToRemove = m_selectedKeyframeFrames;

    if (m_deps.updateClipById) {
        m_deps.updateClipById(clipId, [&](TimelineClip &clip) {
            removeStoredTitleKeyframes(clip, framesToRemove);
        });
    }
    m_selectedKeyframeFrame = -1;
    m_selectedKeyframeFrames.clear();
    applyPostEditEffects();
}

void TitlesTab::centerHorizontal()
{
    if (m_updating || m_selectedKeyframeFrame < 0) return;
    const QString clipId = m_deps.getSelectedClipId ? m_deps.getSelectedClipId() : QString();
    const TimelineClip *clip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
    if (clipId.isEmpty() || !clip || clip->mediaType != ClipMediaType::Title) return;

    const int64_t targetFrame = m_selectedKeyframeFrame;
    if (m_deps.updateClipById) {
        m_deps.updateClipById(clipId, [targetFrame](TimelineClip &clip) {
            updateStoredTitleKeyframeAtFrame(clip, targetFrame, [](TimelineClip::TitleKeyframe& keyframe) {
                keyframe.translationX = 0.0;
            });
        });
    }
    if (m_widgets.titleXSpin) {
        const QSignalBlocker b(m_widgets.titleXSpin);
        m_widgets.titleXSpin->setValue(0.0);
    }
    applyPostEditEffects();
}

void TitlesTab::centerVertical()
{
    if (m_updating || m_selectedKeyframeFrame < 0) return;
    const QString clipId = m_deps.getSelectedClipId ? m_deps.getSelectedClipId() : QString();
    const TimelineClip *clip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
    if (clipId.isEmpty() || !clip || clip->mediaType != ClipMediaType::Title) return;

    const int64_t targetFrame = m_selectedKeyframeFrame;
    if (m_deps.updateClipById) {
        m_deps.updateClipById(clipId, [targetFrame](TimelineClip &clip) {
            updateStoredTitleKeyframeAtFrame(clip, targetFrame, [](TimelineClip::TitleKeyframe& keyframe) {
                keyframe.translationY = 0.0;
            });
        });
    }
    if (m_widgets.titleYSpin) {
        const QSignalBlocker b(m_widgets.titleYSpin);
        m_widgets.titleYSpin->setValue(0.0);
    }
    applyPostEditEffects();
}

void TitlesTab::syncTableToPlayhead()
{
    auto *table = m_widgets.titleKeyframeTable;
    if (shouldSkipSyncToPlayhead(table, m_widgets.titleAutoScrollCheck)) {
        return;
    }

    const TimelineClip *clip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
    if (!clip || clip->mediaType != ClipMediaType::Title || clip->titleKeyframes.isEmpty()) return;

    const int64_t currentFrame = m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : 0;
    const int64_t localFrame = qMax<int64_t>(0, currentFrame - clip->startFrame);

    int bestRow = 0;
    for (int row = 0; row < table->rowCount(); ++row) {
        const int64_t rowFrame = editor::rowFrameRole(table, row);
        if (rowFrame <= localFrame) {
            bestRow = row;
        }
    }
    applySyncedRowSelection(table,
                            bestRow,
                            m_widgets.titleAutoScrollCheck &&
                                m_widgets.titleAutoScrollCheck->isChecked());
}

int TitlesTab::selectedKeyframeIndex(const TimelineClip &clip) const
{
    if (m_selectedKeyframeFrame < 0) return -1;
    for (int i = 0; i < clip.titleKeyframes.size(); ++i) {
        if (clip.titleKeyframes[i].frame == m_selectedKeyframeFrame) return i;
    }
    return -1;
}

int TitlesTab::nearestKeyframeIndex(const TimelineClip &clip, int64_t localFrame) const
{
    if (clip.titleKeyframes.isEmpty()) return -1;
    int best = 0;
    int64_t bestDist = std::abs(clip.titleKeyframes[0].frame - localFrame);
    for (int i = 1; i < clip.titleKeyframes.size(); ++i) {
        const int64_t dist = std::abs(clip.titleKeyframes[i].frame - localFrame);
        if (dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return best;
}

bool TitlesTab::hasRemovableKeyframeSelection() const
{
    return !m_selectedKeyframeFrames.isEmpty();
}

void TitlesTab::onTableItemChanged(QTableWidgetItem *item)
{
    if (m_updating || !item) return;
    auto *table = m_widgets.titleKeyframeTable;
    if (!table) return;

    const int row = item->row();
    const int64_t originalFrame = editor::rowFrameRole(table, row);
    const QString clipId = m_deps.getSelectedClipId ? m_deps.getSelectedClipId() : QString();
    if (clipId.isEmpty()) return;
    const TimelineClip *selectedClip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
    if (!selectedClip || selectedClip->mediaType != ClipMediaType::Title) return;

    const int changedColumn = item->column();
    bool okStart = false;
    bool okEnd = false;
    bool okFrame = false;
    const int64_t startCellValue = table->item(row, 0)
        ? table->item(row, 0)->text().toLongLong(&okStart) : originalFrame;
    const int64_t endCellValue = table->item(row, 1)
        ? table->item(row, 1)->text().toLongLong(&okEnd) : originalFrame;
    const int64_t frameCellValue = table->item(row, 2)
        ? table->item(row, 2)->text().toLongLong(&okFrame) : originalFrame;
    if ((changedColumn == 0 && !okStart) ||
        (changedColumn == 1 && !okEnd) ||
        (changedColumn == 2 && !okFrame)) {
        refresh();
        return;
    }

    // Text column is now at index 3
    const QString text = table->item(row, 3) ? table->item(row, 3)->text() : QString();
    // X column is now at index 4
    const double x = table->item(row, 4) ? table->item(row, 4)->text().toDouble() : 0.0;
    // Y column is now at index 5
    const double y = table->item(row, 5) ? table->item(row, 5)->text().toDouble() : 0.0;
    // Size column is now at index 6
    const double fontSize = table->item(row, 6) ? table->item(row, 6)->text().toDouble() : 48.0;
    // Opacity column is now at index 7
    const double opacity = table->item(row, 7) ? table->item(row, 7)->text().toDouble() : 1.0;

    const int64_t clipEndFrame = qMax<int64_t>(0, selectedClip->durationFrames - 1);
    const auto resolvePythonStyleFrame = [clipEndFrame](int64_t value) -> int64_t {
        if (value < 0) {
            return (clipEndFrame + 1) + value;
        }
        return value;
    };
    const int64_t previousFrame = row > 0 ? selectedClip->titleKeyframes[row - 1].frame : -1;
    const int64_t currentFrame = selectedClip->titleKeyframes[row].frame;
    const int64_t nextFrame = (row + 1 < selectedClip->titleKeyframes.size())
        ? selectedClip->titleKeyframes[row + 1].frame : -1;
    const int64_t nextNextFrame = (row + 2 < selectedClip->titleKeyframes.size())
        ? selectedClip->titleKeyframes[row + 2].frame : -1;

    int64_t updatedCurrentFrame = currentFrame;
    int64_t updatedNextFrame = nextFrame;
    int64_t updatedClipEndFrame = clipEndFrame;

    if (changedColumn == 0 || changedColumn == 2) {
        const int64_t requested = resolvePythonStyleFrame((changedColumn == 0) ? startCellValue : frameCellValue);
        const int64_t minAllowed = previousFrame + 1;
        const int64_t maxAllowed = (nextFrame >= 0) ? (nextFrame - 1) : clipEndFrame;
        updatedCurrentFrame = qBound(minAllowed, requested, qMax(minAllowed, maxAllowed));
    } else if (changedColumn == 1) {
        const int64_t requestedEnd = resolvePythonStyleFrame(endCellValue);
        if (nextFrame >= 0) {
            // Editing End adjusts the next keyframe frame (end = next - 1).
            const int64_t minEnd = currentFrame;
            const int64_t maxEnd = (nextNextFrame >= 0) ? (nextNextFrame - 2) : clipEndFrame;
            const int64_t clampedEnd = qBound(minEnd, requestedEnd, qMax(minEnd, maxEnd));
            updatedNextFrame = clampedEnd + 1;
        } else {
            // Last row End edits the clip duration.
            updatedClipEndFrame = qMax<int64_t>(currentFrame, requestedEnd);
        }
    }

    if (m_deps.updateClipById) {
        m_deps.updateClipById(clipId, [&](TimelineClip &clip) {
            const int64_t originalNextFrame = nextFrame;
            TimelineClip::TitleKeyframe updatedKeyframe = clip.titleKeyframes.value(row);
            updatedKeyframe.frame = updatedCurrentFrame;
            updatedKeyframe.text = text;
            updatedKeyframe.translationX = x;
            updatedKeyframe.translationY = y;
            updatedKeyframe.fontSize = fontSize;
            updatedKeyframe.opacity = opacity;
            replaceStoredTitleKeyframeAtFrame(clip, originalFrame, updatedKeyframe);
            if (changedColumn == 1 && originalNextFrame >= 0) {
                updateStoredTitleKeyframeAtFrame(clip, originalNextFrame, [updatedNextFrame](TimelineClip::TitleKeyframe& keyframe) {
                    keyframe.frame = updatedNextFrame;
                });
            } else if (changedColumn == 1) {
                clip.durationFrames = qMax<int64_t>(1, updatedClipEndFrame + 1);
                if (clip.mediaType == ClipMediaType::Title) {
                    clip.sourceDurationFrames = clip.durationFrames;
                }
            }
        });
    }
    m_selectedKeyframeFrame = updatedCurrentFrame;
    m_selectedKeyframeFrames = {updatedCurrentFrame};
    if (m_deps.setPreviewTimelineClips) m_deps.setPreviewTimelineClips();
    if (m_deps.refreshInspector) m_deps.refreshInspector();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TitlesTab::onTableSelectionChanged()
{
    if (m_updating || m_syncingTableSelection) return;
    auto *table = m_widgets.titleKeyframeTable;
    if (!table) return;

    m_selectedKeyframeFrames = editor::collectSelectedFrameRoles(table);
    m_selectedKeyframeFrame = editor::primarySelectedFrameRole(table);
    
    if (m_selectedKeyframeFrame < 0) {
        m_selectedKeyframeFrames.clear();
        return;
    }
    
    // Suppress auto-sync for this timeline frame to prevent the table from
    // jumping immediately after user clicks a row (which seeks to that frame).
    const TimelineClip *selectedClip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
    if (selectedClip) {
        m_suppressSyncForTimelineFrame = selectedClip->startFrame + m_selectedKeyframeFrame;
    }

    const TimelineClip *clip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
    if (!clip || clip->mediaType != ClipMediaType::Title) {
        return;
    }
    if (clip && m_selectedKeyframeFrame >= 0) {
        const int idx = selectedKeyframeIndex(*clip);
        if (idx >= 0) {
            m_updating = true;
            TitleKeyframeDisplay display;
            const auto &kf = clip->titleKeyframes[idx];
            display.frame = kf.frame;
            display.text = kf.text;
            display.translationX = kf.translationX;
            display.translationY = kf.translationY;
            display.fontSize = kf.fontSize;
            display.opacity = kf.opacity;
            display.fontFamily = kf.fontFamily;
            display.bold = kf.bold;
            display.italic = kf.italic;
            display.color = kf.color;
            display.dropShadowEnabled = kf.dropShadowEnabled;
            display.dropShadowColor = kf.dropShadowColor;
            display.dropShadowOpacity = kf.dropShadowOpacity;
            display.dropShadowOffsetX = kf.dropShadowOffsetX;
            display.dropShadowOffsetY = kf.dropShadowOffsetY;
            display.windowEnabled = kf.windowEnabled;
            display.windowColor = kf.windowColor;
            display.windowOpacity = kf.windowOpacity;
            display.windowPadding = kf.windowPadding;
            display.windowFrameEnabled = kf.windowFrameEnabled;
            display.windowFrameColor = kf.windowFrameColor;
            display.windowFrameOpacity = kf.windowFrameOpacity;
            display.windowFrameWidth = kf.windowFrameWidth;
            display.windowFrameGap = kf.windowFrameGap;
            display.textExtrudeMode = kf.textExtrudeMode;
            display.textExtrudeDepth = kf.vulkan3DExtrudeDepth;
            display.textExtrudeBevel = kf.vulkan3DBevelScale;
            display.linearInterpolation = kf.linearInterpolation;
            updateWidgetsFromKeyframe(display);
            m_updating = false;
        }
    }

    if (m_deps.onKeyframeSelectionChanged) m_deps.onKeyframeSelectionChanged();
}

void TitlesTab::onTableItemClicked(QTableWidgetItem *item)
{
    if (!item || m_updating) return;
    const TimelineClip *selectedClip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
    if (!selectedClip || selectedClip->mediaType != ClipMediaType::Title) return;
    // Column 8 = Interp toggle (0=Start, 1=End, 2=Frame, 3=Text, 4=X, 5=Y, 6=Size, 7=Opacity, 8=Interp)
    if (item->column() == 8) {
        const bool isLinear = item->text() == QStringLiteral("Linear");
        item->setText(isLinear ? QStringLiteral("Step") : QStringLiteral("Linear"));

        const int64_t frame = editor::rowFrameRole(m_widgets.titleKeyframeTable, item->row());
        const QString clipId = m_deps.getSelectedClipId ? m_deps.getSelectedClipId() : QString();
        if (!clipId.isEmpty() && m_deps.updateClipById) {
            m_deps.updateClipById(clipId, [&](TimelineClip &clip) {
                for (auto &kf : clip.titleKeyframes) {
                    if (kf.frame == frame) {
                        kf.linearInterpolation = !isLinear;
                        break;
                    }
                }
            });
        }
        if (m_deps.setPreviewTimelineClips) m_deps.setPreviewTimelineClips();
        if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
        if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
    }
}

void TitlesTab::onTableCustomContextMenu(const QPoint &pos)
{
    auto *table = m_widgets.titleKeyframeTable;
    if (!table) return;

    int row = -1;
    QTableWidgetItem *item = editor::ensureContextRowSelected(table, pos, &row);
    if (!item) return;

    const int64_t anchorFrame = item->data(Qt::UserRole).toLongLong();
    const int64_t previousFrame = editor::rowFrameRole(table, row - 1);
    const int64_t nextFrame = editor::rowFrameRole(table, row + 1);

    QMenu menu;
    auto *addAbove = menu.addAction(QStringLiteral("Add Keyframe Above"));
    addAbove->setEnabled(previousFrame >= 0);
    auto *addBelow = menu.addAction(QStringLiteral("Add Keyframe Below"));
    addBelow->setEnabled(nextFrame >= 0);
    menu.addSeparator();
    
    const int deletableRowCount = editor::countSelectedFrameRoles(table, [](int64_t) { return true; });
    auto *deleteAction = menu.addAction(deletableRowCount == 1
                                            ? QStringLiteral("Delete Row")
                                            : QStringLiteral("Delete Rows"));
    deleteAction->setEnabled(deletableRowCount > 0);

    QAction *chosen = menu.exec(table->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    const QString clipId = m_deps.getSelectedClipId ? m_deps.getSelectedClipId() : QString();
    if (clipId.isEmpty()) return;

    const TimelineClip *selectedClip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
    if (!selectedClip || selectedClip->mediaType != ClipMediaType::Title) return;

    if (chosen == deleteAction && deleteAction->isEnabled()) {
        removeSelectedKeyframes();
    } else if ((chosen == addAbove && addAbove->isEnabled()) ||
               (chosen == addBelow && addBelow->isEnabled())) {
        const int64_t frameA = (chosen == addAbove) ? previousFrame : anchorFrame;
        const int64_t frameB = (chosen == addAbove) ? anchorFrame : nextFrame;
        const int64_t midpointFrame = frameA + ((frameB - frameA) / 2);
        
        if (midpointFrame > frameA && midpointFrame < frameB) {
            // Find the two surrounding keyframes and interpolate
            const TimelineClip::TitleKeyframe *kfA = nullptr;
            const TimelineClip::TitleKeyframe *kfB = nullptr;
            for (const auto &kf : selectedClip->titleKeyframes) {
                if (kf.frame == frameA) kfA = &kf;
                if (kf.frame == frameB) kfB = &kf;
            }
            
            if (kfA && kfB && m_deps.updateClipById) {
                const TimelineClip::TitleKeyframe midpoint =
                    interpolateStoredTitleKeyframe(*kfA, *kfB, midpointFrame);

                m_deps.updateClipById(clipId, [&](TimelineClip &clip) {
                    upsertStoredTitleKeyframe(clip, midpoint);
                });
                m_selectedKeyframeFrame = midpointFrame;
                m_selectedKeyframeFrames = {midpointFrame};
                applyPostEditEffects({.refreshInspector = false});
                refresh();
            }
        }
    }
}

bool TitlesTab::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_widgets.titleTextEdit) {
        // Handle Ctrl+Enter in title text edit to insert line break.
        if (event->type() == QEvent::KeyPress) {
            QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
            if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) &&
                (keyEvent->modifiers() & Qt::ControlModifier)) {
                QTextCursor cursor = m_widgets.titleTextEdit->textCursor();
                cursor.insertText("\n");
                return true;
            }
        }
        // Commit a full inspector save when the text editor loses focus.
        if (event->type() == QEvent::FocusOut) {
            if (!m_updating) {
                applyKeyframeFromInspector();
            }
        }
        // Never route text edit key events to keyframe-table delete handlers.
        return QObject::eventFilter(watched, event);
    }

    return KeyframeTabBase::eventFilter(watched, event);
}
