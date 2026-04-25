#include "speakers_tab.h"

#include "decoder_context.h"
#include "transcript_engine.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
const QLatin1String kTranscriptWordSpeakerKey("speaker");
const QLatin1String kTranscriptSegmentSpeakerKey("speaker");
const QLatin1String kTranscriptSpeakerProfilesKey("speaker_profiles");
const QLatin1String kTranscriptSpeakerNameKey("name");
const QLatin1String kTranscriptSpeakerLocationKey("location");
const QLatin1String kTranscriptSpeakerLocationXKey("x");
const QLatin1String kTranscriptSpeakerLocationYKey("y");
const QLatin1String kTranscriptSpeakerTrackingKey("tracking");
const QLatin1String kTranscriptSpeakerTrackingModeKey("mode");
const QLatin1String kTranscriptSpeakerTrackingAutoStateKey("auto_state");
const QLatin1String kTranscriptSpeakerTrackingRef1Key("ref1");
const QLatin1String kTranscriptSpeakerTrackingRef2Key("ref2");
const QLatin1String kTranscriptSpeakerTrackingFrameKey("frame");
const QLatin1String kTranscriptSpeakerTrackingKeyframesKey("keyframes");
const QLatin1String kTranscriptSpeakerTrackingConfidenceKey("confidence");
const QLatin1String kTranscriptSpeakerTrackingSourceKey("source");

QJsonObject transcriptTrackingReferencePoint(const QJsonObject& tracking,
                                             const QLatin1String& key,
                                             bool* okOut)
{
    if (okOut) {
        *okOut = false;
    }
    const QJsonObject ref = tracking.value(QString(key)).toObject();
    if (ref.isEmpty()) {
        return {};
    }
    if (!ref.contains(QString(kTranscriptSpeakerTrackingFrameKey)) ||
        !ref.contains(QString(kTranscriptSpeakerLocationXKey)) ||
        !ref.contains(QString(kTranscriptSpeakerLocationYKey))) {
        return {};
    }
    if (okOut) {
        *okOut = true;
    }
    return ref;
}
}

SpeakersTab::SpeakersTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : TableTabBase(deps, parent)
    , m_widgets(widgets)
{
}

void SpeakersTab::wire()
{
    if (m_widgets.speakersTable) {
        connect(m_widgets.speakersTable, &QTableWidget::itemChanged,
                this, &SpeakersTab::onSpeakersTableItemChanged);
        connect(m_widgets.speakersTable, &QTableWidget::itemClicked,
                this, &SpeakersTab::onSpeakersTableItemClicked);
        connect(m_widgets.speakersTable, &QTableWidget::itemSelectionChanged,
                this, &SpeakersTab::onSpeakersSelectionChanged);
    }
    if (m_widgets.speakerSetReference1Button) {
        connect(m_widgets.speakerSetReference1Button, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerSetReference1Clicked);
    }
    if (m_widgets.speakerSetReference2Button) {
        connect(m_widgets.speakerSetReference2Button, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerSetReference2Clicked);
    }
    if (m_widgets.speakerPickReference1Button) {
        connect(m_widgets.speakerPickReference1Button, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerPickReference1Clicked);
    }
    if (m_widgets.speakerPickReference2Button) {
        connect(m_widgets.speakerPickReference2Button, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerPickReference2Clicked);
    }
    if (m_widgets.speakerPreviousSegmentButton) {
        connect(m_widgets.speakerPreviousSegmentButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerPreviousSegmentClicked);
    }
    if (m_widgets.speakerNextSegmentButton) {
        connect(m_widgets.speakerNextSegmentButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerNextSegmentClicked);
    }
    if (m_widgets.speakerClearReferencesButton) {
        connect(m_widgets.speakerClearReferencesButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerClearReferencesClicked);
    }
    if (m_widgets.speakerRunAutoTrackButton) {
        connect(m_widgets.speakerRunAutoTrackButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerRunAutoTrackClicked);
    }
}

bool SpeakersTab::clipSupportsTranscript(const TimelineClip& clip) const
{
    return clip.mediaType == ClipMediaType::Audio || clip.hasAudio;
}

QString SpeakersTab::originalTranscriptPathForClip(const QString& clipFilePath) const
{
    return transcriptPathForClipFile(clipFilePath);
}

bool SpeakersTab::activeCutMutable() const
{
    if (m_loadedTranscriptPath.isEmpty() || m_loadedClipFilePath.isEmpty()) {
        return false;
    }
    return m_loadedTranscriptPath != originalTranscriptPathForClip(m_loadedClipFilePath);
}

void SpeakersTab::refresh()
{
    m_updating = true;
    m_loadedTranscriptPath.clear();
    m_loadedClipFilePath.clear();
    m_loadedTranscriptDoc = QJsonDocument();
    m_avatarCache.clear();
    m_pendingReferencePick = 0;

    if (m_widgets.speakersTable) {
        m_widgets.speakersTable->clearContents();
        m_widgets.speakersTable->setRowCount(0);
        m_widgets.speakersTable->setEnabled(false);
        m_widgets.speakersTable->setIconSize(QSize(28, 28));
    }

    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || !clipSupportsTranscript(*clip)) {
        if (m_widgets.speakersInspectorClipLabel) {
            m_widgets.speakersInspectorClipLabel->setText(QStringLiteral("No transcript cut selected"));
        }
        if (m_widgets.speakersInspectorDetailsLabel) {
            m_widgets.speakersInspectorDetailsLabel->setText(
                QStringLiteral("Select a transcript cut to name speakers and set on-screen locations."));
        }
        m_updating = false;
        updateSpeakerTrackingStatusLabel();
        return;
    }

    m_loadedClipFilePath = clip->filePath;
    const QString transcriptPath = activeTranscriptPathForClipFile(clip->filePath);
    m_loadedTranscriptPath = transcriptPath;

    QFile transcriptFile(transcriptPath);
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        if (m_widgets.speakersInspectorClipLabel) {
            m_widgets.speakersInspectorClipLabel->setText(QStringLiteral("Speakers\n%1").arg(clip->label));
        }
        if (m_widgets.speakersInspectorDetailsLabel) {
            m_widgets.speakersInspectorDetailsLabel->setText(QStringLiteral("No transcript file found."));
        }
        m_updating = false;
        updateSpeakerTrackingStatusLabel();
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument transcriptDoc = QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
        if (m_widgets.speakersInspectorClipLabel) {
            m_widgets.speakersInspectorClipLabel->setText(QStringLiteral("Speakers\n%1").arg(clip->label));
        }
        if (m_widgets.speakersInspectorDetailsLabel) {
            m_widgets.speakersInspectorDetailsLabel->setText(QStringLiteral("Invalid transcript JSON file."));
        }
        m_updating = false;
        updateSpeakerTrackingStatusLabel();
        return;
    }

    m_loadedTranscriptDoc = transcriptDoc;

    if (m_widgets.speakersInspectorClipLabel) {
        m_widgets.speakersInspectorClipLabel->setText(QStringLiteral("Speakers\n%1").arg(clip->label));
    }
    if (m_widgets.speakersInspectorDetailsLabel) {
        m_widgets.speakersInspectorDetailsLabel->setText(
            activeCutMutable()
                ? QStringLiteral("Edit speaker names, on-screen locations, and tracking references for this cut.")
                : QStringLiteral("Original cut is immutable. Use + New Cut in Transcript to edit speaker metadata."));
    }

    refreshSpeakersTable(transcriptDoc.object());
    if (m_widgets.speakersTable) {
        m_widgets.speakersTable->setEnabled(activeCutMutable());
    }

    m_updating = false;
    updateSpeakerTrackingStatusLabel();
}

void SpeakersTab::refreshSpeakersTable(const QJsonObject& transcriptRoot)
{
    if (!m_widgets.speakersTable) {
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return;
    }

    QSet<QString> speakerIds;
    const QJsonArray segments = transcriptRoot.value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const QString segmentSpeaker = segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        if (!segmentSpeaker.isEmpty()) {
            speakerIds.insert(segmentSpeaker);
        }
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QString wordSpeaker =
                wordValue.toObject().value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (!wordSpeaker.isEmpty()) {
                speakerIds.insert(wordSpeaker);
            }
        }
    }

    QStringList ids = speakerIds.values();
    std::sort(ids.begin(), ids.end(), [](const QString& a, const QString& b) {
        return a.localeAwareCompare(b) < 0;
    });

    const QJsonObject profiles = transcriptRoot.value(QString(kTranscriptSpeakerProfilesKey)).toObject();

    QSignalBlocker blocker(m_widgets.speakersTable);
    m_widgets.speakersTable->setRowCount(ids.size());
    for (int row = 0; row < ids.size(); ++row) {
        const QString id = ids.at(row);
        const QJsonObject profile = profiles.value(id).toObject();
        const QString name = profile.value(QString(kTranscriptSpeakerNameKey)).toString(id);
        const QJsonObject location = profile.value(QString(kTranscriptSpeakerLocationKey)).toObject();
        const double x = location.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5);
        const double y = location.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85);
        const QJsonObject tracking = profile.value(QString(kTranscriptSpeakerTrackingKey)).toObject();
        const QString trackingMode =
            tracking.value(QString(kTranscriptSpeakerTrackingModeKey)).toString(QStringLiteral("Manual"));
        const int keyframeCount =
            tracking.value(QString(kTranscriptSpeakerTrackingKeyframesKey)).toArray().size();

        const auto formatReference = [](const QJsonObject& refObj) -> QString {
            if (refObj.isEmpty()) {
                return QStringLiteral("—");
            }
            const int64_t frame = refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            const double rx = refObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.0);
            const double ry = refObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.0);
            return QStringLiteral("F%1 (%2, %3)")
                .arg(frame)
                .arg(QString::number(rx, 'f', 3))
                .arg(QString::number(ry, 'f', 3));
        };

        const QString ref1 = formatReference(tracking.value(QString(kTranscriptSpeakerTrackingRef1Key)).toObject());
        const QString ref2 = formatReference(tracking.value(QString(kTranscriptSpeakerTrackingRef2Key)).toObject());

        auto* avatarItem = new QTableWidgetItem();
        avatarItem->setFlags(avatarItem->flags() & ~Qt::ItemIsEditable);
        avatarItem->setData(Qt::UserRole, id);
        avatarItem->setIcon(QIcon(speakerAvatarForRow(*clip, transcriptRoot, profile, id)));
        avatarItem->setTextAlignment(Qt::AlignCenter);
        avatarItem->setSizeHint(QSize(32, 32));

        auto* idItem = new QTableWidgetItem(id);
        idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
        idItem->setData(Qt::UserRole, id);
        auto* nameItem = new QTableWidgetItem(name);
        nameItem->setData(Qt::UserRole, id);
        auto* xItem = new QTableWidgetItem(QString::number(x, 'f', 3));
        xItem->setData(Qt::UserRole, id);
        auto* yItem = new QTableWidgetItem(QString::number(y, 'f', 3));
        yItem->setData(Qt::UserRole, id);
        auto* trackingModeItem = new QTableWidgetItem(
            keyframeCount > 0
                ? QStringLiteral("%1 (%2 keys)").arg(trackingMode).arg(keyframeCount)
                : trackingMode);
        trackingModeItem->setFlags(trackingModeItem->flags() & ~Qt::ItemIsEditable);
        trackingModeItem->setData(Qt::UserRole, id);
        auto* ref1Item = new QTableWidgetItem(ref1);
        ref1Item->setFlags(ref1Item->flags() & ~Qt::ItemIsEditable);
        ref1Item->setData(Qt::UserRole, id);
        auto* ref2Item = new QTableWidgetItem(ref2);
        ref2Item->setFlags(ref2Item->flags() & ~Qt::ItemIsEditable);
        ref2Item->setData(Qt::UserRole, id);

        m_widgets.speakersTable->setItem(row, 0, avatarItem);
        m_widgets.speakersTable->setItem(row, 1, idItem);
        m_widgets.speakersTable->setItem(row, 2, nameItem);
        m_widgets.speakersTable->setItem(row, 3, xItem);
        m_widgets.speakersTable->setItem(row, 4, yItem);
        m_widgets.speakersTable->setItem(row, 5, trackingModeItem);
        m_widgets.speakersTable->setItem(row, 6, ref1Item);
        m_widgets.speakersTable->setItem(row, 7, ref2Item);
        m_widgets.speakersTable->setRowHeight(row, 34);
    }
}

QString SpeakersTab::selectedSpeakerId() const
{
    if (!m_widgets.speakersTable || !m_widgets.speakersTable->selectionModel()) {
        return QString();
    }
    const QModelIndexList selectedRows = m_widgets.speakersTable->selectionModel()->selectedRows();
    if (selectedRows.isEmpty()) {
        return QString();
    }
    QTableWidgetItem* idItem = m_widgets.speakersTable->item(selectedRows.constFirst().row(), 1);
    if (!idItem) {
        return QString();
    }
    return idItem->data(Qt::UserRole).toString().trimmed();
}

int64_t SpeakersTab::firstSourceFrameForSpeaker(const QJsonObject& transcriptRoot,
                                                const QString& speakerId) const
{
    if (speakerId.trimmed().isEmpty()) {
        return -1;
    }
    const QString targetSpeaker = speakerId.trimmed();
    const QJsonArray segments = transcriptRoot.value(QStringLiteral("segments")).toArray();
    double earliestStartSeconds = std::numeric_limits<double>::max();
    bool found = false;
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            QString wordSpeaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (wordSpeaker.isEmpty()) {
                wordSpeaker = segmentSpeaker;
            }
            if (wordSpeaker != targetSpeaker) {
                continue;
            }
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            if (startSeconds < 0.0) {
                continue;
            }
            if (startSeconds < earliestStartSeconds) {
                earliestStartSeconds = startSeconds;
                found = true;
            }
        }
    }
    if (!found) {
        return -1;
    }
    return qMax<int64_t>(0, static_cast<int64_t>(std::floor(earliestStartSeconds * kTimelineFps)));
}

QPixmap SpeakersTab::placeholderSpeakerAvatar(const QString& speakerId) const
{
    QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#243447")));
    painter.drawEllipse(QRectF(1.0, 1.0, 30.0, 30.0));
    painter.setPen(QColor(QStringLiteral("#d8e6f5")));
    QFont font = painter.font();
    font.setBold(true);
    font.setPointSize(10);
    painter.setFont(font);
    const QString fallback = speakerId.trimmed().isEmpty() ? QStringLiteral("?") : speakerId.left(1).toUpper();
    painter.drawText(QRect(0, 0, 32, 32), Qt::AlignCenter, fallback);
    return QPixmap::fromImage(image);
}

QPixmap SpeakersTab::speakerAvatarForRow(const TimelineClip& clip,
                                         const QJsonObject& transcriptRoot,
                                         const QJsonObject& profile,
                                         const QString& speakerId)
{
    const int64_t sourceFrame30 = firstSourceFrameForSpeaker(transcriptRoot, speakerId);
    if (sourceFrame30 < 0) {
        return placeholderSpeakerAvatar(speakerId);
    }

    const QJsonObject locationObj = profile.value(QString(kTranscriptSpeakerLocationKey)).toObject();
    qreal locX = qBound<qreal>(0.0, locationObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    qreal locY = qBound<qreal>(0.0, locationObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    const QString cacheKey = QStringLiteral("%1|%2|%3|%4|%5")
        .arg(m_loadedTranscriptPath)
        .arg(speakerId)
        .arg(sourceFrame30)
        .arg(static_cast<int>(std::round(locX * 1000.0)))
        .arg(static_cast<int>(std::round(locY * 1000.0)));
    const auto cached = m_avatarCache.constFind(cacheKey);
    if (cached != m_avatarCache.cend()) {
        return cached.value();
    }

    const qreal sourceFps = resolvedSourceFps(clip);
    const int64_t decodeFrame =
        qMax<int64_t>(0, static_cast<int64_t>(std::floor((static_cast<qreal>(sourceFrame30) / kTimelineFps) * sourceFps)));
    const QString mediaPath = interactivePreviewMediaPathForClip(clip);

    editor::DecoderContext ctx(mediaPath);
    QPixmap avatar = placeholderSpeakerAvatar(speakerId);
    if (ctx.initialize()) {
        const editor::FrameHandle frame = ctx.decodeFrame(decodeFrame);
        const QImage image = frame.hasCpuImage() ? frame.cpuImage() : QImage();
        if (!image.isNull()) {
            const bool hasSections = QFileInfo::exists(m_loadedTranscriptPath);
            if (hasSections) {
                const QVector<TranscriptSection> sections = loadTranscriptSections(m_loadedTranscriptPath);
                bool resolved = false;
                const QPointF tracked = transcriptSpeakerLocationForSourceFrame(
                    m_loadedTranscriptPath, sections, sourceFrame30, &resolved);
                if (resolved) {
                    locX = qBound<qreal>(0.0, tracked.x(), 1.0);
                    locY = qBound<qreal>(0.0, tracked.y(), 1.0);
                }
            }

            const int width = image.width();
            const int height = image.height();
            if (width > 0 && height > 0) {
                const int side = qMax(48, qMin(width, height) / 3);
                int cx = static_cast<int>(std::round(locX * static_cast<qreal>(width)));
                int cy = static_cast<int>(std::round(locY * static_cast<qreal>(height)));
                int left = qBound(0, cx - (side / 2), qMax(0, width - side));
                int top = qBound(0, cy - (side / 2), qMax(0, height - side));
                QImage crop = image.copy(QRect(left, top, qMin(side, width), qMin(side, height)))
                                  .scaled(32, 32,
                                          Qt::KeepAspectRatioByExpanding,
                                          Qt::SmoothTransformation)
                                  .convertToFormat(QImage::Format_ARGB32_Premultiplied);
                if (!crop.isNull()) {
                    QImage rounded(32, 32, QImage::Format_ARGB32_Premultiplied);
                    rounded.fill(Qt::transparent);
                    QPainter painter(&rounded);
                    painter.setRenderHint(QPainter::Antialiasing, true);
                    QPainterPath path;
                    path.addEllipse(1.0, 1.0, 30.0, 30.0);
                    painter.setClipPath(path);
                    painter.drawImage(QRect(0, 0, 32, 32), crop);
                    painter.setClipping(false);
                    painter.setPen(QPen(QColor(QStringLiteral("#5e89b3")), 1.0));
                    painter.setBrush(Qt::NoBrush);
                    painter.drawEllipse(QRectF(1.0, 1.0, 30.0, 30.0));
                    avatar = QPixmap::fromImage(rounded);
                }
            }
        }
    }
    m_avatarCache.insert(cacheKey, avatar);
    return avatar;
}

QString SpeakersTab::speakerTrackingSummary(const QJsonObject& profile) const
{
    const QJsonObject tracking = profile.value(QString(kTranscriptSpeakerTrackingKey)).toObject();
    if (tracking.isEmpty()) {
        return QStringLiteral("Tracking: Manual (no references)");
    }
    const auto refSummary = [](const QJsonObject& refObj) -> QString {
        if (refObj.isEmpty()) {
            return QStringLiteral("unset");
        }
        const int64_t frame = refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
        const double x = refObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.0);
        const double y = refObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.0);
        return QStringLiteral("F%1 (%2, %3)")
            .arg(frame)
            .arg(QString::number(x, 'f', 3))
            .arg(QString::number(y, 'f', 3));
    };

    const QString mode = tracking.value(QString(kTranscriptSpeakerTrackingModeKey)).toString(QStringLiteral("Manual"));
    const QString autoState = tracking.value(QString(kTranscriptSpeakerTrackingAutoStateKey)).toString();
    const int keyframeCount = tracking.value(QString(kTranscriptSpeakerTrackingKeyframesKey)).toArray().size();
    QString summary = QStringLiteral("Tracking: %1 | Ref1=%2 | Ref2=%3")
        .arg(mode)
        .arg(refSummary(tracking.value(QString(kTranscriptSpeakerTrackingRef1Key)).toObject()))
        .arg(refSummary(tracking.value(QString(kTranscriptSpeakerTrackingRef2Key)).toObject()));
    if (keyframeCount > 0) {
        summary += QStringLiteral(" | Keys=%1").arg(keyframeCount);
    }
    if (!autoState.isEmpty()) {
        summary += QStringLiteral(" | Auto=%1").arg(autoState);
    }
    return summary;
}

void SpeakersTab::updateSpeakerTrackingStatusLabel()
{
    if (!m_widgets.speakerTrackingStatusLabel) {
        return;
    }

    const bool mutableCut = activeCutMutable();
    const QString speakerId = selectedSpeakerId();
    const bool canEdit = mutableCut && !speakerId.isEmpty();

    if (m_widgets.speakerSetReference1Button) {
        m_widgets.speakerSetReference1Button->setEnabled(canEdit);
    }
    if (m_widgets.speakerSetReference2Button) {
        m_widgets.speakerSetReference2Button->setEnabled(canEdit);
    }
    if (m_widgets.speakerClearReferencesButton) {
        m_widgets.speakerClearReferencesButton->setEnabled(canEdit);
    }
    if (m_widgets.speakerRunAutoTrackButton) {
        m_widgets.speakerRunAutoTrackButton->setEnabled(canEdit);
    }
    if (m_widgets.speakerPreviousSegmentButton) {
        m_widgets.speakerPreviousSegmentButton->setEnabled(!speakerId.isEmpty());
    }
    if (m_widgets.speakerNextSegmentButton) {
        m_widgets.speakerNextSegmentButton->setEnabled(!speakerId.isEmpty());
    }
    if (m_widgets.speakerPickReference1Button) {
        m_widgets.speakerPickReference1Button->setEnabled(canEdit);
        m_widgets.speakerPickReference1Button->setText(
            m_pendingReferencePick == 1
                ? QStringLiteral("Picking Ref 1...")
                : QStringLiteral("Pick Ref 1 (Shift+Click)"));
    }
    if (m_widgets.speakerPickReference2Button) {
        m_widgets.speakerPickReference2Button->setEnabled(canEdit);
        m_widgets.speakerPickReference2Button->setText(
            m_pendingReferencePick == 2
                ? QStringLiteral("Picking Ref 2...")
                : QStringLiteral("Pick Ref 2 (Shift+Click)"));
    }

    if (!mutableCut) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Tracking references are editable only on derived cuts (not Original)."));
        return;
    }
    if (speakerId.isEmpty()) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Select one speaker row, then set Ref 1 / Ref 2 at the current playhead frame."));
        return;
    }
    if (!m_loadedTranscriptDoc.isObject()) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Tracking scaffold ready. Transcript not loaded."));
        return;
    }

    const QJsonObject profiles =
        m_loadedTranscriptDoc.object().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    if (m_pendingReferencePick == 1 || m_pendingReferencePick == 2) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Pick Ref %1 armed for %2. Hold Shift and click speaker position in Preview.")
                .arg(m_pendingReferencePick)
                .arg(speakerId));
        return;
    }
    m_widgets.speakerTrackingStatusLabel->setText(
        QStringLiteral("%1 | Speaker=%2 | Tip: Shift+Click in Preview to set picked reference.")
            .arg(speakerTrackingSummary(profile), speakerId));
}

bool SpeakersTab::saveSpeakerProfileEdit(int tableRow, int column, const QString& valueText)
{
    if (!m_widgets.speakersTable || !m_loadedTranscriptDoc.isObject()) {
        return false;
    }
    QTableWidgetItem* idItem = m_widgets.speakersTable->item(tableRow, 1);
    if (!idItem) {
        return false;
    }
    const QString speakerId = idItem->data(Qt::UserRole).toString().trimmed();
    if (speakerId.isEmpty()) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    QJsonObject location = profile.value(QString(kTranscriptSpeakerLocationKey)).toObject();

    if (column == 2) {
        profile[QString(kTranscriptSpeakerNameKey)] = valueText.trimmed().isEmpty() ? speakerId : valueText.trimmed();
    } else if (column == 3 || column == 4) {
        bool ok = false;
        const double parsed = valueText.toDouble(&ok);
        if (!ok) {
            return false;
        }
        const double bounded = qBound(0.0, parsed, 1.0);
        if (column == 3) {
            location[QString(kTranscriptSpeakerLocationXKey)] = bounded;
        } else {
            location[QString(kTranscriptSpeakerLocationYKey)] = bounded;
        }
        profile[QString(kTranscriptSpeakerLocationKey)] = location;
    } else {
        return false;
    }

    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);

    editor::TranscriptEngine engine;
    if (!engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
        return false;
    }
    return true;
}

bool SpeakersTab::saveSpeakerTrackingReferenceAt(const QString& speakerId,
                                                 int referenceIndex,
                                                 int64_t frame,
                                                 qreal xNorm,
                                                 qreal yNorm)
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.isEmpty()) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    QJsonObject location = profile.value(QString(kTranscriptSpeakerLocationKey)).toObject();
    const double x = qBound(0.0, static_cast<double>(xNorm), 1.0);
    const double y = qBound(0.0, static_cast<double>(yNorm), 1.0);
    location[QString(kTranscriptSpeakerLocationXKey)] = x;
    location[QString(kTranscriptSpeakerLocationYKey)] = y;
    profile[QString(kTranscriptSpeakerLocationKey)] = location;

    QJsonObject tracking = profile.value(QString(kTranscriptSpeakerTrackingKey)).toObject();
    tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("ReferencePoints");
    tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] = QStringLiteral("refs_updated");

    QJsonObject refObj;
    refObj[QString(kTranscriptSpeakerTrackingFrameKey)] = static_cast<qint64>(frame);
    refObj[QString(kTranscriptSpeakerLocationXKey)] = x;
    refObj[QString(kTranscriptSpeakerLocationYKey)] = y;
    if (referenceIndex == 1) {
        tracking[QString(kTranscriptSpeakerTrackingRef1Key)] = refObj;
    } else {
        tracking[QString(kTranscriptSpeakerTrackingRef2Key)] = refObj;
    }

    profile[QString(kTranscriptSpeakerTrackingKey)] = tracking;
    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);

    editor::TranscriptEngine engine;
    return engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc);
}

bool SpeakersTab::saveSpeakerTrackingReference(const QString& speakerId, int referenceIndex)
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.isEmpty()) {
        return false;
    }

    const int64_t frame = m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : 0;

    QJsonObject root = m_loadedTranscriptDoc.object();
    const QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QJsonObject location = profile.value(QString(kTranscriptSpeakerLocationKey)).toObject();
    const qreal x = qBound(0.0, location.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    const qreal y = qBound(0.0, location.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    return saveSpeakerTrackingReferenceAt(speakerId, referenceIndex, frame, x, y);
}

bool SpeakersTab::clearSpeakerTrackingReferences(const QString& speakerId)
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.isEmpty()) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    profile.remove(QString(kTranscriptSpeakerTrackingKey));
    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);

    editor::TranscriptEngine engine;
    return engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc);
}

void SpeakersTab::onSpeakersTableItemChanged(QTableWidgetItem* item)
{
    if (m_updating || !item || !activeCutMutable()) {
        return;
    }
    if (!saveSpeakerProfileEdit(item->row(), item->column(), item->text())) {
        refresh();
        return;
    }

    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    updateSpeakerTrackingStatusLabel();
}

void SpeakersTab::onSpeakersSelectionChanged()
{
    if (m_updating) {
        return;
    }
    updateSpeakerTrackingStatusLabel();
}

void SpeakersTab::onSpeakersTableItemClicked(QTableWidgetItem* item)
{
    if (m_updating || !item || !m_widgets.speakersTable) {
        return;
    }
    // Preserve in-place editing workflow for editable columns.
    if (item->column() == 2 || item->column() == 3 || item->column() == 4) {
        return;
    }
    const QString speakerId = selectedSpeakerId();
    if (!speakerId.isEmpty()) {
        seekToSpeakerFirstWord(speakerId);
    }
}

void SpeakersTab::seekToSpeakerFirstWord(const QString& speakerId)
{
    if (speakerId.isEmpty() || !m_loadedTranscriptDoc.isObject() || !m_deps.seekToTimelineFrame) {
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return;
    }

    const QString targetSpeaker = speakerId.trimmed();
    if (targetSpeaker.isEmpty()) {
        return;
    }

    const QJsonArray segments = m_loadedTranscriptDoc.object().value(QStringLiteral("segments")).toArray();
    double earliestStartSeconds = std::numeric_limits<double>::max();
    bool found = false;
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            QString wordSpeaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (wordSpeaker.isEmpty()) {
                wordSpeaker = segmentSpeaker;
            }
            if (wordSpeaker != targetSpeaker) {
                continue;
            }
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            if (startSeconds < 0.0) {
                continue;
            }
            if (startSeconds < earliestStartSeconds) {
                earliestStartSeconds = startSeconds;
                found = true;
            }
        }
    }

    if (!found) {
        return;
    }

    const int64_t sourceFrame =
        qMax<int64_t>(0, static_cast<int64_t>(std::floor(earliestStartSeconds * kTimelineFps)));
    const int64_t timelineFrame = qMax<int64_t>(
        clip->startFrame, clip->startFrame + (sourceFrame - clip->sourceInFrame));
    m_deps.seekToTimelineFrame(timelineFrame);
}

void SpeakersTab::onSpeakerSetReference1Clicked()
{
    if (!activeCutMutable()) {
        return;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    if (!saveSpeakerTrackingReference(speakerId, 1)) {
        refresh();
        return;
    }

    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    refresh();
}

void SpeakersTab::onSpeakerSetReference2Clicked()
{
    if (!activeCutMutable()) {
        return;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    if (!saveSpeakerTrackingReference(speakerId, 2)) {
        refresh();
        return;
    }

    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    refresh();
}

void SpeakersTab::onSpeakerPickReference1Clicked()
{
    if (!activeCutMutable()) {
        return;
    }
    m_pendingReferencePick = (m_pendingReferencePick == 1) ? 0 : 1;
    updateSpeakerTrackingStatusLabel();
}

void SpeakersTab::onSpeakerPickReference2Clicked()
{
    if (!activeCutMutable()) {
        return;
    }
    m_pendingReferencePick = (m_pendingReferencePick == 2) ? 0 : 2;
    updateSpeakerTrackingStatusLabel();
}

bool SpeakersTab::seekToSpeakerSegmentRelative(const QString& speakerId, int direction)
{
    if (speakerId.isEmpty() || direction == 0 || !m_loadedTranscriptDoc.isObject() || !m_deps.seekToTimelineFrame) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }

    const int64_t currentTimeline = m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : clip->startFrame;
    const int64_t currentSourceFrame = qMax<int64_t>(0, clip->sourceInFrame + (currentTimeline - clip->startFrame));
    QVector<int64_t> speakerFrames;
    const QString targetSpeaker = speakerId.trimmed();
    const QJsonArray segments = m_loadedTranscriptDoc.object().value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            QString wordSpeaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (wordSpeaker.isEmpty()) {
                wordSpeaker = segmentSpeaker;
            }
            if (wordSpeaker != targetSpeaker) {
                continue;
            }
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            if (startSeconds < 0.0) {
                continue;
            }
            const int64_t sourceFrame =
                qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)));
            speakerFrames.push_back(sourceFrame);
        }
    }
    if (speakerFrames.isEmpty()) {
        return false;
    }
    std::sort(speakerFrames.begin(), speakerFrames.end());
    speakerFrames.erase(std::unique(speakerFrames.begin(), speakerFrames.end()), speakerFrames.end());

    int64_t chosenSource = -1;
    if (direction > 0) {
        for (int64_t sourceFrame : speakerFrames) {
            if (sourceFrame > currentSourceFrame) {
                chosenSource = sourceFrame;
                break;
            }
        }
        if (chosenSource < 0) {
            chosenSource = speakerFrames.constFirst();
        }
    } else {
        for (int i = speakerFrames.size() - 1; i >= 0; --i) {
            if (speakerFrames.at(i) < currentSourceFrame) {
                chosenSource = speakerFrames.at(i);
                break;
            }
        }
        if (chosenSource < 0) {
            chosenSource = speakerFrames.constLast();
        }
    }

    const int64_t timelineFrame = qMax<int64_t>(
        clip->startFrame, clip->startFrame + (chosenSource - clip->sourceInFrame));
    m_deps.seekToTimelineFrame(timelineFrame);
    return true;
}

void SpeakersTab::onSpeakerPreviousSegmentClicked()
{
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    seekToSpeakerSegmentRelative(speakerId, -1);
}

void SpeakersTab::onSpeakerNextSegmentClicked()
{
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    seekToSpeakerSegmentRelative(speakerId, +1);
}

void SpeakersTab::onSpeakerClearReferencesClicked()
{
    if (!activeCutMutable()) {
        return;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    if (!clearSpeakerTrackingReferences(speakerId)) {
        refresh();
        return;
    }

    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    refresh();
}

void SpeakersTab::onSpeakerRunAutoTrackClicked()
{
    if (!activeCutMutable()) {
        return;
    }

    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    QJsonObject tracking = profile.value(QString(kTranscriptSpeakerTrackingKey)).toObject();
    if (tracking.isEmpty()) {
        tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("ReferencePoints");
    }

    bool hasRef1 = false;
    bool hasRef2 = false;
    const QJsonObject ref1 =
        transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef1Key, &hasRef1);
    const QJsonObject ref2 =
        transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef2Key, &hasRef2);

    if (!hasRef1 && !hasRef2) {
        QMessageBox::warning(
            nullptr,
            QStringLiteral("Auto-Track Speakers"),
            QStringLiteral("Set at least one reference point (Ref 1 or Ref 2) before running auto-track."));
        return;
    }

    const auto createPoint = [](int64_t frame, double x, double y, double confidence, const QString& source) {
        QJsonObject p;
        p[QString(kTranscriptSpeakerTrackingFrameKey)] = static_cast<qint64>(frame);
        p[QString(kTranscriptSpeakerLocationXKey)] = qBound(0.0, x, 1.0);
        p[QString(kTranscriptSpeakerLocationYKey)] = qBound(0.0, y, 1.0);
        p[QString(kTranscriptSpeakerTrackingConfidenceKey)] = qBound(0.0, confidence, 1.0);
        p[QString(kTranscriptSpeakerTrackingSourceKey)] = source;
        return p;
    };

    QJsonArray keyframes;
    if (hasRef1 && hasRef2) {
        const int64_t frame1 = ref1.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
        const int64_t frame2 = ref2.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
        const double x1 = qBound(0.0, ref1.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
        const double y1 = qBound(0.0, ref1.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
        const double x2 = qBound(0.0, ref2.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
        const double y2 = qBound(0.0, ref2.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);

        const int64_t startFrame = qMin(frame1, frame2);
        const int64_t endFrame = qMax(frame1, frame2);
        const int64_t span = qMax<int64_t>(1, endFrame - startFrame);
        constexpr int64_t kStepFrames = 15;

        for (int64_t frame = startFrame; frame <= endFrame; frame += kStepFrames) {
            const double t = static_cast<double>(frame - startFrame) / static_cast<double>(span);
            const double x = x1 + (x2 - x1) * t;
            const double y = y1 + (y2 - y1) * t;
            keyframes.push_back(createPoint(frame, x, y, 0.70, QStringLiteral("autotrack_linear_v1")));
        }
        const int64_t lastFrame = keyframes.isEmpty()
            ? -1
            : keyframes.at(keyframes.size() - 1)
                  .toObject()
                  .value(QString(kTranscriptSpeakerTrackingFrameKey))
                  .toVariant()
                  .toLongLong();
        if (keyframes.isEmpty() || lastFrame != endFrame) {
            keyframes.push_back(createPoint(endFrame, x2, y2, 0.70, QStringLiteral("autotrack_linear_v1")));
        }
        tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("AutoTrackLinear");
        tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] =
            QStringLiteral("completed_linear_v1_open_for_model_tracking");
    } else {
        const QJsonObject onlyRef = hasRef1 ? ref1 : ref2;
        const int64_t frame = onlyRef.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
        const double x = qBound(0.0, onlyRef.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
        const double y = qBound(0.0, onlyRef.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
        keyframes.push_back(createPoint(frame, x, y, 0.60, QStringLiteral("autotrack_anchor_v1")));
        tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("AnchorHold");
        tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] =
            QStringLiteral("completed_anchor_v1_open_for_model_tracking");
    }

    tracking[QString(kTranscriptSpeakerTrackingKeyframesKey)] = keyframes;
    profile[QString(kTranscriptSpeakerTrackingKey)] = tracking;
    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);

    editor::TranscriptEngine engine;
    if (!engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
        refresh();
        return;
    }

    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    refresh();
}

bool SpeakersTab::handlePreviewPoint(const QString& clipId, qreal xNorm, qreal yNorm)
{
    if (m_pendingReferencePick <= 0 || m_pendingReferencePick > 2 || !activeCutMutable()) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || clip->id != clipId) {
        return false;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        m_pendingReferencePick = 0;
        updateSpeakerTrackingStatusLabel();
        return false;
    }
    const int64_t frame = m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : clip->startFrame;
    if (!saveSpeakerTrackingReferenceAt(speakerId, m_pendingReferencePick, frame, xNorm, yNorm)) {
        m_pendingReferencePick = 0;
        refresh();
        return false;
    }

    m_pendingReferencePick = 0;
    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    refresh();
    return true;
}
