#include "mask_tab.h"
#include "editor_effect_presets.h"
#include "editor_tab_edit_effects.h"
#include "mask_sidecar.h"
#include "mask_fuzzy_remove.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QDir>
#include <QFileInfo>
#include <QProgressDialog>
#include <QPointer>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

MaskTab::MaskTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
}

namespace {
constexpr int kSidecarIdRole = Qt::UserRole + 1;
constexpr int kSidecarReadyRole = Qt::UserRole + 2;

TabEditCallbacks maskEditCallbacks(const MaskTab::Dependencies& deps)
{
    return TabEditCallbacks{
        .updatePreview = deps.setPreviewTimelineClips,
        .refreshInspector = deps.refreshInspector,
        .scheduleSave = deps.scheduleSaveState,
        .pushHistory = deps.pushHistorySnapshot,
    };
}

editor::masks::MaskSidecar autoMaskSidecarForClip(const TimelineClip& clip)
{
    const QVector<editor::masks::MaskSidecar> sidecars =
        editor::masks::discoverMaskSidecars(clip);
    const auto ready = std::find_if(
        sidecars.cbegin(), sidecars.cend(), [](const editor::masks::MaskSidecar& sidecar) {
            return sidecar.isReadyForTimeline();
        });
    return ready == sidecars.cend() ? editor::masks::MaskSidecar{} : *ready;
}

QString maskSourceIdForClip(const TimelineClip& clip)
{
    if (clip.clipRole == ClipRole::MaskMatte) {
        return clip.linkedSourceClipId.trimmed();
    }
    return clip.clipRole == ClipRole::Media ? clip.id.trimmed() : QString();
}

QString maskSidecarIdForClip(const TimelineClip& clip)
{
    const QString persistedId = clip.generatedFromMaskId.trimmed();
    return !persistedId.isEmpty()
        ? persistedId
        : editor::masks::stableMaskSidecarId(clip.maskFramesDir);
}

bool supportsBiRefNetRefinement(const editor::masks::MaskSidecar& sidecar)
{
    return sidecar.isReadyForTimeline() &&
        sidecar.sourceType.contains(QStringLiteral("sam"), Qt::CaseInsensitive) &&
        !sidecar.sourceType.contains(
            QStringLiteral("continuous_alpha"), Qt::CaseInsensitive);
}

}

void MaskTab::wire()
{
    if (!m_treatmentEditTimer) {
        m_treatmentEditTimer = new QTimer(this);
        m_treatmentEditTimer->setSingleShot(true);
        m_treatmentEditTimer->setInterval(75);
        connect(m_treatmentEditTimer, &QTimer::timeout, this, [this]() {
            const bool zLevelEdited = m_pendingTreatmentZLevelEdited;
            m_pendingTreatmentZLevelEdited = false;
            applyTreatmentEdit(false, zLevelEdited);
        });
    }
    auto connectApply = [this](auto* widget) {
        if (widget) {
            connect(widget, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
                scheduleTreatmentEdit(false);
            });
            connect(widget, &QDoubleSpinBox::editingFinished, this, [this]() {
                if (m_treatmentEditTimer) {
                    m_treatmentEditTimer->stop();
                }
                m_pendingTreatmentZLevelEdited = false;
                applyTreatmentEdit(true);
            });
        }
    };

    if (m_widgets.enabledCheck) {
        connect(m_widgets.enabledCheck, &QCheckBox::toggled, this, [this](bool) { apply(true); });
    }
    if (m_widgets.framesDirEdit) {
        connect(m_widgets.framesDirEdit, &QLineEdit::editingFinished, this, [this]() { apply(true); });
    }
    if (m_widgets.sidecarCombo) {
        connect(m_widgets.sidecarCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this](int index) {
            if (m_updating || index < 0 || !m_widgets.framesDirEdit) return;
            m_widgets.framesDirEdit->setText(
                m_widgets.sidecarCombo->itemData(index).toString());
            if (m_widgets.enabledCheck) {
                QSignalBlocker blocker(m_widgets.enabledCheck);
                m_widgets.enabledCheck->setChecked(true);
            }
            apply(true);
        });
    }
    if (m_widgets.browseButton) {
        connect(m_widgets.browseButton, &QPushButton::clicked, this, [this]() {
            if (!m_deps.chooseMaskDirectory || !m_widgets.framesDirEdit) {
                return;
            }
            const QString selected = m_deps.chooseMaskDirectory(
                m_widgets.browseButton,
                m_widgets.framesDirEdit->text().trimmed());
            if (selected.isEmpty()) {
                return;
            }
            m_widgets.framesDirEdit->setText(selected);
            if (m_widgets.enabledCheck) {
                QSignalBlocker blocker(m_widgets.enabledCheck);
                m_widgets.enabledCheck->setChecked(true);
            }
            apply(true);
        });
    }
    if (m_widgets.newPromptButton) {
        connect(m_widgets.newPromptButton, &QPushButton::clicked, this, [this]() {
            const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
            if (clip && m_deps.generatePromptMask) {
                const QString sourceId = clip->clipRole == ClipRole::MaskMatte
                    ? clip->linkedSourceClipId.trimmed()
                    : clip->id;
                if (!sourceId.isEmpty()) {
                    m_deps.generatePromptMask(sourceId);
                }
            }
        });
    }
    if (m_widgets.biRefNetRefineButton) {
        connect(m_widgets.biRefNetRefineButton, &QPushButton::clicked, this, [this]() {
            const TimelineClip* clip =
                m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
            if (!clip || clip->clipRole != ClipRole::MaskMatte ||
                !m_deps.refineMaskWithBiRefNet) {
                return;
            }
            const editor::masks::MaskSidecar sidecar =
                editor::masks::inspectMaskSidecar(
                    clip->maskFramesDir,
                    QFileInfo(clip->filePath).completeBaseName(),
                    clip->filePath);
            if (!supportsBiRefNetRefinement(sidecar)) {
                return;
            }
            const int radius = m_widgets.biRefNetGuideRadiusSpin
                ? m_widgets.biRefNetGuideRadiusSpin->value()
                : 24;
            m_deps.refineMaskWithBiRefNet(
                clip->linkedSourceClipId.trimmed(), sidecar.directory, radius);
        });
    }
    if (m_widgets.fuzzyRemoveButton) {
        connect(m_widgets.fuzzyRemoveButton, &QPushButton::toggled, this, [this](bool armed) {
            if (m_deps.setMaskFuzzyRemoveMode) m_deps.setMaskFuzzyRemoveMode(armed);
            if (m_widgets.fuzzyStatusLabel) {
                m_widgets.fuzzyStatusLabel->setText(
                    armed ? QStringLiteral("Click the unwanted mask foreground in the preview.")
                          : QString());
            }
        });
    }
    if (m_widgets.zLevelSpin) {
        connect(m_widgets.zLevelSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, [this](int) { scheduleTreatmentEdit(true); });
        connect(m_widgets.zLevelSpin, &QSpinBox::editingFinished,
                this, [this]() {
                    if (m_treatmentEditTimer) {
                        m_treatmentEditTimer->stop();
                    }
                    m_pendingTreatmentZLevelEdited = false;
                    applyTreatmentEdit(true, true);
                });
    }
    for (QCheckBox* check : {m_widgets.invertCheck,
                             m_widgets.temporalStabilizeCheck,
                             m_widgets.showOnlyCheck,
                             m_widgets.foregroundLayerCheck,
                             m_widgets.repeatEnabledCheck,
                             m_widgets.shadowEnabledCheck}) {
        if (check) {
            connect(check, &QCheckBox::toggled, this, [this](bool) { apply(true); });
        }
    }
    connectApply(m_widgets.featherSpin);
    connectApply(m_widgets.featherPowerSpin);
    if (m_widgets.featherFalloffCombo) {
        connect(m_widgets.featherFalloffCombo,
                qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            if (m_updating) return;
            if (m_widgets.featherPowerSpin) {
                m_widgets.featherPowerSpin->setEnabled(
                    m_widgets.featherFalloffCombo->currentData().toInt() == 0);
            }
            apply(true);
        });
    }
    connectApply(m_widgets.dilateSpin);
    connectApply(m_widgets.erodeSpin);
    connectApply(m_widgets.blurSpin);
    connectApply(m_widgets.temporalStabilizeStrengthSpin);
    if (m_widgets.temporalStabilizeMotionRadiusSpin) {
        connect(m_widgets.temporalStabilizeMotionRadiusSpin,
                qOverload<int>(&QSpinBox::valueChanged),
                this, [this](int) { scheduleTreatmentEdit(); });
        connect(m_widgets.temporalStabilizeMotionRadiusSpin,
                &QSpinBox::editingFinished,
                this, [this]() { applyTreatmentEdit(true); });
    }
    connectApply(m_widgets.opacitySpin);
    connectApply(m_widgets.repeatDeltaXSpin);
    connectApply(m_widgets.repeatDeltaYSpin);
    connectApply(m_widgets.shadowRadiusSpin);
    connectApply(m_widgets.shadowOffsetXSpin);
    connectApply(m_widgets.shadowOffsetYSpin);
    connectApply(m_widgets.shadowOpacitySpin);
}

void MaskTab::handlePreviewPoint(const QString& clipId,
                                 int64_t sourceFrame,
                                 int64_t sourcePresentationTimestamp,
                                 qreal xNorm,
                                 qreal yNorm)
{
    const TimelineClip* selected = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!selected || selected->id != clipId ||
        selected->clipRole != ClipRole::MaskMatte ||
        !m_widgets.fuzzyRemoveButton || !m_widgets.fuzzyRemoveButton->isChecked()) {
        return;
    }
    const QString selectedId = selected->id;
    editor::masks::FuzzyRemoveRequest request;
    request.sourceDirectory = selected->maskFramesDir;
    request.sourceMediaPath = selected->filePath;
    request.sourceFrame = sourceFrame;
    request.sourcePresentationTimestamp = sourcePresentationTimestamp;
    request.xNorm = xNorm;
    request.yNorm = yNorm;
    request.spatialReachPixels = m_widgets.fuzzySpatialReachSpin
        ? m_widgets.fuzzySpatialReachSpin->value() : 12;
    request.temporalReachFrames = m_widgets.fuzzyTemporalReachSpin
        ? m_widgets.fuzzyTemporalReachSpin->value() : 120;

    m_widgets.fuzzyRemoveButton->setChecked(false);
    m_widgets.fuzzyRemoveButton->setEnabled(false);
    if (m_widgets.fuzzyStatusLabel) {
        m_widgets.fuzzyStatusLabel->setText(QStringLiteral("Analyzing region safely…"));
    }
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    auto* progress = new QProgressDialog(
        QStringLiteral("Analyzing mask continuity…"),
        QStringLiteral("Cancel"),
        0,
        qMax(1, request.temporalReachFrames * 2),
        m_widgets.fuzzyRemoveButton);
    progress->setWindowTitle(QStringLiteral("Mask Removal Analysis"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->show();
    connect(progress, &QProgressDialog::canceled, this, [cancel]() {
        cancel->store(true, std::memory_order_relaxed);
    });
    auto* watcher = new QFutureWatcher<editor::masks::FuzzyRemoveAnalysis>(this);
    connect(watcher, &QFutureWatcher<editor::masks::FuzzyRemoveAnalysis>::finished,
            this, [this, watcher, progress, selectedId]() {
        editor::masks::FuzzyRemoveAnalysis analysis = watcher->result();
        watcher->deleteLater();
        progress->close();
        progress->deleteLater();
        if (m_widgets.fuzzyRemoveButton) m_widgets.fuzzyRemoveButton->setEnabled(true);
        if (!analysis.succeeded()) {
            if (m_widgets.fuzzyStatusLabel) {
                m_widgets.fuzzyStatusLabel->setText(
                    analysis.cancelled
                        ? QStringLiteral("Mask analysis cancelled.")
                        : (analysis.error.isEmpty()
                               ? QStringLiteral("No safely trackable region was found.")
                               : analysis.error));
            }
            return;
        }
        const TimelineClip* current =
            m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
        if (!current || current->id != selectedId) {
            if (m_widgets.fuzzyStatusLabel) {
                m_widgets.fuzzyStatusLabel->setText(QStringLiteral(
                    "Selection changed; the analyzed mask edit was not applied."));
            }
            return;
        }
        const bool accepted = m_deps.confirmFuzzyRemoveAnalysis
            ? m_deps.confirmFuzzyRemoveAnalysis(analysis)
            : confirmFuzzyRemoveAnalysis(analysis);
        if (!accepted) {
            if (m_widgets.fuzzyStatusLabel) {
                m_widgets.fuzzyStatusLabel->setText(
                    QStringLiteral("Mask removal cancelled before applying."));
            }
            return;
        }
        materializeFuzzyRemoveAnalysis(selectedId, std::move(analysis));
    });
    QPointer<QProgressDialog> guardedProgress(progress);
    watcher->setFuture(QtConcurrent::run([request, cancel, guardedProgress]() {
        return editor::masks::analyzeFuzzyRemoveMaskRegion(
            request,
            cancel,
            [guardedProgress](int completed, int total, const QString& phase) {
                if (!guardedProgress) return;
                QMetaObject::invokeMethod(
                    guardedProgress,
                    [guardedProgress, completed, total, phase]() {
                        if (!guardedProgress) return;
                        guardedProgress->setMaximum(qMax(1, total));
                        guardedProgress->setValue(qMin(completed, total));
                        guardedProgress->setLabelText(phase);
                    },
                    Qt::QueuedConnection);
            });
    }));
}

bool MaskTab::confirmFuzzyRemoveAnalysis(
    const editor::masks::FuzzyRemoveAnalysis& analysis) const
{
    QDialog dialog(m_widgets.fuzzyRemoveButton);
    dialog.setWindowTitle(QStringLiteral("Review Mask Removal"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* preview = new QLabel(&dialog);
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumSize(480, 270);
    preview->setPixmap(QPixmap::fromImage(analysis.seedPreview).scaled(
        QSize(720, 480), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    layout->addWidget(preview);
    auto* summary = new QLabel(
        QStringLiteral(
            "Red pixels will be removed.\n"
            "Frames: %1–%2 (%3 total) · Pixels: %4\n"
            "Backward: %5\nForward: %6")
            .arg(analysis.firstMaskOrdinal + 1)
            .arg(analysis.lastMaskOrdinal + 1)
            .arg(analysis.frames.size())
            .arg(analysis.selectedPixels)
            .arg(analysis.backwardStopReason, analysis.forwardStopReason),
        &dialog);
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto* warning = new QLabel(
        QStringLiteral(
            "The source sidecar remains unchanged. Applying creates a reversible "
            "derived cache and records the edit recipe in project history."),
        &dialog);
    warning->setWordWrap(true);
    layout->addWidget(warning);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    return dialog.exec() == QDialog::Accepted;
}

void MaskTab::materializeFuzzyRemoveAnalysis(
    const QString& selectedId,
    editor::masks::FuzzyRemoveAnalysis analysis)
{
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    auto* progress = new QProgressDialog(
        QStringLiteral("Publishing non-destructive mask edit…"),
        QStringLiteral("Cancel"),
        0,
        qMax(1, analysis.frames.size()),
        m_widgets.fuzzyRemoveButton);
    progress->setWindowTitle(QStringLiteral("Mask Removal"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->show();
    connect(progress, &QProgressDialog::canceled, this, [cancel]() {
        cancel->store(true, std::memory_order_relaxed);
    });
    if (m_widgets.fuzzyRemoveButton) m_widgets.fuzzyRemoveButton->setEnabled(false);
    if (m_widgets.fuzzyStatusLabel) {
        m_widgets.fuzzyStatusLabel->setText(QStringLiteral("Publishing reviewed mask edit…"));
    }

    auto* watcher = new QFutureWatcher<editor::masks::FuzzyRemoveResult>(this);
    connect(watcher, &QFutureWatcher<editor::masks::FuzzyRemoveResult>::finished,
            this, [this, watcher, progress, selectedId, analysis]() {
        const editor::masks::FuzzyRemoveResult result = watcher->result();
        watcher->deleteLater();
        progress->close();
        progress->deleteLater();
        if (m_widgets.fuzzyRemoveButton) m_widgets.fuzzyRemoveButton->setEnabled(true);
        if (!result.succeeded()) {
            if (m_widgets.fuzzyStatusLabel) {
                m_widgets.fuzzyStatusLabel->setText(
                    result.cancelled ? QStringLiteral("Mask removal cancelled.")
                                     : result.error);
            }
            return;
        }
        bool sourceStillCurrent = false;
        const bool updated = m_deps.updateClipById &&
            m_deps.updateClipById(
                selectedId,
                [&result, &analysis, &sourceStillCurrent](TimelineClip& clip) {
                if (QDir::cleanPath(clip.maskFramesDir) !=
                    QDir::cleanPath(analysis.request.sourceDirectory)) {
                    return;
                }
                sourceStillCurrent = true;
                if (clip.maskOriginalFramesDir.trimmed().isEmpty()) {
                    clip.maskOriginalFramesDir = analysis.request.sourceDirectory;
                }
                TimelineClip::MaskFuzzyRemoveEdit edit;
                edit.recipeHash = result.recipeHash;
                edit.algorithm = QString::fromLatin1(
                    editor::masks::kFuzzyRemoveAlgorithmVersion);
                edit.sourceSidecarDirectory = analysis.request.sourceDirectory;
                edit.materializedSidecarDirectory = result.outputDirectory;
                edit.sourceFrame = analysis.request.sourceFrame;
                edit.sourcePresentationTimestamp =
                    analysis.request.sourcePresentationTimestamp;
                edit.seedMaskOrdinal = analysis.seedMaskOrdinal;
                edit.firstMaskOrdinal = analysis.firstMaskOrdinal;
                edit.lastMaskOrdinal = analysis.lastMaskOrdinal;
                edit.xNorm = analysis.request.xNorm;
                edit.yNorm = analysis.request.yNorm;
                edit.spatialReachPixels = analysis.request.spatialReachPixels;
                edit.temporalReachFrames = analysis.request.temporalReachFrames;
                edit.foregroundThreshold = analysis.request.foregroundThreshold;
                edit.maximumAreaGrowth = analysis.request.maximumAreaGrowth;
                edit.minimumAreaRatio = analysis.request.minimumAreaRatio;
                edit.maximumFrameFraction = analysis.request.maximumFrameFraction;
                edit.ambiguityRatio = analysis.request.ambiguityRatio;
                edit.changedFrames = result.changedFrames;
                edit.removedPixels = result.removedPixels;
                clip.maskFuzzyRemoveEdits.push_back(std::move(edit));
                clip.maskFramesDir = result.outputDirectory;
                clip.generatedFromMaskId =
                    editor::masks::stableMaskSidecarId(result.outputDirectory);
            });
        if (!updated || !sourceStillCurrent) {
            if (m_widgets.fuzzyStatusLabel) {
                m_widgets.fuzzyStatusLabel->setText(QStringLiteral(
                    "The derived cache was created, but the clip's source mask changed; "
                    "the edit was not attached."));
            }
            return;
        }
        if (m_deps.setPreviewTimelineClips) m_deps.setPreviewTimelineClips();
        if (m_deps.refreshInspector) m_deps.refreshInspector();
        if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
        if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
        if (m_widgets.fuzzyStatusLabel) {
            m_widgets.fuzzyStatusLabel->setText(
                QStringLiteral(
                    "Removed %1 pixels across %2 frame(s). Source preserved; edit is undoable.")
                    .arg(result.removedPixels)
                    .arg(result.changedFrames));
        }
    });
    QPointer<QProgressDialog> guardedProgress(progress);
    watcher->setFuture(QtConcurrent::run([analysis, cancel, guardedProgress]() {
        return editor::masks::materializeFuzzyRemoveMaskRegion(
            analysis,
            cancel,
            [guardedProgress](int completed, int total, const QString& phase) {
                if (!guardedProgress) return;
                QMetaObject::invokeMethod(
                    guardedProgress,
                    [guardedProgress, completed, total, phase]() {
                        if (!guardedProgress) return;
                        guardedProgress->setMaximum(qMax(1, total));
                        guardedProgress->setValue(qMin(completed, total));
                        guardedProgress->setLabelText(phase);
                    },
                    Qt::QueuedConnection);
            });
    }));
}

void MaskTab::refresh()
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    const bool validClip = clip &&
                           (clip->clipRole == ClipRole::Media ||
                            clip->clipRole == ClipRole::MaskMatte) &&
                           (!m_deps.clipHasVisuals || m_deps.clipHasVisuals(*clip)) &&
                           clip->mediaType == ClipMediaType::Video;
    const bool maskInspectorActive =
        m_deps.isMaskInspectorActive && m_deps.isMaskInspectorActive();
    if (!maskInspectorActive && m_widgets.fuzzyRemoveButton &&
        m_widgets.fuzzyRemoveButton->isChecked()) {
        m_widgets.fuzzyRemoveButton->setChecked(false);
    }

    // The Masks tab edits child-owned state. A source selection is only a
    // discovery context: resolve its chosen sidecar to a materialized child
    // before populating or enabling any treatment controls.
    if (validClip &&
        clip->clipRole == ClipRole::Media &&
        maskInspectorActive) {
        const QString sourceId = clip->id.trimmed();
        QString directory = clip->maskFramesDir.trimmed();
        QString sidecarId = directory.isEmpty()
            ? QString()
            : editor::masks::stableMaskSidecarId(directory);
        if (directory.isEmpty()) {
            const editor::masks::MaskSidecar discoveredSidecar = autoMaskSidecarForClip(*clip);
            if (discoveredSidecar.isValid()) {
                directory = discoveredSidecar.directory;
                sidecarId = discoveredSidecar.id;
            }
        }

        QString childId = m_deps.findMaskMatteChildForSidecar && !sidecarId.isEmpty()
            ? m_deps.findMaskMatteChildForSidecar(sourceId, sidecarId)
            : QString();
        if (childId.isEmpty() &&
            !directory.isEmpty() &&
            m_deps.materializeMaskMatteForSidecar) {
            childId = m_deps.materializeMaskMatteForSidecar(sourceId, directory);
        }
        if (!childId.isEmpty() && childId != sourceId && m_deps.selectClipById) {
            m_deps.selectClipById(childId);
            return;
        }
    }

    m_updating = true;

    if (m_widgets.clipLabel) {
        m_widgets.clipLabel->setText(validClip
            ? QStringLiteral("%1\n%2").arg(clip->label, QDir::toNativeSeparators(clip->filePath))
            : QStringLiteral("Select a video clip to edit its mask."));
        m_widgets.clipLabel->setToolTip(validClip ? QDir::toNativeSeparators(clip->filePath) : QString());
    }

    if (m_widgets.enabledCheck) QSignalBlocker blocker(m_widgets.enabledCheck);
    setControlsEnabled(validClip);
    setTreatmentControlsEnabled(validClip && clip->clipRole == ClipRole::MaskMatte);

    auto setSpin = [](QDoubleSpinBox* spin, double value) {
        if (!spin) return;
        spin->setValue(value);
    };
    auto setCheck = [](QCheckBox* check, bool value) {
        if (!check) return;
        QSignalBlocker blocker(check);
        check->setChecked(value);
    };

    const bool effectiveMaskEnabled = validClip ? clip->maskEnabled : false;
    const QString effectiveMaskFramesDir = validClip ? clip->maskFramesDir : QString();

    if (validClip) {
        const bool treatmentActive = clip->clipRole == ClipRole::MaskMatte;
        if (m_widgets.zLevelSpin) {
            QSignalBlocker blocker(m_widgets.zLevelSpin);
            m_widgets.zLevelSpin->setValue(effectiveClipZLevel(*clip));
        }
        setCheck(m_widgets.enabledCheck, effectiveMaskEnabled);
        if (m_widgets.framesDirEdit) {
            QSignalBlocker blocker(m_widgets.framesDirEdit);
            m_widgets.framesDirEdit->setText(effectiveMaskFramesDir);
        }
        if (m_widgets.sidecarCombo && maskInspectorActive) {
            QSignalBlocker blocker(m_widgets.sidecarCombo);
            m_widgets.sidecarCombo->clear();
            const QVector<editor::masks::MaskSidecar> sidecars =
                editor::masks::discoverMaskSidecars(*clip);
            for (const editor::masks::MaskSidecar& sidecar : sidecars) {
                const QString kind = sidecar.sourceType.contains(
                                         QStringLiteral("continuous_alpha"),
                                         Qt::CaseInsensitive)
                    ? QStringLiteral("soft alpha")
                    : QStringLiteral("binary");
                const QString coverage = sidecar.firstFrame >= 0
                    ? QStringLiteral("%1 · %2 frames · %3–%4")
                          .arg(kind).arg(sidecar.frameCount).arg(sidecar.firstFrame).arg(sidecar.lastFrame)
                    : QStringLiteral("%1 · %2 frames").arg(kind).arg(sidecar.frameCount);
                const QString readiness = sidecar.isReadyForTimeline()
                    ? QString()
                    : QStringLiteral(" — %1").arg(sidecar.readinessIssue);
                m_widgets.sidecarCombo->addItem(
                    QStringLiteral("%1 (%2)%3").arg(
                        sidecar.displayName, coverage, readiness),
                    sidecar.directory);
                m_widgets.sidecarCombo->setItemData(
                    m_widgets.sidecarCombo->count() - 1, sidecar.id, kSidecarIdRole);
                m_widgets.sidecarCombo->setItemData(
                    m_widgets.sidecarCombo->count() - 1,
                    sidecar.isReadyForTimeline(),
                    kSidecarReadyRole);
                if (!sidecar.isReadyForTimeline()) {
                    m_widgets.sidecarCombo->setItemData(
                        m_widgets.sidecarCombo->count() - 1,
                        sidecar.readinessIssue,
                        Qt::ToolTipRole);
                }
            }
            int selected = m_widgets.sidecarCombo->findData(effectiveMaskFramesDir);
            if (selected < 0 && !effectiveMaskFramesDir.trimmed().isEmpty()) {
                const QString issue = clip->maskSidecarAvailabilityIssue.trimmed();
                m_widgets.sidecarCombo->addItem(
                    issue.isEmpty()
                        ? QStringLiteral("Current sidecar (unavailable)")
                        : QStringLiteral("Current sidecar — %1").arg(issue),
                    effectiveMaskFramesDir);
                m_widgets.sidecarCombo->setItemData(
                    m_widgets.sidecarCombo->count() - 1,
                    maskSidecarIdForClip(*clip),
                    kSidecarIdRole);
                m_widgets.sidecarCombo->setItemData(
                    m_widgets.sidecarCombo->count() - 1,
                    false,
                    kSidecarReadyRole);
                m_widgets.sidecarCombo->setItemData(
                    m_widgets.sidecarCombo->count() - 1,
                    issue.isEmpty() ? QStringLiteral("Sidecar unavailable") : issue,
                    Qt::ToolTipRole);
                selected = m_widgets.sidecarCombo->count() - 1;
            } else if (sidecars.isEmpty()) {
                m_widgets.sidecarCombo->addItem(QStringLiteral("No sidecar masks found"), QString());
            }
            m_widgets.sidecarCombo->setCurrentIndex(selected >= 0 ? selected : 0);
        }
        setSpin(m_widgets.featherSpin, clip->maskFeather);
        setSpin(m_widgets.featherPowerSpin, clip->maskFeatherGamma);
        if (m_widgets.featherFalloffCombo) {
            QSignalBlocker blocker(m_widgets.featherFalloffCombo);
            const int index = m_widgets.featherFalloffCombo->findData(clip->maskFeatherFalloff);
            m_widgets.featherFalloffCombo->setCurrentIndex(index >= 0 ? index : 0);
        }
        if (m_widgets.featherPowerSpin) {
            m_widgets.featherPowerSpin->setEnabled(
                treatmentActive && clip->maskFeatherFalloff == 0);
        }
        const editor::masks::MaskSidecar activeSidecar =
            editor::masks::inspectMaskSidecar(
                effectiveMaskFramesDir,
                QFileInfo(clip->filePath).completeBaseName(),
                clip->filePath);
        const bool canRefine = treatmentActive &&
            supportsBiRefNetRefinement(activeSidecar);
        if (m_widgets.biRefNetRefineButton) {
            m_widgets.biRefNetRefineButton->setEnabled(canRefine);
        }
        if (m_widgets.biRefNetGuideRadiusSpin) {
            m_widgets.biRefNetGuideRadiusSpin->setEnabled(canRefine);
        }
        setSpin(m_widgets.dilateSpin, clip->maskDilate);
        setSpin(m_widgets.erodeSpin, clip->maskErode);
        setSpin(m_widgets.blurSpin, clip->maskBlur);
        setCheck(m_widgets.temporalStabilizeCheck,
                 clip->maskTemporalStabilizeEnabled);
        setSpin(m_widgets.temporalStabilizeStrengthSpin,
                clip->maskTemporalStabilizeStrength);
        if (m_widgets.temporalStabilizeMotionRadiusSpin) {
            QSignalBlocker blocker(m_widgets.temporalStabilizeMotionRadiusSpin);
            m_widgets.temporalStabilizeMotionRadiusSpin->setValue(
                clip->maskTemporalStabilizeMotionRadius);
        }
        const bool temporalControlsEnabled =
            treatmentActive && clip->maskTemporalStabilizeEnabled;
        if (m_widgets.temporalStabilizeStrengthSpin) {
            m_widgets.temporalStabilizeStrengthSpin->setEnabled(
                temporalControlsEnabled);
        }
        if (m_widgets.temporalStabilizeMotionRadiusSpin) {
            m_widgets.temporalStabilizeMotionRadiusSpin->setEnabled(
                temporalControlsEnabled);
        }
        setCheck(m_widgets.invertCheck, clip->maskInvert);
        const bool showOnlyAvailable =
            treatmentActive && !clip->maskForegroundLayerEnabled;
        setCheck(m_widgets.showOnlyCheck, showOnlyAvailable && clip->maskShowOnly);
        if (m_widgets.showOnlyCheck) {
            m_widgets.showOnlyCheck->setEnabled(showOnlyAvailable);
        }
        setSpin(m_widgets.opacitySpin, clip->maskOpacity);
        setCheck(m_widgets.foregroundLayerCheck, clip->maskForegroundLayerEnabled);
        setCheck(m_widgets.repeatEnabledCheck, clip->maskRepeatEnabled);
        setSpin(m_widgets.repeatDeltaXSpin, clip->maskRepeatDeltaX);
        setSpin(m_widgets.repeatDeltaYSpin, clip->maskRepeatDeltaY);
        setCheck(m_widgets.shadowEnabledCheck, clip->maskDropShadowEnabled);
        setSpin(m_widgets.shadowRadiusSpin, clip->maskDropShadowRadius);
        setSpin(m_widgets.shadowOffsetXSpin, clip->maskDropShadowOffsetX);
        setSpin(m_widgets.shadowOffsetYSpin, clip->maskDropShadowOffsetY);
        setSpin(m_widgets.shadowOpacitySpin, clip->maskDropShadowOpacity);
    } else {
        setCheck(m_widgets.enabledCheck, false);
        if (m_widgets.framesDirEdit) {
            QSignalBlocker blocker(m_widgets.framesDirEdit);
            m_widgets.framesDirEdit->clear();
        }
    }

    m_updating = false;
}

void MaskTab::apply(bool pushHistory, bool zLevelEdited)
{
    if (m_updating || !m_deps.updateClipById) {
        return;
    }
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip ||
        (selectedClip->clipRole != ClipRole::Media &&
         selectedClip->clipRole != ClipRole::MaskMatte) ||
        selectedClip->mediaType != ClipMediaType::Video) {
        return;
    }

    QString requestedDirectory =
        m_widgets.framesDirEdit ? m_widgets.framesDirEdit->text().trimmed() : QString();
    const QString sourceId = maskSourceIdForClip(*selectedClip);
    const editor::masks::MaskSidecar requestedSidecar =
        editor::masks::inspectMaskSidecar(
            requestedDirectory,
            QFileInfo(selectedClip->filePath).completeBaseName(),
            selectedClip->filePath);
    const bool requestedSidecarReady =
        requestedDirectory.isEmpty() || requestedSidecar.isReadyForTimeline();

    if (selectedClip->clipRole == ClipRole::Media) {
        // A source parent is only a discovery/materialization context. Never
        // copy child-owned sidecar or mask-treatment fields onto it.
        QString childId;
        const QString requestedSidecarId = requestedDirectory.isEmpty()
            ? QString()
            : editor::masks::stableMaskSidecarId(requestedDirectory);
        if (!requestedSidecarId.isEmpty() && m_deps.findMaskMatteChildForSidecar) {
            childId = m_deps.findMaskMatteChildForSidecar(sourceId, requestedSidecarId);
        }
        if (requestedSidecarReady && childId.isEmpty() &&
            !requestedDirectory.isEmpty() &&
            m_deps.materializeMaskMatteForSidecar) {
            childId = m_deps.materializeMaskMatteForSidecar(sourceId, requestedDirectory);
        }
        if (!childId.isEmpty() && m_deps.selectClipById) {
            m_deps.selectClipById(childId);
        }
        return;
    }

    const bool clearedChildAssociation =
        requestedDirectory.isEmpty();
    if (clearedChildAssociation) {
        // A materialized child has a durable sidecar identity. Clearing the
        // text field disables it through maskEnabled; it must not turn a UI
        // edit into an implicit association deletion that disk discovery will
        // immediately recreate.
        requestedDirectory = selectedClip->maskFramesDir.trimmed();
    }
    const QString requestedSidecarId = requestedDirectory.isEmpty()
        ? QString()
        : editor::masks::stableMaskSidecarId(requestedDirectory);
    const QString currentSidecarId = maskSidecarIdForClip(*selectedClip);
    if (!requestedSidecarId.isEmpty() &&
        !sourceId.isEmpty() &&
        requestedSidecarId != currentSidecarId) {
        QString childId = m_deps.findMaskMatteChildForSidecar
            ? m_deps.findMaskMatteChildForSidecar(sourceId, requestedSidecarId)
            : QString();
        if (childId.isEmpty() && m_deps.materializeMaskMatteForSidecar) {
            childId = m_deps.materializeMaskMatteForSidecar(sourceId, requestedDirectory);
        }
        if (!childId.isEmpty() && m_deps.selectClipById) {
            // Switching associations means switching children. It must not
            // retarget the selected child's effects, grading, or mask treatment.
            m_deps.selectClipById(childId);
        }
        return;
    }
    if (!requestedSidecarId.isEmpty() &&
        !sourceId.isEmpty() &&
        m_deps.findMaskMatteChildForSidecar) {
        const QString existingChildId =
            m_deps.findMaskMatteChildForSidecar(sourceId, requestedSidecarId);
        if (!existingChildId.isEmpty() && existingChildId != selectedClip->id) {
            // Every discovered sidecar has its own materialized child. Switching
            // the inspector therefore switches selection instead of retargeting
            // another child's visual state to the same association.
            if (m_deps.selectClipById) {
                m_deps.selectClipById(existingChildId);
            }
            return;
        }
    }

    const QString id = selectedClip->id;
    const bool updated = m_deps.updateClipById(
        id,
        [this, requestedDirectory, clearedChildAssociation, zLevelEdited](TimelineClip& clip) {
        if (zLevelEdited && m_widgets.zLevelSpin) {
            clip.zLevel = m_widgets.zLevelSpin->value();
            clip.zLevelUserSet = true;
        }
        clip.maskEnabled = !clearedChildAssociation &&
                           m_widgets.enabledCheck &&
                           m_widgets.enabledCheck->isChecked();
        setMaskSidecarAssociation(clip, requestedDirectory);
        clip.maskFeather = m_widgets.featherSpin ? m_widgets.featherSpin->value() : 0.0;
        clip.maskFeatherGamma = m_widgets.featherPowerSpin ? m_widgets.featherPowerSpin->value() : 2.0;
        clip.maskFeatherFalloff = m_widgets.featherFalloffCombo
            ? qBound(0, m_widgets.featherFalloffCombo->currentData().toInt(), 5) : 0;
        clip.maskDilate = m_widgets.dilateSpin ? m_widgets.dilateSpin->value() : 0.0;
        clip.maskErode = m_widgets.erodeSpin ? m_widgets.erodeSpin->value() : 0.0;
        clip.maskBlur = m_widgets.blurSpin ? m_widgets.blurSpin->value() : 0.0;
        clip.maskTemporalStabilizeEnabled =
            m_widgets.temporalStabilizeCheck &&
            m_widgets.temporalStabilizeCheck->isChecked();
        clip.maskTemporalStabilizeStrength =
            m_widgets.temporalStabilizeStrengthSpin
                ? m_widgets.temporalStabilizeStrengthSpin->value() : 0.75;
        clip.maskTemporalStabilizeMotionRadius =
            m_widgets.temporalStabilizeMotionRadiusSpin
                ? m_widgets.temporalStabilizeMotionRadiusSpin->value() : 4;
        clip.maskInvert = m_widgets.invertCheck && m_widgets.invertCheck->isChecked();
        clip.maskShowOnly =
            !clip.maskForegroundLayerEnabled &&
            m_widgets.showOnlyCheck &&
            m_widgets.showOnlyCheck->isChecked();
        clip.maskOpacity = m_widgets.opacitySpin ? m_widgets.opacitySpin->value() : 1.0;
        clip.maskForegroundLayerEnabled =
            m_widgets.foregroundLayerCheck &&
            m_widgets.foregroundLayerCheck->isChecked();
        clip.maskRepeatEnabled =
            m_widgets.repeatEnabledCheck &&
            m_widgets.repeatEnabledCheck->isChecked();
        clip.maskRepeatDeltaX =
            m_widgets.repeatDeltaXSpin ? m_widgets.repeatDeltaXSpin->value() : 160.0;
        clip.maskRepeatDeltaY =
            m_widgets.repeatDeltaYSpin ? m_widgets.repeatDeltaYSpin->value() : 0.0;
        if (clip.maskForegroundLayerEnabled) {
            clip.maskShowOnly = false;
        }
        clip.maskDropShadowEnabled = m_widgets.shadowEnabledCheck && m_widgets.shadowEnabledCheck->isChecked();
        clip.maskDropShadowRadius = m_widgets.shadowRadiusSpin ? m_widgets.shadowRadiusSpin->value() : 12.0;
        clip.maskDropShadowOffsetX = m_widgets.shadowOffsetXSpin ? m_widgets.shadowOffsetXSpin->value() : 0.0;
        clip.maskDropShadowOffsetY = m_widgets.shadowOffsetYSpin ? m_widgets.shadowOffsetYSpin->value() : 4.0;
        clip.maskDropShadowOpacity = m_widgets.shadowOpacitySpin ? m_widgets.shadowOpacitySpin->value() : 0.45;
        });
    if (!updated) {
        return;
    }
    applyTabEditEffects(maskEditCallbacks(m_deps), TabEditEffects{.pushHistory = pushHistory});
}

void MaskTab::scheduleTreatmentEdit(bool zLevelEdited)
{
    if (m_updating) {
        return;
    }
    m_pendingTreatmentZLevelEdited = m_pendingTreatmentZLevelEdited || zLevelEdited;
    if (!m_treatmentEditTimer) {
        applyTreatmentEdit(false, m_pendingTreatmentZLevelEdited);
        m_pendingTreatmentZLevelEdited = false;
        return;
    }
    m_treatmentEditTimer->start();
}

void MaskTab::applyTreatmentEdit(bool commit, bool zLevelEdited)
{
    if (m_updating || !m_deps.updateClipById) {
        return;
    }
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip ||
        selectedClip->clipRole != ClipRole::MaskMatte ||
        selectedClip->mediaType != ClipMediaType::Video) {
        return;
    }

    const QString id = selectedClip->id;
    const bool updated = m_deps.updateClipById(
        id,
        [this, zLevelEdited](TimelineClip& clip) {
            if (zLevelEdited && m_widgets.zLevelSpin) {
                clip.zLevel = m_widgets.zLevelSpin->value();
                clip.zLevelUserSet = true;
            }
            clip.maskFeather = m_widgets.featherSpin ? m_widgets.featherSpin->value() : 0.0;
            clip.maskFeatherGamma = m_widgets.featherPowerSpin ? m_widgets.featherPowerSpin->value() : 2.0;
            clip.maskFeatherFalloff = m_widgets.featherFalloffCombo
                ? qBound(0, m_widgets.featherFalloffCombo->currentData().toInt(), 5) : 0;
            clip.maskDilate = m_widgets.dilateSpin ? m_widgets.dilateSpin->value() : 0.0;
            clip.maskErode = m_widgets.erodeSpin ? m_widgets.erodeSpin->value() : 0.0;
            clip.maskBlur = m_widgets.blurSpin ? m_widgets.blurSpin->value() : 0.0;
            clip.maskTemporalStabilizeEnabled =
                m_widgets.temporalStabilizeCheck &&
                m_widgets.temporalStabilizeCheck->isChecked();
            clip.maskTemporalStabilizeStrength =
                m_widgets.temporalStabilizeStrengthSpin
                    ? m_widgets.temporalStabilizeStrengthSpin->value() : 0.75;
            clip.maskTemporalStabilizeMotionRadius =
                m_widgets.temporalStabilizeMotionRadiusSpin
                    ? m_widgets.temporalStabilizeMotionRadiusSpin->value() : 4;
            clip.maskOpacity = m_widgets.opacitySpin ? m_widgets.opacitySpin->value() : 1.0;
            clip.maskRepeatDeltaX =
                m_widgets.repeatDeltaXSpin ? m_widgets.repeatDeltaXSpin->value() : 160.0;
            clip.maskRepeatDeltaY =
                m_widgets.repeatDeltaYSpin ? m_widgets.repeatDeltaYSpin->value() : 0.0;
            clip.maskDropShadowRadius =
                m_widgets.shadowRadiusSpin ? m_widgets.shadowRadiusSpin->value() : 12.0;
            clip.maskDropShadowOffsetX =
                m_widgets.shadowOffsetXSpin ? m_widgets.shadowOffsetXSpin->value() : 0.0;
            clip.maskDropShadowOffsetY =
                m_widgets.shadowOffsetYSpin ? m_widgets.shadowOffsetYSpin->value() : 4.0;
            clip.maskDropShadowOpacity =
                m_widgets.shadowOpacitySpin ? m_widgets.shadowOpacitySpin->value() : 0.45;
        });
    if (!updated) {
        return;
    }

    applyTabEditEffects(maskEditCallbacks(m_deps),
                        TabEditEffects{.refreshInspector = commit,
                                       .scheduleSave = commit,
                                       .pushHistory = commit});
}

void MaskTab::setControlsEnabled(bool enabled)
{
    for (QWidget* widget : {static_cast<QWidget*>(m_widgets.enabledCheck),
                            static_cast<QWidget*>(m_widgets.framesDirEdit),
                            static_cast<QWidget*>(m_widgets.sidecarCombo),
                            static_cast<QWidget*>(m_widgets.browseButton),
                            static_cast<QWidget*>(m_widgets.newPromptButton),
                            static_cast<QWidget*>(m_widgets.biRefNetRefineButton),
                            static_cast<QWidget*>(m_widgets.biRefNetGuideRadiusSpin),
                            static_cast<QWidget*>(m_widgets.fuzzyRemoveButton),
                            static_cast<QWidget*>(m_widgets.fuzzySpatialReachSpin),
                            static_cast<QWidget*>(m_widgets.fuzzyTemporalReachSpin),
                            static_cast<QWidget*>(m_widgets.zLevelSpin),
                            static_cast<QWidget*>(m_widgets.featherSpin),
                            static_cast<QWidget*>(m_widgets.featherFalloffCombo),
                            static_cast<QWidget*>(m_widgets.featherPowerSpin),
                            static_cast<QWidget*>(m_widgets.dilateSpin),
                            static_cast<QWidget*>(m_widgets.erodeSpin),
                            static_cast<QWidget*>(m_widgets.blurSpin),
                            static_cast<QWidget*>(m_widgets.temporalStabilizeCheck),
                            static_cast<QWidget*>(m_widgets.temporalStabilizeStrengthSpin),
                            static_cast<QWidget*>(m_widgets.temporalStabilizeMotionRadiusSpin),
                            static_cast<QWidget*>(m_widgets.invertCheck),
                            static_cast<QWidget*>(m_widgets.showOnlyCheck),
                            static_cast<QWidget*>(m_widgets.opacitySpin),
                            static_cast<QWidget*>(m_widgets.foregroundLayerCheck),
                            static_cast<QWidget*>(m_widgets.repeatEnabledCheck),
                            static_cast<QWidget*>(m_widgets.repeatDeltaXSpin),
                            static_cast<QWidget*>(m_widgets.repeatDeltaYSpin),
                            static_cast<QWidget*>(m_widgets.shadowEnabledCheck),
                            static_cast<QWidget*>(m_widgets.shadowRadiusSpin),
                            static_cast<QWidget*>(m_widgets.shadowOffsetXSpin),
                            static_cast<QWidget*>(m_widgets.shadowOffsetYSpin),
                            static_cast<QWidget*>(m_widgets.shadowOpacitySpin)}) {
        if (widget) {
            widget->setEnabled(enabled);
        }
    }
    if (enabled && m_widgets.showOnlyCheck) {
        m_widgets.showOnlyCheck->setEnabled(true);
    }
}

void MaskTab::setTreatmentControlsEnabled(bool enabled)
{
    for (QWidget* widget : {static_cast<QWidget*>(m_widgets.enabledCheck),
                            static_cast<QWidget*>(m_widgets.biRefNetRefineButton),
                            static_cast<QWidget*>(m_widgets.biRefNetGuideRadiusSpin),
                            static_cast<QWidget*>(m_widgets.fuzzyRemoveButton),
                            static_cast<QWidget*>(m_widgets.fuzzySpatialReachSpin),
                            static_cast<QWidget*>(m_widgets.fuzzyTemporalReachSpin),
                            static_cast<QWidget*>(m_widgets.zLevelSpin),
                            static_cast<QWidget*>(m_widgets.featherSpin),
                            static_cast<QWidget*>(m_widgets.featherFalloffCombo),
                            static_cast<QWidget*>(m_widgets.featherPowerSpin),
                            static_cast<QWidget*>(m_widgets.dilateSpin),
                            static_cast<QWidget*>(m_widgets.erodeSpin),
                            static_cast<QWidget*>(m_widgets.blurSpin),
                            static_cast<QWidget*>(m_widgets.temporalStabilizeCheck),
                            static_cast<QWidget*>(m_widgets.temporalStabilizeStrengthSpin),
                            static_cast<QWidget*>(m_widgets.temporalStabilizeMotionRadiusSpin),
                            static_cast<QWidget*>(m_widgets.invertCheck),
                            static_cast<QWidget*>(m_widgets.showOnlyCheck),
                            static_cast<QWidget*>(m_widgets.opacitySpin),
                            static_cast<QWidget*>(m_widgets.foregroundLayerCheck),
                            static_cast<QWidget*>(m_widgets.repeatEnabledCheck),
                            static_cast<QWidget*>(m_widgets.repeatDeltaXSpin),
                            static_cast<QWidget*>(m_widgets.repeatDeltaYSpin),
                            static_cast<QWidget*>(m_widgets.shadowEnabledCheck),
                            static_cast<QWidget*>(m_widgets.shadowRadiusSpin),
                            static_cast<QWidget*>(m_widgets.shadowOffsetXSpin),
                            static_cast<QWidget*>(m_widgets.shadowOffsetYSpin),
                            static_cast<QWidget*>(m_widgets.shadowOpacitySpin)}) {
        if (widget) {
            widget->setEnabled(enabled);
        }
    }
    if (enabled && m_widgets.featherPowerSpin && m_widgets.featherFalloffCombo) {
        m_widgets.featherPowerSpin->setEnabled(
            m_widgets.featherFalloffCombo->currentData().toInt() == 0);
    }
    if (enabled && m_widgets.showOnlyCheck) {
        const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
        m_widgets.showOnlyCheck->setEnabled(clip && !clip->maskForegroundLayerEnabled);
    }
    if (enabled) {
        const bool repeatEnabled =
            m_widgets.repeatEnabledCheck && m_widgets.repeatEnabledCheck->isChecked();
        if (m_widgets.repeatDeltaXSpin) {
            m_widgets.repeatDeltaXSpin->setEnabled(repeatEnabled);
        }
        if (m_widgets.repeatDeltaYSpin) {
            m_widgets.repeatDeltaYSpin->setEnabled(repeatEnabled);
        }
    }
}
