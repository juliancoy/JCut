#include "editor.h"
#include "transcript_engine.h"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <limits>

using namespace editor;

namespace {

void setError(QString* errorOut, const QString& error)
{
    if (errorOut) {
        *errorOut = error;
    }
}

QJsonDocument buildHarnessTranscript(double durationSeconds)
{
    const double boundedDuration = qMax(1.0, durationSeconds);

    QJsonObject word{
        {QStringLiteral("word"), QStringLiteral("boxstream")},
        {QStringLiteral("start"), 0.0},
        {QStringLiteral("end"), boundedDuration},
        {QStringLiteral("speaker"), QStringLiteral("SPEAKER_00")}
    };

    QJsonObject segment{
        {QStringLiteral("start"), 0.0},
        {QStringLiteral("end"), boundedDuration},
        {QStringLiteral("speaker"), QStringLiteral("SPEAKER_00")},
        {QStringLiteral("text"), QStringLiteral("boxstream")},
        {QStringLiteral("words"), QJsonArray{word}}
    };

    QJsonObject speakerProfile{
        {QStringLiteral("name"), QStringLiteral("SPEAKER_00")},
        {QStringLiteral("location"), QJsonObject{
            {QStringLiteral("x"), 0.5},
            {QStringLiteral("y"), 0.85}
        }}
    };

    return QJsonDocument(QJsonObject{
        {QStringLiteral("segments"), QJsonArray{segment}},
        {QStringLiteral("speaker_profiles"), QJsonObject{
            {QStringLiteral("SPEAKER_00"), speakerProfile}
        }}
    });
}

bool ensureEditableTranscriptForHarness(const TimelineClip& clip,
                                        bool createHarnessTranscript,
                                        QString* errorOut)
{
    QString editablePath;
    if (ensureEditableTranscriptForClipFile(clip.filePath, &editablePath)) {
        setActiveTranscriptPathForClipFile(clip.filePath, editablePath);
        return true;
    }

    if (!createHarnessTranscript) {
        setError(errorOut,
                 QStringLiteral("No transcript exists for %1. Re-run with transcript creation enabled or create the app transcript first.")
                     .arg(clip.filePath));
        return false;
    }

    const QFileInfo editableInfo(editablePath);
    if (!QDir().mkpath(editableInfo.absolutePath())) {
        setError(errorOut,
                 QStringLiteral("Failed to create transcript directory: %1")
                     .arg(editableInfo.absolutePath()));
        return false;
    }

    const double durationSeconds =
        qMax(1.0, static_cast<double>(qMax<int64_t>(1, clip.durationFrames)) /
                      qMax(1.0, clip.sourceFps));
    const QJsonDocument doc = buildHarnessTranscript(durationSeconds);
    TranscriptEngine engine;
    if (!engine.saveTranscriptJson(editablePath, doc)) {
        setError(errorOut,
                 QStringLiteral("Failed to write harness transcript: %1")
                     .arg(editablePath));
        return false;
    }

    setActiveTranscriptPathForClipFile(clip.filePath, editablePath);
    return true;
}

int64_t firstBoxstreamFrameForClip(const QString& transcriptPath, const QString& clipId)
{
    TranscriptEngine engine;
    QJsonObject artifactRoot;
    if (!engine.loadBoxstreamArtifact(transcriptPath, &artifactRoot)) {
        return -1;
    }

    int64_t firstFrame = std::numeric_limits<int64_t>::max();
    const QJsonObject byClip = artifactRoot.value(QStringLiteral("continuity_boxstreams_by_clip")).toObject();
    const QJsonObject continuityRoot = byClip.value(clipId.trimmed()).toObject();
    const QJsonArray streams = continuityRoot.value(QStringLiteral("streams")).toArray();
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject stream = streamValue.toObject();
        const QJsonArray keyframes = stream.value(QStringLiteral("keyframes")).toArray();
        for (const QJsonValue& value : keyframes) {
            const QJsonObject keyframe = value.toObject();
            if (keyframe.contains(QStringLiteral("frame"))) {
                firstFrame = std::min<int64_t>(
                    firstFrame,
                    keyframe.value(QStringLiteral("frame")).toVariant().toLongLong());
            }
        }
    }
    return firstFrame == std::numeric_limits<int64_t>::max() ? -1 : firstFrame;
}

} // namespace

bool EditorWindow::prepareVulkanBoxStreamPreviewRun(const QString& filePath,
                                                    bool createHarnessTranscript,
                                                    QString* errorOut)
{
    const QFileInfo info(filePath);
    if (!info.exists() || !info.isFile()) {
        setError(errorOut, QStringLiteral("Input video does not exist: %1").arg(filePath));
        return false;
    }
    if (!m_timeline || !m_preview || !m_speakersTab) {
        setError(errorOut, QStringLiteral("Editor preview/timeline components are not initialized."));
        return false;
    }

    qputenv("JCUT_RENDER_BACKEND", QByteArrayLiteral("vulkan"));
    m_renderBackendPreference = QStringLiteral("vulkan");
    if (m_renderBackendCombo) {
        const int backendIndex = m_renderBackendCombo->findData(QStringLiteral("vulkan"));
        if (backendIndex >= 0) {
            m_renderBackendCombo->setCurrentIndex(backendIndex);
        }
    }
    m_preview->setRenderBackendPreference(QStringLiteral("vulkan"));

    const int beforeCount = m_timeline->clips().size();
    addFileToTimeline(info.absoluteFilePath(), 0);
    if (m_timeline->clips().size() <= beforeCount) {
        setError(errorOut, QStringLiteral("Failed to add video to timeline: %1").arg(filePath));
        return false;
    }

    const TimelineClip clip = m_timeline->clips().last();
    m_timeline->setSelectedClipId(clip.id);
    m_timeline->setCurrentFrame(clip.startFrame);
    m_preview->beginBulkUpdate();
    m_preview->setTimelineTracks(m_timeline->tracks());
    m_preview->setTimelineClips(m_timeline->clips());
    m_preview->setSelectedClipId(clip.id);
    m_preview->setShowSpeakerTrackBoxes(true);
    m_preview->setBoxstreamOverlaySource(QStringLiteral("all"));
    m_preview->endBulkUpdate();

    if (!ensureEditableTranscriptForHarness(clip, createHarnessTranscript, errorOut)) {
        return false;
    }

    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
    const int64_t firstBoxstreamFrame = firstBoxstreamFrameForClip(transcriptPath, clip.id);
    if (firstBoxstreamFrame >= 0) {
        setCurrentFrame(clip.startFrame + firstBoxstreamFrame, false);
    } else {
        setCurrentFrame(clip.startFrame, false);
    }

    if (m_inspectorTabs) {
        for (int i = 0; i < m_inspectorTabs->count(); ++i) {
            if (m_inspectorTabs->tabText(i).compare(QStringLiteral("Speakers"), Qt::CaseInsensitive) == 0) {
                m_inspectorTabs->setCurrentIndex(i);
                break;
            }
        }
    }
    m_speakersTab->refresh();
    refreshClipInspector();
    updateTransportLabels();
    return true;
}

bool EditorWindow::triggerGenerateBoxStreamForSelectedClip(QString* errorOut)
{
    if (!m_timeline || !m_speakersTab || !m_timeline->selectedClip()) {
        setError(errorOut, QStringLiteral("No selected clip is available for Generate FaceStream."));
        return false;
    }

    m_speakersTab->refresh();
    const bool started = m_speakersTab->generateBoxStreamForSelectedClip();
    if (!started) {
        setError(errorOut, QStringLiteral("Generate FaceStream did not start."));
        return false;
    }
    if (m_preview && m_timeline->selectedClip()) {
        m_preview->invalidateTranscriptOverlayCache(m_timeline->selectedClip()->filePath);
        m_preview->setShowSpeakerTrackBoxes(true);
        m_preview->setBoxstreamOverlaySource(QStringLiteral("all"));
        const int64_t firstBoxstreamFrame =
            firstBoxstreamFrameForClip(activeTranscriptPathForClipFile(m_timeline->selectedClip()->filePath),
                                       m_timeline->selectedClip()->id);
        if (firstBoxstreamFrame >= 0) {
            setCurrentFrame(m_timeline->selectedClip()->startFrame + firstBoxstreamFrame, false);
        }
    }
    if (m_inspectorPane) {
        m_inspectorPane->refresh();
    }
    return true;
}
