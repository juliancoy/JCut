#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QSize>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;
class QWidget;

namespace jcut::facedetections {

inline constexpr int kDefaultDetectorStride = 4;
inline constexpr int kDefaultDetectorMaxDetections = 512;
inline constexpr int kDefaultDetectorScrfdTargetSize = 960;
inline constexpr int kDefaultDetectorMaxFacesPerFrame = 16;
inline constexpr float kDefaultDetectorThreshold = 0.30f;
inline constexpr float kDefaultDetectorNmsIouThreshold = 0.35f;
inline constexpr float kDefaultDetectorTrackMatchIouThreshold = 0.35f;
inline constexpr float kDefaultDetectorNewTrackMinConfidence = 0.35f;
inline constexpr const char* kDefaultScrfdModelVariant = "500m";
inline constexpr bool kDefaultDetectorPrimaryFaceOnly = false;
inline constexpr bool kDefaultDetectorSmallFaceFallback = false;
inline constexpr bool kDefaultDetectorScrfdTiled = false;
inline constexpr float kDefaultDetectorRoiX1 = 0.0f;
inline constexpr float kDefaultDetectorRoiY1 = 0.0f;
inline constexpr float kDefaultDetectorRoiX2 = 1.0f;
inline constexpr float kDefaultDetectorRoiY2 = 1.0f;
inline constexpr float kDefaultDetectorMinFaceAreaRatio = 0.0f;
inline constexpr float kDefaultDetectorMaxFaceAreaRatio = 0.18f;
inline constexpr float kDefaultDetectorMinAspect = 0.50f;
inline constexpr float kDefaultDetectorMaxAspect = 1.60f;
inline constexpr bool kDefaultDetectorUseProxySource = false;

struct DetectorRuntimeSettings {
    int stride = kDefaultDetectorStride;
    int maxDetections = kDefaultDetectorMaxDetections;
    int scrfdTargetSize = kDefaultDetectorScrfdTargetSize;
    int maxFacesPerFrame = kDefaultDetectorMaxFacesPerFrame;
    QString scrfdModelVariant = QString::fromLatin1(kDefaultScrfdModelVariant);
    float threshold = kDefaultDetectorThreshold;
    float nmsIouThreshold = kDefaultDetectorNmsIouThreshold;
    float trackMatchIouThreshold = kDefaultDetectorTrackMatchIouThreshold;
    float newTrackMinConfidence = kDefaultDetectorNewTrackMinConfidence;
    bool primaryFaceOnly = kDefaultDetectorPrimaryFaceOnly;
    bool smallFaceFallback = kDefaultDetectorSmallFaceFallback;
    bool scrfdTiled = kDefaultDetectorScrfdTiled;
    float roiX1 = kDefaultDetectorRoiX1;
    float roiY1 = kDefaultDetectorRoiY1;
    float roiX2 = kDefaultDetectorRoiX2;
    float roiY2 = kDefaultDetectorRoiY2;
    float minFaceAreaRatio = kDefaultDetectorMinFaceAreaRatio;
    float maxFaceAreaRatio = kDefaultDetectorMaxFaceAreaRatio;
    float minAspect = kDefaultDetectorMinAspect;
    float maxAspect = kDefaultDetectorMaxAspect;
    bool useProxySource = kDefaultDetectorUseProxySource;
};

struct DetectorSettingsProfileDefinition {
    QString id;
    QString label;
    QString description;
    DetectorRuntimeSettings settings;
};

struct ScrfdModelVariantDefinition {
    QString id;
    QString label;
    QString description;
    QString modelStem;
};

struct DetectorSettingsPanel {
    QWidget* widget = nullptr;
    QComboBox* profileCombo = nullptr;
    QComboBox* scrfdModelVariant = nullptr;
    QSlider* stride = nullptr;
    QLabel* strideValue = nullptr;
    QSlider* maxDetections = nullptr;
    QLabel* maxDetectionsValue = nullptr;
    QSlider* scrfdTargetSize = nullptr;
    QLabel* scrfdTargetSizeValue = nullptr;
    QSlider* threshold = nullptr;
    QLabel* thresholdValue = nullptr;
    QSlider* nms = nullptr;
    QLabel* nmsValue = nullptr;
    QSlider* trackMatch = nullptr;
    QLabel* trackMatchValue = nullptr;
    QSlider* newTrack = nullptr;
    QLabel* newTrackValue = nullptr;
    QSlider* roiX1 = nullptr;
    QLabel* roiX1Value = nullptr;
    QSlider* roiY1 = nullptr;
    QLabel* roiY1Value = nullptr;
    QSlider* roiX2 = nullptr;
    QLabel* roiX2Value = nullptr;
    QSlider* roiY2 = nullptr;
    QLabel* roiY2Value = nullptr;
    QSlider* minArea = nullptr;
    QLabel* minAreaValue = nullptr;
    QSlider* maxArea = nullptr;
    QLabel* maxAreaValue = nullptr;
    QSlider* minAspect = nullptr;
    QLabel* minAspectValue = nullptr;
    QSlider* maxAspect = nullptr;
    QLabel* maxAspectValue = nullptr;
    QSlider* maxFaces = nullptr;
    QLabel* maxFacesValue = nullptr;
    QCheckBox* primaryFaceOnly = nullptr;
    QCheckBox* smallFaceFallback = nullptr;
    QCheckBox* scrfdTiled = nullptr;
    QLabel* settingsPath = nullptr;
};

struct FaceDetectionsPreflightDialogOptions {
    QString title = QStringLiteral("JCut DNN FaceDetections Generator");
    QString introText;
    QString detailText;
    QString proceedButtonText = QStringLiteral("Proceed");
    QString cancelButtonText = QStringLiteral("Cancel");
    QSize initialSize = QSize(760, 420);
    bool showLivePreviewToggle = true;
    bool livePreviewChecked = true;
    bool showTrackingControls = false;
    bool showApplyClipGradingToggle = false;
    bool applyClipGradingChecked = false;
    QString applyClipGradingLabel = QStringLiteral("Apply clip grading during detection");
    bool showRestartFromScratchToggle = false;
    bool restartFromScratchChecked = false;
    QString restartFromScratchLabel = QStringLiteral("Restart from scratch (delete facedetections.part)");
    bool showUseProxySourceToggle = false;
    bool useProxySourceChecked = false;
    QString useProxySourceLabel = QStringLiteral("Use proxy media as FaceDetections input");
};

struct FaceDetectionsPreflightDialogResult {
    bool accepted = false;
    bool livePreview = true;
    bool applyClipGrading = false;
    bool restartFromScratch = false;
    bool useProxySource = false;
    QString saveError;
};

QString detectorSettingsPathForVideo(const QString& videoPath);
QVector<ScrfdModelVariantDefinition> supportedScrfdModelVariants();
bool scrfdModelVariantById(const QString& id, ScrfdModelVariantDefinition* definitionOut);
QString normalizeScrfdModelVariantId(const QString& id);
bool ensureScrfdModelVariantAssets(const QString& variantId,
                                   QString* paramPathOut,
                                   QString* binPathOut,
                                   QString* errorMessage = nullptr);
bool ensureRes10FaceDnnModelAssets(const QString& baseDir,
                                   QString* prototxtOut,
                                   QString* modelOut,
                                   QString* errorMessage = nullptr);
QVector<DetectorSettingsProfileDefinition> builtInDetectorProfiles();
bool builtInDetectorProfileById(const QString& id, DetectorRuntimeSettings* settingsOut);
QJsonObject detectorRuntimeSettingsToJson(const DetectorRuntimeSettings& settings,
                                          const QString& detector,
                                          int scrfdTargetSize = kDefaultDetectorScrfdTargetSize);
bool applyDetectorRuntimeSettingsObject(const QJsonObject& object, DetectorRuntimeSettings* settings);
bool loadDetectorRuntimeSettingsFile(const QString& path,
                                     DetectorRuntimeSettings* settings,
                                     QDateTime* lastAppliedMtime = nullptr);
bool saveDetectorRuntimeSettingsFile(const QString& path,
                                     const DetectorRuntimeSettings& settings,
                                     const QString& detector,
                                     int scrfdTargetSize = kDefaultDetectorScrfdTargetSize,
                                     QString* errorMessage = nullptr);
DetectorSettingsPanel createDetectorSettingsPanel(DetectorRuntimeSettings* settings,
                                                  const QString& detector,
                                                  int scrfdTargetSize,
                                                  const QString& settingsPath,
                                                  bool showTrackingControls = false,
                                                  QWidget* parent = nullptr);
void syncDetectorSettingsPanel(DetectorSettingsPanel* panel, const DetectorRuntimeSettings& settings);
FaceDetectionsPreflightDialogResult runFaceDetectionsPreflightDialog(
    DetectorRuntimeSettings* settings,
    const QString& detector,
    int scrfdTargetSize,
    const QString& settingsPath,
    const FaceDetectionsPreflightDialogOptions& options,
    QWidget* parent = nullptr);

} // namespace jcut::facedetections
