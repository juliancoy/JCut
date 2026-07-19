#pragma once

#include "preview_interaction_state.h"
#include "playback_stage_metrics.h"

#include <functional>
#include <QJsonObject>
#include <QImage>
#include <QPointer>
#include <QRectF>
#include <QVulkanInstance>

#include <memory>

class QWidget;
class QLabel;
class QFrame;
class QStackedLayout;
class QVulkanWindow;
class DirectVulkanPreviewWindow;

struct DirectVulkanPreviewStats {
    int64_t handoffAttempts = 0;
    int64_t handoffSuccesses = 0;
    int64_t handoffFailures = 0;
    int64_t sampledImageReady = 0;
    int64_t textureDraws = 0;
    int64_t checkerDraws = 0;
    int64_t clearFallbackDraws = 0;
    int64_t explicitFailureDraws = 0;
    QString lastClearFallbackReason;
    int64_t activeClipDraws = 0;
    double lastUploadMs = 0.0;
    QString lastHandoffMode;
    QString lastHandoffError;
    QString lastProbePath;
    QString lastProbeReason;
    QString lastHardwareSwFormat;
    QString lastVulkanImageFormat;
    QString lastYuvRgbMatrix;
    QString lastDiagnosticReadbackFormat;
    QString lastDecoderDiagnosticReadbackFormat;
    QSize lastExternalImageSize;
    QSize lastDecoderDiagnosticReadbackSize;
    QSize lastDiagnosticReadbackSize;
    QString lastEffectsPath;
    QString lastUnsupportedEffect;
    QRectF lastTargetRect;
    QRectF lastFittedRect;
    double lastAppliedBrightness = 0.0;
    double lastAppliedContrast = 1.0;
    double lastAppliedSaturation = 1.0;
    double lastAppliedOpacity = 1.0;
    double lastAppliedRotation = 0.0;
    double lastAppliedScaleX = 1.0;
    double lastAppliedScaleY = 1.0;
    bool lastCurveLutApplied = false;
    int64_t diagnosticReadbackRequests = 0;
    int64_t diagnosticReadbackCopies = 0;
    int64_t decoderDiagnosticReadbackCopies = 0;
    int64_t previewUpdateRequests = 0;
    int64_t previewUpdatesDelivered = 0;
    double lastPreviewUpdateLatencyMs = 0.0;
    double maxPreviewUpdateLatencyMs = 0.0;
    double lastPresentIntervalMs = 0.0;
    double maxPresentIntervalMs = 0.0;
    int descriptorSetIndex = -1;
    int descriptorSetCount = 0;
    int activeClipHandoffResourceCount = 0;
    int retiredClipHandoffResourceCount = 0;
    bool finalCompositeStretchPrepared = false;
    bool finalCompositeStretchDrawn = false;
    QString finalCompositeStretchSourceClipId;
    QString finalCompositeStretchSourceLabel;
    QString finalCompositeStretchReason;
    int transcriptCandidateCount = 0;
    int transcriptPreparedCount = 0;
    int transcriptDrawnCount = 0;
    int titleCandidateCount = 0;
    int titlePreparedCount = 0;
    int titleDrawnCount = 0;
    QString lastTitleSkipReason;
    QString lastTitleClipId;
    QString lastTranscriptSkipReason;
    QString lastTranscriptClipId;
    QString lastTranscriptPath;
    QString lastTranscriptTimingSource;
    int64_t lastTranscriptTimelineSample = -1;
    int64_t lastTranscriptFrame = -1;
    int64_t lastTranscriptPresentedMediaSourceFrame = -1;
    QString lastTextPrepFailureReason;
    QString lastTextDrawFailureReason;
    editor::PlaybackStageMetric textPrepStageMetric;
    editor::PlaybackStageMetric textDrawStageMetric;
    editor::PlaybackStageMetric gpuHandoffStageMetric;
    editor::PlaybackStageMetric commandRecordingStageMetric;
    editor::PlaybackStageMetric presentationStageMetric;
};

class DirectVulkanPreviewPresenter final {
public:
    explicit DirectVulkanPreviewPresenter(PreviewInteractionState* state, QWidget* parent = nullptr);
    ~DirectVulkanPreviewPresenter();

    DirectVulkanPreviewPresenter(const DirectVulkanPreviewPresenter&) = delete;
    DirectVulkanPreviewPresenter& operator=(const DirectVulkanPreviewPresenter&) = delete;

    QWidget* widget() const;
    bool isActive() const;
    bool hasFailed() const;
    bool updatePending() const;
    int64_t presentedFrames() const;
    int64_t lastPresentedSourceFrame() const;
    QString failureReason() const;
    QString backendName() const;
    void setInteractionCallbacks(
        std::function<void(const QString&)> selectionRequested,
        std::function<void(const QString&, qreal, qreal, bool)> moveRequested,
        std::function<void(const QString&, qreal, qreal, qreal, qreal, bool)> transformRequested = {},
        std::function<void(int64_t)> playbackSampleRequested = {},
        std::function<void(const QString&, qreal, qreal)> correctionPointRequested = {},
        std::function<void(const QString&, qreal, qreal)> speakerPointRequested = {},
        std::function<void(const QString&, qreal, qreal, qreal)> speakerBoxRequested = {},
        std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)> faceStreamBoxRequested = {},
        std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)> faceStreamBoxFocusClearRequested = {},
        std::function<void(const QString&)> faceStreamBoxClickStatus = {},
        std::function<void(const QString&)> createKeyframeRequested = {});

    void requestUpdate();
    void requestPipelineTapReadback();
    QImage latestPipelineTapImage() const;
    void updateTitle();
    QJsonObject pipelineHealthSnapshot() const;
    QJsonObject profilingSnapshot() const;
    void resetProfilingStats();

private:
    void showFailure(const QString& reason);
    void updateDiagnosticChrome();
    void updateAudioOverlay();

    std::unique_ptr<QVulkanInstance> m_instance;
    std::unique_ptr<QWidget> m_placeholder;
    QPointer<QWidget> m_windowContainer;
    QPointer<QWidget> m_overlayWidget;
    QPointer<QLabel> m_statusLabel;
    QPointer<QLabel> m_errorLabel;
    QPointer<QWidget> m_audioInfoPanel;
    QPointer<QLabel> m_audioTitleLabel;
    QPointer<QLabel> m_audioSummaryLabel;
    QPointer<QLabel> m_audioFooterLabel;
    QPointer<QFrame> m_audioHoverCard;
    QPointer<QLabel> m_audioHoverAvatarLabel;
    QPointer<QLabel> m_audioHoverNameLabel;
    QPointer<QLabel> m_audioHoverOrgLabel;
    QPointer<QLabel> m_audioHoverMetaLabel;
    QPointer<QLabel> m_audioHoverDescLabel;
    QPointer<QStackedLayout> m_stack;
    DirectVulkanPreviewWindow* m_window = nullptr;
    PreviewInteractionState* m_state = nullptr;
    QString m_failureReason;
    QString m_lastDiagnosticChromeStyle;
    QString m_lastDiagnosticChromeText;
    QRect m_lastDiagnosticChromeGeometry;
    bool m_lastDiagnosticChromeLabelVisible = false;
    bool m_active = false;
    int64_t m_presentedFrames = 0;
    int64_t m_lastPresentedSourceFrame = -1;
    DirectVulkanPreviewStats m_stats;
};
