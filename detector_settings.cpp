#include "detector_settings.h"

#include <QCheckBox>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

namespace jcut::boxstream {
namespace {

QString percentText(float value)
{
    return QStringLiteral("%1").arg(static_cast<double>(value), 0, 'f', 2);
}

QString areaRatioText(float value)
{
    return QStringLiteral("%1%").arg(static_cast<double>(value * 100.0f), 0, 'f', 3);
}

} // namespace

QString detectorSettingsPathForVideo(const QString& videoPath)
{
    const QFileInfo info(videoPath);
    const QString baseName = info.completeBaseName().isEmpty() ? info.fileName() : info.completeBaseName();
    return info.dir().absoluteFilePath(baseName + QStringLiteral("_detectorsettings.json"));
}

QJsonObject detectorRuntimeSettingsToJson(const DetectorRuntimeSettings& s,
                                          const QString& detector,
                                          int scrfdTargetSize)
{
    return {
        {QStringLiteral("detector"), detector},
        {QStringLiteral("scrfd_target_size"), scrfdTargetSize},
        {QStringLiteral("stride"), s.stride},
        {QStringLiteral("max_detections"), s.maxDetections},
        {QStringLiteral("max_faces_per_frame"), s.maxFacesPerFrame},
        {QStringLiteral("threshold"), s.threshold},
        {QStringLiteral("nms_iou_threshold"), s.nmsIouThreshold},
        {QStringLiteral("track_match_iou_threshold"), s.trackMatchIouThreshold},
        {QStringLiteral("new_track_min_confidence"), s.newTrackMinConfidence},
        {QStringLiteral("primary_face_only"), s.primaryFaceOnly},
        {QStringLiteral("small_face_fallback"), s.smallFaceFallback},
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
    if (o.contains(QStringLiteral("max_faces_per_frame"))) s->maxFacesPerFrame = std::max(0, o.value(QStringLiteral("max_faces_per_frame")).toInt(s->maxFacesPerFrame));
    if (o.contains(QStringLiteral("threshold"))) s->threshold = std::clamp(static_cast<float>(o.value(QStringLiteral("threshold")).toDouble(s->threshold)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("nms_iou_threshold"))) s->nmsIouThreshold = std::clamp(static_cast<float>(o.value(QStringLiteral("nms_iou_threshold")).toDouble(s->nmsIouThreshold)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("track_match_iou_threshold"))) s->trackMatchIouThreshold = std::clamp(static_cast<float>(o.value(QStringLiteral("track_match_iou_threshold")).toDouble(s->trackMatchIouThreshold)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("new_track_min_confidence"))) s->newTrackMinConfidence = std::clamp(static_cast<float>(o.value(QStringLiteral("new_track_min_confidence")).toDouble(s->newTrackMinConfidence)), 0.0f, 1.0f);
    if (o.contains(QStringLiteral("primary_face_only"))) s->primaryFaceOnly = o.value(QStringLiteral("primary_face_only")).toBool(s->primaryFaceOnly);
    if (o.contains(QStringLiteral("small_face_fallback"))) s->smallFaceFallback = o.value(QStringLiteral("small_face_fallback")).toBool(s->smallFaceFallback);
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
    f.write(QJsonDocument(detectorRuntimeSettingsToJson(settings, detector, scrfdTargetSize)).toJson(QJsonDocument::Indented));
    f.write("\n");
    return true;
}

void syncDetectorSettingsPanel(DetectorSettingsPanel* panel, const DetectorRuntimeSettings& s)
{
    if (!panel || !panel->widget) return;
    panel->threshold->setValue(qRound(s.threshold * 100.0f));
    panel->thresholdValue->setText(percentText(s.threshold));
    panel->nms->setValue(qRound(s.nmsIouThreshold * 100.0f));
    panel->nmsValue->setText(percentText(s.nmsIouThreshold));
    panel->trackMatch->setValue(qRound(s.trackMatchIouThreshold * 100.0f));
    panel->trackMatchValue->setText(percentText(s.trackMatchIouThreshold));
    panel->newTrack->setValue(qRound(s.newTrackMinConfidence * 100.0f));
    panel->newTrackValue->setText(percentText(s.newTrackMinConfidence));
    panel->minArea->setValue(qRound(s.minFaceAreaRatio * 100000.0f));
    panel->minAreaValue->setText(areaRatioText(s.minFaceAreaRatio));
    panel->maxFaces->setValue(s.maxFacesPerFrame);
    panel->maxFacesValue->setText(s.maxFacesPerFrame == 0 ? QStringLiteral("unlimited") : QString::number(s.maxFacesPerFrame));
    panel->primaryFaceOnly->setChecked(s.primaryFaceOnly);
    panel->smallFaceFallback->setChecked(s.smallFaceFallback);
}

DetectorSettingsPanel createDetectorSettingsPanel(DetectorRuntimeSettings* settings,
                                                  const QString& detector,
                                                  int scrfdTargetSize,
                                                  const QString& settingsPath,
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
        if (!saveDetectorRuntimeSettingsFile(settingsPath, *settings, detector, scrfdTargetSize, &error) && !error.isEmpty()) {
            qWarning().noquote() << error;
        }
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

    addSlider(QStringLiteral("Confidence threshold"),
              QStringLiteral("Minimum detector confidence accepted as a face. Lower this to catch smaller or blurrier faces; raise it to reject false positives such as text, hands, or background shapes."),
              1, 99, qRound(settings->threshold * 100.0f), &panel.threshold, &panel.thresholdValue,
              [](int v) { return percentText(v / 100.0f); }, [settings](int v) { settings->threshold = v / 100.0f; });
    addSlider(QStringLiteral("NMS IoU"),
              QStringLiteral("Overlap threshold for merging duplicate face boxes. Lower values suppress nearby duplicate boxes more aggressively; higher values keep more overlapping detections."),
              0, 100, qRound(settings->nmsIouThreshold * 100.0f), &panel.nms, &panel.nmsValue,
              [](int v) { return percentText(v / 100.0f); }, [settings](int v) { settings->nmsIouThreshold = v / 100.0f; });
    addSlider(QStringLiteral("Track match IoU"),
              QStringLiteral("How much a new detection must overlap an existing track to continue it. Lower values tolerate camera/subject motion; higher values prevent unrelated faces from stealing a track."),
              0, 100, qRound(settings->trackMatchIouThreshold * 100.0f), &panel.trackMatch, &panel.trackMatchValue,
              [](int v) { return percentText(v / 100.0f); }, [settings](int v) { settings->trackMatchIouThreshold = v / 100.0f; });
    addSlider(QStringLiteral("New track confidence"),
              QStringLiteral("Minimum confidence required to create a brand-new track. Lower this when valid faces are not starting tracks; raise it if noisy detections create extra tracks."),
              1, 99, qRound(settings->newTrackMinConfidence * 100.0f), &panel.newTrack, &panel.newTrackValue,
              [](int v) { return percentText(v / 100.0f); }, [settings](int v) { settings->newTrackMinConfidence = v / 100.0f; });
    addSlider(QStringLiteral("Min face area"),
              QStringLiteral("Smallest accepted face box as a percent of frame area. Lower this for distant/small faces; raise it to reject tiny false positives."),
              0, 2000, qRound(settings->minFaceAreaRatio * 100000.0f), &panel.minArea, &panel.minAreaValue,
              [](int v) { return areaRatioText(v / 100000.0f); }, [settings](int v) { settings->minFaceAreaRatio = v / 100000.0f; });
    addSlider(QStringLiteral("Max faces/frame"),
              QStringLiteral("Maximum accepted faces per processed frame after filtering. 0 means unlimited. Use 1 or Primary mode for single-speaker videos; increase for panels or crowds."),
              0, 64, settings->maxFacesPerFrame, &panel.maxFaces, &panel.maxFacesValue,
              [](int v) { return v == 0 ? QStringLiteral("unlimited") : QString::number(v); }, [settings](int v) { settings->maxFacesPerFrame = v; });

    panel.primaryFaceOnly = new QCheckBox(QStringLiteral("Keep only strongest face"), panel.widget);
    panel.primaryFaceOnly->setToolTip(QStringLiteral("When enabled, only the strongest face detection is kept and only one track is allowed. Use this for single-subject clips to avoid track explosions."));
    form->addRow(QStringLiteral("Primary mode"), panel.primaryFaceOnly);
    QObject::connect(panel.primaryFaceOnly, &QCheckBox::toggled, panel.widget, [syncing, settings, persist](bool checked) {
        if (*syncing || !settings) return;
        settings->primaryFaceOnly = checked;
        persist();
    });
    panel.smallFaceFallback = new QCheckBox(QStringLiteral("Enable Res10 tiled fallback"), panel.widget);
    panel.smallFaceFallback->setToolTip(QStringLiteral("Runs extra cropped Res10 passes when the zero-copy Res10 detector misses. This can help some small faces but is slower and may produce false positives; SCRFD usually handles small faces better."));
    form->addRow(QStringLiteral("Small-face fallback"), panel.smallFaceFallback);
    QObject::connect(panel.smallFaceFallback, &QCheckBox::toggled, panel.widget, [syncing, settings, persist](bool checked) {
        if (*syncing || !settings) return;
        settings->smallFaceFallback = checked;
        persist();
    });
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

} // namespace jcut::boxstream
