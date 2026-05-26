#pragma once

#include "editor_shared_core.h"

#include <QColor>
#include <QJsonDocument>
#include <QJsonObject>

QString transcriptPathForClipFile(const QString& filePath);
SpeakerProfile speakerProfileFromJson(const QString& speakerId, const QJsonObject& profileObj);
QJsonObject speakerProfileToJson(const SpeakerProfile& profile, const QJsonObject& base = QJsonObject());
QString transcriptEditablePathForClipFile(const QString& filePath);
QString transcriptWorkingPathForClipFile(const QString& filePath);
QStringList transcriptCutPathsForClipFile(const QString& filePath);
QString activeTranscriptPathForClipFile(const QString& filePath);
bool facedetectionsSidecarExistsForClipFile(const QString& filePath);
QString transcriptPathForRuntimeSidecarForClipFile(const QString& filePath,
                                                   const QString& preferredTranscriptPath = QString());
void setActiveTranscriptPathForClipFile(const QString& filePath, const QString& transcriptPath);
void clearActiveTranscriptPathForClipFile(const QString& filePath);
void clearAllActiveTranscriptPaths();
bool ensureEditableTranscriptForClipFile(const QString& filePath, QString* editablePathOut = nullptr);
bool loadTranscriptJsonCached(const QString& transcriptPath, QJsonDocument* documentOut);
std::shared_ptr<const TranscriptRuntimeDocument> loadTranscriptRuntimeDocument(const QString& transcriptPath);
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
void invalidateTranscriptSpeakerProfileCache(const QString& transcriptPath = QString());
void invalidateTranscriptJsonCache(const QString& transcriptPath = QString());
QJsonObject transcriptSpeakerTrackingConfigSnapshot();
bool applyTranscriptSpeakerTrackingConfigPatch(const QJsonObject& patch,
                                               QString* errorOut = nullptr);
QJsonObject transcriptSpeakerTrackingProfilingSnapshot();
void resetTranscriptSpeakerTrackingProfiling();
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
                                                             int64_t sourceFrame);
QString wrappedTranscriptSectionText(const QString& text, int maxCharsPerLine, int maxLines);
TranscriptOverlayLayout layoutTranscriptSection(const TranscriptSection& section,
                                                int64_t sourceFrame,
                                                int maxCharsPerLine,
                                                int maxLines,
                                                bool autoScroll);
QString transcriptOverlayHtml(const TranscriptOverlayLayout& layout,
                              const QColor& textColor,
                              const QColor& highlightTextColor,
                              const QColor& highlightFillColor);
QString transcriptSpeakerTitleForSourceFrame(const QString& transcriptPath,
                                             const QVector<TranscriptSection>& sections,
                                             int64_t sourceFrame);
QString transcriptSpeakerSentenceForSourceFrame(const TranscriptRuntimeDocument& runtimeDocument,
                                                const QString& speakerId,
                                                int64_t sourceFrame);
SpeakerProfile transcriptSpeakerProfileForSourceFrame(const QString& transcriptPath,
                                                      const QVector<TranscriptSection>& sections,
                                                      int64_t sourceFrame);
