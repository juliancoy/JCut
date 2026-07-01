#include "transcript_tab.h"

#include "clip_serialization.h"
#include "editor_shared.h"
#include "editor_tab_edit_effects.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QColor>
#include <QColorDialog>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
TabEditCallbacks transcriptEditCallbacks(const TranscriptTab::Dependencies& deps) {
    return TabEditCallbacks{
        .updatePreview = deps.setPreviewTimelineClips,
        .refreshInspector = deps.refreshInspector,
        .scheduleSave = deps.scheduleSaveState,
        .pushHistory = deps.pushHistorySnapshot,
    };
}

const QLatin1String kTranscriptWordSkippedKey("skipped");
const QLatin1String kTranscriptWordSpeakerKey("speaker");
const QLatin1String kTranscriptSegmentSpeakerKey("speaker");
const QLatin1String kTranscriptWordEditsKey("transcript_edits");
const QLatin1String kTranscriptDeletedEditsCountKey("transcript_deleted_edits");
const QLatin1String kTranscriptEditTimingTag("timing");
const QLatin1String kTranscriptEditTextTag("text");
const QLatin1String kTranscriptEditSkipTag("skip");
const QLatin1String kTranscriptEditInsertedTag("inserted");
const QLatin1String kTranscriptWordRenderOrderKey("render_order");
const QLatin1String kTranscriptWordOriginalSegmentKey("original_segment_index");
const QLatin1String kTranscriptWordOriginalWordKey("original_word_index");
const QLatin1String kTranscriptSpeakerProfilesKey("speaker_profiles");
const QLatin1String kTranscriptSpeakerNameKey("name");
const QLatin1String kAllSpeakersFilterValue("__all__");
constexpr int kTranscriptTableRowHeight = 30;

enum TranscriptTableColumn {
    kTranscriptColSourceStart = 0,
    kTranscriptColSourceEnd = 1,
    kTranscriptColSpeaker = 2,
    kTranscriptColText = 3,
    kTranscriptColEdits = 4
};

constexpr int kEditFlagNone = 0;
constexpr int kEditFlagTiming = 1 << 0;
constexpr int kEditFlagText = 1 << 1;
constexpr int kEditFlagSkip = 1 << 2;
constexpr int kEditFlagInserted = 1 << 3;

int transcriptEditFlagsFromWordObject(const QJsonObject& word)
{
    int flags = kEditFlagNone;
    const QJsonArray editArray = word.value(QString(kTranscriptWordEditsKey)).toArray();
    for (const QJsonValue& value : editArray) {
        const QString tag = value.toString();
        if (tag == QString(kTranscriptEditTimingTag)) {
            flags |= kEditFlagTiming;
        } else if (tag == QString(kTranscriptEditTextTag)) {
            flags |= kEditFlagText;
        } else if (tag == QString(kTranscriptEditSkipTag)) {
            flags |= kEditFlagSkip;
        } else if (tag == QString(kTranscriptEditInsertedTag)) {
            flags |= kEditFlagInserted;
        }
    }
    return flags;
}

void transcriptAppendEditTag(QJsonObject* word, const QString& tag)
{
    if (!word) {
        return;
    }
    QJsonArray editArray = word->value(QString(kTranscriptWordEditsKey)).toArray();
    for (const QJsonValue& value : editArray) {
        if (value.toString() == tag) {
            return;
        }
    }
    editArray.push_back(tag);
    (*word)[QString(kTranscriptWordEditsKey)] = editArray;
}

bool clipSupportsTranscript(const TimelineClip& clip) {
    return clip.mediaType == ClipMediaType::Audio || clip.hasAudio;
}

QString transcriptCutStateLabel(bool mutableCut)
{
    return mutableCut ? QStringLiteral("Editable Cut") : QStringLiteral("Original (Immutable)");
}

QString transcriptFollowStateLabel(bool enabled)
{
    return enabled ? QStringLiteral("Follow Playback: On") : QStringLiteral("Follow Playback: Off");
}

QString normalizedTranscriptSearchText(const QString& value)
{
    QString normalized;
    normalized.reserve(value.size());
    for (const QChar ch : value) {
        if (ch.isLetterOrNumber()) {
            normalized.append(ch.toLower());
        }
    }
    return normalized;
}

bool transcriptContainsFuzzySubsequence(const QString& query, const QString& candidate)
{
    if (query.isEmpty()) {
        return true;
    }
    int queryIndex = 0;
    for (const QChar ch : candidate) {
        if (ch == query.at(queryIndex)) {
            ++queryIndex;
            if (queryIndex == query.size()) {
                return true;
            }
        }
    }
    return false;
}

int transcriptEditDistance(const QString& a, const QString& b)
{
    if (a.isEmpty()) {
        return b.size();
    }
    if (b.isEmpty()) {
        return a.size();
    }

    QVector<int> previous(b.size() + 1);
    QVector<int> current(b.size() + 1);
    for (int j = 0; j <= b.size(); ++j) {
        previous[j] = j;
    }
    for (int i = 1; i <= a.size(); ++i) {
        current[0] = i;
        for (int j = 1; j <= b.size(); ++j) {
            const int substitutionCost = a.at(i - 1) == b.at(j - 1) ? 0 : 1;
            current[j] = std::min({previous[j] + 1,
                                   current[j - 1] + 1,
                                   previous[j - 1] + substitutionCost});
        }
        previous.swap(current);
    }
    return previous[b.size()];
}

int transcriptFuzzySearchScore(const QString& rawQuery, const QString& rawCandidate)
{
    const QString query = normalizedTranscriptSearchText(rawQuery);
    const QString candidate = normalizedTranscriptSearchText(rawCandidate);
    if (query.isEmpty() || candidate.isEmpty()) {
        return query.isEmpty() ? 0 : std::numeric_limits<int>::max();
    }
    if (candidate == query) {
        return 0;
    }
    if (candidate.startsWith(query)) {
        return 10 + (candidate.size() - query.size());
    }
    if (candidate.contains(query)) {
        return 25 + (candidate.size() - query.size());
    }

    const int editDistance = transcriptEditDistance(query, candidate);
    const int maxLength = qMax(query.size(), candidate.size());
    const int maxAllowedDistance = qMax(1, maxLength / 3);
    int bestScore = std::numeric_limits<int>::max();
    if (editDistance <= maxAllowedDistance) {
        bestScore = 50 + editDistance * 8 + qAbs(candidate.size() - query.size());
    }
    if (transcriptContainsFuzzySubsequence(query, candidate)) {
        bestScore = qMin(bestScore, 80 + candidate.size() - query.size());
    }
    return bestScore;
}

QString transcriptExportSpeakerLabel(const QJsonObject& profiles, const QString& speakerId)
{
    const QString trimmedSpeakerId = speakerId.trimmed();
    const QJsonObject profile = profiles.value(trimmedSpeakerId).toObject();
    const QString name = profile.value(QString(kTranscriptSpeakerNameKey)).toString().trimmed();
    const QString organization = profile.value(QStringLiteral("organization")).toString().trimmed();
    QString label = name.isEmpty() ? trimmedSpeakerId : name;
    if (!organization.isEmpty()) {
        label += QStringLiteral(" - ") + organization;
    }
    return label.isEmpty() ? QStringLiteral("Unknown Speaker") : label;
}

QString transcriptExportSpeakerDisplayName(const QJsonObject& profiles, const QString& speakerId)
{
    const QString trimmedSpeakerId = speakerId.trimmed();
    const QString name =
        profiles.value(trimmedSpeakerId).toObject().value(QString(kTranscriptSpeakerNameKey)).toString().trimmed();
    return name.isEmpty() ? trimmedSpeakerId : name;
}

QString defaultTranscriptExportPath(const QString& transcriptPath)
{
    const QFileInfo info(transcriptPath);
    QString baseName = info.completeBaseName().trimmed();
    if (baseName.isEmpty()) {
        baseName = QStringLiteral("transcript");
    }
    const QString fileName = baseName + QStringLiteral("_export.txt");
    return info.absoluteDir().exists()
        ? info.absoluteDir().filePath(fileName)
        : QDir::home().filePath(fileName);
}

QString sanitizedTranscriptVideoBaseName(QString value, const QString& fallback)
{
    value = value.simplified();
    value.remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")));
    value.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._ -]+")), QStringLiteral(""));
    value.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral("_"));
    value.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    value = value.trimmed();
    while (value.startsWith(QLatin1Char('.')) || value.startsWith(QLatin1Char('_')) ||
           value.startsWith(QLatin1Char('-'))) {
        value.remove(0, 1);
    }
    while (value.endsWith(QLatin1Char('.')) || value.endsWith(QLatin1Char('_')) ||
           value.endsWith(QLatin1Char('-'))) {
        value.chop(1);
    }
    if (value.isEmpty()) {
        value = fallback.trimmed();
    }
    return value.left(80);
}

QString transcriptExportSpeedSuffix(qreal speed)
{
    const qreal normalizedSpeed = std::isfinite(speed) && speed > 0.001 ? speed : 1.0;
    QString value = QString::number(normalizedSpeed, 'f', 3);
    while (value.contains(QLatin1Char('.')) && value.endsWith(QLatin1Char('0'))) {
        value.chop(1);
    }
    if (value.endsWith(QLatin1Char('.'))) {
        value.chop(1);
    }
    return QStringLiteral("_%1x").arg(value);
}

QString transcriptBaseNameWithExportSpeed(const QString& baseName, qreal speed)
{
    static const QRegularExpression speedSuffixPattern(
        QStringLiteral("_[0-9]+(?:\\.[0-9]+)?x$"));
    QString stripped = baseName;
    stripped.remove(speedSuffixPattern);
    stripped = stripped.trimmed();
    return (stripped.isEmpty() ? QStringLiteral("render") : stripped) + transcriptExportSpeedSuffix(speed);
}

QStringList transcriptSectionTrackIds(const QJsonObject& assignment)
{
    QJsonArray entries = assignment.value(QStringLiteral("tracks")).toArray();
    if (entries.isEmpty()) {
        const int trackId = assignment.value(QStringLiteral("track_id")).toInt(-1);
        if (trackId >= 0) {
            entries.push_back(QJsonObject{{QStringLiteral("track_id"), trackId}});
        }
    }

    QStringList ids;
    QSet<QString> seen;
    for (const QJsonValue& value : entries) {
        const int trackId = value.toObject().value(QStringLiteral("track_id")).toInt(-1);
        if (trackId < 0) {
            continue;
        }
        const QString id = QString::number(trackId);
        if (seen.contains(id)) {
            continue;
        }
        seen.insert(id);
        ids.push_back(id);
    }
    return ids;
}

QString transcriptSectionKey(const QString& speakerId, int64_t startFrame, int64_t endFrame)
{
    return QStringLiteral("%1|%2|%3")
        .arg(speakerId.trimmed())
        .arg(startFrame)
        .arg(endFrame);
}

QJsonObject transcriptSectionAssignment(const QJsonObject& transcriptRoot,
                                        const QString& clipId,
                                        const QString& speakerId,
                                        int64_t startFrame,
                                        int64_t endFrame)
{
    if (clipId.trimmed().isEmpty()) {
        return {};
    }
    const QString key = transcriptSectionKey(speakerId, startFrame, endFrame);
    const QJsonObject speakerFlow = transcriptRoot.value(QStringLiteral("speaker_flow")).toObject();
    const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    const QJsonObject clipRoot = clipsRoot.value(clipId.trimmed()).toObject();
    const QJsonObject resolvedCurrent = clipRoot.value(QStringLiteral("resolved_current")).toObject();
    const QJsonArray sectionMap = resolvedCurrent.value(QStringLiteral("section_track_map")).toArray();
    for (const QJsonValue& value : sectionMap) {
        const QJsonObject row = value.toObject();
        if (row.value(QStringLiteral("section_key")).toString() == key) {
            return row;
        }
    }
    return {};
}

QString transcriptSectionVideoFileName(const QJsonObject& transcriptRoot,
                                       const QString& clipId,
                                       const QString& speakerId,
                                       const QString& speakerDisplayName,
                                       int sectionOrdinal,
                                       int64_t startFrame,
                                       int64_t endFrame,
                                       qreal speed,
                                       const QString& outputFormat)
{
    QString speakerName = speakerDisplayName.simplified();
    if (speakerName.isEmpty()) {
        speakerName = speakerId.simplified();
    }
    if (speakerName.isEmpty()) {
        speakerName = QStringLiteral("Speaker");
    }

    const QStringList tracks = transcriptSectionTrackIds(
        transcriptSectionAssignment(transcriptRoot, clipId, speakerId, startFrame, endFrame));
    QString title;
    if (tracks.isEmpty()) {
        title = QStringLiteral("%1 no track").arg(speakerName);
    } else if (tracks.size() == 1) {
        title = QStringLiteral("%1 track %2").arg(speakerName, tracks.constFirst());
    } else {
        title = QStringLiteral("%1 tracks %2").arg(speakerName, tracks.join(QLatin1Char('-')));
    }

    const QString fallbackTitle = sectionOrdinal > 0
        ? QStringLiteral("%1 %2").arg(speakerName).arg(sectionOrdinal)
        : QStringLiteral("%1 section %2 %3").arg(speakerName).arg(startFrame).arg(endFrame);
    const QString base = transcriptBaseNameWithExportSpeed(
        sanitizedTranscriptVideoBaseName(title, fallbackTitle), speed);
    const QString suffix = outputFormat.trimmed().isEmpty() ? QStringLiteral("mp4") : outputFormat.trimmed();
    return QStringLiteral("%1.%2").arg(base, suffix);
}

QString buildContiguousTranscriptTextExport(const QJsonObject& transcriptRoot,
                                            const QString& clipId,
                                            qreal speed,
                                            const QString& outputFormat)
{
    struct ExportSection {
        QString speakerId;
        QStringList words;
        int64_t startFrame = -1;
        int64_t endFrame = -1;
    };

    const QJsonObject profiles = transcriptRoot.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonArray segments = transcriptRoot.value(QStringLiteral("segments")).toArray();
    QVector<ExportSection> sections;
    ExportSection current;
    auto flushCurrent = [&sections, &current]() {
        if (!current.speakerId.trimmed().isEmpty() && !current.words.isEmpty()) {
            sections.push_back(current);
        }
        current = ExportSection{};
    };
    auto appendText = [&current, &flushCurrent](const QString& speakerId, const QString& text) {
        const QString trimmedSpeakerId = speakerId.trimmed();
        const QString simplifiedText = text.simplified();
        if (trimmedSpeakerId.isEmpty() || simplifiedText.isEmpty()) {
            if (trimmedSpeakerId.isEmpty()) {
                flushCurrent();
            }
            return;
        }
        if (current.speakerId != trimmedSpeakerId) {
            flushCurrent();
            current.speakerId = trimmedSpeakerId;
        }
        current.words.push_back(simplifiedText);
    };

    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        if (!words.isEmpty()) {
            for (const QJsonValue& wordValue : words) {
                const QJsonObject wordObj = wordValue.toObject();
                if (wordObj.value(QString(kTranscriptWordSkippedKey)).toBool(false)) {
                    continue;
                }
                QString speakerId = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
                if (speakerId.isEmpty()) {
                    speakerId = segmentSpeaker;
                }
                const QString text =
                    wordObj.value(QStringLiteral("word"))
                        .toString(wordObj.value(QStringLiteral("text")).toString());
                const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
                const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(startSeconds);
                appendText(speakerId, text);
                if (!speakerId.trimmed().isEmpty() && !text.simplified().isEmpty()) {
                    if (startSeconds >= 0.0) {
                        const int64_t startFrame =
                            qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)));
                        if (current.startFrame < 0) {
                            current.startFrame = startFrame;
                        }
                    }
                    if (endSeconds >= 0.0) {
                        current.endFrame = qMax<int64_t>(
                            current.endFrame,
                            qMax<int64_t>(0, static_cast<int64_t>(std::ceil(endSeconds * kTimelineFps))));
                    }
                }
            }
            continue;
        }

        const QString text = segmentObj.value(QStringLiteral("text")).toString();
        appendText(segmentSpeaker, text);
        if (!segmentSpeaker.trimmed().isEmpty() && !text.simplified().isEmpty()) {
            const double startSeconds = segmentObj.value(QStringLiteral("start")).toDouble(-1.0);
            const double endSeconds = segmentObj.value(QStringLiteral("end")).toDouble(startSeconds);
            if (startSeconds >= 0.0) {
                const int64_t startFrame =
                    qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)));
                if (current.startFrame < 0) {
                    current.startFrame = startFrame;
                }
            }
            if (endSeconds >= 0.0) {
                current.endFrame = qMax<int64_t>(
                    current.endFrame,
                    qMax<int64_t>(0, static_cast<int64_t>(std::ceil(endSeconds * kTimelineFps))));
            }
        }
    }
    flushCurrent();

    QStringList paragraphs;
    paragraphs.reserve(sections.size());
    int sectionOrdinal = 1;
    for (const ExportSection& section : std::as_const(sections)) {
        const QString speakerLabel = transcriptExportSpeakerLabel(profiles, section.speakerId);
        const QString speakerDisplayName = transcriptExportSpeakerDisplayName(profiles, section.speakerId);
        const QString videoFileName = transcriptSectionVideoFileName(
            transcriptRoot,
            clipId,
            section.speakerId,
            speakerDisplayName,
            sectionOrdinal,
            section.startFrame,
            section.endFrame,
            speed,
            outputFormat);
        paragraphs.push_back(
            QStringLiteral("Video: %1\n%2: %3")
                .arg(videoFileName,
                     speakerLabel,
                     section.words.join(QLatin1Char(' ')).simplified()));
        ++sectionOrdinal;
    }
    return paragraphs.join(QStringLiteral("\n\n"));
}
}

TranscriptTab::TranscriptTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
    m_manualSelectionTimer.invalidate();
    m_deferredSeekTimer.setSingleShot(true);
    connect(&m_deferredSeekTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingSeekTimelineFrame < 0 || !m_deps.seekToTimelineFrame) {
            return;
        }
        m_deps.seekToTimelineFrame(m_pendingSeekTimelineFrame);
        m_pendingSeekTimelineFrame = -1;
    });
    m_refreshDebounceTimer.setSingleShot(true);
    connect(&m_refreshDebounceTimer, &QTimer::timeout, this, [this]() {
        m_refreshQueued = false;
        refresh();
    });
    connect(&m_transcriptLoadWatcher, &QFutureWatcher<TranscriptDocumentLoadResult>::finished, this, [this]() {
        const TranscriptDocumentLoadResult result = m_transcriptLoadWatcher.result();
        if (!result.ok) {
            if (m_transcriptSession.matches(result.clipFilePath, result.transcriptPath) &&
                m_widgets.transcriptInspectorDetailsLabel) {
                m_widgets.transcriptInspectorDetailsLabel->setText(
                    result.error.isEmpty() ? QStringLiteral("Invalid transcript JSON file.") : result.error);
            }
            return;
        }
        if (!m_transcriptSession.matches(result.clipFilePath, result.transcriptPath)) {
            return;
        }
        m_transcriptSession.assign(result.clipFilePath, result.transcriptPath, result.document);
        const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
        if (!clip || clip->filePath != result.clipFilePath) {
            return;
        }
        applyLoadedTranscriptDocumentData(*clip, originalTranscriptPathForClip(result.clipFilePath));
    });
    connect(&m_transcriptRowsBuildWatcher, &QFutureWatcher<TranscriptRowsBuildResult>::finished, this, [this]() {
        applyTranscriptRowsBuildResult(m_transcriptRowsBuildWatcher.result());
    });
}

bool TranscriptTab::updateLoadedTranscriptDocument(const std::function<bool(QJsonObject&)>& mutator)
{
    return m_transcriptSession.mutateRoot(mutator);
}

bool TranscriptTab::saveLoadedTranscriptDocument()
{
    if (m_transcriptSession.transcriptPath().trimmed().isEmpty()) {
        return false;
    }
    rehydrateLoadedTranscriptDocumentFromMemory();
    if (!m_transcriptSession.hasObjectDocument()) {
        return false;
    }
    queueLoadedTranscriptDocumentSave();
    return true;
}

bool TranscriptTab::activeTranscriptDocumentSnapshot(QString* clipFilePathOut,
                                                     QString* transcriptPathOut,
                                                     QJsonDocument* documentOut) const
{
    if (m_transcriptSession.clipFilePath().trimmed().isEmpty() ||
        m_transcriptSession.transcriptPath().trimmed().isEmpty() ||
        !m_transcriptSession.hasObjectDocument()) {
        return false;
    }
    if (clipFilePathOut) {
        *clipFilePathOut = m_transcriptSession.clipFilePath();
    }
    if (transcriptPathOut) {
        *transcriptPathOut = QFileInfo(m_transcriptSession.transcriptPath()).absoluteFilePath();
    }
    if (documentOut) {
        *documentOut = m_transcriptSession.document();
    }
    return true;
}

void TranscriptTab::restoreTranscriptDocumentSnapshot(const QString& clipFilePath,
                                                      const QString& transcriptPath,
                                                      const QJsonDocument& document)
{
    if (clipFilePath.trimmed().isEmpty() ||
        transcriptPath.trimmed().isEmpty() ||
        !document.isObject()) {
        return;
    }
    const QString absolutePath = QFileInfo(transcriptPath).absoluteFilePath();
    if (!m_transcriptSession.matches(clipFilePath, absolutePath) &&
        !m_transcriptSession.matches(clipFilePath, transcriptPath)) {
        return;
    }
    m_transcriptSession.assign(clipFilePath, absolutePath, document);
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (clip && clip->filePath == clipFilePath) {
        applyLoadedTranscriptDocumentData(*clip, originalTranscriptPathForClip(clipFilePath));
    }
}

void TranscriptTab::queueLoadedTranscriptDocumentSave()
{
    if (m_transcriptSession.transcriptPath().trimmed().isEmpty() || !m_transcriptSession.hasObjectDocument()) {
        return;
    }
    m_transcriptSession.queueSave(
        shouldUseSynchronousTranscriptIo(),
        [this](const QString& path, const QJsonDocument& doc) {
            m_transcriptEngine.saveTranscriptJson(path, doc);
        });
}

void TranscriptTab::startTranscriptLoadRequest(const QString& clipFilePath, const QString& transcriptPath)
{
    if (clipFilePath.trimmed().isEmpty() || transcriptPath.trimmed().isEmpty()) {
        return;
    }
    if (shouldUseSynchronousTranscriptIo()) {
        const TranscriptDocumentLoadResult result = loadTranscriptDocumentResultWithEngine(
            clipFilePath,
            transcriptPath,
            QStringLiteral("Invalid transcript JSON file."));
        if (result.ok) {
            m_transcriptSession.assign(clipFilePath, transcriptPath, result.document);
            const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
            if (clip && clip->filePath == clipFilePath) {
                applyLoadedTranscriptDocumentData(*clip, originalTranscriptPathForClip(clipFilePath));
            }
        } else if (m_widgets.transcriptInspectorDetailsLabel) {
            m_widgets.transcriptInspectorDetailsLabel->setText(
                result.error);
        }
        return;
    }
    ++m_transcriptLoadRequestId;
    const qint64 requestId = m_transcriptLoadRequestId;
    if (m_widgets.transcriptInspectorDetailsLabel) {
        m_widgets.transcriptInspectorDetailsLabel->setText(QStringLiteral("Loading transcript..."));
    }
    m_transcriptLoadWatcher.setFuture(QtConcurrent::run([clipFilePath, transcriptPath, requestId]() {
        Q_UNUSED(requestId);
        return loadTranscriptDocumentResultWithEngine(
            clipFilePath,
            transcriptPath,
            QStringLiteral("Invalid transcript JSON file."));
    }));
}

void TranscriptTab::requestRefresh(int delayMs)
{
    const int safeDelayMs = qBound(0, delayMs, 500);
    m_refreshQueued = true;
    m_refreshDebounceTimer.start(safeDelayMs);
}

void TranscriptTab::wire()
{
    // Wire up the editable cut title
    if (m_widgets.transcriptInspectorClipLabel) {
        connect(m_widgets.transcriptInspectorClipLabel, &QLineEdit::editingFinished, this, [this]() {
            if (m_updating || !m_deps.getSelectedClip || !m_deps.updateClipById) {
                return;
            }
            const TimelineClip* clip = m_deps.getSelectedClip();
            if (!clip) {
                return;
            }
            const QString newLabel = m_widgets.transcriptInspectorClipLabel->text().trimmed();
            if (newLabel.isEmpty() || newLabel == clip->label) {
                // Revert to actual clip label if empty or unchanged
                if (newLabel.isEmpty()) {
                    m_widgets.transcriptInspectorClipLabel->setText(clip->label);
                }
                return;
            }
            m_deps.updateClipById(clip->id, [&newLabel](TimelineClip& editableClip) {
                editableClip.label = newLabel;
            });
            applyTabEditEffects(transcriptEditCallbacks(m_deps),
                                TabEditEffects{.updatePreview = false});
        });
    }

    if (m_widgets.transcriptTable) {
        configureTranscriptTableView();
        m_widgets.transcriptTable->setDragEnabled(true);
        m_widgets.transcriptTable->setAcceptDrops(true);
        m_widgets.transcriptTable->viewport()->setAcceptDrops(true);
        m_widgets.transcriptTable->setDropIndicatorShown(true);
        m_widgets.transcriptTable->setDragDropMode(QAbstractItemView::InternalMove);
        m_widgets.transcriptTable->setDefaultDropAction(Qt::MoveAction);
        connect(m_widgets.transcriptTable, &QTableWidget::cellClicked,
                this, [this](int row, int) { onTranscriptRowClicked(row); });
        connect(m_widgets.transcriptTable, &QTableWidget::itemDoubleClicked,
                this, &TranscriptTab::onTranscriptItemDoubleClicked);
        connect(m_widgets.transcriptTable, &QTableWidget::itemChanged,
                this, &TranscriptTab::applyTableEdit);
        connect(m_widgets.transcriptTable, &QTableWidget::itemSelectionChanged,
                this, &TranscriptTab::onTranscriptSelectionChanged);
        m_widgets.transcriptTable->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.transcriptTable, &QWidget::customContextMenuRequested,
                this, &TranscriptTab::onTranscriptCustomContextMenu);
        if (m_widgets.transcriptTable->horizontalHeader()) {
            m_widgets.transcriptTable->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(m_widgets.transcriptTable->horizontalHeader(), &QHeaderView::customContextMenuRequested,
                    this, &TranscriptTab::onTranscriptHeaderContextMenu);
        }
        m_widgets.transcriptTable->installEventFilter(this);
        if (m_widgets.transcriptTable->viewport()) {
            m_widgets.transcriptTable->viewport()->installEventFilter(this);
        }
        if (m_widgets.transcriptTable->model()) {
            connect(m_widgets.transcriptTable->model(), &QAbstractItemModel::rowsMoved, this,
                    [this](const QModelIndex&, int, int, const QModelIndex&, int) {
                        if (m_updating) {
                            return;
                        }
                        persistRenderOrderFromTable();
                    });
        }
    }
    if (m_widgets.transcriptFollowCurrentWordCheckBox) {
        connect(m_widgets.transcriptFollowCurrentWordCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onFollowCurrentWordToggled);
    }
    if (m_widgets.transcriptOverlayEnabledCheckBox) {
        connect(m_widgets.transcriptOverlayEnabledCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptPlacementModeCombo) {
        connect(m_widgets.transcriptPlacementModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptBackgroundVisibleCheckBox) {
        connect(m_widgets.transcriptBackgroundVisibleCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptBackgroundOpacitySpin) {
        connect(m_widgets.transcriptBackgroundOpacitySpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptBackgroundCornerRadiusSpin) {
        connect(m_widgets.transcriptBackgroundCornerRadiusSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    for (QPushButton* button : {m_widgets.transcriptTextColorButton,
                                m_widgets.transcriptBackgroundColorButton,
                                m_widgets.transcriptHighlightColorButton}) {
        if (button) {
            connect(button, &QPushButton::clicked, this, &TranscriptTab::onOverlayColorButtonClicked);
        }
    }
    if (m_widgets.transcriptShadowEnabledCheckBox) {
        connect(m_widgets.transcriptShadowEnabledCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptShowSpeakerTitleCheckBox) {
        connect(m_widgets.transcriptShowSpeakerTitleCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptHighlightCurrentWordCheckBox) {
        connect(m_widgets.transcriptHighlightCurrentWordCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptMaxLinesSpin) {
        connect(m_widgets.transcriptMaxLinesSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptMaxCharsSpin) {
        connect(m_widgets.transcriptMaxCharsSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptAutoScrollCheckBox) {
        connect(m_widgets.transcriptAutoScrollCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptOverlayXSpin) {
        connect(m_widgets.transcriptOverlayXSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptOverlayYSpin) {
        connect(m_widgets.transcriptOverlayYSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptCenterHorizontalButton) {
        connect(m_widgets.transcriptCenterHorizontalButton, &QPushButton::clicked,
                this, &TranscriptTab::onCenterHorizontalClicked);
    }
    if (m_widgets.transcriptCenterVerticalButton) {
        connect(m_widgets.transcriptCenterVerticalButton, &QPushButton::clicked,
                this, &TranscriptTab::onCenterVerticalClicked);
    }
    if (m_widgets.transcriptOverlayWidthSpin) {
        connect(m_widgets.transcriptOverlayWidthSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptOverlayHeightSpin) {
        connect(m_widgets.transcriptOverlayHeightSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptFontFamilyCombo) {
        connect(m_widgets.transcriptFontFamilyCombo, &QFontComboBox::currentFontChanged,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptFontSizeSpin) {
        connect(m_widgets.transcriptFontSizeSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptBoldCheckBox) {
        connect(m_widgets.transcriptBoldCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptItalicCheckBox) {
        connect(m_widgets.transcriptItalicCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptPrependMsSpin) {
        connect(m_widgets.transcriptPrependMsSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onPrependMsChanged);
    }
    if (m_widgets.transcriptPostpendMsSpin) {
        connect(m_widgets.transcriptPostpendMsSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onPostpendMsChanged);
    }
    if (m_widgets.transcriptOffsetMsSpin) {
        connect(m_widgets.transcriptOffsetMsSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOffsetMsChanged);
    }
    if (m_widgets.speechFilterEnabledCheckBox) {
        connect(m_widgets.speechFilterEnabledCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onSpeechFilterEnabledToggled);
    }
    if (m_widgets.speechFilterFadeSamplesSpin) {
        connect(m_widgets.speechFilterFadeSamplesSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onSpeechFilterFadeSamplesChanged);
    }
    if (m_widgets.transcriptUnifiedEditModeCheckBox) {
        connect(m_widgets.transcriptUnifiedEditModeCheckBox, &QCheckBox::toggled,
                this, [this](bool) { requestRefresh(); });
    }
    if (m_widgets.transcriptSearchFilterLineEdit) {
        connect(m_widgets.transcriptSearchFilterLineEdit, &QLineEdit::textChanged,
                this, [this](const QString&) { requestRefresh(); });
        connect(m_widgets.transcriptSearchFilterLineEdit, &QLineEdit::returnPressed,
                this, &TranscriptTab::onTranscriptSearchReturnPressed);
    }
    if (m_widgets.transcriptSpeakerFilterCombo) {
        connect(m_widgets.transcriptSpeakerFilterCombo, &QComboBox::currentIndexChanged,
                this, [this](int) { requestRefresh(); });
    }
    if (m_widgets.transcriptShowExcludedLinesCheckBox) {
        connect(m_widgets.transcriptShowExcludedLinesCheckBox, &QCheckBox::toggled,
                this, [this](bool) { requestRefresh(); });
    }
    if (m_widgets.transcriptScriptVersionCombo) {
        connect(m_widgets.transcriptScriptVersionCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &TranscriptTab::onTranscriptScriptVersionChanged);
        m_widgets.transcriptScriptVersionCombo->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.transcriptScriptVersionCombo, &QWidget::customContextMenuRequested,
                this, &TranscriptTab::onTranscriptScriptVersionContextMenu);
        // Wire up editable combo for renaming cut versions
        if (m_widgets.transcriptScriptVersionCombo->lineEdit()) {
            connect(m_widgets.transcriptScriptVersionCombo->lineEdit(), &QLineEdit::editingFinished,
                    this, &TranscriptTab::onTranscriptCutLabelEdited);
        }
    }
    if (m_widgets.transcriptNewVersionButton) {
        connect(m_widgets.transcriptNewVersionButton, &QPushButton::clicked,
                this, &TranscriptTab::onTranscriptCreateVersion);
    }
    if (m_widgets.transcriptDeleteVersionButton) {
        connect(m_widgets.transcriptDeleteVersionButton, &QPushButton::clicked,
                this, &TranscriptTab::onTranscriptDeleteVersion);
    }
    if (m_widgets.transcriptExportTextButton) {
        connect(m_widgets.transcriptExportTextButton, &QPushButton::clicked,
                this, &TranscriptTab::onTranscriptExportText);
    }
}

void TranscriptTab::onTranscriptExportText()
{
    if (!m_transcriptSession.hasObjectDocument()) {
        QMessageBox::information(
            m_widgets.transcriptTable,
            QStringLiteral("Export Transcript"),
            QStringLiteral("Load a transcript before exporting."));
        return;
    }

    const QJsonObject transcriptRoot = m_transcriptSession.rootObject();
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    const QString clipId = clip ? clip->id : QString();
    const qreal speed = m_deps.exportPlaybackSpeed ? m_deps.exportPlaybackSpeed() : 1.0;
    const QString outputFormat = m_deps.outputFormat ? m_deps.outputFormat() : QStringLiteral("mp4");
    const QString exportText = buildContiguousTranscriptTextExport(
        transcriptRoot, clipId, speed, outputFormat);
    if (exportText.trimmed().isEmpty()) {
        QMessageBox::information(
            m_widgets.transcriptTable,
            QStringLiteral("Export Transcript"),
            QStringLiteral("No transcript text is available to export."));
        return;
    }

    const QString selectedPath = QFileDialog::getSaveFileName(
        m_widgets.transcriptTable,
        QStringLiteral("Export Transcript Text"),
        defaultTranscriptExportPath(m_transcriptSession.transcriptPath()),
        QStringLiteral("Text Files (*.txt);;All Files (*)"));
    if (selectedPath.trimmed().isEmpty()) {
        return;
    }

    QSaveFile outputFile(selectedPath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(
            m_widgets.transcriptTable,
            QStringLiteral("Export Transcript"),
            QStringLiteral("Could not write transcript export:\n%1").arg(outputFile.errorString()));
        return;
    }

    const QByteArray payload = (exportText + QLatin1Char('\n')).toUtf8();
    if (outputFile.write(payload) != payload.size() || !outputFile.commit()) {
        QMessageBox::warning(
            m_widgets.transcriptTable,
            QStringLiteral("Export Transcript"),
            QStringLiteral("Could not save transcript export:\n%1").arg(outputFile.errorString()));
        return;
    }

    if (m_widgets.transcriptInspectorDetailsLabel) {
        m_widgets.transcriptInspectorDetailsLabel->setText(
            QStringLiteral("Exported transcript text: %1").arg(QDir::toNativeSeparators(selectedPath)));
    }
}

void TranscriptTab::refresh()
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (m_widgets.transcriptTable && m_widgets.transcriptTable->currentRow() >= 0) {
        persistSelectionIdentityFromRow(m_widgets.transcriptTable->currentRow());
    }
    m_updating = true;
    m_manualSelectionTimer.invalidate();

    const QString previousClipFilePath = m_transcriptSession.clipFilePath();
    if (m_widgets.transcriptTable) {
        m_widgets.transcriptTable->clearContents();
        m_widgets.transcriptTable->setRowCount(0);
    }
    m_followRanges.clear();
    m_excludedRanges.clear();
    m_allTranscriptRows.clear();
    m_wordEditIndex.clear();
    m_lastSyncRow = -1;
    syncSpeechFilterControlsFromWidgets();

    if (!clip || !clipSupportsTranscript(*clip)) {
        clearActiveTranscriptPathForClipFile(previousClipFilePath);
        m_transcriptSession.clear();
        m_transcriptDocumentSegments.clear();
        m_transcriptWordAddressById.clear();
        m_renderOrderedWordIds.clear();
        m_allTranscriptRows.clear();
        m_wordEditIndex.clear();
        m_persistedSelectedClipId.clear();
        m_persistedSelectedSegmentIndex = -1;
        m_persistedSelectedWordIndex = -1;
        m_persistedSelectedWordId = -1;
        m_lastSyncClipId.clear();
        if (m_widgets.transcriptInspectorClipLabel) {
            m_widgets.transcriptInspectorClipLabel->setText(QString());
            m_widgets.transcriptInspectorClipLabel->setPlaceholderText(QStringLiteral("No transcript selected"));
        }
        if (m_widgets.transcriptInspectorDetailsLabel) {
            m_widgets.transcriptInspectorDetailsLabel->setText(
                QStringLiteral("Select an audio clip with a WhisperX JSON transcript.\n"
                               "Workflow: 1) Select/Create Cut  2) Edit Transcript/Speakers  3) Preview  4) Render"));
        }
        refreshScriptVersionSelector(QString(), QString());
        m_updating = false;
        return;
    }

    if (m_widgets.transcriptInspectorClipLabel) {
        m_widgets.transcriptInspectorClipLabel->setText(clip->label);
        m_widgets.transcriptInspectorClipLabel->setPlaceholderText(QString());
    }

    updateOverlayWidgetsFromClip(*clip);
    loadTranscriptFile(*clip);
    m_updating = false;
}

void TranscriptTab::syncSpeechFilterControlsFromWidgets()
{
    if (m_widgets.speechFilterFadeSamplesSpin) {
        m_speechFilterFadeSamples = qMax(0, m_widgets.speechFilterFadeSamplesSpin->value());
    }
    if (m_widgets.speechFilterEnabledCheckBox) {
        m_speechFilterEnabled = m_widgets.speechFilterEnabledCheckBox->isChecked();
    }
}

void TranscriptTab::syncTableToPlayhead(int64_t absolutePlaybackSample,
                                        double sourceSeconds,
                                        int64_t sourceFrame)
{
    if (!m_widgets.transcriptTable || m_updating) return;
    if (sourceFrame < 0) {
        static bool warnedMissingSourceFrame = false;
        if (!warnedMissingSourceFrame) {
            warnedMissingSourceFrame = true;
            qWarning().noquote()
                << QStringLiteral("[TRANSCRIPT TIMING WARN] syncTableToPlayhead called without sourceFrame; falling back to sourceSeconds-derived frame.");
        }
    }
    const int64_t previousSourceFrame = m_lastSyncSourceFrame;
    const int64_t currentSourceFrame = sourceFrame >= 0
        ? sourceFrame
        : qMax<int64_t>(0, static_cast<int64_t>(std::floor(sourceSeconds * kTimelineFps)));
    m_lastSyncSourceFrame = currentSourceFrame;
    if (!m_widgets.transcriptFollowCurrentWordCheckBox ||
        !m_widgets.transcriptFollowCurrentWordCheckBox->isChecked()) {
        m_lastSyncAbsolutePlaybackSample = absolutePlaybackSample;
        return;
    }

    const int64_t previousAbsolutePlaybackSample = m_lastSyncAbsolutePlaybackSample;
    const bool playbackAdvanced =
        (previousAbsolutePlaybackSample >= 0 &&
         absolutePlaybackSample != previousAbsolutePlaybackSample);
    m_lastSyncAbsolutePlaybackSample = absolutePlaybackSample;

    // Hold manual selection while paused, but resume follow immediately once
    // playback advances so click-to-seek + play tracks words in time.
    if (hasActiveManualSelection() && !playbackAdvanced) {
        return;
    }
    if (playbackAdvanced) {
        m_manualSelectionTimer.invalidate();
    }

    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || !clipSupportsTranscript(*clip) || m_transcriptSession.transcriptPath().isEmpty()) {
        m_widgets.transcriptTable->clearSelection();
        m_lastSyncRow = -1;
        m_lastSyncClipId.clear();
        return;
    }
    m_lastSyncClipId = clip->id;

    if (m_followRanges.isEmpty()) {
        m_widgets.transcriptTable->clearSelection();
        m_lastSyncRow = -1;
        return;
    }

    // If playhead is inside a skipped/outside-cut region, do not auto-select nearby rows.
    const auto excludedIt = std::lower_bound(
        m_excludedRanges.cbegin(), m_excludedRanges.cend(), currentSourceFrame,
        [](const ExcludedRange& range, int64_t frame) { return range.endFrame < frame; });
    if (excludedIt != m_excludedRanges.cend() &&
        excludedIt->startFrame <= currentSourceFrame &&
        currentSourceFrame <= excludedIt->endFrame) {
        m_widgets.transcriptTable->clearSelection();
        m_lastSyncRow = -1;
        return;
    }

    int matchingRow = -1;
    int previousEligibleRow = -1;
    int nextEligibleRow = -1;
    int nearestEligibleRow = -1;
    int64_t nearestDistance = std::numeric_limits<int64_t>::max();

    const auto upperIt = std::upper_bound(
        m_followRanges.cbegin(), m_followRanges.cend(), currentSourceFrame,
        [](int64_t value, const FollowRange& range) { return value < range.startFrame; });
    const int upperIndex = static_cast<int>(std::distance(m_followRanges.cbegin(), upperIt));

    int previousIndex = upperIndex - 1;
    if (previousIndex >= 0) {
        previousEligibleRow = m_followRanges.at(previousIndex).row;
    }
    if (upperIndex < m_followRanges.size()) {
        nextEligibleRow = m_followRanges.at(upperIndex).row;
    }

    for (int i = previousIndex; i >= 0; --i) {
        const FollowRange& range = m_followRanges.at(i);
        if (range.endFrame < currentSourceFrame) {
            break;
        }
        if (currentSourceFrame >= range.startFrame && currentSourceFrame <= range.endFrame) {
            matchingRow = range.row;
            break;
        }
    }
    if (matchingRow < 0) {
        for (int i = upperIndex; i < m_followRanges.size(); ++i) {
            const FollowRange& range = m_followRanges.at(i);
            if (range.startFrame > currentSourceFrame) {
                break;
            }
            if (currentSourceFrame >= range.startFrame && currentSourceFrame <= range.endFrame) {
                matchingRow = range.row;
                break;
            }
        }
    }

    if (previousIndex >= 0) {
        const FollowRange& range = m_followRanges.at(previousIndex);
        const int64_t distance = qMax<int64_t>(0, currentSourceFrame - range.endFrame);
        nearestDistance = distance;
        nearestEligibleRow = range.row;
    }
    if (upperIndex < m_followRanges.size()) {
        const FollowRange& range = m_followRanges.at(upperIndex);
        const int64_t distance = qMax<int64_t>(0, range.startFrame - currentSourceFrame);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearestEligibleRow = range.row;
        }
    }

    const int64_t sourceDeltaFrames =
        previousSourceFrame >= 0 ? qAbs(currentSourceFrame - previousSourceFrame) : 0;
    const int64_t maxGapFrames = qMax<int64_t>(
        1, static_cast<int64_t>(std::ceil(sourceDeltaFrames * 1.5)) + 1);
    const bool forwardMotion =
        (previousSourceFrame >= 0 && currentSourceFrame > previousSourceFrame) ||
        (previousSourceFrame >= 0 && currentSourceFrame == previousSourceFrame &&
         previousAbsolutePlaybackSample >= 0 &&
         absolutePlaybackSample > previousAbsolutePlaybackSample);
    const bool backwardMotion =
        (previousSourceFrame >= 0 && currentSourceFrame < previousSourceFrame) ||
        (previousSourceFrame >= 0 && currentSourceFrame == previousSourceFrame &&
         previousAbsolutePlaybackSample >= 0 &&
         absolutePlaybackSample < previousAbsolutePlaybackSample);

    const bool playbackSampleMoved =
        previousAbsolutePlaybackSample >= 0 &&
        absolutePlaybackSample != previousAbsolutePlaybackSample;

    if (matchingRow >= 0 && playbackSampleMoved) {
        if (forwardMotion && matchingRow == previousEligibleRow && nextEligibleRow >= 0) {
            const FollowRange& nextRange = m_followRanges.at(upperIndex);
            if ((nextRange.startFrame - currentSourceFrame) <= maxGapFrames) {
                matchingRow = nextEligibleRow;
            }
        } else if (backwardMotion && matchingRow == nextEligibleRow && previousEligibleRow >= 0) {
            const FollowRange& previousRange = m_followRanges.at(previousIndex);
            if ((currentSourceFrame - previousRange.endFrame) <= maxGapFrames) {
                matchingRow = previousEligibleRow;
            }
        }
    }

    if (matchingRow < 0) {
        if (forwardMotion && nextEligibleRow >= 0) {
            const FollowRange& nextRange = m_followRanges.at(upperIndex);
            if ((nextRange.startFrame - currentSourceFrame) <= maxGapFrames) {
                matchingRow = nextEligibleRow;
            }
        } else if (backwardMotion && previousEligibleRow >= 0) {
            const FollowRange& previousRange = m_followRanges.at(previousIndex);
            if ((currentSourceFrame - previousRange.endFrame) <= maxGapFrames) {
                matchingRow = previousEligibleRow;
            }
        } else if (nearestEligibleRow >= 0 && nearestDistance <= maxGapFrames) {
            matchingRow = nearestEligibleRow;
        }
    }

    if (matchingRow < 0) {
        m_widgets.transcriptTable->clearSelection();
        m_lastSyncRow = -1;
        return;
    }

    if (matchingRow == m_lastSyncRow &&
        m_widgets.transcriptTable->selectionModel() &&
        m_widgets.transcriptTable->selectionModel()->isRowSelected(matchingRow, QModelIndex())) {
        return;
    }

    if (!m_widgets.transcriptTable->selectionModel()->isRowSelected(matchingRow, QModelIndex())) {
        m_suppressSelectionSideEffects = true;
        m_widgets.transcriptTable->setCurrentCell(matchingRow, 0);
        m_widgets.transcriptTable->selectRow(matchingRow);
        m_suppressSelectionSideEffects = false;
    }
    persistSelectionIdentityFromRow(matchingRow);
    m_lastSyncRow = matchingRow;

    if (m_widgets.transcriptTable->item(matchingRow, 0)) {
        QTableWidgetItem* item = m_widgets.transcriptTable->item(matchingRow, 0);
        m_widgets.transcriptTable->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    }
}

QJsonObject TranscriptTab::debugSnapshot() const
{
    QJsonObject snapshot{
        {QStringLiteral("follow_current_word"),
         m_widgets.transcriptFollowCurrentWordCheckBox
             ? m_widgets.transcriptFollowCurrentWordCheckBox->isChecked()
             : false},
        {QStringLiteral("updating"), m_updating},
        {QStringLiteral("manual_selection_active"), hasActiveManualSelection()},
        {QStringLiteral("manual_selection_hold_ms"), m_manualSelectionHoldMs},
        {QStringLiteral("last_sync_absolute_playback_sample"),
         static_cast<qint64>(m_lastSyncAbsolutePlaybackSample)},
        {QStringLiteral("last_sync_source_frame"), static_cast<qint64>(m_lastSyncSourceFrame)},
        {QStringLiteral("last_sync_clip_id"), m_lastSyncClipId},
        {QStringLiteral("last_sync_row"), m_lastSyncRow},
        {QStringLiteral("persisted_selected_clip_id"), m_persistedSelectedClipId},
        {QStringLiteral("persisted_selected_word_id"), m_persistedSelectedWordId},
        {QStringLiteral("persisted_selected_segment_index"), m_persistedSelectedSegmentIndex},
        {QStringLiteral("persisted_selected_word_index"), m_persistedSelectedWordIndex},
        {QStringLiteral("follow_range_count"), m_followRanges.size()},
        {QStringLiteral("excluded_range_count"), m_excludedRanges.size()},
        {QStringLiteral("transcript_path"), m_transcriptSession.transcriptPath()},
        {QStringLiteral("has_object_document"), m_transcriptSession.hasObjectDocument()},
        {QStringLiteral("all_row_count"), m_allTranscriptRows.size()}};

    if (!m_widgets.transcriptTable) {
        snapshot[QStringLiteral("has_table")] = false;
        return snapshot;
    }

    const QTableWidget* table = m_widgets.transcriptTable;
    snapshot[QStringLiteral("has_table")] = true;
    snapshot[QStringLiteral("table_row_count")] = table->rowCount();
    snapshot[QStringLiteral("current_row")] = table->currentRow();

    QJsonArray selectedRows;
    if (table->selectionModel()) {
        const QModelIndexList rows = table->selectionModel()->selectedRows();
        for (const QModelIndex& index : rows) {
            selectedRows.push_back(index.row());
        }
    }
    snapshot[QStringLiteral("selected_rows")] = selectedRows;

    const int firstVisibleRow = table->rowAt(0);
    snapshot[QStringLiteral("first_visible_row")] = firstVisibleRow;

    auto appendRowSnapshot = [table](QJsonObject* parent,
                                     const QString& key,
                                     int row) {
        if (!parent || row < 0 || row >= table->rowCount()) {
            return;
        }
        const QTableWidgetItem* startItem = table->item(row, kTranscriptColSourceStart);
        const QTableWidgetItem* endItem = table->item(row, kTranscriptColSourceEnd);
        const QTableWidgetItem* textItem = table->item(row, kTranscriptColText);
        if (!startItem || !endItem) {
            return;
        }

        QJsonObject rowSnapshot{
            {QStringLiteral("row"), row},
            {QStringLiteral("start_frame"),
             static_cast<qint64>(startItem->data(Qt::UserRole + 2).toLongLong())},
            {QStringLiteral("end_frame"),
             static_cast<qint64>(endItem->data(Qt::UserRole + 3).toLongLong())},
            {QStringLiteral("render_start_frame"),
             static_cast<qint64>(startItem->data(Qt::UserRole + 10).toLongLong())},
            {QStringLiteral("render_end_frame"),
             static_cast<qint64>(endItem->data(Qt::UserRole + 11).toLongLong())},
            {QStringLiteral("is_gap"), startItem->data(Qt::UserRole + 4).toBool()},
            {QStringLiteral("is_skipped"), startItem->data(Qt::UserRole + 7).toBool()},
            {QStringLiteral("is_outside_active_cut"), startItem->data(Qt::UserRole + 12).toBool()},
            {QStringLiteral("segment_index"), startItem->data(Qt::UserRole + 5).toInt()},
            {QStringLiteral("word_index"), startItem->data(Qt::UserRole + 6).toInt()},
            {QStringLiteral("word_id"), startItem->data(Qt::UserRole + 16).toInt()}};
        if (textItem) {
            rowSnapshot[QStringLiteral("text")] = textItem->text().left(120);
        }
        (*parent)[key] = rowSnapshot;
    };

    appendRowSnapshot(&snapshot, QStringLiteral("current_row_state"), table->currentRow());
    appendRowSnapshot(&snapshot, QStringLiteral("last_sync_row_state"), m_lastSyncRow);
    appendRowSnapshot(&snapshot, QStringLiteral("first_visible_row_state"), firstVisibleRow);

    int playheadRow = -1;
    int64_t nearestDistance = std::numeric_limits<int64_t>::max();
    if (m_lastSyncSourceFrame >= 0) {
        for (const FollowRange& range : m_followRanges) {
            const bool contains =
                range.startFrame <= m_lastSyncSourceFrame &&
                m_lastSyncSourceFrame <= range.endFrame;
            const int64_t distance = contains
                ? 0
                : qMin(qAbs(m_lastSyncSourceFrame - range.startFrame),
                       qAbs(m_lastSyncSourceFrame - range.endFrame));
            if (distance < nearestDistance) {
                nearestDistance = distance;
                playheadRow = range.row;
            }
        }
    }
    snapshot[QStringLiteral("nearest_playhead_row")] = playheadRow;
    snapshot[QStringLiteral("nearest_playhead_distance_frames")] =
        nearestDistance == std::numeric_limits<int64_t>::max()
            ? -1
            : static_cast<qint64>(nearestDistance);
    appendRowSnapshot(&snapshot, QStringLiteral("nearest_playhead_row_state"), playheadRow);

    return snapshot;
}

void TranscriptTab::applyTableEdit(QTableWidgetItem* item)
{
    if (m_updating || !item || m_transcriptSession.transcriptPath().isEmpty() ||
        !m_transcriptSession.hasObjectDocument()) {
        return;
    }
    if (!activeCutMutable()) {
        refresh();
        return;
    }

    const int wordId = item->data(Qt::UserRole + 16).toInt();
    const bool isGap = item->data(Qt::UserRole + 4).toBool();
    if (isGap || wordId < 0) return;
    TranscriptDocumentWord* word = transcriptWordById(wordId);
    if (!word) {
        refresh();
        return;
    }
    if (item->column() == kTranscriptColSourceStart || item->column() == kTranscriptColSourceEnd) {
        double seconds = 0.0;
        if (!m_transcriptEngine.parseTranscriptTime(item->text(), &seconds)) {
            refresh();
            return;
        }
        if (item->column() == kTranscriptColSourceStart) {
            const double currentEnd = word->endSeconds;
            const double currentStart = word->startSeconds;
            if (!qFuzzyCompare(currentStart + 1.0, qMin(seconds, currentEnd) + 1.0)) {
                if (!word->editTags.contains(QString(kTranscriptEditTimingTag))) {
                    word->editTags.push_back(QString(kTranscriptEditTimingTag));
                }
            }
            word->startSeconds = qMin(seconds, currentEnd);
        } else {
            const double currentStart = word->startSeconds;
            const double currentEnd = word->endSeconds;
            if (!qFuzzyCompare(currentEnd + 1.0, qMax(seconds, currentStart) + 1.0)) {
                if (!word->editTags.contains(QString(kTranscriptEditTimingTag))) {
                    word->editTags.push_back(QString(kTranscriptEditTimingTag));
                }
            }
            word->endSeconds = qMax(seconds, currentStart);
        }
    } else if (item->column() == kTranscriptColText) {
        if (word->text != item->text()) {
            if (!word->editTags.contains(QString(kTranscriptEditTextTag))) {
                word->editTags.push_back(QString(kTranscriptEditTextTag));
            }
        }
        word->text = item->text();
    } else {
        return;
    }
    rehydrateLoadedTranscriptDocumentFromMemory();
    saveLoadedTranscriptDocument();
    refresh();
    emit transcriptDocumentChanged();
    applyTabEditEffects(transcriptEditCallbacks(m_deps),
                        TabEditEffects{.updatePreview = false, .refreshInspector = false});
}

void TranscriptTab::deleteSelectedRows()
{
    if (m_updating || !m_widgets.transcriptTable || m_transcriptSession.transcriptPath().isEmpty() ||
        !m_transcriptSession.hasObjectDocument()) {
        return;
    }
    if (!activeCutMutable()) {
        return;
    }

    QItemSelectionModel* selectionModel = m_widgets.transcriptTable->selectionModel();
    if (!selectionModel) return;

    const QModelIndexList selectedRows = selectionModel->selectedRows();
    if (selectedRows.isEmpty()) return;

    QSet<int> deleteWordIds;
    for (const QModelIndex& index : selectedRows) {
        const int row = index.row();
        QTableWidgetItem* item = m_widgets.transcriptTable->item(row, 0);
        if (!item || item->data(Qt::UserRole + 4).toBool() || item->data(Qt::UserRole + 12).toBool()) continue;
        const int wordId = item->data(Qt::UserRole + 16).toInt();
        if (wordId < 0) continue;
        deleteWordIds.insert(wordId);
    }
    if (deleteWordIds.isEmpty()) return;

    int deletedCount = 0;
    for (int segmentIndex = 0; segmentIndex < m_transcriptDocumentSegments.size(); ++segmentIndex) {
        auto& words = m_transcriptDocumentSegments[segmentIndex].words;
        QVector<TranscriptDocumentWord> filteredWords;
        filteredWords.reserve(words.size());
        for (const TranscriptDocumentWord& word : std::as_const(words)) {
            if (deleteWordIds.contains(word.wordId)) {
                ++deletedCount;
            } else {
                filteredWords.push_back(word);
            }
        }
        words = std::move(filteredWords);
    }
    m_renderOrderedWordIds.erase(
        std::remove_if(m_renderOrderedWordIds.begin(), m_renderOrderedWordIds.end(),
                       [&deleteWordIds](const int wordId) { return deleteWordIds.contains(wordId); }),
        m_renderOrderedWordIds.end());
    rebuildTranscriptWordAddressIndex();

    if (deletedCount > 0) {
        updateLoadedTranscriptDocument([&](QJsonObject& loadedRoot) {
            const int previousDeletedEdits = loadedRoot.value(QString(kTranscriptDeletedEditsCountKey)).toInt(0);
            loadedRoot[QString(kTranscriptDeletedEditsCountKey)] = previousDeletedEdits + deletedCount;
            return true;
        });
    }
    normalizeTranscriptRenderOrder();
    rehydrateLoadedTranscriptDocumentFromMemory();
    saveLoadedTranscriptDocument();
    refresh();
    emit transcriptDocumentChanged();
    applyTabEditEffects(transcriptEditCallbacks(m_deps),
                        TabEditEffects{.updatePreview = false, .refreshInspector = false});
}

void TranscriptTab::setSelectedRowsSkipped(bool skipped)
{
    if (m_updating || !m_widgets.transcriptTable || m_transcriptSession.transcriptPath().isEmpty() ||
        !m_transcriptSession.hasObjectDocument()) {
        return;
    }
    if (!activeCutMutable()) {
        return;
    }

    QItemSelectionModel* selectionModel = m_widgets.transcriptTable->selectionModel();
    if (!selectionModel) {
        return;
    }

    const QModelIndexList selectedRows = selectionModel->selectedRows();
    if (selectedRows.isEmpty()) {
        return;
    }

    QSet<int> targetWordIds;
    for (const QModelIndex& index : selectedRows) {
        QTableWidgetItem* item = m_widgets.transcriptTable->item(index.row(), 0);
        if (!item || item->data(Qt::UserRole + 4).toBool() || item->data(Qt::UserRole + 12).toBool()) {
            continue;
        }
        const int wordId = item->data(Qt::UserRole + 16).toInt();
        if (wordId < 0) {
            continue;
        }
        targetWordIds.insert(wordId);
    }
    if (targetWordIds.isEmpty()) {
        return;
    }

    bool changed = false;
    for (const int wordId : std::as_const(targetWordIds)) {
        if (TranscriptDocumentWord* word = transcriptWordById(wordId)) {
            const bool currentSkipped = word->skipped;
            if (currentSkipped == skipped) {
                continue;
            }
            word->skipped = skipped;
            if (!word->editTags.contains(QString(kTranscriptEditSkipTag))) {
                word->editTags.push_back(QString(kTranscriptEditSkipTag));
            }
            changed = true;
        }
    }

    if (!changed) {
        return;
    }

    rehydrateLoadedTranscriptDocumentFromMemory();
    saveLoadedTranscriptDocument();
    refresh();
    emit transcriptDocumentChanged();
    applyTabEditEffects(transcriptEditCallbacks(m_deps),
                        TabEditEffects{.updatePreview = false, .refreshInspector = false});
}

void TranscriptTab::scheduleSeekToTranscriptRow(int row)
{
    if (!m_widgets.transcriptTable || !m_deps.getSelectedClip || !m_deps.seekToTimelineFrame) {
        return;
    }

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip || !clipSupportsTranscript(*selectedClip)) {
        return;
    }

    QTableWidgetItem* item = m_widgets.transcriptTable->item(row, 0);
    if (!item || item->data(Qt::UserRole + 4).toBool() || item->data(Qt::UserRole + 12).toBool()) {
        return;
    }

    const int64_t startFrame = item->data(Qt::UserRole + 2).toLongLong();
    const int64_t timelineFrame = qMax<int64_t>(
        selectedClip->startFrame,
        selectedClip->startFrame + (startFrame - selectedClip->sourceInFrame));
    m_pendingSeekTimelineFrame = timelineFrame;
    m_deferredSeekTimer.start(QApplication::doubleClickInterval());
}

void TranscriptTab::configureTranscriptTableView()
{
    QTableWidget* table = m_widgets.transcriptTable;
    if (!table) {
        return;
    }

    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table->setWordWrap(false);
    table->setTextElideMode(Qt::ElideRight);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);

    if (QHeaderView* vertical = table->verticalHeader()) {
        vertical->setVisible(false);
        vertical->setSectionResizeMode(QHeaderView::Fixed);
        vertical->setDefaultSectionSize(kTranscriptTableRowHeight);
        vertical->setMinimumSectionSize(kTranscriptTableRowHeight);
    }
    if (QHeaderView* horizontal = table->horizontalHeader()) {
        horizontal->setHighlightSections(false);
        horizontal->setStretchLastSection(false);
        horizontal->setSectionsClickable(true);
        horizontal->setMinimumSectionSize(56);
        horizontal->setSectionResizeMode(QHeaderView::Interactive);
        if (table->columnCount() > kTranscriptColSourceStart) {
            horizontal->resizeSection(kTranscriptColSourceStart, 108);
        }
        if (table->columnCount() > kTranscriptColSourceEnd) {
            horizontal->resizeSection(kTranscriptColSourceEnd, 108);
        }
        if (table->columnCount() > kTranscriptColSpeaker) {
            horizontal->resizeSection(kTranscriptColSpeaker, 96);
        }
        if (table->columnCount() > kTranscriptColText) {
            horizontal->setSectionResizeMode(kTranscriptColText, QHeaderView::Stretch);
        }
        if (table->columnCount() > kTranscriptColEdits) {
            horizontal->resizeSection(kTranscriptColEdits, 92);
        }
    }
}

void TranscriptTab::onTranscriptRowClicked(int row)
{
    if (m_updating || m_suppressSelectionSideEffects ||
        row < 0 || !m_widgets.transcriptTable ||
        row >= m_widgets.transcriptTable->rowCount() ||
        !m_deps.getSelectedClip || !m_deps.seekToTimelineFrame) {
        return;
    }

    const Qt::KeyboardModifiers modifiers = QApplication::keyboardModifiers();
    if (modifiers.testFlag(Qt::ShiftModifier) ||
        modifiers.testFlag(Qt::ControlModifier) ||
        modifiers.testFlag(Qt::MetaModifier)) {
        return;
    }
    if (m_widgets.transcriptTable && m_widgets.transcriptTable->selectionModel() &&
        m_widgets.transcriptTable->selectionModel()->selectedRows().size() > 1) {
        return;
    }

    scheduleSeekToTranscriptRow(row);
}

bool TranscriptTab::transcriptSearchMatchesText(const QString& query, const QString& text) const
{
    return transcriptFuzzySearchScore(query, text) < std::numeric_limits<int>::max();
}

int TranscriptTab::bestFuzzyTranscriptSearchWordId(const QString& query) const
{
    const QString trimmedQuery = query.trimmed();
    if (trimmedQuery.isEmpty()) {
        return -1;
    }

    const QString speakerFilterValue = activeSpeakerFilter();
    const bool hasSpeakerFilter =
        !speakerFilterValue.isEmpty() && speakerFilterValue != QString(kAllSpeakersFilterValue);

    int bestWordId = -1;
    int bestScore = std::numeric_limits<int>::max();
    int bestRenderOrder = std::numeric_limits<int>::max();
    for (const TranscriptRow& row : m_allTranscriptRows) {
        if (row.isGap || row.isOutsideActiveCut || row.isSkipped || row.wordId < 0) {
            continue;
        }
        if (hasSpeakerFilter && row.speaker != speakerFilterValue) {
            continue;
        }
        const int score = transcriptFuzzySearchScore(trimmedQuery, row.text);
        if (score == std::numeric_limits<int>::max()) {
            continue;
        }
        const int renderOrder = row.renderOrder >= 0 ? row.renderOrder : std::numeric_limits<int>::max();
        if (score < bestScore || (score == bestScore && renderOrder < bestRenderOrder)) {
            bestScore = score;
            bestRenderOrder = renderOrder;
            bestWordId = row.wordId;
        }
    }
    return bestWordId;
}

bool TranscriptTab::selectVisibleTranscriptWord(int wordId)
{
    if (!m_widgets.transcriptTable || wordId < 0) {
        return false;
    }
    for (int row = 0; row < m_widgets.transcriptTable->rowCount(); ++row) {
        QTableWidgetItem* item = m_widgets.transcriptTable->item(row, kTranscriptColSourceStart);
        if (!item || item->data(Qt::UserRole + 16).toInt() != wordId) {
            continue;
        }
        m_suppressSelectionSideEffects = true;
        m_widgets.transcriptTable->setCurrentCell(row, kTranscriptColSourceStart);
        m_widgets.transcriptTable->selectRow(row);
        m_suppressSelectionSideEffects = false;
        persistSelectionIdentityFromRow(row);
        m_manualSelectionTimer.restart();
        if (QTableWidgetItem* scrollItem = m_widgets.transcriptTable->item(row, kTranscriptColText)) {
            m_widgets.transcriptTable->scrollToItem(scrollItem, QAbstractItemView::PositionAtCenter);
        }
        scheduleSeekToTranscriptRow(row);
        return true;
    }
    return false;
}

void TranscriptTab::onTranscriptSearchReturnPressed()
{
    if (m_updating || !m_widgets.transcriptSearchFilterLineEdit || !m_widgets.transcriptTable) {
        return;
    }

    const int wordId = bestFuzzyTranscriptSearchWordId(m_widgets.transcriptSearchFilterLineEdit->text());
    if (wordId < 0) {
        return;
    }

    if (m_refreshQueued) {
        m_refreshDebounceTimer.stop();
        m_refreshQueued = false;
    }
    populateTable(filteredRowsForSpeaker(m_allTranscriptRows));
    selectVisibleTranscriptWord(wordId);
}

void TranscriptTab::onTranscriptItemClicked(QTableWidgetItem* item)
{
    if (!item) {
        return;
    }
    onTranscriptRowClicked(item->row());
}

void TranscriptTab::onTranscriptItemDoubleClicked(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    m_deferredSeekTimer.stop();
    m_pendingSeekTimelineFrame = -1;
}

void TranscriptTab::onTranscriptSelectionChanged()
{
    if (m_updating || m_suppressSelectionSideEffects || !m_widgets.transcriptTable) {
        return;
    }
    QItemSelectionModel* selectionModel = m_widgets.transcriptTable->selectionModel();
    if (!selectionModel) {
        return;
    }

    const QModelIndexList selectedRows = selectionModel->selectedRows();
    if (selectedRows.isEmpty()) {
        return;
    }

    m_manualSelectionTimer.restart();

    if (selectedRows.size() != 1) {
        m_deferredSeekTimer.stop();
        m_pendingSeekTimelineFrame = -1;
        return;
    }

    persistSelectionIdentityFromRow(selectedRows.constFirst().row());

    const Qt::KeyboardModifiers modifiers = QApplication::keyboardModifiers();
    if (modifiers.testFlag(Qt::ShiftModifier) ||
        modifiers.testFlag(Qt::ControlModifier) ||
        modifiers.testFlag(Qt::MetaModifier)) {
        return;
    }

    QWidget* focus = QApplication::focusWidget();
    const bool tableHasFocus =
        focus && (focus == m_widgets.transcriptTable || m_widgets.transcriptTable->isAncestorOf(focus));
    if (!tableHasFocus) {
        return;
    }

    scheduleSeekToTranscriptRow(selectedRows.constFirst().row());
}

void TranscriptTab::persistSelectionIdentityFromRow(int row)
{
    if (!m_widgets.transcriptTable || row < 0 || row >= m_widgets.transcriptTable->rowCount()) {
        return;
    }
    QTableWidgetItem* item = m_widgets.transcriptTable->item(row, 0);
    if (!item || item->data(Qt::UserRole + 4).toBool() || item->data(Qt::UserRole + 12).toBool()) {
        return;
    }
    if (const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr) {
        m_persistedSelectedClipId = selectedClip->id;
    }
    m_persistedSelectedSegmentIndex = item->data(Qt::UserRole + 5).toInt();
    m_persistedSelectedWordIndex = item->data(Qt::UserRole + 6).toInt();
    m_persistedSelectedWordId = item->data(Qt::UserRole + 16).toInt();
}

bool TranscriptTab::hasActiveManualSelection() const
{
    if (!m_widgets.transcriptTable) {
        return false;
    }
    QItemSelectionModel* selectionModel = m_widgets.transcriptTable->selectionModel();
    if (!selectionModel) {
        return false;
    }
    const QModelIndexList selectedRows = selectionModel->selectedRows();
    if (selectedRows.size() > 1) {
        return true;
    }
    if (!m_manualSelectionTimer.isValid()) {
        return false;
    }
    return m_manualSelectionTimer.elapsed() < qMax(0, m_manualSelectionHoldMs);
}

void TranscriptTab::setManualSelectionHoldMs(int valueMs)
{
    m_manualSelectionHoldMs = qMax(0, valueMs);
}

void TranscriptTab::onFollowCurrentWordToggled(bool checked)
{
    Q_UNUSED(checked);
    if (m_updating) return;
    applyTabEditEffects(transcriptEditCallbacks(m_deps),
                        TabEditEffects{.updatePreview = false, .pushHistory = false});
}

void TranscriptTab::onPrependMsChanged(int value)
{
    Q_UNUSED(value);
    refresh();
    emit speechFilterParametersChanged();
    applyTabEditEffects(transcriptEditCallbacks(m_deps),
                        TabEditEffects{.updatePreview = false, .refreshInspector = false});
}

void TranscriptTab::onPostpendMsChanged(int value)
{
    Q_UNUSED(value);
    refresh();
    emit speechFilterParametersChanged();
    applyTabEditEffects(transcriptEditCallbacks(m_deps),
                        TabEditEffects{.updatePreview = false, .refreshInspector = false});
}

void TranscriptTab::onOffsetMsChanged(int value)
{
    Q_UNUSED(value);
    refresh();
    emit speechFilterParametersChanged();
    applyTabEditEffects(transcriptEditCallbacks(m_deps),
                        TabEditEffects{.updatePreview = false, .refreshInspector = false});
}

void TranscriptTab::onSpeechFilterEnabledToggled(bool enabled)
{
    m_speechFilterEnabled = enabled;
    emit speechFilterParametersChanged();
    applyTabEditEffects(transcriptEditCallbacks(m_deps),
                        TabEditEffects{.updatePreview = false, .refreshInspector = false});
}

void TranscriptTab::onSpeechFilterFadeSamplesChanged(int value)
{
    m_speechFilterFadeSamples = qMax(0, value);
    emit speechFilterParametersChanged();
    applyTabEditEffects(transcriptEditCallbacks(m_deps),
                        TabEditEffects{.updatePreview = false, .refreshInspector = false});
}

void TranscriptTab::onTranscriptCustomContextMenu(const QPoint& pos)
{
    if (!m_widgets.transcriptTable || m_transcriptSession.transcriptPath().isEmpty() ||
        !m_transcriptSession.hasObjectDocument()) {
        return;
    }

    const QModelIndex index = m_widgets.transcriptTable->indexAt(pos);
    if (!index.isValid()) return;

    const int row = index.row();
    QTableWidgetItem* sourceItem = m_widgets.transcriptTable->item(row, kTranscriptColSourceStart);
    if (!sourceItem) return;
    const bool isGap = sourceItem->data(Qt::UserRole + 4).toBool();
    const bool isOutsideCut = sourceItem->data(Qt::UserRole + 12).toBool();
    if (isGap || isOutsideCut) return;

    QMenu menu;
    QAction* addAbove = nullptr;
    QAction* addBelow = nullptr;
    QAction* expandAction = nullptr;
    QAction* restoreAction = nullptr;
    QAction* skipAction = nullptr;
    QAction* deleteAction = nullptr;
    const bool rowSkipped = sourceItem->data(Qt::UserRole + 7).toBool();
    const int wordId = sourceItem->data(Qt::UserRole + 16).toInt();
    const int originalSegmentIndex = sourceItem->data(Qt::UserRole + 13).toInt();
    const int originalWordIndex = sourceItem->data(Qt::UserRole + 14).toInt();
    if (activeCutMutable()) {
        addAbove = menu.addAction(QStringLiteral("Add Word Above"));
        addBelow = menu.addAction(QStringLiteral("Add Word Below"));
        menu.addSeparator();
        expandAction = menu.addAction(QStringLiteral("Expand Word Timing"));
        restoreAction = menu.addAction(QStringLiteral("Restore Word to Original"));
        restoreAction->setEnabled(wordId >= 0 && originalSegmentIndex >= 0 && originalWordIndex >= 0);
        skipAction = menu.addAction(rowSkipped ? QStringLiteral("Unskip Word")
                                               : QStringLiteral("Skip Word"));
        menu.addSeparator();
        deleteAction = menu.addAction(QStringLiteral("Delete Word"));
    } else {
        QAction* immutableNotice = menu.addAction(QStringLiteral("Original Cut (Immutable)"));
        immutableNotice->setEnabled(false);
        QAction* copyNotice = menu.addAction(QStringLiteral("Use + New Cut to edit words"));
        copyNotice->setEnabled(false);
    }

    QAction* chosen = menu.exec(m_widgets.transcriptTable->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == addAbove) {
        insertWordAtRow(row, true);
    } else if (chosen == addBelow) {
        insertWordAtRow(row, false);
    } else if (chosen == expandAction) {
        expandSelectedRow(row);
    } else if (chosen == restoreAction) {
        restoreWordToOriginalState(wordId);
    } else if (chosen == skipAction) {
        setSelectedRowsSkipped(!rowSkipped);
    } else if (chosen == deleteAction) {
        deleteSelectedRows();
    }
}

void TranscriptTab::onTranscriptHeaderContextMenu(const QPoint& pos)
{
    if (!m_widgets.transcriptTable || !m_widgets.transcriptTable->horizontalHeader()) {
        return;
    }

    QHeaderView* header = m_widgets.transcriptTable->horizontalHeader();
    QMenu menu;
    for (int column = 0; column < m_widgets.transcriptTable->columnCount(); ++column) {
        const QString label = m_widgets.transcriptTable->horizontalHeaderItem(column)
            ? m_widgets.transcriptTable->horizontalHeaderItem(column)->text()
            : QStringLiteral("Column %1").arg(column + 1);
        QAction* action = menu.addAction(label);
        action->setCheckable(true);

        const bool isTextColumn = (column == kTranscriptColText);
        const bool visible = !m_widgets.transcriptTable->isColumnHidden(column);
        action->setChecked(visible);
        if (isTextColumn) {
            action->setEnabled(false);
            action->setToolTip(QStringLiteral("Text column is always visible."));
            continue;
        }

        connect(action, &QAction::toggled, this, [this, column](bool checked) {
            if (!m_widgets.transcriptTable) {
                return;
            }
            m_widgets.transcriptTable->setColumnHidden(column, !checked);
        });
    }

    menu.exec(header->viewport()->mapToGlobal(pos));
}

void TranscriptTab::insertWordAtRow(int row, bool above)
{
    if (!m_widgets.transcriptTable || m_transcriptSession.transcriptPath().isEmpty() ||
        !m_transcriptSession.hasObjectDocument()) {
        return;
    }
    if (!activeCutMutable()) {
        return;
    }

    // Get the times of the word we're inserting relative to
    QTableWidgetItem* currentItem = m_widgets.transcriptTable->item(row, 0);
    if (!currentItem || currentItem->data(Qt::UserRole + 12).toBool()) return;

    const double currentStart = currentItem->data(Qt::UserRole).toDouble();
    const double currentEnd = currentItem->data(Qt::UserRole + 1).toDouble();
    const int currentWordId = currentItem->data(Qt::UserRole + 16).toInt();
    const auto neighborIt = m_wordEditIndex.constFind(currentWordId);
    if (neighborIt == m_wordEditIndex.cend()) {
        return;
    }
    const auto addressIt = m_transcriptWordAddressById.constFind(currentWordId);
    if (addressIt == m_transcriptWordAddressById.cend()) {
        return;
    }
    const int currentSegmentIndex = addressIt->segmentIndex;
    const int currentWordIndex = addressIt->wordIndex;

    double newWordStart, newWordEnd;
    int targetSegmentIndex, targetWordIndex;
    int insertRenderOrder = neighborIt->renderOrder;
    if (insertRenderOrder < 0) {
        insertRenderOrder = row;
    }
    if (!above) {
        ++insertRenderOrder;
    }
    insertRenderOrder = qMax(0, insertRenderOrder);
    const QString currentSpeaker = neighborIt->speaker.trimmed();

    if (above) {
        const double prevEnd = neighborIt->hasPreviousWord ? neighborIt->previousWordEndSeconds : 0.0;
        newWordStart = (prevEnd + currentStart) / 2.0;
        newWordEnd = qMin(newWordStart + 0.1, currentStart - 0.01);
        if (newWordEnd <= newWordStart) {
            newWordStart = currentStart - 0.2;
            newWordEnd = currentStart - 0.05;
        }
        targetSegmentIndex = currentSegmentIndex;
        targetWordIndex = currentWordIndex;
    } else {
        const double nextStart = neighborIt->hasNextWord ? neighborIt->nextWordStartSeconds : (currentEnd + 1.0);
        newWordStart = (currentEnd + nextStart) / 2.0;
        newWordEnd = qMin(newWordStart + 0.1, nextStart - 0.01);
        if (newWordEnd <= newWordStart || newWordStart < currentEnd) {
            newWordStart = currentEnd + 0.05;
            newWordEnd = currentEnd + 0.2;
        }
        targetSegmentIndex = currentSegmentIndex;
        targetWordIndex = currentWordIndex + 1;
    }

    // Ensure valid timing
    newWordStart = qMax(0.0, newWordStart);
    newWordEnd = qMax(newWordStart + 0.01, newWordEnd);

    TranscriptDocumentWord newWord;
    newWord.wordId = m_nextTranscriptWordId++;
    newWord.text = QStringLiteral("[new]");
    newWord.speaker = currentSpeaker;
    newWord.startSeconds = newWordStart;
    newWord.endSeconds = newWordEnd;
    newWord.renderOrder = insertRenderOrder;
    newWord.originalSegmentIndex = -1;
    newWord.originalWordIndex = -1;
    newWord.editTags.push_back(QString(kTranscriptEditInsertedTag));

    if (targetSegmentIndex >= 0 && targetSegmentIndex < m_transcriptDocumentSegments.size()) {
        auto& words = m_transcriptDocumentSegments[targetSegmentIndex].words;
        const int insertIndex = qBound(0, targetWordIndex, words.size());
        words.insert(insertIndex, newWord);
        const auto renderIt = std::find(m_renderOrderedWordIds.begin(), m_renderOrderedWordIds.end(), currentWordId);
        const int renderIndex = renderIt == m_renderOrderedWordIds.end()
            ? m_renderOrderedWordIds.size()
            : static_cast<int>(std::distance(m_renderOrderedWordIds.begin(), renderIt)) + (above ? 0 : 1);
        m_renderOrderedWordIds.insert(renderIndex, newWord.wordId);
        rebuildTranscriptWordAddressIndex();
        normalizeTranscriptRenderOrder();
        rehydrateLoadedTranscriptDocumentFromMemory();
        saveLoadedTranscriptDocument();
        refresh();
        emit transcriptDocumentChanged();
        applyTabEditEffects(transcriptEditCallbacks(m_deps),
                            TabEditEffects{.updatePreview = false, .refreshInspector = false});
    }
}

void TranscriptTab::expandSelectedRow(int row)
{
    if (!m_widgets.transcriptTable || m_transcriptSession.transcriptPath().isEmpty() ||
        !m_transcriptSession.hasObjectDocument()) {
        return;
    }
    if (!activeCutMutable()) {
        return;
    }

    QTableWidgetItem* currentItem = m_widgets.transcriptTable->item(row, 0);
    if (!currentItem) return;

    const bool isGap = currentItem->data(Qt::UserRole + 4).toBool();
    const bool isOutsideCut = currentItem->data(Qt::UserRole + 12).toBool();
    if (isGap || isOutsideCut) return;

    const int wordId = currentItem->data(Qt::UserRole + 16).toInt();
    if (wordId < 0) return;
    const auto neighborIt = m_wordEditIndex.constFind(wordId);
    if (neighborIt == m_wordEditIndex.cend()) {
        return;
    }
    const double newStartTime = neighborIt->previousWordEndSeconds;
    const bool hasPreviousWord = neighborIt->hasPreviousWord;
    const double newEndTime = neighborIt->nextWordStartSeconds;
    const bool hasNextWord = neighborIt->hasNextWord;

    TranscriptDocumentWord* word = transcriptWordById(wordId);
    if (!word) return;

    if (hasPreviousWord) {
        if (!qFuzzyCompare(word->startSeconds + 1.0,
                           newStartTime + 1.0)) {
            if (!word->editTags.contains(QString(kTranscriptEditTimingTag))) {
                word->editTags.push_back(QString(kTranscriptEditTimingTag));
            }
        }
        word->startSeconds = newStartTime;
    }
    if (hasNextWord) {
        if (!qFuzzyCompare(word->endSeconds + 1.0,
                           newEndTime + 1.0)) {
            if (!word->editTags.contains(QString(kTranscriptEditTimingTag))) {
                word->editTags.push_back(QString(kTranscriptEditTimingTag));
            }
        }
        word->endSeconds = newEndTime;
    }
    rehydrateLoadedTranscriptDocumentFromMemory();
    saveLoadedTranscriptDocument();
    refresh();
    emit transcriptDocumentChanged();
    applyTabEditEffects(transcriptEditCallbacks(m_deps),
                        TabEditEffects{.updatePreview = false, .refreshInspector = false});
}

bool TranscriptTab::restoreWordToOriginalState(int wordId)
{
    if (m_updating || wordId < 0 || m_transcriptSession.transcriptPath().isEmpty() ||
        !m_transcriptSession.hasObjectDocument() || !activeCutMutable()) {
        return false;
    }

    TranscriptDocumentWord* word = transcriptWordById(wordId);
    if (!word || word->originalSegmentIndex < 0 || word->originalWordIndex < 0) {
        return false;
    }

    const QString originalPath = originalTranscriptPathForClip(m_transcriptSession.clipFilePath());
    QJsonDocument originalDoc;
    if (originalPath.isEmpty() ||
        !loadTranscriptJsonCached(originalPath, &originalDoc) ||
        !originalDoc.isObject()) {
        return false;
    }

    const QJsonArray segments = originalDoc.object().value(QStringLiteral("segments")).toArray();
    if (word->originalSegmentIndex >= segments.size()) {
        return false;
    }
    const QJsonObject segmentObject = segments.at(word->originalSegmentIndex).toObject();
    const QJsonArray words = segmentObject.value(QStringLiteral("words")).toArray();
    if (word->originalWordIndex >= words.size()) {
        return false;
    }
    const QJsonObject originalWord = words.at(word->originalWordIndex).toObject();
    const QString originalText =
        originalWord.value(QStringLiteral("word"))
            .toString(originalWord.value(QStringLiteral("text")).toString());
    const double originalStart = originalWord.value(QStringLiteral("start")).toDouble(0.0);
    const double originalEnd = qMax(originalStart,
                                    originalWord.value(QStringLiteral("end")).toDouble(originalStart));
    QString originalSpeaker = originalWord.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
    if (originalSpeaker.isEmpty()) {
        originalSpeaker = segmentObject.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
    }
    const bool originalSkipped = originalWord.value(QString(kTranscriptWordSkippedKey)).toBool(false);

    const bool changed =
        word->text != originalText ||
        !qFuzzyCompare(word->startSeconds + 1.0, originalStart + 1.0) ||
        !qFuzzyCompare(word->endSeconds + 1.0, originalEnd + 1.0) ||
        word->speaker.trimmed() != originalSpeaker ||
        word->skipped != originalSkipped ||
        !word->editTags.isEmpty();
    if (!changed) {
        return false;
    }

    word->text = originalText;
    word->startSeconds = originalStart;
    word->endSeconds = originalEnd;
    word->speaker = originalSpeaker;
    word->skipped = originalSkipped;
    word->editTags.clear();

    rehydrateLoadedTranscriptDocumentFromMemory();
    saveLoadedTranscriptDocument();
    refresh();
    emit transcriptDocumentChanged();
    applyTabEditEffects(transcriptEditCallbacks(m_deps),
                        TabEditEffects{.updatePreview = false, .refreshInspector = false});
    return true;
}

bool TranscriptTab::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == m_widgets.transcriptTable ||
         (m_widgets.transcriptTable && watched == m_widgets.transcriptTable->viewport())) &&
        event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Delete || keyEvent->key() == Qt::Key_Backspace) {
            deleteSelectedRows();
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}
