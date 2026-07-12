#pragma once

#include "editor_shared_core.h"
#include "editor_timeline_types.h"

#include <QColor>
#include <QJsonDocument>
#include <QJsonObject>

struct TranscriptSourceKey {
    QString sourcePath;
    int audioStreamIndex = -1;

    bool isValid() const;
    bool usesAudioStream() const;
    QString canonicalKey() const;
    QString fileStem() const;
    QString displayName() const;
    QJsonObject toJson() const;
};

TranscriptSourceKey transcriptSourceKeyFromClip(const TimelineClip& clip);
TranscriptSourceKey transcriptSourceKeyFromLegacyFilePath(const QString& filePath);
void setTranscriptSourceRootPath(const QString& rootPath);
QString transcriptSourceRootPath();

QString transcriptPathForClipFile(const QString& filePath);
QString transcriptSourceKeyForClip(const TimelineClip& clip);
QString transcriptPathForSource(const TranscriptSourceKey& source);
QString transcriptPathForClip(const TimelineClip& clip);
SpeakerProfile speakerProfileFromJson(const QString& speakerId, const QJsonObject& profileObj);
QJsonObject speakerProfileToJson(const SpeakerProfile& profile, const QJsonObject& base = QJsonObject());
QString transcriptEditablePathForClipFile(const QString& filePath);
QString transcriptEditablePathForSource(const TranscriptSourceKey& source);
QString transcriptEditablePathForClip(const TimelineClip& clip);
QString transcriptWorkingPathForClipFile(const QString& filePath);
QString transcriptWorkingPathForSource(const TranscriptSourceKey& source);
QString transcriptWorkingPathForClip(const TimelineClip& clip);
QStringList transcriptCutPathsForClipFile(const QString& filePath);
QStringList transcriptCutPathsForSource(const TranscriptSourceKey& source);
QStringList transcriptCutPathsForClip(const TimelineClip& clip);
QString activeTranscriptPathForClipFile(const QString& filePath);
QString activeTranscriptPathForSource(const TranscriptSourceKey& source);
QString activeTranscriptPathForClip(const TimelineClip& clip);
bool facedetectionsSidecarExistsForClipFile(const QString& filePath);
bool facedetectionsSidecarExistsForSource(const TranscriptSourceKey& source);
bool facedetectionsSidecarExistsForClip(const TimelineClip& clip);
QString transcriptPathForRuntimeSidecarForClipFile(const QString& filePath,
                                                   const QString& preferredTranscriptPath = QString());
QString transcriptPathForRuntimeSidecarForSource(const TranscriptSourceKey& source,
                                                 const QString& preferredTranscriptPath = QString());
QString transcriptPathForRuntimeSidecarForClip(const TimelineClip& clip,
                                               const QString& preferredTranscriptPath = QString());
void setActiveTranscriptPathForClipFile(const QString& filePath, const QString& transcriptPath);
void setActiveTranscriptPathForSource(const TranscriptSourceKey& source, const QString& transcriptPath);
void setActiveTranscriptPathForClip(const TimelineClip& clip, const QString& transcriptPath);
void clearActiveTranscriptPathForClipFile(const QString& filePath);
void clearActiveTranscriptPathForSource(const TranscriptSourceKey& source);
void clearActiveTranscriptPathForClip(const TimelineClip& clip);
void clearAllActiveTranscriptPaths();
bool ensureEditableTranscriptForClipFile(const QString& filePath, QString* editablePathOut = nullptr);
bool ensureEditableTranscriptForSource(const TranscriptSourceKey& source, QString* editablePathOut = nullptr);
bool ensureEditableTranscriptForClip(const TimelineClip& clip, QString* editablePathOut = nullptr);
bool loadTranscriptJsonCached(const QString& transcriptPath, QJsonDocument* documentOut);
QVector<TranscriptSection> transcriptSectionsFromDocument(const QJsonDocument& document);
std::shared_ptr<const TranscriptRuntimeDocument> loadTranscriptRuntimeDocument(const QString& transcriptPath);
std::shared_ptr<const TranscriptRuntimeDocument> cachedTranscriptRuntimeDocumentMemoryOnly(
    const QString& transcriptPath);
QVector<TranscriptSection> loadTranscriptSections(const QString& transcriptPath);
QPointF transcriptSpeakerLocationForSourceFrame(const QString& transcriptPath,
                                                const QVector<TranscriptSection>& sections,
                                                int64_t sourceFrame,
                                                bool* okOut = nullptr);
bool transcriptSpeakerTrackingSampleForClipFileAtSourceFrame(const QString& clipFilePath,
                                                             const QString& speakerId,
                                                             int64_t sourceFrame,
                                                             qreal minConfidence,
                                                             QPointF* locationOut,
                                                             qreal* boxSizeOut);
bool transcriptSpeakerTrackingSampleForClipFileAtSourceFrameMemoryOnly(
    const QString& clipFilePath,
    const QString& speakerId,
    int64_t sourceFrame,
    qreal minConfidence,
    QPointF* locationOut,
    qreal* boxSizeOut);
QString transcriptActiveSpeakerForClipFileAtSourceFrame(const QString& clipFilePath,
                                                        int64_t sourceFrame);
QString transcriptActiveSpeakerForClipFileAtSourceFrameMemoryOnly(const QString& clipFilePath,
                                                                  int64_t sourceFrame);
bool transcriptSpeakerGradingForClipFileAtSourceFrame(const QString& clipFilePath,
                                                      int64_t sourceFrame,
                                                      TimelineClip::GradingKeyframe* gradingOut);
bool transcriptActiveSpeakerTrackingSampleForClipFileAtSourceFrame(const QString& clipFilePath,
                                                                   int64_t sourceFrame,
                                                                   qreal minConfidence,
                                                                   QPointF* locationOut,
                                                                   qreal* boxSizeOut,
                                                                   QString* speakerIdOut = nullptr);
void invalidateTranscriptSpeakerProfileCache(const QString& transcriptPath = QString());
void invalidateTranscriptJsonCache(const QString& transcriptPath = QString());
QJsonObject transcriptSpeakerTrackingConfigSnapshot();
bool applyTranscriptSpeakerTrackingConfigPatch(const QJsonObject& patch,
                                               QString* errorOut = nullptr);
QJsonObject transcriptSpeakerTrackingProfilingSnapshot();
void resetTranscriptSpeakerTrackingProfiling();
struct TranscriptOverlayTiming {
    int prependMs = 150;
    int postpendMs = 70;
    int offsetMs = 0;
};
ExportRangeSegment transcriptPaddedWordRange(const TranscriptWord& word,
                                             int prependMs,
                                             int postpendMs,
                                             int offsetMs = 0);
QVector<ExportRangeSegment> transcriptPaddedWordRanges(const QVector<TranscriptSection>& sections,
                                                       int prependMs,
                                                       int postpendMs,
                                                       int offsetMs = 0);
QPointF transcriptOverlayTranslationForOutput(const TimelineClip& clip,
                                              const QSize& outputSize,
                                              const QString& transcriptPath,
                                              const QVector<TranscriptSection>& sections,
                                              int64_t sourceFrame);
QRectF transcriptOverlayRectInOutputSpace(const TimelineClip& clip,
                                          const QSize& outputSize,
                                          const QString& transcriptPath,
                                          const QVector<TranscriptSection>& sections,
                                          int64_t sourceFrame);
int transcriptOverlayEffectiveLinesForBox(const TimelineClip& clip);
int transcriptOverlayEffectiveCharsForBox(const TimelineClip& clip);
TranscriptOverlayLayout transcriptOverlayLayoutAtSourceFrame(const TimelineClip& clip,
                                                             const QVector<TranscriptSection>& sections,
                                                             int64_t sourceFrame,
                                                             const TranscriptOverlayTiming& timing = {});
QString transcriptOverlaySpeakerAtSourceFrame(const QVector<TranscriptSection>& sections,
                                              int64_t sourceFrame,
                                              ExportRangeSegment* activeRangeOut = nullptr,
                                              const TranscriptOverlayTiming& timing = {});
QString wrappedTranscriptSectionText(const QString& text, int maxCharsPerLine, int maxLines);
TranscriptOverlayLayout layoutTranscriptSection(const TranscriptSection& section,
                                                int64_t sourceFrame,
                                                int maxCharsPerLine,
                                                int maxLines,
                                                bool autoScroll);
QString transcriptOverlayHtml(const TranscriptOverlayLayout& layout,
                              const QColor& textColor,
                              const QColor& highlightTextColor,
                              const QColor& highlightFillColor,
                              bool highlightCurrentWord = true);
QString transcriptSpeakerTitleForSourceFrame(const QString& transcriptPath,
                                             const QVector<TranscriptSection>& sections,
                                             int64_t sourceFrame,
                                             const TranscriptOverlayTiming& timing = {});
QString transcriptSpeakerSentenceForSourceFrame(const TranscriptRuntimeDocument& runtimeDocument,
                                                const QString& speakerId,
                                                int64_t sourceFrame);
SpeakerProfile transcriptSpeakerProfileForSourceFrame(const QString& transcriptPath,
                                                      const QVector<TranscriptSection>& sections,
                                                      int64_t sourceFrame,
                                                      const TranscriptOverlayTiming& timing = {});
