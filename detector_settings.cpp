#include "detector_settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>

namespace jcut::facestream {
namespace {

DetectorRuntimeSettings immutableDefaultDetectorSettings()
{
    return DetectorRuntimeSettings{};
}

DetectorRuntimeSettings fastPreviewDetectorSettings()
{
    DetectorRuntimeSettings settings = immutableDefaultDetectorSettings();
    settings.stride = 8;
    settings.maxDetections = 256;
    settings.scrfdTargetSize = 640;
    settings.maxFacesPerFrame = 12;
    settings.threshold = 0.32f;
    return settings;
}

DetectorRuntimeSettings panelRecallDetectorSettings()
{
    DetectorRuntimeSettings settings = immutableDefaultDetectorSettings();
    settings.maxDetections = 768;
    settings.scrfdTargetSize = 1280;
    settings.maxFacesPerFrame = 24;
    settings.threshold = 0.24f;
    settings.scrfdTiled = true;
    settings.maxFaceAreaRatio = 0.22f;
    return settings;
}

const std::array<DetectorSettingsProfileDefinition, 3>& detectorProfiles()
{
    static const std::array<DetectorSettingsProfileDefinition, 3> profiles{{
        DetectorSettingsProfileDefinition{
            QStringLiteral("immutable-default"),
            QStringLiteral("Default (Immutable)"),
            QStringLiteral("Canonical balanced baseline for most panel and event videos."),
            immutableDefaultDetectorSettings()
        },
        DetectorSettingsProfileDefinition{
            QStringLiteral("fast-preview"),
            QStringLiteral("Fast Preview"),
            QStringLiteral("Reduced GPU cost and faster iteration with lower small-face recall."),
            fastPreviewDetectorSettings()
        },
        DetectorSettingsProfileDefinition{
            QStringLiteral("panel-recall"),
            QStringLiteral("Panel Recall"),
            QStringLiteral("Higher recall for crowded panels and smaller faces at a higher GPU cost."),
            panelRecallDetectorSettings()
        }
    }};
    return profiles;
}

QString percentText(float value)
{
    return QStringLiteral("%1").arg(static_cast<double>(value), 0, 'f', 2);
}

QString areaRatioText(float value)
{
    return QStringLiteral("%1%").arg(static_cast<double>(value * 100.0f), 0, 'f', 3);
}

QString aspectText(float value)
{
    return QStringLiteral("%1").arg(static_cast<double>(value), 0, 'f', 2);
}

QString detectorProfileSummary(const DetectorRuntimeSettings& settings)
{
    return QStringLiteral("stride %1 | target %2 | threshold %3 | max faces %4")
        .arg(settings.stride)
        .arg(settings.scrfdTargetSize)
        .arg(static_cast<double>(settings.threshold), 0, 'f', 2)
        .arg(settings.maxFacesPerFrame);
}

} // namespace

QString detectorSettingsPathForVideo(const QString& videoPath)
{
    const QFileInfo info(videoPath);
    const QString baseName = info.completeBaseName().isEmpty() ? info.fileName() : info.completeBaseName();
    return info.dir().absoluteFilePath(baseName + QStringLiteral("_detectorsettings.json"));
}

QVector<DetectorSettingsProfileDefinition> builtInDetectorProfiles()
{
    QVector<DetectorSettingsProfileDefinition> profiles;
    profiles.reserve(static_cast<int>(detectorProfiles().size()));
    for (const DetectorSettingsProfileDefinition& profile : detectorProfiles()) {
        profiles.push_back(profile);
    }
    return profiles;
}

bool builtInDetectorProfileById(const QString& id, DetectorRuntimeSettings* settingsOut)
{
    if (!settingsOut) {
        return false;
    }
    for (const DetectorSettingsProfileDefinition& profile : detectorProfiles()) {
        if (profile.id == id) {
            *settingsOut = profile.settings;
            return true;
        }
    }
    return false;
}

QJsonObject detectorRuntimeSettingsToJson(const DetectorRuntimeSettings& s,
                                          const QString& detector,
                                          int scrfdTargetSize)
{
    return {
        {QStringLiteral("detector"), detector},
        {QStringLiteral("scrfd_target_size"), s.scrfdTargetSize > 0 ? s.scrfdTargetSize : scrfdTargetSize},
        {QStringLiteral("stride"), s.stride},
        {QStringLiteral("max_detections"), s.maxDetections},
        {QStringLiteral("max_faces_per_frame"), s.maxFacesPerFrame},
        {QStringLiteral("threshold"), s.threshold},
        {QStringLiteral("nms_iou_threshold"), s.nmsIouThreshold},
        {QStringLiteral("track_match_iou_threshold"), s.trackMatchIouThreshold},
        {QStringLiteral("new_track_min_confidence"), s.newTrackMinConfidence},
        {QStringLiteral("primary_face_only"), s.primaryFaceOnly},
        {QStringLiteral("small_face_fallback"), s.smallFaceFallback},
        {QStringLiteral("scrfd_tiled"), s.scrfdTiled},
        {QStringLiteral("roi_x1"), s.roiX1},
        {QStringLiteral("roi_y1"), s.roiY1},
        {QStringLiteral("roi_x2"), s.roiX2},
        {QStringLiteral("roi_y2"), s.roiY2},
        {QStringLiteral("min_face_area_ratio"), s.minFaceAreaRatio},
        {QStringLiteral("max_face_area_ratio"), s.maxFaceAreaRatio},
        {QStringLiteral("min_aspect"), s.minAspect},
        {QStringLiteral("max_aspect"), s.maxAspect}
    };
}

bool applyDetectorRuntimeSettingsObject(const QJsonObject& o, DetectorRuntimeSettings* s)
{
    if (!s) return false;
    if (o.contains(QStringLiteral("stride"))) s->stride = std::max(1, o.value(QStringLiteral("stride")).toInt(s->stride));
    if (o.contains(QStringLiteral("max_detections"))) s->maxDetections = std::max(1, o.value(QStringLiteral("max_detections")).toInt(s->maxDetections));
    if (o.contains(QStringLiteral("scrfd_target_size"))) s->scrfdTargetSize = std::max(320, o.value(QStringLiteral("scrfd_target_size")).toInt(s->scrfdTargetSize));
    if (o.contains(QStringLiteral("max_faces_per_frame"))) s->maxFacesPerFrame = std::max(0, o.value(QStringLiteral("max_faces_per_frame")).toInt(s->maxFacesPerFrame));
    if (o.contains(QStringLiteral("threshold"))) s->threshold = std::clamp(static_cast<float>(o.value(QStringLiteral("threshold")).toDouble(s->threshold)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("nms_iou_threshold"))) s->nmsIouThreshold = std::clamp(static_cast<float>(o.value(QStringLiteral("nms_iou_threshold")).toDouble(s->nmsIouThreshold)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("track_match_iou_threshold"))) s->trackMatchIouThreshold = std::clamp(static_cast<float>(o.value(QStringLiteral("track_match_iou_threshold")).toDouble(s->trackMatchIouThreshold)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("new_track_min_confidence"))) s->newTrackMinConfidence = std::clamp(static_cast<float>(o.value(QStringLiteral("new_track_min_confidence")).toDouble(s->newTrackMinConfidence)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("primary_face_only"))) s->primaryFaceOnly = o.value(QStringLiteral("primary_face_only")).toBool(s->primaryFaceOnly);
    if (o.contains(QStringLiteral("small_face_fallback"))) s->smallFaceFallback = o.value(QStringLiteral("small_face_fallback")).toBool(s->smallFaceFallback);
    if (o.contains(QStringLiteral("scrfd_tiled"))) s->scrfdTiled = o.value(QStringLiteral("scrfd_tiled")).toBool(s->scrfdTiled);
    if (o.contains(QStringLiteral("roi_x1"))) s->roiX1 = std::clamp(static_cast<float>(o.value(QStringLiteral("roi_x1")).toDouble(s->roiX1)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("roi_y1"))) s->roiY1 = std::clamp(static_cast<float>(o.value(QStringLiteral("roi_y1")).toDouble(s->roiY1)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("roi_x2"))) s->roiX2 = std::clamp(static_cast<float>(o.value(QStringLiteral("roi_x2")).toDouble(s->roiX2)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("roi_y2"))) s->roiY2 = std::clamp(static_cast<float>(o.value(QStringLiteral("roi_y2")).toDouble(s->roiY2)), 0.0f, 1.0f);
    if (s->roiX2 < s->roiX1) std::swap(s->roiX1, s->roiX2);
    if (s->roiY2 < s->roiY1) std::swap(s->roiY1, s->roiY2);
    if (o.contains(QStringLiteral("min_face_area_ratio"))) s->minFaceAreaRatio = std::clamp(static_cast<float>(o.value(QStringLiteral("min_face_area_ratio")).toDouble(s->minFaceAreaRatio)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("max_face_area_ratio"))) s->maxFaceAreaRatio = std::clamp(static_cast<float>(o.value(QStringLiteral("max_face_area_ratio")).toDouble(s->maxFaceAreaRatio)), 0.0f, 1.0f);
    if (s->maxFaceAreaRatio < s->minFaceAreaRatio) std::swap(s->minFaceAreaRatio, s->maxFaceAreaRatio);
    if (o.contains(QStringLiteral("min_aspect"))) s->minAspect = std::clamp(static_cast<float>(o.value(QStringLiteral("min_aspect")).toDouble(s->minAspect)), 0.0f, 100.0f);
    if (o.contains(QStringLiteral("max_aspect"))) s->maxAspect = std::clamp(static_cast<float>(o.value(QStringLiteral("max_aspect")).toDouble(s->maxAspect)), 0.0f, 100.0f);
    if (s->maxAspect < s->minAspect) std::swap(s->minAspect, s->maxAspect);
    return true;
}

bool loadDetectorRuntimeSettingsFile(const QString& path,
                                     DetectorRuntimeSettings* settings,
                                     QDateTime* lastAppliedMtime)
{
    if (!settings || path.isEmpty()) return false;
    const QFileInfo info(path);
    if (!info.exists()) return false;
    const QDateTime mtime = info.lastModified();
    if (lastAppliedMtime && lastAppliedMtime->isValid() && mtime <= *lastAppliedMtime) return false;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) return false;
    if (!applyDetectorRuntimeSettingsObject(doc.object(), settings)) return false;
    if (lastAppliedMtime) *lastAppliedMtime = mtime;
    return true;
}

bool saveDetectorRuntimeSettingsFile(const QString& path,
                                     const DetectorRuntimeSettings& settings,
                                     const QString& detector,
                                     int scrfdTargetSize,
                                     QString* errorMessage)
{
    if (path.isEmpty()) return false;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to write detector settings: %1").arg(path);
        return false;
    }
    f.write(QJsonDocument(detectorRuntimeSettingsToJson(
        settings,
        detector,
        settings.scrfdTargetSize > 0 ? settings.scrfdTargetSize : scrfdTargetSize)).toJson(QJsonDocument::Indented));
    f.write("\n");
    return true;
}

void syncDetectorSettingsPanel(DetectorSettingsPanel* panel, const DetectorRuntimeSettings& s)
{
    if (!panel || !panel->widget) return;
    if (panel->threshold) {
        panel->threshold->setValue(qRound(s.threshold * 100.0f));
    }
    if (panel->thresholdValue) {
        panel->thresholdValue->setText(percentText(s.threshold));
    }
    if (panel->nms) {
        panel->nms->setValue(qRound(s.nmsIouThreshold * 100.0f));
    }
    if (panel->nmsValue) {
        panel->nmsValue->setText(percentText(s.nmsIouThreshold));
    }
    if (panel->trackMatch) {
        panel->trackMatch->setValue(qRound(s.trackMatchIouThreshold * 100.0f));
    }
    if (panel->trackMatchValue) {
        panel->trackMatchValue->setText(percentText(s.trackMatchIouThreshold));
    }
    if (panel->newTrack) {
        panel->newTrack->setValue(qRound(s.newTrackMinConfidence * 100.0f));
    }
    if (panel->newTrackValue) {
        panel->newTrackValue->setText(percentText(s.newTrackMinConfidence));
    }
    if (panel->roiX1) {
        panel->roiX1->setValue(qRound(s.roiX1 * 100.0f));
    }
    if (panel->roiX1Value) {
        panel->roiX1Value->setText(percentText(s.roiX1));
    }
    if (panel->roiY1) {
        panel->roiY1->setValue(qRound(s.roiY1 * 100.0f));
    }
    if (panel->roiY1Value) {
        panel->roiY1Value->setText(percentText(s.roiY1));
    }
    if (panel->roiX2) {
        panel->roiX2->setValue(qRound(s.roiX2 * 100.0f));
    }
    if (panel->roiX2Value) {
        panel->roiX2Value->setText(percentText(s.roiX2));
    }
    if (panel->roiY2) {
        panel->roiY2->setValue(qRound(s.roiY2 * 100.0f));
    }
    if (panel->roiY2Value) {
        panel->roiY2Value->setText(percentText(s.roiY2));
    }
    if (panel->minArea) {
        panel->minArea->setValue(qRound(s.minFaceAreaRatio * 100000.0f));
    }
    if (panel->minAreaValue) {
        panel->minAreaValue->setText(areaRatioText(s.minFaceAreaRatio));
    }
    if (panel->maxArea) {
        panel->maxArea->setValue(qRound(s.maxFaceAreaRatio * 100000.0f));
    }
    if (panel->maxAreaValue) {
        panel->maxAreaValue->setText(areaRatioText(s.maxFaceAreaRatio));
    }
    if (panel->minAspect) {
        panel->minAspect->setValue(qRound(s.minAspect * 100.0f));
    }
    if (panel->minAspectValue) {
        panel->minAspectValue->setText(aspectText(s.minAspect));
    }
    if (panel->maxAspect) {
        panel->maxAspect->setValue(qRound(s.maxAspect * 100.0f));
    }
    if (panel->maxAspectValue) {
        panel->maxAspectValue->setText(aspectText(s.maxAspect));
    }
    if (panel->maxFaces) {
        panel->maxFaces->setValue(s.maxFacesPerFrame);
    }
    if (panel->maxFacesValue) {
        panel->maxFacesValue->setText(s.maxFacesPerFrame == 0 ? QStringLiteral("unlimited") : QString::number(s.maxFacesPerFrame));
    }
    if (panel->primaryFaceOnly) {
        panel->primaryFaceOnly->setChecked(s.primaryFaceOnly);
    }
    if (panel->smallFaceFallback) {
        panel->smallFaceFallback->setChecked(s.smallFaceFallback);
    }
    if (panel->scrfdTiled) {
        panel->scrfdTiled->setChecked(s.scrfdTiled);
    }
    if (panel->stride) {
        panel->stride->setValue(s.stride);
    }
    if (panel->strideValue) {
        panel->strideValue->setText(QString::number(s.stride));
    }
    if (panel->maxDetections) {
        panel->maxDetections->setValue(s.maxDetections);
    }
    if (panel->maxDetectionsValue) {
        panel->maxDetectionsValue->setText(QString::number(s.maxDetections));
    }
    if (panel->scrfdTargetSize) {
        panel->scrfdTargetSize->setValue(s.scrfdTargetSize);
    }
    if (panel->scrfdTargetSizeValue) {
        panel->scrfdTargetSizeValue->setText(QString::number(s.scrfdTargetSize));
    }
}

DetectorSettingsPanel createDetectorSettingsPanel(DetectorRuntimeSettings* settings,
                                                  const QString& detector,
                                                  int scrfdTargetSize,
                                                  const QString& settingsPath,
                                                  bool showTrackingControls,
                                                  QWidget* parent)
{
    DetectorSettingsPanel panel;
    panel.widget = new QWidget(parent);
    panel.widget->setWindowTitle(QStringLiteral("JCut Detector Settings"));
    auto* root = new QVBoxLayout(panel.widget);
    root->setContentsMargins(8, 8, 8, 8);
    auto* form = new QFormLayout;
    root->addLayout(form);
    const auto syncing = std::make_shared<bool>(false);
    auto persist = [settings, detector, scrfdTargetSize, settingsPath]() {
        QString error;
        if (!saveDetectorRuntimeSettingsFile(settingsPath,
                                             *settings,
                                             detector,
                                             settings && settings->scrfdTargetSize > 0 ? settings->scrfdTargetSize : scrfdTargetSize,
                                             &error) && !error.isEmpty()) {
            qWarning().noquote() << error;
        }
    };
    auto applyProfile = [syncing, settings, persist, &panel](const DetectorRuntimeSettings& profileSettings) {
        if (!settings) {
            return;
        }
        *syncing = true;
        *settings = profileSettings;
        syncDetectorSettingsPanel(&panel, *settings);
        *syncing = false;
        persist();
    };
    auto addSlider = [&](const QString& name, const QString& tooltip, int minValue, int maxValue, int initialValue,
                         QSlider** sliderOut, QLabel** labelOut,
                         const std::function<QString(int)>& labelText,
                         const std::function<void(int)>& applyValue) {
        auto* row = new QWidget(panel.widget);
        row->setToolTip(tooltip);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* slider = new QSlider(Qt::Horizontal, row);
        slider->setRange(minValue, maxValue);
        slider->setValue(initialValue);
        slider->setToolTip(tooltip);
        auto* valueLabel = new QLabel(labelText(initialValue), row);
        valueLabel->setMinimumWidth(72);
        valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        valueLabel->setToolTip(tooltip);
        layout->addWidget(slider, 1);
        layout->addWidget(valueLabel);
        form->addRow(name, row);
        QObject::connect(slider, &QSlider::valueChanged, panel.widget, [syncing, settings, valueLabel, labelText, applyValue, persist](int value) {
            valueLabel->setText(labelText(value));
            if (*syncing || !settings) return;
            applyValue(value);
            persist();
        });
        *sliderOut = slider;
        *labelOut = valueLabel;
    };

    auto* profileRow = new QWidget(panel.widget);
    auto* profileLayout = new QHBoxLayout(profileRow);
    profileLayout->setContentsMargins(0, 0, 0, 0);
    panel.profileCombo = new QComboBox(profileRow);
    auto* applyProfileButton = new QPushButton(QStringLiteral("Apply"), profileRow);
    profileLayout->addWidget(panel.profileCombo, 1);
    profileLayout->addWidget(applyProfileButton);
    form->addRow(QStringLiteral("Profile"), profileRow);
    panel.profileCombo->setToolTip(QStringLiteral(
        "Built-in immutable detector profiles. Applying one replaces the current saved per-video settings."));
    applyProfileButton->setToolTip(panel.profileCombo->toolTip());
    for (const DetectorSettingsProfileDefinition& profile : detectorProfiles()) {
        panel.profileCombo->addItem(profile.label, profile.id);
        const int index = panel.profileCombo->count() - 1;
        panel.profileCombo->setItemData(
            index,
            QStringLiteral("%1\n%2")
                .arg(profile.description, detectorProfileSummary(profile.settings)),
            Qt::ToolTipRole);
    }
    QObject::connect(applyProfileButton, &QPushButton::clicked, panel.widget, [applyProfile, &panel]() {
        if (!panel.profileCombo) {
            return;
        }
        const QString selectedId = panel.profileCombo->currentData().toString();
        for (const DetectorSettingsProfileDefinition& profile : detectorProfiles()) {
            if (selectedId == profile.id) {
                applyProfile(profile.settings);
                return;
            }
        }
    });

    addSlider(QStringLiteral("Stride"),
              QStringLiteral("Run detection every Nth source frame. Lower values improve recall and preview responsiveness; higher values are faster but can skip faces between samples."),
              1, 120, settings->stride, &panel.stride, &panel.strideValue,
              [](int v) { return QString::number(v); }, [settings](int v) { settings->stride = std::max(1, v); });
    addSlider(QStringLiteral("Max detections"),
              QStringLiteral("Maximum number of raw detector boxes kept before post-filtering. Raise this for crowded scenes or panels; lower it to reduce duplicate/noisy candidates."),
              1, 1024, settings->maxDetections, &panel.maxDetections, &panel.maxDetectionsValue,
              [](int v) { return QString::number(v); }, [settings](int v) { settings->maxDetections = std::max(1, v); });
    const bool scrfdFamily = detector.contains(QStringLiteral("scrfd"), Qt::CaseInsensitive) ||
                             detector.compare(QStringLiteral("jcut-dnn"), Qt::CaseInsensitive) == 0;
    if (scrfdFamily) {
        addSlider(QStringLiteral("SCRFD target"),
                  QStringLiteral("Input size for the SCRFD model. Raise this to improve small-face recall at higher GPU cost; lower it for speed."),
                  320, 1280, settings->scrfdTargetSize, &panel.scrfdTargetSize, &panel.scrfdTargetSizeValue,
                  [](int v) { return QString::number(v); }, [settings](int v) { settings->scrfdTargetSize = std::max(320, v); });
    }
    addSlider(QStringLiteral("Confidence threshold"),
              QStringLiteral("Minimum detector confidence accepted as a face. Lower this to catch smaller or blurrier faces; raise it to reject false positives such as text, hands, or background shapes."),
              1, 99, qRound(settings->threshold * 100.0f), &panel.threshold, &panel.thresholdValue,
              [](int v) { return percentText(v / 100.0f); }, [settings](int v) { settings->threshold = v / 100.0f; });
    addSlider(QStringLiteral("NMS IoU"),
              QStringLiteral("Overlap threshold for merging duplicate face boxes. Lower values suppress nearby duplicate boxes more aggressively; higher values keep more overlapping detections."),
              0, 100, qRound(settings->nmsIouThreshold * 100.0f), &panel.nms, &panel.nmsValue,
              [](int v) { return percentText(v / 100.0f); }, [settings](int v) { settings->nmsIouThreshold = v / 100.0f; });
    if (showTrackingControls) {
        addSlider(QStringLiteral("Track match IoU"),
                  QStringLiteral("How much a new detection must overlap an existing track to continue it. Lower values tolerate camera/subject motion; higher values prevent unrelated faces from stealing a track."),
                  0, 100, qRound(settings->trackMatchIouThreshold * 100.0f), &panel.trackMatch, &panel.trackMatchValue,
                  [](int v) { return percentText(v / 100.0f); }, [settings](int v) { settings->trackMatchIouThreshold = v / 100.0f; });
        addSlider(QStringLiteral("New track confidence"),
                  QStringLiteral("Minimum confidence required to create a brand-new track. Lower this when valid faces are not starting tracks; raise it if noisy detections create extra tracks."),
                  1, 99, qRound(settings->newTrackMinConfidence * 100.0f), &panel.newTrack, &panel.newTrackValue,
                  [](int v) { return percentText(v / 100.0f); }, [settings](int v) { settings->newTrackMinConfidence = v / 100.0f; });
    }
    addSlider(QStringLiteral("ROI left"),
              QStringLiteral("Left edge of the allowed face region as a percent of frame width. Raise this to ignore detections on the far left."),
              0, 100, qRound(settings->roiX1 * 100.0f), &panel.roiX1, &panel.roiX1Value,
              [](int v) { return percentText(v / 100.0f); }, [settings](int v) {
                  settings->roiX1 = v / 100.0f;
                  if (settings->roiX2 < settings->roiX1) std::swap(settings->roiX1, settings->roiX2);
              });
    addSlider(QStringLiteral("ROI top"),
              QStringLiteral("Top edge of the allowed face region as a percent of frame height. Raise this to ignore detections near the top."),
              0, 100, qRound(settings->roiY1 * 100.0f), &panel.roiY1, &panel.roiY1Value,
              [](int v) { return percentText(v / 100.0f); }, [settings](int v) {
                  settings->roiY1 = v / 100.0f;
                  if (settings->roiY2 < settings->roiY1) std::swap(settings->roiY1, settings->roiY2);
              });
    addSlider(QStringLiteral("ROI right"),
              QStringLiteral("Right edge of the allowed face region as a percent of frame width. Lower this to ignore detections on the far right."),
              0, 100, qRound(settings->roiX2 * 100.0f), &panel.roiX2, &panel.roiX2Value,
              [](int v) { return percentText(v / 100.0f); }, [settings](int v) {
                  settings->roiX2 = v / 100.0f;
                  if (settings->roiX2 < settings->roiX1) std::swap(settings->roiX1, settings->roiX2);
              });
    addSlider(QStringLiteral("ROI bottom"),
              QStringLiteral("Bottom edge of the allowed face region as a percent of frame height. Lower this to ignore detections near the bottom."),
              0, 100, qRound(settings->roiY2 * 100.0f), &panel.roiY2, &panel.roiY2Value,
              [](int v) { return percentText(v / 100.0f); }, [settings](int v) {
                  settings->roiY2 = v / 100.0f;
                  if (settings->roiY2 < settings->roiY1) std::swap(settings->roiY1, settings->roiY2);
              });
    addSlider(QStringLiteral("Min face area"),
              QStringLiteral("Smallest accepted face box as a percent of frame area. Lower this for distant/small faces; raise it to reject tiny false positives."),
              0, 2000, qRound(settings->minFaceAreaRatio * 100000.0f), &panel.minArea, &panel.minAreaValue,
              [](int v) { return areaRatioText(v / 100000.0f); }, [settings](int v) { settings->minFaceAreaRatio = v / 100000.0f; });
    addSlider(QStringLiteral("Max face area"),
              QStringLiteral("Largest accepted face box as a percent of frame area. Lower this to reject oversized false positives or partial-frame detections; raise it to allow very close faces."),
              0, 100000, qRound(settings->maxFaceAreaRatio * 100000.0f), &panel.maxArea, &panel.maxAreaValue,
              [](int v) { return areaRatioText(v / 100000.0f); }, [settings](int v) { settings->maxFaceAreaRatio = v / 100000.0f; });
    addSlider(QStringLiteral("Min aspect"),
              QStringLiteral("Smallest allowed width/height ratio for a face box. Raise this to reject tall narrow boxes such as arms or fingers."),
              1, 1000, qRound(settings->minAspect * 100.0f), &panel.minAspect, &panel.minAspectValue,
              [](int v) { return aspectText(v / 100.0f); }, [settings](int v) {
                  settings->minAspect = v / 100.0f;
                  if (settings->maxAspect < settings->minAspect) std::swap(settings->minAspect, settings->maxAspect);
              });
    addSlider(QStringLiteral("Max aspect"),
              QStringLiteral("Largest allowed width/height ratio for a face box. Lower this to reject wide non-face boxes such as hands or partial torsos."),
              1, 1000, qRound(settings->maxAspect * 100.0f), &panel.maxAspect, &panel.maxAspectValue,
              [](int v) { return aspectText(v / 100.0f); }, [settings](int v) {
                  settings->maxAspect = v / 100.0f;
                  if (settings->maxAspect < settings->minAspect) std::swap(settings->minAspect, settings->maxAspect);
              });
    addSlider(QStringLiteral("Max faces/frame"),
              QStringLiteral("Maximum accepted faces per processed frame after filtering. 0 means unlimited. Use lower values for single-speaker videos and higher values for panels or crowds."),
              0, 64, settings->maxFacesPerFrame, &panel.maxFaces, &panel.maxFacesValue,
              [](int v) { return v == 0 ? QStringLiteral("unlimited") : QString::number(v); }, [settings](int v) { settings->maxFacesPerFrame = v; });

    if (showTrackingControls) {
        panel.primaryFaceOnly = new QCheckBox(QStringLiteral("Keep only strongest face"), panel.widget);
        panel.primaryFaceOnly->setToolTip(QStringLiteral("When enabled, only the strongest face detection is kept and only one track is allowed. Use this for single-subject clips to avoid track explosions."));
        form->addRow(QStringLiteral("Primary mode"), panel.primaryFaceOnly);
        QObject::connect(panel.primaryFaceOnly, &QCheckBox::toggled, panel.widget, [syncing, settings, persist](bool checked) {
            if (*syncing || !settings) return;
            settings->primaryFaceOnly = checked;
            persist();
        });
    }
    panel.smallFaceFallback = new QCheckBox(QStringLiteral("Enable Res10 tiled fallback"), panel.widget);
    panel.smallFaceFallback->setToolTip(QStringLiteral("Runs extra cropped Res10 passes when the zero-copy Res10 detector misses. This can help some small faces but is slower and may produce false positives; SCRFD usually handles small faces better."));
    form->addRow(QStringLiteral("Small-face fallback"), panel.smallFaceFallback);
    QObject::connect(panel.smallFaceFallback, &QCheckBox::toggled, panel.widget, [syncing, settings, persist](bool checked) {
        if (*syncing || !settings) return;
        settings->smallFaceFallback = checked;
        persist();
    });
    if (scrfdFamily) {
        panel.scrfdTiled = new QCheckBox(QStringLiteral("Enable SCRFD 2x2 tiled pass"), panel.widget);
        panel.scrfdTiled->setToolTip(QStringLiteral("Runs SCRFD on the full frame plus overlapping 2x2 tiles to improve recall on smaller panel faces at higher GPU cost."));
        form->addRow(QStringLiteral("SCRFD tiling"), panel.scrfdTiled);
        QObject::connect(panel.scrfdTiled, &QCheckBox::toggled, panel.widget, [syncing, settings, persist](bool checked) {
            if (*syncing || !settings) return;
            settings->scrfdTiled = checked;
            persist();
        });
    }
    panel.settingsPath = new QLabel(settingsPath, panel.widget);
    panel.settingsPath->setToolTip(QStringLiteral("Detector settings are saved here automatically and loaded the next time this video is opened."));
    panel.settingsPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    panel.settingsPath->setWordWrap(true);
    root->addWidget(panel.settingsPath);

    *syncing = true;
    syncDetectorSettingsPanel(&panel, *settings);
    *syncing = false;
    return panel;
}

FaceStreamPreflightDialogResult runFaceStreamPreflightDialog(
    DetectorRuntimeSettings* settings,
    const QString& detector,
    int scrfdTargetSize,
    const QString& settingsPath,
    const FaceStreamPreflightDialogOptions& options,
    QWidget* parent)
{
    FaceStreamPreflightDialogResult result;
    result.livePreview = options.livePreviewChecked;
    result.applyClipGrading = options.applyClipGradingChecked;
    if (!settings) {
        result.saveError = QStringLiteral("Detector settings are unavailable.");
        return result;
    }

    QDialog preflightDialog(parent);
    preflightDialog.setWindowTitle(options.title.trimmed().isEmpty()
                                       ? QStringLiteral("JCut DNN FaceStream Generator")
                                       : options.title);
    preflightDialog.setWindowFlag(Qt::Window, true);
    preflightDialog.resize(options.initialSize.isValid() ? options.initialSize : QSize(760, 420));

    auto* layout = new QVBoxLayout(&preflightDialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    if (!options.introText.trimmed().isEmpty()) {
        auto* infoLabel = new QLabel(options.introText, &preflightDialog);
        infoLabel->setWordWrap(true);
        layout->addWidget(infoLabel);
    }

    if (!options.detailText.trimmed().isEmpty()) {
        auto* detailLabel = new QLabel(options.detailText, &preflightDialog);
        detailLabel->setWordWrap(true);
        detailLabel->setStyleSheet(QStringLiteral("color: #8fa3b8; font-size: 11px;"));
        layout->addWidget(detailLabel);
    }

    QCheckBox* livePreviewCheckbox = nullptr;
    if (options.showLivePreviewToggle) {
        livePreviewCheckbox = new QCheckBox(QStringLiteral("Show live preview"), &preflightDialog);
        livePreviewCheckbox->setChecked(options.livePreviewChecked);
        layout->addWidget(livePreviewCheckbox);
    }

    QCheckBox* applyClipGradingCheckbox = nullptr;
    if (options.showApplyClipGradingToggle) {
        applyClipGradingCheckbox = new QCheckBox(
            options.applyClipGradingLabel.trimmed().isEmpty()
                ? QStringLiteral("Apply clip grading during detection")
                : options.applyClipGradingLabel,
            &preflightDialog);
        applyClipGradingCheckbox->setChecked(options.applyClipGradingChecked);
        layout->addWidget(applyClipGradingCheckbox);
    }

    DetectorSettingsPanel detectorPanel =
        createDetectorSettingsPanel(settings,
                                    detector,
                                    scrfdTargetSize,
                                    settingsPath,
                                    options.showTrackingControls,
                                    &preflightDialog);
    layout->addWidget(detectorPanel.widget);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* cancelButton = new QPushButton(
        options.cancelButtonText.trimmed().isEmpty() ? QStringLiteral("Cancel") : options.cancelButtonText,
        &preflightDialog);
    auto* proceedButton = new QPushButton(
        options.proceedButtonText.trimmed().isEmpty() ? QStringLiteral("Proceed") : options.proceedButtonText,
        &preflightDialog);
    buttons->addWidget(cancelButton);
    buttons->addWidget(proceedButton);
    layout->addLayout(buttons);

    QObject::connect(cancelButton, &QPushButton::clicked, &preflightDialog, &QDialog::reject);
    QObject::connect(proceedButton, &QPushButton::clicked, &preflightDialog, &QDialog::accept);

    if (preflightDialog.exec() != QDialog::Accepted) {
        return result;
    }

    result.accepted = true;
    result.livePreview = livePreviewCheckbox ? livePreviewCheckbox->isChecked() : options.livePreviewChecked;
    result.applyClipGrading =
        applyClipGradingCheckbox ? applyClipGradingCheckbox->isChecked() : options.applyClipGradingChecked;
    saveDetectorRuntimeSettingsFile(settingsPath,
                                    *settings,
                                    detector,
                                    scrfdTargetSize,
                                    &result.saveError);
    return result;
}

} // namespace jcut::facestream
