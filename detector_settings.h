#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

class QCheckBox;
class QLabel;
class QSlider;
class QWidget;

namespace jcut::boxstream {

struct DetectorRuntimeSettings {
    int stride = 12;
    int maxDetections = 256;
    int maxFacesPerFrame = 32;
    float threshold = 0.45f;
    float nmsIouThreshold = 0.35f;
    float trackMatchIouThreshold = 0.22f;
    float newTrackMinConfidence = 0.45f;
    bool primaryFaceOnly = true;
    bool smallFaceFallback = false;
    float roiX1 = 0.0f;
    float roiY1 = 0.0f;
    float roiX2 = 1.0f;
    float roiY2 = 1.0f;
    float minFaceAreaRatio = 0.0005f;
    float maxFaceAreaRatio = 1.0f;
    float minAspect = 0.45f;
    float maxAspect = 1.80f;
};

struct DetectorSettingsPanel {
    QWidget* widget = nullptr;
    QSlider* threshold = nullptr;
    QLabel* thresholdValue = nullptr;
    QSlider* nms = nullptr;
    QLabel* nmsValue = nullptr;
    QSlider* trackMatch = nullptr;
    QLabel* trackMatchValue = nullptr;
    QSlider* newTrack = nullptr;
    QLabel* newTrackValue = nullptr;
    QSlider* minArea = nullptr;
    QLabel* minAreaValue = nullptr;
    QSlider* maxFaces = nullptr;
    QLabel* maxFacesValue = nullptr;
    QCheckBox* primaryFaceOnly = nullptr;
    QCheckBox* smallFaceFallback = nullptr;
    QLabel* settingsPath = nullptr;
};

QString detectorSettingsPathForVideo(const QString& videoPath);
QJsonObject detectorRuntimeSettingsToJson(const DetectorRuntimeSettings& settings,
                                          const QString& detector,
                                          int scrfdTargetSize = 640);
bool applyDetectorRuntimeSettingsObject(const QJsonObject& object, DetectorRuntimeSettings* settings);
bool loadDetectorRuntimeSettingsFile(const QString& path,
                                     DetectorRuntimeSettings* settings,
                                     QDateTime* lastAppliedMtime = nullptr);
bool saveDetectorRuntimeSettingsFile(const QString& path,
                                     const DetectorRuntimeSettings& settings,
                                     const QString& detector,
                                     int scrfdTargetSize = 640,
                                     QString* errorMessage = nullptr);
DetectorSettingsPanel createDetectorSettingsPanel(DetectorRuntimeSettings* settings,
                                                  const QString& detector,
                                                  int scrfdTargetSize,
                                                  const QString& settingsPath,
                                                  QWidget* parent = nullptr);
void syncDetectorSettingsPanel(DetectorSettingsPanel* panel, const DetectorRuntimeSettings& settings);

} // namespace jcut::boxstream
