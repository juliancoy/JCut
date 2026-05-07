#include "decoder_context.h"
#include "debug_controls.h"
#include "editor_shared.h"
#include "transcript_engine.h"
#include "vulkan_preview_surface.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>

namespace {

constexpr const char* kHarnessClipId = "boxstream-offscreen-source";

void appendLog(QPlainTextEdit* log, const QString& line)
{
    if (!log || line.isEmpty()) {
        return;
    }
    log->appendPlainText(line.trimmed());
}

QJsonDocument buildHarnessTranscript(double durationSeconds)
{
    const double boundedDuration = qMax(1.0, durationSeconds);
    QJsonObject word{
        {QStringLiteral("word"), QStringLiteral("facestream")},
        {QStringLiteral("start"), 0.0},
        {QStringLiteral("end"), boundedDuration},
        {QStringLiteral("speaker"), QStringLiteral("SPEAKER_00")}
    };
    QJsonObject segment{
        {QStringLiteral("start"), 0.0},
        {QStringLiteral("end"), boundedDuration},
        {QStringLiteral("speaker"), QStringLiteral("SPEAKER_00")},
        {QStringLiteral("text"), QStringLiteral("facestream")},
        {QStringLiteral("words"), QJsonArray{word}}
    };
    return QJsonDocument(QJsonObject{
        {QStringLiteral("segments"), QJsonArray{segment}}
    });
}

bool ensureHarnessTranscript(const QString& videoPath, double durationSeconds, QString* transcriptPathOut, QString* errorOut)
{
    const QString editablePath = transcriptEditablePathForClipFile(videoPath);
    if (transcriptPathOut) {
        *transcriptPathOut = editablePath;
    }
    if (QFileInfo::exists(editablePath)) {
        setActiveTranscriptPathForClipFile(videoPath, editablePath);
        return true;
    }
    const QString originalPath = transcriptPathForClipFile(videoPath);
    const QFileInfo editableInfo(editablePath);
    if (!QDir().mkpath(editableInfo.absolutePath())) {
        if (errorOut) *errorOut = QStringLiteral("Failed to create transcript directory: %1").arg(editableInfo.absolutePath());
        return false;
    }
    if (QFileInfo::exists(originalPath)) {
        QFile::remove(editablePath);
        if (!QFile::copy(originalPath, editablePath)) {
            if (errorOut) *errorOut = QStringLiteral("Failed to create editable transcript copy: %1").arg(editablePath);
            return false;
        }
    } else {
        editor::TranscriptEngine engine;
        if (!engine.saveTranscriptJson(editablePath, buildHarnessTranscript(durationSeconds))) {
            if (errorOut) *errorOut = QStringLiteral("Failed to write harness transcript: %1").arg(editablePath);
            return false;
        }
    }
    setActiveTranscriptPathForClipFile(videoPath, editablePath);
    return true;
}

TimelineClip buildClip(const QString& videoPath, const editor::DecoderContext& decoder)
{
    TimelineClip clip;
    clip.id = QString::fromLatin1(kHarnessClipId);
    clip.filePath = QFileInfo(videoPath).absoluteFilePath();
    clip.label = QFileInfo(videoPath).fileName();
    clip.mediaType = ClipMediaType::Video;
    clip.sourceKind = MediaSourceKind::File;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.startFrame = 0;
    clip.sourceInFrame = 0;
    clip.sourceFps = decoder.info().fps > 0.0 ? decoder.info().fps : 30.0;
    clip.sourceDurationFrames = qMax<int64_t>(1, decoder.info().durationFrames);
    clip.durationFrames = clip.sourceDurationFrames;
    clip.playbackRate = 1.0;
    clip.trackIndex = 0;
    clip.opacity = 1.0;
    clip.color = QColor(QStringLiteral("#36d399"));
    normalizeClipTransformKeyframes(clip);
    normalizeClipGradingKeyframes(clip);
    normalizeClipOpacityKeyframes(clip);
    return clip;
}

int64_t firstBoxstreamFrame(const QJsonObject& artifactRoot, const QString& clipId)
{
    int64_t first = std::numeric_limits<int64_t>::max();
    const QJsonObject byClip = artifactRoot.value(QStringLiteral("continuity_boxstreams_by_clip")).toObject();
    const QJsonArray streams = byClip.value(clipId).toObject().value(QStringLiteral("streams")).toArray();
    for (const QJsonValue& streamValue : streams) {
        const QJsonArray keyframes = streamValue.toObject().value(QStringLiteral("keyframes")).toArray();
        for (const QJsonValue& keyframeValue : keyframes) {
            const QJsonObject keyframe = keyframeValue.toObject();
            if (keyframe.contains(QStringLiteral("frame"))) {
                first = std::min<int64_t>(first, keyframe.value(QStringLiteral("frame")).toVariant().toLongLong());
            }
        }
    }
    return first == std::numeric_limits<int64_t>::max() ? -1 : first;
}

int boxCount(const QJsonObject& artifactRoot, const QString& clipId)
{
    int count = 0;
    const QJsonObject byClip = artifactRoot.value(QStringLiteral("continuity_boxstreams_by_clip")).toObject();
    const QJsonArray streams = byClip.value(clipId).toObject().value(QStringLiteral("streams")).toArray();
    for (const QJsonValue& streamValue : streams) {
        count += streamValue.toObject().value(QStringLiteral("keyframes")).toArray().size();
    }
    return count;
}

class BoxstreamPreviewHarness final : public QWidget {
public:
    BoxstreamPreviewHarness(QString videoPath,
                            QString transcriptPath,
                            TimelineClip clip,
                            QSize frameSize,
                            bool autoRun,
                            QWidget* parent = nullptr)
        : QWidget(parent),
          m_videoPath(std::move(videoPath)),
          m_transcriptPath(std::move(transcriptPath)),
          m_clip(std::move(clip)),
          m_autoRun(autoRun)
    {
        setWindowTitle(QStringLiteral("JCut DNN FaceStream Generator"));
        resize(1200, 780);
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(10, 10, 10, 10);
        root->setSpacing(8);

        m_status = new QLabel(QStringLiteral("Initializing Vulkan preview..."), this);
        root->addWidget(m_status);

        m_preview = std::make_unique<VulkanPreviewSurface>(this);
        if (QWidget* previewWidget = m_preview->asWidget()) {
            root->addWidget(previewWidget, 1);
        }

        m_runButton = new QPushButton(QStringLiteral("Run JCut DNN FaceStream Generator"), this);
        root->addWidget(m_runButton);
        m_log = new QPlainTextEdit(this);
        m_log->setReadOnly(true);
        m_log->setMaximumBlockCount(2000);
        root->addWidget(m_log, 1);

        TimelineTrack track;
        track.name = QStringLiteral("Video");
        track.height = 120;
        m_tracks = QVector<TimelineTrack>{track};

        m_preview->beginBulkUpdate();
        m_preview->setOutputSize(frameSize.isValid() ? frameSize : QSize(1920, 1080));
        m_preview->setTimelineTracks(m_tracks);
        m_preview->setTimelineClips(QVector<TimelineClip>{m_clip});
        m_preview->setSelectedClipId(m_clip.id);
        m_preview->setShowSpeakerTrackBoxes(true);
        m_preview->setBoxstreamOverlaySource(QStringLiteral("all"));
        m_preview->setCurrentFrame(0);
        m_preview->setCurrentPlaybackSample(0);
        m_preview->endBulkUpdate();

        connect(m_runButton, &QPushButton::clicked, this, [this]() { runGenerator(); });
        connect(&m_tickTimer, &QTimer::timeout, this, [this]() { advancePreview(); });
        m_tickTimer.setInterval(33);
        m_tickTimer.start();

        m_status->setText(QStringLiteral("Direct Vulkan preview active. Boxes come only from generated FaceStream continuity artifacts."));
        appendLog(m_log, QStringLiteral("video=%1").arg(m_videoPath));
        appendLog(m_log, QStringLiteral("transcript=%1").arg(m_transcriptPath));
        if (m_autoRun) {
            QTimer::singleShot(250, this, [this]() { runGenerator(); });
        }
    }

private:
    void setPreviewFrame(int64_t frame)
    {
        const int64_t bounded = qBound<int64_t>(0, frame, qMax<int64_t>(0, m_clip.durationFrames - 1));
        m_currentFrame = bounded;
        m_preview->setCurrentFrame(bounded);
        m_preview->setCurrentPlaybackSample(frameToSamples(bounded));
    }

    void advancePreview()
    {
        if (m_holdOnBoxes) {
            m_preview->setCurrentFrame(m_currentFrame);
            m_preview->setCurrentPlaybackSample(frameToSamples(m_currentFrame));
            return;
        }
        setPreviewFrame(m_currentFrame + 1);
    }

    void runGenerator()
    {
        if (m_process) {
            return;
        }
        m_runButton->setEnabled(false);
        m_status->setText(QStringLiteral("Running JCut DNN FaceStream Generator..."));
        appendLog(m_log, QStringLiteral("starting JCut DNN FaceStream Generator"));

        const QString program = QCoreApplication::applicationDirPath() + QStringLiteral("/jcut_vulkan_boxstream_offscreen");
        const QString outDir = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("vulkan_boxstream_preview_artifacts"));
        QDir().mkpath(outDir);
        m_process = std::make_unique<QProcess>(this);
        m_process->setProgram(program);
        m_process->setArguments(QStringList{
            m_videoPath,
            QStringLiteral("--out-dir"), outDir,
            QStringLiteral("--detector"), QStringLiteral("jcut-dnn"),
            QStringLiteral("--decode"), QStringLiteral("hardware_zero_copy"),
            QStringLiteral("--no-preview-window")
        });
        m_process->setProcessChannelMode(QProcess::MergedChannels);
        connect(m_process.get(), &QProcess::readyReadStandardOutput, this, [this]() {
            appendLog(m_log, QString::fromLocal8Bit(m_process->readAllStandardOutput()));
        });
        connect(m_process.get(), qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, outDir](int exitCode, QProcess::ExitStatus status) {
            appendLog(m_log, QStringLiteral("generator finished exit=%1 status=%2").arg(exitCode).arg(static_cast<int>(status)));
            importGeneratedArtifact(outDir);
            m_process.reset();
            m_runButton->setEnabled(true);
        });
        m_process->start();
    }

    void importGeneratedArtifact(const QString& outDir)
    {
        const QString artifactPath = QDir(outDir).filePath(QStringLiteral("continuity_boxstream.json"));
        QFile file(artifactPath);
        if (!file.open(QIODevice::ReadOnly)) {
            m_status->setText(QStringLiteral("Generator finished, but no continuity artifact was produced."));
            return;
        }
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            m_status->setText(QStringLiteral("Generated continuity artifact is invalid JSON."));
            return;
        }
        QJsonObject root = doc.object();
        QJsonObject byClip = root.value(QStringLiteral("continuity_boxstreams_by_clip")).toObject();
        if (!byClip.contains(m_clip.id) && byClip.size() == 1) {
            const QJsonObject value = byClip.begin().value().toObject();
            byClip = QJsonObject{{m_clip.id, value}};
            root[QStringLiteral("continuity_boxstreams_by_clip")] = byClip;
        }
        editor::TranscriptEngine engine;
        if (!engine.saveBoxstreamArtifact(m_transcriptPath, root)) {
            m_status->setText(QStringLiteral("Failed to save generated FaceStream artifact beside transcript."));
            return;
        }
        m_preview->invalidateTranscriptOverlayCache(m_clip.filePath);
        const int boxes = boxCount(root, m_clip.id);
        const int64_t firstFrame = firstBoxstreamFrame(root, m_clip.id);
        if (firstFrame >= 0) {
            setPreviewFrame(firstFrame);
            m_holdOnBoxes = true;
        }
        m_status->setText(QStringLiteral("Generated artifact imported. boxes=%1 first_frame=%2").arg(boxes).arg(firstFrame));
    }

    QString m_videoPath;
    QString m_transcriptPath;
    TimelineClip m_clip;
    QVector<TimelineTrack> m_tracks;
    std::unique_ptr<VulkanPreviewSurface> m_preview;
    std::unique_ptr<QProcess> m_process;
    QLabel* m_status = nullptr;
    QPushButton* m_runButton = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QTimer m_tickTimer;
    int64_t m_currentFrame = 0;
    bool m_holdOnBoxes = false;
    bool m_autoRun = true;
};

} // namespace

int main(int argc, char** argv)
{
    qputenv("JCUT_RENDER_BACKEND", QByteArrayLiteral("vulkan"));
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("JCut DNN FaceStream Generator"));
    qRegisterMetaType<editor::FrameHandle>();

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Standalone JCut DNN FaceStream Generator preview harness."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("video"), QStringLiteral("Input video path."));
    QCommandLineOption noAutoRunOption(QStringLiteral("no-auto-run"), QStringLiteral("Open preview without starting generation."));
    QCommandLineOption debugDecodeOption(QStringLiteral("debug-decode"), QStringLiteral("Enable decode debug logging."));
    parser.addOption(noAutoRunOption);
    parser.addOption(debugDecodeOption);
    parser.process(app);

    if (parser.isSet(debugDecodeOption)) {
        editor::setDebugDecodeEnabled(true);
    }
    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) {
        std::fprintf(stderr, "Missing input video path.\n");
        parser.showHelp(2);
    }

    const QString videoPath = QFileInfo(args.first()).absoluteFilePath();
    editor::DecoderContext decoder(videoPath);
    if (!decoder.initialize()) {
        std::fprintf(stderr, "Failed to initialize decoder: %s\n", videoPath.toLocal8Bit().constData());
        return 2;
    }

    QString transcriptPath;
    QString error;
    const double durationSeconds = decoder.info().durationFrames > 0 && decoder.info().fps > 0.0
        ? static_cast<double>(decoder.info().durationFrames) / decoder.info().fps
        : 1.0;
    if (!ensureHarnessTranscript(videoPath, durationSeconds, &transcriptPath, &error)) {
        std::fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
        return 2;
    }

    BoxstreamPreviewHarness window(videoPath,
                                   transcriptPath,
                                   buildClip(videoPath, decoder),
                                   decoder.info().frameSize,
                                   !parser.isSet(noAutoRunOption));
    window.show();
    return app.exec();
}
