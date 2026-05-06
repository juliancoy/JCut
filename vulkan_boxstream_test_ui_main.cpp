#include "boxstream_generation.h"
#include "decoder_context.h"
#include "render_internal.h"

#include <QApplication>
#include <QBoxLayout>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFormLayout>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>

#include <algorithm>
#include <memory>

#if JCUT_HAVE_OPENCV
#include <opencv2/objdetect.hpp>
#endif

namespace {

using namespace jcut::boxstream;

class BoxstreamTestWindow final : public QWidget {
public:
    BoxstreamTestWindow()
    {
        setWindowTitle(QStringLiteral("JCut Vulkan FaceStream Integrated Test UI"));
        resize(980, 760);

        auto* root = new QVBoxLayout(this);
        auto* form = new QFormLayout;
        m_videoPath = new QLineEdit(QStringLiteral("/mnt/Cancer/PanelVid2TikTok/Politics/YTDown.com_YouTube_Meet-the-Candidates-for-Baltimore-County_Media_Hho5MORgIj8_001_1080p.mp4"), this);
        m_maxFrames = new QSpinBox(this);
        m_maxFrames->setRange(1, 200000);
        m_maxFrames->setValue(600);
        m_stride = new QSpinBox(this);
        m_stride->setRange(1, 10000);
        m_stride->setValue(6);
        form->addRow(QStringLiteral("Video"), m_videoPath);
        form->addRow(QStringLiteral("Max frames"), m_maxFrames);
        form->addRow(QStringLiteral("Step frames"), m_stride);
        root->addLayout(form);

        auto* buttons = new QHBoxLayout;
        m_runButton = new QPushButton(QStringLiteral("Run Integrated Native-Hybrid Vulkan Scan"), this);
        m_stopButton = new QPushButton(QStringLiteral("Stop"), this);
        m_stopButton->setEnabled(false);
        buttons->addWidget(m_runButton);
        buttons->addWidget(m_stopButton);
        buttons->addStretch(1);
        root->addLayout(buttons);

        m_status = new QLabel(QStringLiteral("Idle"), this);
        root->addWidget(m_status);
        m_preview = new QLabel(QStringLiteral("Preview will appear here"), this);
        m_preview->setMinimumSize(640, 360);
        m_preview->setAlignment(Qt::AlignCenter);
        m_preview->setStyleSheet(QStringLiteral("background:#111; border:1px solid #444; color:#bbb;"));
        root->addWidget(m_preview, 1);

        m_log = new QPlainTextEdit(this);
        m_log->setReadOnly(true);
        m_log->setMaximumBlockCount(2000);
        root->addWidget(m_log, 1);

        connect(m_runButton, &QPushButton::clicked, this, [this]() { startRun(); });
        connect(m_stopButton, &QPushButton::clicked, this, [this]() { stopRun(); });
        connect(&m_scanTimer, &QTimer::timeout, this, [this]() { processOneFrame(); });
        m_scanTimer.setInterval(0);
    }

private:
    void startRun()
    {
#if !JCUT_HAVE_OPENCV
        m_status->setText(QStringLiteral("OpenCV is not enabled in this build."));
#else
        if (m_running) {
            return;
        }
        const QString video = m_videoPath->text().trimmed();
        if (!QFileInfo::exists(video)) {
            m_status->setText(QStringLiteral("Video does not exist."));
            return;
        }
        resetRunState();
        m_mediaPath = video;
        m_scanEnd = std::max<int64_t>(0, m_maxFrames->value() - 1);
        m_stepFrames = std::max(1, m_stride->value());

        const QString cascadePath = findCascadeFile(QStringLiteral("haarcascade_frontalface_default.xml"));
        if (cascadePath.isEmpty() || !m_faceCascade.load(cascadePath.toStdString())) {
            m_status->setText(QStringLiteral("Failed to load Haar cascade."));
            return;
        }
        const QString altPath = findCascadeFile(QStringLiteral("haarcascade_frontalface_alt2.xml"));
        if (!altPath.isEmpty()) {
            m_faceCascadeAlt.load(altPath.toStdString());
        }
        const QString profilePath = findCascadeFile(QStringLiteral("haarcascade_profileface.xml"));
        if (!profilePath.isEmpty()) {
            m_faceCascadeProfile.load(profilePath.toStdString());
        }

        m_decoder = std::make_unique<editor::DecoderContext>(m_mediaPath);
        if (!m_decoder->initialize()) {
            m_status->setText(QStringLiteral("Failed to initialize decoder."));
            return;
        }
        m_outputSize = m_decoder->info().frameSize.isValid() ? m_decoder->info().frameSize : QSize(1920, 1080);
        m_renderer = std::make_unique<render_detail::OffscreenVulkanRenderer>();
        QString error;
        if (!m_renderer->initialize(m_outputSize, &error)) {
            m_status->setText(QStringLiteral("Vulkan renderer init failed: %1").arg(error));
            return;
        }

        m_clip.id = QStringLiteral("boxstream-vulkan-source");
        m_clip.filePath = m_mediaPath;
        m_clip.proxyPath.clear();
        m_clip.useProxy = false;
        m_clip.mediaType = ClipMediaType::Video;
        m_clip.videoEnabled = true;
        m_clip.audioEnabled = false;
        m_clip.hasAudio = false;
        m_clip.startFrame = 0;
        m_clip.sourceInFrame = 0;
        m_clip.durationFrames = m_scanEnd + 1;
        m_clip.sourceDurationFrames = m_scanEnd + 1;
        m_clip.playbackRate = 1.0;
        m_clip.trackIndex = 0;
        m_clip.opacity = 1.0;
        m_clip.contrast = 1.0;
        m_clip.saturation = 1.0;
        m_clip.baseScaleX = 1.0;
        m_clip.baseScaleY = 1.0;
        m_clip.transcriptOverlay.enabled = false;
        m_clip.speakerFramingEnabled = false;

        m_running = true;
        m_runButton->setEnabled(false);
        m_stopButton->setEnabled(true);
        m_wall.start();
        m_log->appendPlainText(QStringLiteral("Integrated path: OffscreenVulkanRenderer::renderFrame -> Haar continuity scan -> buildScanPreview"));
        m_scanTimer.start();
#endif
    }

    void stopRun()
    {
        if (!m_running) {
            return;
        }
        finishRun(QStringLiteral("Stopped"));
    }

    QImage renderFrameWithUiPath(int64_t timelineFrame, qint64* decodeMs, qint64* textureMs, qint64* compositeMs, qint64* readbackMs)
    {
        TimelineClip clip = m_clip;
        clip.startFrame = timelineFrame;
        clip.sourceInFrame = std::max<int64_t>(0, timelineFrame);
        clip.durationFrames = 1;
        clip.sourceDurationFrames = std::max<int64_t>(clip.sourceInFrame + 1, m_clip.sourceDurationFrames);

        RenderRequest request;
        request.outputPath = QStringLiteral("boxstream://vulkan");
        request.outputFormat = QStringLiteral("boxstream-preview");
        request.outputSize = m_outputSize;
        request.bypassGrading = true;
        request.correctionsEnabled = false;
        request.clips = QVector<TimelineClip>{clip};
        request.tracks = QVector<TimelineTrack>{TimelineTrack{}};
        request.exportStartFrame = timelineFrame;
        request.exportEndFrame = timelineFrame;

        return m_renderer->renderFrame(request,
                                       timelineFrame,
                                       m_renderDecoders,
                                       nullptr,
                                       &m_asyncFrameCache,
                                       QVector<TimelineClip>{clip},
                                       nullptr,
                                       decodeMs,
                                       textureMs,
                                       compositeMs,
                                       readbackMs,
                                       nullptr,
                                       nullptr);
    }

    void processOneFrame()
    {
#if JCUT_HAVE_OPENCV
        if (!m_running) {
            return;
        }
        if (m_timelineFrame > m_scanEnd) {
            finishRun(QStringLiteral("Finished"));
            return;
        }

        qint64 decodeMs = 0;
        qint64 textureMs = 0;
        qint64 compositeMs = 0;
        qint64 readbackMs = 0;
        QImage image = renderFrameWithUiPath(m_timelineFrame, &decodeMs, &textureMs, &compositeMs, &readbackMs);
        if (!image.isNull()) {
            ++m_vulkanFrames;
        } else if (m_decoder) {
            ++m_fallbackFrames;
            const editor::FrameHandle frame = m_decoder->decodeFrame(m_timelineFrame);
            image = frame.hasCpuImage() ? frame.cpuImage() : QImage();
        }
        if (image.isNull()) {
            m_timelineFrame += m_stepFrames;
            return;
        }

        const cv::Mat bgr = qImageToBgrMat(image);
        cv::Mat gray;
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
        cv::equalizeHist(gray, gray);

        const int minSide = std::max(1, std::min(image.width(), image.height()));
        const cv::Size minFace(std::max(18, minSide / 22), std::max(18, minSide / 22));
        const cv::Size maxFace(std::max(minFace.width + 1, (minSide * 3) / 4),
                               std::max(minFace.height + 1, (minSide * 3) / 4));
        std::vector<WeightedDetection> weighted;
        auto runCascade = [&](cv::CascadeClassifier& cascade, int neighbors, double bias) {
            if (cascade.empty()) {
                return;
            }
            std::vector<cv::Rect> raw;
            std::vector<int> rejectLevels;
            std::vector<double> levelWeights;
            cascade.detectMultiScale(gray, raw, rejectLevels, levelWeights,
                                      1.08, neighbors, cv::CASCADE_SCALE_IMAGE, minFace, maxFace, true);
            for (int i = 0; i < static_cast<int>(raw.size()); ++i) {
                const double weight = (i < static_cast<int>(levelWeights.size()) ? levelWeights[i] : 0.0) + bias;
                if (weight >= 0.2) {
                    weighted.push_back({raw[i], weight});
                }
            }
        };
        runCascade(m_faceCascade, 5, 0.0);
        runCascade(m_faceCascadeAlt, 4, -0.05);
        runCascade(m_faceCascadeProfile, 4, -0.10);
        const std::vector<WeightedDetection> filtered = filterAndSuppressDetections(std::move(weighted), image.size());
        std::vector<cv::Rect> detections;
        for (const WeightedDetection& det : filtered) {
            detections.push_back(det.box);
        }

        updateTracks(detections, image.size());
        m_totalDetections += static_cast<int>(detections.size());
        ++m_processedFrames;
        m_decodeMs += decodeMs;
        m_textureMs += textureMs;
        m_compositeMs += compositeMs;
        m_readbackMs += readbackMs;

        const QImage preview = buildScanPreview(image, detections, static_cast<int>(m_tracks.size()));
        m_preview->setPixmap(QPixmap::fromImage(preview.scaled(m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        m_status->setText(QStringLiteral("Frame %1/%2 | tracks=%3 detections=%4 | vulkan_frames=%5 fallback=%6 | decode=%7ms texture=%8ms composite=%9ms readback=%10ms")
                              .arg(m_timelineFrame)
                              .arg(m_scanEnd)
                              .arg(m_tracks.size())
                              .arg(m_totalDetections)
                              .arg(m_vulkanFrames)
                              .arg(m_fallbackFrames)
                              .arg(decodeMs)
                              .arg(textureMs)
                              .arg(compositeMs)
                              .arg(readbackMs));
        m_log->appendPlainText(QStringLiteral("frame=%1 detections=%2 tracks=%3 decode_ms=%4 texture_ms=%5 composite_ms=%6 readback_ms=%7")
                                   .arg(m_timelineFrame)
                                   .arg(detections.size())
                                   .arg(m_tracks.size())
                                   .arg(decodeMs)
                                   .arg(textureMs)
                                   .arg(compositeMs)
                                   .arg(readbackMs));
        m_timelineFrame += m_stepFrames;
#endif
    }

#if JCUT_HAVE_OPENCV
    void updateTracks(const std::vector<cv::Rect>& detections, const QSize& imageSize)
    {
        for (const cv::Rect& det : detections) {
            int bestTrackIndex = -1;
            double bestScore = 0.0;
            for (int i = 0; i < static_cast<int>(m_tracks.size()); ++i) {
                if (m_timelineFrame - m_tracks[i].lastTimelineFrame > (m_stepFrames * 5)) {
                    continue;
                }
                const double score = iou(m_tracks[i].box, det);
                if (score > bestScore) {
                    bestScore = score;
                    bestTrackIndex = i;
                }
            }
            if (bestTrackIndex < 0 || bestScore < 0.2) {
                ContinuityTrackState state;
                state.id = m_nextTrackId++;
                state.box = det;
                state.lastTimelineFrame = m_timelineFrame;
                m_tracks.push_back(state);
            } else {
                m_tracks[bestTrackIndex].box = det;
                m_tracks[bestTrackIndex].lastTimelineFrame = m_timelineFrame;
            }
        }
        Q_UNUSED(imageSize);
    }
#endif

    void finishRun(const QString& prefix)
    {
        m_scanTimer.stop();
        m_running = false;
        m_runButton->setEnabled(true);
        m_stopButton->setEnabled(false);
        const double frames = std::max(1, m_processedFrames);
        m_status->setText(QStringLiteral("%1 | processed=%2 tracks=%3 detections=%4 vulkan_frames=%5 fallback=%6 avg_decode_ms=%7 avg_texture_ms=%8 avg_composite_ms=%9 avg_readback_ms=%10 elapsed_ms=%11")
                              .arg(prefix)
                              .arg(m_processedFrames)
                              .arg(m_tracks.size())
                              .arg(m_totalDetections)
                              .arg(m_vulkanFrames)
                              .arg(m_fallbackFrames)
                              .arg(m_decodeMs / frames, 0, 'f', 3)
                              .arg(m_textureMs / frames, 0, 'f', 3)
                              .arg(m_compositeMs / frames, 0, 'f', 3)
                              .arg(m_readbackMs / frames, 0, 'f', 3)
                              .arg(m_wall.elapsed()));
        m_log->appendPlainText(m_status->text());
        qDeleteAll(m_renderDecoders);
        m_renderDecoders.clear();
        m_asyncFrameCache.clear();
        m_renderer.reset();
        m_decoder.reset();
    }

    void resetRunState()
    {
        m_scanTimer.stop();
        m_log->clear();
        m_preview->setText(QStringLiteral("Preview will appear here"));
        m_preview->setPixmap(QPixmap());
        qDeleteAll(m_renderDecoders);
        m_renderDecoders.clear();
        m_asyncFrameCache.clear();
        m_renderer.reset();
        m_decoder.reset();
#if JCUT_HAVE_OPENCV
        m_faceCascade = cv::CascadeClassifier();
        m_faceCascadeAlt = cv::CascadeClassifier();
        m_faceCascadeProfile = cv::CascadeClassifier();
        m_tracks.clear();
#endif
        m_timelineFrame = 0;
        m_scanEnd = 0;
        m_stepFrames = 6;
        m_processedFrames = 0;
        m_totalDetections = 0;
        m_vulkanFrames = 0;
        m_fallbackFrames = 0;
        m_nextTrackId = 0;
        m_decodeMs = 0;
        m_textureMs = 0;
        m_compositeMs = 0;
        m_readbackMs = 0;
    }

    QLineEdit* m_videoPath = nullptr;
    QSpinBox* m_maxFrames = nullptr;
    QSpinBox* m_stride = nullptr;
    QPushButton* m_runButton = nullptr;
    QPushButton* m_stopButton = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_preview = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QTimer m_scanTimer;
    QElapsedTimer m_wall;
    bool m_running = false;
    QString m_mediaPath;
    QSize m_outputSize;
    TimelineClip m_clip;
    std::unique_ptr<render_detail::OffscreenVulkanRenderer> m_renderer;
    std::unique_ptr<editor::DecoderContext> m_decoder;
    QHash<QString, editor::DecoderContext*> m_renderDecoders;
    QHash<render_detail::RenderAsyncFrameKey, editor::FrameHandle> m_asyncFrameCache;
    int64_t m_timelineFrame = 0;
    int64_t m_scanEnd = 0;
    int m_stepFrames = 6;
    int m_processedFrames = 0;
    int m_totalDetections = 0;
    int m_vulkanFrames = 0;
    int m_fallbackFrames = 0;
    int m_nextTrackId = 0;
    double m_decodeMs = 0.0;
    double m_textureMs = 0.0;
    double m_compositeMs = 0.0;
    double m_readbackMs = 0.0;
#if JCUT_HAVE_OPENCV
    cv::CascadeClassifier m_faceCascade;
    cv::CascadeClassifier m_faceCascadeAlt;
    cv::CascadeClassifier m_faceCascadeProfile;
    std::vector<ContinuityTrackState> m_tracks;
#endif
};

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    BoxstreamTestWindow window;
    window.show();
    return app.exec();
}
