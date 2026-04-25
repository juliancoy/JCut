#include "speakers_tab.h"

#include "decoder_context.h"
#include "transcript_engine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QEvent>
#include <QDialog>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QImage>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSet>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cstdlib>
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
const QLatin1String kTranscriptSpeakerFramingKey("framing");
const QLatin1String kTranscriptSpeakerTrackingKey("tracking");
const QLatin1String kTranscriptSpeakerTrackingModeKey("mode");
const QLatin1String kTranscriptSpeakerTrackingEnabledKey("enabled");
const QLatin1String kTranscriptSpeakerTrackingAutoStateKey("auto_state");
const QLatin1String kTranscriptSpeakerTrackingRef1Key("ref1");
const QLatin1String kTranscriptSpeakerTrackingRef2Key("ref2");
const QLatin1String kTranscriptSpeakerTrackingFrameKey("frame");
const QLatin1String kTranscriptSpeakerTrackingBoxSizeKey("box_size");
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

bool transcriptTrackingEnabled(const QJsonObject& tracking)
{
    if (tracking.isEmpty()) {
        return false;
    }
    const QString mode = tracking.value(QString(kTranscriptSpeakerTrackingModeKey))
                             .toString(QStringLiteral("Manual"))
                             .trimmed();
    const bool modeCanTrack =
        mode.compare(QStringLiteral("manual"), Qt::CaseInsensitive) != 0 &&
        mode.compare(QStringLiteral("referencepoints"), Qt::CaseInsensitive) != 0;
    if (!modeCanTrack) {
        return false;
    }
    if (tracking.contains(QString(kTranscriptSpeakerTrackingEnabledKey))) {
        return tracking.value(QString(kTranscriptSpeakerTrackingEnabledKey)).toBool(false);
    }
    // Backward compatibility for legacy transcripts without explicit enable state.
    return !tracking.value(QString(kTranscriptSpeakerTrackingKeyframesKey)).toArray().isEmpty();
}

bool transcriptTrackingHasPointstream(const QJsonObject& tracking)
{
    return !tracking.value(QString(kTranscriptSpeakerTrackingKeyframesKey)).toArray().isEmpty();
}

QJsonObject speakerFramingObject(const QJsonObject& profile)
{
    QJsonObject framing = profile.value(QString(kTranscriptSpeakerFramingKey)).toObject();
    if (framing.isEmpty()) {
        framing = profile.value(QString(kTranscriptSpeakerTrackingKey)).toObject();
    }
    return framing;
}

void setSpeakerFramingObject(QJsonObject& profile, const QJsonObject& framing)
{
    profile[QString(kTranscriptSpeakerFramingKey)] = framing;
    // Keep legacy key mirrored for backward compatibility with older builds/tools.
    profile[QString(kTranscriptSpeakerTrackingKey)] = framing;
}

int64_t canonicalToSourceFrameForTracking(int64_t frame30, double sourceFps)
{
    const double fps = sourceFps > 0.0 ? sourceFps : 30.0;
    return qMax<int64_t>(
        0, static_cast<int64_t>(std::floor((static_cast<double>(frame30) / 30.0) * fps)));
}

QImage toGray8(const QImage& image)
{
    if (image.isNull()) {
        return {};
    }
    if (image.format() == QImage::Format_Grayscale8) {
        return image;
    }
    return image.convertToFormat(QImage::Format_Grayscale8);
}

bool cropSquareGray(const QImage& frameGray,
                    qreal xNorm,
                    qreal yNorm,
                    qreal boxNorm,
                    QImage* outCrop,
                    int* outLeft,
                    int* outTop,
                    int* outSide)
{
    if (!outCrop || frameGray.isNull()) {
        return false;
    }
    const int w = frameGray.width();
    const int h = frameGray.height();
    const int minSide = qMax(1, qMin(w, h));
    int side = static_cast<int>(std::round(qBound<qreal>(0.01, boxNorm, 1.0) * static_cast<qreal>(minSide)));
    side = qBound(20, side, minSide);
    const int cx = static_cast<int>(std::round(qBound<qreal>(0.0, xNorm, 1.0) * static_cast<qreal>(w)));
    const int cy = static_cast<int>(std::round(qBound<qreal>(0.0, yNorm, 1.0) * static_cast<qreal>(h)));
    const int left = qBound(0, cx - (side / 2), qMax(0, w - side));
    const int top = qBound(0, cy - (side / 2), qMax(0, h - side));
    *outCrop = frameGray.copy(left, top, side, side);
    if (outLeft) {
        *outLeft = left;
    }
    if (outTop) {
        *outTop = top;
    }
    if (outSide) {
        *outSide = side;
    }
    return !outCrop->isNull();
}

bool trackTemplateSad(const QImage& frameGray,
                      qreal predictedX,
                      qreal predictedY,
                      qreal predictedBox,
                      const QImage& tmplGray,
                      qreal searchScale,
                      int scanStrideCap,
                      qreal* outX,
                      qreal* outY,
                      qreal* outBox,
                      qreal* outConfidence)
{
    if (frameGray.isNull() || tmplGray.isNull() || tmplGray.width() < 8 || tmplGray.height() < 8) {
        return false;
    }

    const int fw = frameGray.width();
    const int fh = frameGray.height();
    const int minSide = qMax(1, qMin(fw, fh));
    const int tmplW = tmplGray.width();
    const int tmplH = tmplGray.height();
    const int cx = static_cast<int>(std::round(qBound<qreal>(0.0, predictedX, 1.0) * static_cast<qreal>(fw)));
    const int cy = static_cast<int>(std::round(qBound<qreal>(0.0, predictedY, 1.0) * static_cast<qreal>(fh)));

    int side = static_cast<int>(std::round(qBound<qreal>(0.01, predictedBox, 1.0) * static_cast<qreal>(minSide)));
    side = qBound(qMax(tmplW, tmplH), side, minSide);
    const qreal boundedSearchScale = qBound<qreal>(1.0, searchScale, 8.0);
    int searchSide = qBound(side, static_cast<int>(std::round(side * boundedSearchScale)), minSide);
    searchSide = qMax(searchSide, qMax(tmplW + 2, tmplH + 2));
    const int searchLeft = qBound(0, cx - (searchSide / 2), qMax(0, fw - searchSide));
    const int searchTop = qBound(0, cy - (searchSide / 2), qMax(0, fh - searchSide));
    const int searchRight = searchLeft + searchSide - tmplW;
    const int searchBottom = searchTop + searchSide - tmplH;
    if (searchRight < searchLeft || searchBottom < searchTop) {
        return false;
    }

    const int sampleStride = qBound(1, tmplW / 36, 3);
    const int scanStride = qBound(1, tmplW / 20, qMax(1, scanStrideCap));
    const uchar* frameBits = frameGray.constBits();
    const int frameStride = frameGray.bytesPerLine();
    const uchar* tmplBits = tmplGray.constBits();
    const int tmplStride = tmplGray.bytesPerLine();

    qint64 bestSad = std::numeric_limits<qint64>::max();
    int bestX = searchLeft;
    int bestY = searchTop;
    for (int y = searchTop; y <= searchBottom; y += scanStride) {
        for (int x = searchLeft; x <= searchRight; x += scanStride) {
            qint64 sad = 0;
            for (int ty = 0; ty < tmplH; ty += sampleStride) {
                const uchar* fRow = frameBits + ((y + ty) * frameStride) + x;
                const uchar* tRow = tmplBits + (ty * tmplStride);
                for (int tx = 0; tx < tmplW; tx += sampleStride) {
                    sad += std::llabs(static_cast<qint64>(fRow[tx]) - static_cast<qint64>(tRow[tx]));
                }
                if (sad >= bestSad) {
                    break;
                }
            }
            if (sad < bestSad) {
                bestSad = sad;
                bestX = x;
                bestY = y;
            }
        }
    }

    const int matchCx = bestX + (tmplW / 2);
    const int matchCy = bestY + (tmplH / 2);
    const qreal xNorm = qBound<qreal>(0.0, static_cast<qreal>(matchCx) / static_cast<qreal>(fw), 1.0);
    const qreal yNorm = qBound<qreal>(0.0, static_cast<qreal>(matchCy) / static_cast<qreal>(fh), 1.0);
    const qreal boxNorm =
        qBound<qreal>(0.01, static_cast<qreal>(qMax(tmplW, tmplH)) / static_cast<qreal>(minSide), 1.0);

    const qreal samplesX = static_cast<qreal>((tmplW + sampleStride - 1) / sampleStride);
    const qreal samplesY = static_cast<qreal>((tmplH + sampleStride - 1) / sampleStride);
    const qreal samples = qMax<qreal>(1.0, samplesX * samplesY);
    const qreal normalizedSad =
        qBound<qreal>(0.0, static_cast<qreal>(bestSad) / (255.0 * samples), 1.0);
    const qreal confidence = qBound<qreal>(0.0, 1.0 - normalizedSad, 1.0);

    if (outX) {
        *outX = xNorm;
    }
    if (outY) {
        *outY = yNorm;
    }
    if (outBox) {
        *outBox = boxNorm;
    }
    if (outConfidence) {
        *outConfidence = confidence;
    }
    return true;
}

class SpeakerNameItemDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        if (!index.data(Qt::UserRole + 10).toBool()) {
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRect borderRect = option.rect.adjusted(4, 6, -4, -6);
        painter->setPen(QPen(QColor(QStringLiteral("#29c46a")), 1.5));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(borderRect, 4.0, 4.0);
        painter->restore();
    }
};
}

SpeakersTab::SpeakersTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : TableTabBase(deps, parent)
    , m_widgets(widgets)
    , m_speakerDeps(deps)
{
}

void SpeakersTab::wire()
{
    if (m_widgets.speakersTable) {
        m_widgets.speakersTable->setContextMenuPolicy(Qt::CustomContextMenu);
        m_widgets.speakersTable->setItemDelegateForColumn(
            2, new SpeakerNameItemDelegate(m_widgets.speakersTable));
        connect(m_widgets.speakersTable, &QTableWidget::itemChanged,
                this, &SpeakersTab::onSpeakersTableItemChanged);
        connect(m_widgets.speakersTable, &QTableWidget::itemClicked,
                this, &SpeakersTab::onSpeakersTableItemClicked);
        connect(m_widgets.speakersTable, &QTableWidget::itemSelectionChanged,
                this, &SpeakersTab::onSpeakersSelectionChanged);
        connect(m_widgets.speakersTable, &QWidget::customContextMenuRequested,
                this, &SpeakersTab::onSpeakersTableContextMenuRequested);
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
        m_widgets.speakerPickReference1Button->setToolTip(
            QStringLiteral("Required baseline. Arm Ref 1 pick mode, then in Preview hold Shift and drag a square over the speaker head for framing."));
        connect(m_widgets.speakerPickReference1Button, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerPickReference1Clicked);
    }
    if (m_widgets.speakerPickReference2Button) {
        m_widgets.speakerPickReference2Button->setToolTip(
            QStringLiteral("Optional quality boost. Arm Ref 2 and pick another frame for better framing interpolation."));
        connect(m_widgets.speakerPickReference2Button, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerPickReference2Clicked);
    }
    if (m_widgets.speakerClearReferencesButton) {
        connect(m_widgets.speakerClearReferencesButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerClearReferencesClicked);
    }
    if (m_widgets.speakerRunAutoTrackButton) {
        m_widgets.speakerRunAutoTrackButton->setToolTip(
            QStringLiteral("Single-reference mode works. Add Ref 2 optionally for better framing quality."));
        connect(m_widgets.speakerRunAutoTrackButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerRunAutoTrackClicked);
    }
    if (m_widgets.speakerGuideButton) {
        m_widgets.speakerGuideButton->setToolTip(
            QStringLiteral("Open a quick guide for speaker reference picking and auto-track."));
        connect(m_widgets.speakerGuideButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerGuideClicked);
    }
    if (m_widgets.selectedSpeakerPreviousSentenceButton) {
        connect(m_widgets.selectedSpeakerPreviousSentenceButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerPreviousSentenceClicked);
    }
    if (m_widgets.selectedSpeakerNextSentenceButton) {
        connect(m_widgets.selectedSpeakerNextSentenceButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerNextSentenceClicked);
    }
    if (m_widgets.selectedSpeakerRandomSentenceButton) {
        connect(m_widgets.selectedSpeakerRandomSentenceButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerRandomSentenceClicked);
    }
    if (m_widgets.selectedSpeakerRef1ImageLabel) {
        m_widgets.selectedSpeakerRef1ImageLabel->installEventFilter(this);
        m_widgets.selectedSpeakerRef1ImageLabel->setCursor(Qt::OpenHandCursor);
    }
    if (m_widgets.selectedSpeakerRef2ImageLabel) {
        m_widgets.selectedSpeakerRef2ImageLabel->installEventFilter(this);
        m_widgets.selectedSpeakerRef2ImageLabel->setCursor(Qt::OpenHandCursor);
    }
}

bool SpeakersTab::eventFilter(QObject* watched, QEvent* event)
{
    QLabel* label = qobject_cast<QLabel*>(watched);
    const bool isRef1 = watched == m_widgets.selectedSpeakerRef1ImageLabel;
    const bool isRef2 = watched == m_widgets.selectedSpeakerRef2ImageLabel;
    if (!isRef1 && !isRef2) {
        return TableTabBase::eventFilter(watched, event);
    }
    const int referenceIndex = isRef1 ? 1 : 2;

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() != Qt::LeftButton) {
            break;
        }
        if (beginSelectedReferenceAvatarDrag(referenceIndex, mouseEvent->pos())) {
            if (label) {
                label->setCursor(Qt::ClosedHandCursor);
                label->grabMouse();
            }
            mouseEvent->accept();
            return true;
        }
        // If drag is not available (for example unset ref), allow click-to-arm
        // replacement in Preview so users can still update the reference quickly.
        if (activeCutMutable()) {
            const QString speakerId = selectedSpeakerId();
            if (!speakerId.isEmpty()) {
                armReferencePickForSpeaker(speakerId, referenceIndex);
                mouseEvent->accept();
                return true;
            }
        }
        break;
    }
    case QEvent::MouseMove: {
        if (!m_selectedAvatarDragActive || m_selectedAvatarDragReferenceIndex != referenceIndex) {
            break;
        }
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        updateSelectedReferenceAvatarDrag(mouseEvent->pos());
        mouseEvent->accept();
        return true;
    }
    case QEvent::MouseButtonRelease: {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (!m_selectedAvatarDragActive || m_selectedAvatarDragReferenceIndex != referenceIndex ||
            mouseEvent->button() != Qt::LeftButton) {
            break;
        }
        finishSelectedReferenceAvatarDrag(true);
        if (label) {
            label->releaseMouse();
            label->setCursor(Qt::OpenHandCursor);
        }
        mouseEvent->accept();
        return true;
    }
    case QEvent::Wheel: {
        auto* wheelEvent = static_cast<QWheelEvent*>(event);
        if (adjustSelectedReferenceAvatarZoom(referenceIndex, wheelEvent->angleDelta().y())) {
            wheelEvent->accept();
            return true;
        }
        break;
    }
    case QEvent::Hide:
    case QEvent::Destroy: {
        if (m_selectedAvatarDragActive && m_selectedAvatarDragReferenceIndex == referenceIndex) {
            finishSelectedReferenceAvatarDrag(false);
            if (label) {
                label->releaseMouse();
                label->setCursor(Qt::OpenHandCursor);
            }
        }
        break;
    }
    default:
        break;
    }
    return TableTabBase::eventFilter(watched, event);
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
    const QString selectedSpeakerBeforeClear = selectedSpeakerId();
    const QString preferredSpeakerId =
        selectedSpeakerBeforeClear.isEmpty() ? m_lastSelectedSpeakerIdHint : selectedSpeakerBeforeClear;
    const QString previousTranscriptPath = m_loadedTranscriptPath;
    const QString previousClipFilePath = m_loadedClipFilePath;
    m_loadedTranscriptPath.clear();
    m_loadedClipFilePath.clear();
    m_loadedTranscriptDoc = QJsonDocument();
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
        updateSelectedSpeakerPanel();
        return;
    }

    m_loadedClipFilePath = clip->filePath;
    const QString transcriptPath = activeTranscriptPathForClipFile(clip->filePath);
    m_loadedTranscriptPath = transcriptPath;
    if (previousTranscriptPath != m_loadedTranscriptPath ||
        previousClipFilePath != m_loadedClipFilePath) {
        m_avatarCache.clear();
    }

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
        updateSelectedSpeakerPanel();
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
        updateSelectedSpeakerPanel();
        return;
    }

    m_loadedTranscriptDoc = transcriptDoc;

    if (m_widgets.speakersInspectorClipLabel) {
        m_widgets.speakersInspectorClipLabel->setText(QStringLiteral("Speakers\n%1").arg(clip->label));
    }
    if (m_widgets.speakersInspectorDetailsLabel) {
        m_widgets.speakersInspectorDetailsLabel->setText(
            activeCutMutable()
                ? QStringLiteral("Edit speaker names, on-screen locations, and framing references for this cut.")
                : QStringLiteral("Original cut is immutable. Use + New Cut in Transcript to edit speaker metadata."));
    }

    refreshSpeakersTable(transcriptDoc.object(), preferredSpeakerId);
    if (m_widgets.speakersTable) {
        m_widgets.speakersTable->setEnabled(activeCutMutable());
    }

    m_updating = false;
    updateSpeakerTrackingStatusLabel();
    updateSelectedSpeakerPanel();
}

void SpeakersTab::refreshSpeakersTable(const QJsonObject& transcriptRoot,
                                       const QString& preferredSpeakerId)
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
    const QString selectionToRestore =
        preferredSpeakerId.isEmpty() ? selectedSpeakerId() : preferredSpeakerId;

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
        const QJsonObject tracking = speakerFramingObject(profile);
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
            const double boxSize =
                refObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0);
            if (boxSize > 0.0) {
                return QStringLiteral("F%1 (%2, %3) s=%4")
                    .arg(frame)
                    .arg(QString::number(rx, 'f', 3))
                    .arg(QString::number(ry, 'f', 3))
                    .arg(QString::number(boxSize, 'f', 3));
            }
            return QStringLiteral("F%1 (%2, %3)")
                .arg(frame)
                .arg(QString::number(rx, 'f', 3))
                .arg(QString::number(ry, 'f', 3));
        };

        const QJsonObject ref1Obj = tracking.value(QString(kTranscriptSpeakerTrackingRef1Key)).toObject();
        const QJsonObject ref2Obj = tracking.value(QString(kTranscriptSpeakerTrackingRef2Key)).toObject();
        const QString ref1Summary = formatReference(ref1Obj);
        const QString ref2Summary = formatReference(ref2Obj);

        auto* avatarItem = new QTableWidgetItem();
        avatarItem->setFlags(avatarItem->flags() & ~Qt::ItemIsEditable);
        avatarItem->setData(Qt::UserRole, id);
        avatarItem->setIcon(QIcon(speakerAvatarForRow(*clip, transcriptRoot, profile, id)));
        avatarItem->setToolTip(
            QStringLiteral("Primary avatar. Unset by default. Click avatar and square-select in Preview to set."));
        avatarItem->setTextAlignment(Qt::AlignCenter);
        avatarItem->setSizeHint(QSize(32, 32));

        auto* idItem = new QTableWidgetItem(id);
        idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
        idItem->setData(Qt::UserRole, id);
        auto* nameItem = new QTableWidgetItem(name);
        nameItem->setData(Qt::UserRole, id);
        nameItem->setData(Qt::UserRole + 10, keyframeCount > 0);
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
        auto* ref1Item = new QTableWidgetItem();
        ref1Item->setFlags(ref1Item->flags() & ~Qt::ItemIsEditable);
        ref1Item->setData(Qt::UserRole, id);
        ref1Item->setData(Qt::UserRole + 1, 1);
        ref1Item->setIcon(QIcon(speakerReferenceAvatar(*clip, id, ref1Obj)));
        ref1Item->setToolTip(ref1Summary == QStringLiteral("—")
                                 ? QStringLiteral("Ref 1: not set. Click to arm square-select, then Shift+drag in Preview.")
                                 : QStringLiteral("Ref 1: %1\nClick to re-arm square-select and overwrite.").arg(ref1Summary));
        ref1Item->setTextAlignment(Qt::AlignCenter);
        ref1Item->setSizeHint(QSize(30, 30));

        auto* ref2Item = new QTableWidgetItem();
        ref2Item->setFlags(ref2Item->flags() & ~Qt::ItemIsEditable);
        ref2Item->setData(Qt::UserRole, id);
        ref2Item->setData(Qt::UserRole + 1, 2);
        ref2Item->setIcon(QIcon(speakerReferenceAvatar(*clip, id, ref2Obj)));
        ref2Item->setToolTip(ref2Summary == QStringLiteral("—")
                                 ? QStringLiteral("Ref 2: not set. Click to arm square-select, then Shift+drag in Preview.")
                                 : QStringLiteral("Ref 2: %1\nClick to re-arm square-select and overwrite.").arg(ref2Summary));
        ref2Item->setTextAlignment(Qt::AlignCenter);
        ref2Item->setSizeHint(QSize(30, 30));

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

    bool restoredSelection = false;
    if (!selectionToRestore.isEmpty()) {
        for (int row = 0; row < m_widgets.speakersTable->rowCount(); ++row) {
            QTableWidgetItem* idItem = m_widgets.speakersTable->item(row, 1);
            if (!idItem) {
                continue;
            }
            const QString rowSpeakerId = idItem->data(Qt::UserRole).toString().trimmed();
            if (rowSpeakerId != selectionToRestore) {
                continue;
            }
            m_widgets.speakersTable->setCurrentCell(row, 1);
            m_widgets.speakersTable->selectRow(row);
            m_lastSelectedSpeakerIdHint = rowSpeakerId;
            restoredSelection = true;
            break;
        }
    }
    if (!restoredSelection && !ids.isEmpty()) {
        QTableWidgetItem* idItem = m_widgets.speakersTable->item(0, 1);
        if (idItem) {
            m_widgets.speakersTable->setCurrentCell(0, 1);
            m_widgets.speakersTable->selectRow(0);
            m_lastSelectedSpeakerIdHint = idItem->data(Qt::UserRole).toString().trimmed();
        }
    }
}

QString SpeakersTab::selectedSpeakerId() const
{
    if (!m_widgets.speakersTable || !m_widgets.speakersTable->selectionModel()) {
        return QString();
    }
    int row = -1;
    const QModelIndexList selectedRows = m_widgets.speakersTable->selectionModel()->selectedRows();
    if (!selectedRows.isEmpty()) {
        row = selectedRows.constFirst().row();
    } else {
        row = m_widgets.speakersTable->currentRow();
    }
    if (row < 0) {
        return QString();
    }
    QTableWidgetItem* idItem = m_widgets.speakersTable->item(row, 1);
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

QPixmap SpeakersTab::unsetSpeakerAvatar(int size) const
{
    const int iconSize = qMax(16, size);
    QImage image(iconSize, iconSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF border(1.0, 1.0, iconSize - 2.0, iconSize - 2.0);
    painter.setPen(QPen(QColor(QStringLiteral("#5a6d82")), 1.0, Qt::DashLine));
    painter.setBrush(QColor(QStringLiteral("#1a2533")));
    painter.drawRoundedRect(border, 4.0, 4.0);
    painter.setPen(QColor(QStringLiteral("#9fb3c8")));
    QFont font = painter.font();
    font.setBold(true);
    font.setPointSize(qMax(7, iconSize / 4));
    painter.setFont(font);
    painter.drawText(QRectF(0.0, 0.0, iconSize, iconSize), Qt::AlignCenter, QStringLiteral("?"));
    return QPixmap::fromImage(image);
}

QPixmap SpeakersTab::speakerAvatarForRow(const TimelineClip& clip,
                                         const QJsonObject& transcriptRoot,
                                         const QJsonObject& profile,
                                         const QString& speakerId)
{
    Q_UNUSED(transcriptRoot);
    const QJsonObject tracking = speakerFramingObject(profile);
    const QJsonObject ref1Obj = tracking.value(QString(kTranscriptSpeakerTrackingRef1Key)).toObject();
    const QJsonObject ref2Obj = tracking.value(QString(kTranscriptSpeakerTrackingRef2Key)).toObject();
    if (!ref1Obj.isEmpty()) {
        return speakerReferenceAvatar(clip, speakerId, ref1Obj);
    }
    if (!ref2Obj.isEmpty()) {
        return speakerReferenceAvatar(clip, speakerId, ref2Obj);
    }
    return unsetSpeakerAvatar(32);

#if 0
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
#endif
}

QPixmap SpeakersTab::speakerReferenceAvatar(const TimelineClip& clip,
                                            const QString& speakerId,
                                            const QJsonObject& refObj,
                                            int size)
{
    if (refObj.isEmpty()) {
        Q_UNUSED(speakerId);
        return unsetSpeakerAvatar(size);
    }
    const int avatarSize = qMax(16, size);

    const int64_t sourceFrame30 =
        qMax<int64_t>(0, refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
    qreal locX = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    qreal locY = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    const qreal boxSizeNorm = qBound<qreal>(
        -1.0, refObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0), 1.0);

    const QString cacheKey = QStringLiteral("ref|%1|%2|%3|%4|%5|%6")
        .arg(m_loadedTranscriptPath)
        .arg(speakerId)
        .arg(sourceFrame30)
        .arg(static_cast<int>(std::round(locX * 1000.0)))
        .arg(static_cast<int>(std::round(locY * 1000.0)))
        .arg(static_cast<int>(std::round(qMax<qreal>(0.0, boxSizeNorm) * 1000.0))) +
        QStringLiteral("|s=%1").arg(avatarSize);
    const auto cached = m_avatarCache.constFind(cacheKey);
    if (cached != m_avatarCache.cend()) {
        return cached.value();
    }

    const qreal sourceFps = resolvedSourceFps(clip);
    const int64_t decodeFrame = qMax<int64_t>(
        0, static_cast<int64_t>(std::floor((static_cast<qreal>(sourceFrame30) / kTimelineFps) * sourceFps)));
    const QString mediaPath = interactivePreviewMediaPathForClip(clip);

    editor::DecoderContext ctx(mediaPath);
    QPixmap avatar = placeholderSpeakerAvatar(speakerId);
    if (ctx.initialize()) {
        const editor::FrameHandle frame = ctx.decodeFrame(decodeFrame);
        const QImage image = frame.hasCpuImage() ? frame.cpuImage() : QImage();
        if (!image.isNull() && image.width() > 0 && image.height() > 0) {
            const int width = image.width();
            const int height = image.height();
            const int minSide = qMin(width, height);
            int side = qMax(40, minSide / 3);
            if (boxSizeNorm > 0.0) {
                side = qBound(40, static_cast<int>(std::round(boxSizeNorm * minSide)), minSide);
            }
            const int cx = static_cast<int>(std::round(locX * static_cast<qreal>(width)));
            const int cy = static_cast<int>(std::round(locY * static_cast<qreal>(height)));
            const int left = qBound(0, cx - (side / 2), qMax(0, width - side));
            const int top = qBound(0, cy - (side / 2), qMax(0, height - side));
            QImage crop = image.copy(QRect(left, top, qMin(side, width), qMin(side, height)))
                              .scaled(avatarSize, avatarSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_ARGB32_Premultiplied);
            if (!crop.isNull()) {
                QImage rounded(avatarSize, avatarSize, QImage::Format_ARGB32_Premultiplied);
                rounded.fill(Qt::transparent);
                QPainter painter(&rounded);
                painter.setRenderHint(QPainter::Antialiasing, true);
                QPainterPath path;
                path.addEllipse(1.0, 1.0, avatarSize - 2.0, avatarSize - 2.0);
                painter.setClipPath(path);
                painter.drawImage(QRect(0, 0, avatarSize, avatarSize), crop);
                painter.setClipping(false);
                painter.setPen(QPen(QColor(QStringLiteral("#8dbbe4")), 1.0));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(QRectF(1.0, 1.0, avatarSize - 2.0, avatarSize - 2.0));
                avatar = QPixmap::fromImage(rounded);
            }
        }
    }

    m_avatarCache.insert(cacheKey, avatar);
    return avatar;
}

QString SpeakersTab::speakerTrackingSummary(const QJsonObject& profile) const
{
    const QJsonObject tracking = speakerFramingObject(profile);
    if (tracking.isEmpty()) {
        return QStringLiteral("Framing: Off (no references)");
    }
    const auto refSummary = [](const QJsonObject& refObj) -> QString {
        if (refObj.isEmpty()) {
            return QStringLiteral("unset");
        }
        const int64_t frame = refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
        const double x = refObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.0);
        const double y = refObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.0);
        const double boxSize =
            refObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0);
        if (boxSize > 0.0) {
            return QStringLiteral("F%1 (%2, %3) s=%4")
                .arg(frame)
                .arg(QString::number(x, 'f', 3))
                .arg(QString::number(y, 'f', 3))
                .arg(QString::number(boxSize, 'f', 3));
        }
        return QStringLiteral("F%1 (%2, %3)")
            .arg(frame)
            .arg(QString::number(x, 'f', 3))
            .arg(QString::number(y, 'f', 3));
    };

    const QString mode = tracking.value(QString(kTranscriptSpeakerTrackingModeKey)).toString(QStringLiteral("Manual"));
    const bool enabled = transcriptTrackingEnabled(tracking);
    const QString autoState = tracking.value(QString(kTranscriptSpeakerTrackingAutoStateKey)).toString();
    const int keyframeCount = tracking.value(QString(kTranscriptSpeakerTrackingKeyframesKey)).toArray().size();
    QString summary = QStringLiteral("Framing: %1 (%2) | Ref1=%3 | Ref2=%4")
        .arg(enabled ? QStringLiteral("On") : QStringLiteral("Off"))
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
    bool hasRef1 = false;
    bool hasRef2 = false;

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
        m_widgets.speakerRunAutoTrackButton->setText(QStringLiteral("Auto-Frame"));
    }
    if (m_widgets.speakerGuideButton) {
        m_widgets.speakerGuideButton->setEnabled(true);
    }
    if (m_widgets.selectedSpeakerPreviousSentenceButton) {
        m_widgets.selectedSpeakerPreviousSentenceButton->setEnabled(!speakerId.isEmpty());
    }
    if (m_widgets.selectedSpeakerNextSentenceButton) {
        m_widgets.selectedSpeakerNextSentenceButton->setEnabled(!speakerId.isEmpty());
    }
    if (m_widgets.selectedSpeakerRandomSentenceButton) {
        m_widgets.selectedSpeakerRandomSentenceButton->setEnabled(!speakerId.isEmpty());
    }
    if (m_widgets.speakerPickReference1Button) {
        m_widgets.speakerPickReference1Button->setEnabled(canEdit);
        m_widgets.speakerPickReference1Button->setText(
            m_pendingReferencePick == 1
                ? QStringLiteral("[ARMED] Ref 1")
                : QStringLiteral("Pick Ref 1 (Shift+Drag)"));
    }
    if (m_widgets.speakerPickReference2Button) {
        m_widgets.speakerPickReference2Button->setEnabled(canEdit);
        m_widgets.speakerPickReference2Button->setText(
            m_pendingReferencePick == 2
                ? QStringLiteral("[ARMED] Ref 2")
                : QStringLiteral("Pick Ref 2 (Shift+Drag)"));
    }

    if (!mutableCut) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Framing references are editable only on derived cuts (not Original)."));
        return;
    }
    if (speakerId.isEmpty()) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Select one speaker row, then set Ref 1 / Ref 2 at the current playhead frame."));
        return;
    }
    if (!m_loadedTranscriptDoc.isObject()) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Framing scaffold ready. Transcript not loaded."));
        return;
    }

    const QJsonObject profiles =
        m_loadedTranscriptDoc.object().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QJsonObject tracking = speakerFramingObject(profile);
    transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef1Key, &hasRef1);
    transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef2Key, &hasRef2);
    const bool trackingEnabled = transcriptTrackingEnabled(tracking);

    if (m_widgets.speakerRunAutoTrackButton) {
        const bool hasAnyRef = hasRef1 || hasRef2;
        m_widgets.speakerRunAutoTrackButton->setEnabled(canEdit && hasAnyRef);
        if (!hasAnyRef) {
            m_widgets.speakerRunAutoTrackButton->setText(QStringLiteral("Auto-Frame (need Ref 1)"));
        } else if (hasRef1 && hasRef2) {
            m_widgets.speakerRunAutoTrackButton->setText(QStringLiteral("Auto-Frame (2 refs)"));
        } else {
            m_widgets.speakerRunAutoTrackButton->setText(QStringLiteral("Auto-Frame (1 ref mode)"));
        }
    }

    if (m_pendingReferencePick == 1 || m_pendingReferencePick == 2) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("[ARMED] Ref %1 for %2. In Preview: 1) Move cursor over clip, 2) Hold Shift, 3) Drag square on head, 4) Release.")
                .arg(m_pendingReferencePick)
                .arg(speakerId));
        return;
    }

    QString flowHint;
    if (!hasRef1 && !hasRef2) {
        flowHint = QStringLiteral("Flow: set Ref 1 -> Auto-Frame. Ref 2 is optional for better framing.");
    } else if ((hasRef1 && !hasRef2) || (!hasRef1 && hasRef2)) {
        flowHint = trackingEnabled
            ? QStringLiteral("Framing ON (1-ref mode). Add Ref 2 then run Auto-Frame again for better quality.")
            : QStringLiteral("1-ref mode ready. Auto-Frame to enable framing, or add Ref 2 first.");
    } else {
        flowHint = trackingEnabled
            ? QStringLiteral("Framing ON (2-ref mode). Best quality interpolation active.")
            : QStringLiteral("2-ref mode ready. Run Auto-Frame to enable framing.");
    }
    m_widgets.speakerTrackingStatusLabel->setText(
        QStringLiteral("%1 | Speaker=%2 | %3")
            .arg(speakerTrackingSummary(profile), speakerId, flowHint));
}

void SpeakersTab::updateSelectedSpeakerPanel()
{
    if (!m_widgets.selectedSpeakerIdLabel &&
        !m_widgets.selectedSpeakerRef1ImageLabel &&
        !m_widgets.selectedSpeakerRef2ImageLabel) {
        return;
    }

    const QString speakerId = selectedSpeakerId();
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (speakerId.isEmpty() || !clip || !m_loadedTranscriptDoc.isObject()) {
        if (m_widgets.selectedSpeakerIdLabel) {
            m_widgets.selectedSpeakerIdLabel->setText(QStringLiteral("No speaker selected"));
        }
        if (m_widgets.speakerCurrentSentenceLabel) {
            m_widgets.speakerCurrentSentenceLabel->setText(
                QStringLiteral("Select a speaker to view sentence context."));
        }
        if (m_widgets.selectedSpeakerRef1ImageLabel) {
            m_widgets.selectedSpeakerRef1ImageLabel->setPixmap(unsetSpeakerAvatar(120));
            m_widgets.selectedSpeakerRef1ImageLabel->setToolTip(QStringLiteral("Ref1 unset"));
        }
        if (m_widgets.selectedSpeakerRef2ImageLabel) {
            m_widgets.selectedSpeakerRef2ImageLabel->setPixmap(unsetSpeakerAvatar(120));
            m_widgets.selectedSpeakerRef2ImageLabel->setToolTip(QStringLiteral("Ref2 unset"));
        }
        return;
    }

    const QJsonObject root = m_loadedTranscriptDoc.object();
    const QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QString displayName = profile.value(QString(kTranscriptSpeakerNameKey)).toString(speakerId);
    if (m_widgets.selectedSpeakerIdLabel) {
        m_widgets.selectedSpeakerIdLabel->setText(
            QStringLiteral("%1 (%2)").arg(displayName, speakerId));
    }
    if (m_widgets.speakerCurrentSentenceLabel) {
        m_widgets.speakerCurrentSentenceLabel->setText(
            currentSpeakerSentenceAtCurrentFrame(speakerId));
    }

    const QJsonObject tracking = speakerFramingObject(profile);
    const QJsonObject ref1Obj = tracking.value(QString(kTranscriptSpeakerTrackingRef1Key)).toObject();
    const QJsonObject ref2Obj = tracking.value(QString(kTranscriptSpeakerTrackingRef2Key)).toObject();
    if (m_widgets.selectedSpeakerRef1ImageLabel) {
        m_widgets.selectedSpeakerRef1ImageLabel->setPixmap(speakerReferenceAvatar(*clip, speakerId, ref1Obj, 120));
        m_widgets.selectedSpeakerRef1ImageLabel->setToolTip(
            ref1Obj.isEmpty()
                ? QStringLiteral("Ref1 unset")
                : QStringLiteral("Ref1 set. Drag to reposition, wheel to zoom crop."));
    }
    if (m_widgets.selectedSpeakerRef2ImageLabel) {
        m_widgets.selectedSpeakerRef2ImageLabel->setPixmap(speakerReferenceAvatar(*clip, speakerId, ref2Obj, 120));
        m_widgets.selectedSpeakerRef2ImageLabel->setToolTip(
            ref2Obj.isEmpty()
                ? QStringLiteral("Ref2 unset")
                : QStringLiteral("Ref2 set. Drag to reposition, wheel to zoom crop."));
    }
}

QString SpeakersTab::currentSpeakerSentenceAtCurrentFrame(const QString& speakerId) const
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.trimmed().isEmpty()) {
        return QStringLiteral("No sentence available.");
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return QStringLiteral("No sentence available.");
    }
    const int64_t sourceFrame = currentSourceFrameForClip(*clip);
    const QString targetSpeaker = speakerId.trimmed();

    struct SentenceRun {
        int64_t startFrame = 0;
        int64_t endFrame = 0;
        QString text;
    };
    QVector<SentenceRun> runs;

    const QJsonArray segments = m_loadedTranscriptDoc.object().value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();

        bool runActive = false;
        SentenceRun run;
        QStringList runWords;
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                if (runActive && !runWords.isEmpty()) {
                    run.text = runWords.join(QStringLiteral(" "));
                    runs.push_back(run);
                }
                runActive = false;
                runWords.clear();
                continue;
            }
            QString wordSpeaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (wordSpeaker.isEmpty()) {
                wordSpeaker = segmentSpeaker;
            }
            const QString wordText = wordObj.value(QStringLiteral("word")).toString().trimmed();
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
            if (wordText.isEmpty() || startSeconds < 0.0 || endSeconds < startSeconds) {
                if (runActive && !runWords.isEmpty()) {
                    run.text = runWords.join(QStringLiteral(" "));
                    runs.push_back(run);
                }
                runActive = false;
                runWords.clear();
                continue;
            }
            const int64_t wordStartFrame =
                qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)));
            const int64_t wordEndFrame =
                qMax<int64_t>(wordStartFrame, static_cast<int64_t>(std::ceil(endSeconds * kTimelineFps)) - 1);
            if (wordSpeaker != targetSpeaker) {
                if (runActive && !runWords.isEmpty()) {
                    run.text = runWords.join(QStringLiteral(" "));
                    runs.push_back(run);
                }
                runActive = false;
                runWords.clear();
                continue;
            }

            if (!runActive) {
                run = SentenceRun{};
                run.startFrame = wordStartFrame;
                run.endFrame = wordEndFrame;
                runWords.clear();
                runWords.push_back(wordText);
                runActive = true;
            } else {
                run.endFrame = qMax<int64_t>(run.endFrame, wordEndFrame);
                runWords.push_back(wordText);
            }
        }
        if (runActive && !runWords.isEmpty()) {
            run.text = runWords.join(QStringLiteral(" "));
            runs.push_back(run);
        }
    }

    if (runs.isEmpty()) {
        return QStringLiteral("No sentence found for this speaker.");
    }

    int bestIndex = -1;
    int64_t bestDistance = std::numeric_limits<int64_t>::max();
    for (int i = 0; i < runs.size(); ++i) {
        const SentenceRun& run = runs.at(i);
        if (sourceFrame >= run.startFrame && sourceFrame <= run.endFrame) {
            bestIndex = i;
            break;
        }
        const int64_t distance =
            (sourceFrame < run.startFrame)
                ? (run.startFrame - sourceFrame)
                : (sourceFrame - run.endFrame);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = i;
        }
    }

    if (bestIndex < 0 || bestIndex >= runs.size()) {
        return QStringLiteral("No sentence found for this speaker.");
    }
    return runs.at(bestIndex).text;
}

int64_t SpeakersTab::currentSourceFrameForClip(const TimelineClip& clip) const
{
    const int64_t timelineFrame =
        m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : clip.startFrame;
    const QVector<RenderSyncMarker> markers =
        m_speakerDeps.getRenderSyncMarkers
            ? m_speakerDeps.getRenderSyncMarkers()
            : QVector<RenderSyncMarker>{};
    return transcriptFrameForClipAtTimelineSample(clip, frameToSamples(timelineFrame), markers);
}

QVector<int64_t> SpeakersTab::speakerSourceFrames(const QJsonObject& transcriptRoot,
                                                  const QString& speakerId) const
{
    QVector<int64_t> frames;
    const QString targetSpeaker = speakerId.trimmed();
    if (targetSpeaker.isEmpty()) {
        return frames;
    }
    const QJsonArray segments = transcriptRoot.value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        bool sentenceActive = false;
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                sentenceActive = false;
                continue;
            }
            QString wordSpeaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (wordSpeaker.isEmpty()) {
                wordSpeaker = segmentSpeaker;
            }
            const QString wordText = wordObj.value(QStringLiteral("word")).toString().trimmed();
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            const bool validWord = !wordText.isEmpty() && startSeconds >= 0.0;
            if (wordSpeaker != targetSpeaker || !validWord) {
                sentenceActive = false;
                continue;
            }
            if (!sentenceActive) {
                frames.push_back(
                    qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps))));
                sentenceActive = true;
            }
        }
    }
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
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
                                                 qreal yNorm,
                                                 qreal boxSizeNorm)
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

    QJsonObject tracking = speakerFramingObject(profile);
    const bool previouslyEnabled =
        tracking.value(QString(kTranscriptSpeakerTrackingEnabledKey)).toBool(false);
    tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("ReferencePoints");
    tracking[QString(kTranscriptSpeakerTrackingEnabledKey)] = previouslyEnabled;
    tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] = QStringLiteral("refs_updated");

    QJsonObject refObj;
    refObj[QString(kTranscriptSpeakerTrackingFrameKey)] = static_cast<qint64>(frame);
    refObj[QString(kTranscriptSpeakerLocationXKey)] = x;
    refObj[QString(kTranscriptSpeakerLocationYKey)] = y;
    if (boxSizeNorm > 0.0) {
        refObj[QString(kTranscriptSpeakerTrackingBoxSizeKey)] =
            qBound(0.01, static_cast<double>(boxSizeNorm), 1.0);
    }
    if (referenceIndex == 1) {
        tracking[QString(kTranscriptSpeakerTrackingRef1Key)] = refObj;
    } else {
        tracking[QString(kTranscriptSpeakerTrackingRef2Key)] = refObj;
    }

    setSpeakerFramingObject(profile, tracking);
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
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }

    const int64_t frame = currentSourceFrameForClip(*clip);

    QJsonObject root = m_loadedTranscriptDoc.object();
    const QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QJsonObject location = profile.value(QString(kTranscriptSpeakerLocationKey)).toObject();
    const qreal x = qBound(0.0, location.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    const qreal y = qBound(0.0, location.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    return saveSpeakerTrackingReferenceAt(speakerId, referenceIndex, frame, x, y);
}

bool SpeakersTab::armReferencePickForSpeaker(const QString& speakerId, int referenceIndex)
{
    if (!activeCutMutable() || speakerId.trimmed().isEmpty() || (referenceIndex != 1 && referenceIndex != 2)) {
        return false;
    }
    m_lastSelectedSpeakerIdHint = speakerId.trimmed();
    m_pendingReferencePick = referenceIndex;
    updateSpeakerTrackingStatusLabel();
    return true;
}

bool SpeakersTab::clearSpeakerTrackingReferences(const QString& speakerId)
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.isEmpty()) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    profile.remove(QString(kTranscriptSpeakerFramingKey));
    profile.remove(QString(kTranscriptSpeakerTrackingKey));
    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);

    editor::TranscriptEngine engine;
    return engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc);
}

bool SpeakersTab::setSpeakerTrackingEnabled(const QString& speakerId, bool enabled)
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.isEmpty()) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    QJsonObject tracking = speakerFramingObject(profile);
    if (tracking.isEmpty()) {
        return false;
    }
    tracking[QString(kTranscriptSpeakerTrackingEnabledKey)] = enabled;
    setSpeakerFramingObject(profile, tracking);
    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);

    editor::TranscriptEngine engine;
    return engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc);
}

bool SpeakersTab::deleteSpeakerAutoTrackPointstream(const QString& speakerId)
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.trimmed().isEmpty()) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    QJsonObject tracking = speakerFramingObject(profile);
    if (tracking.isEmpty()) {
        return false;
    }
    if (!transcriptTrackingHasPointstream(tracking)) {
        return true;
    }

    tracking[QString(kTranscriptSpeakerTrackingKeyframesKey)] = QJsonArray();
    tracking[QString(kTranscriptSpeakerTrackingEnabledKey)] = false;
    tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("ReferencePoints");
    tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] = QStringLiteral("deleted_pointstream");
    setSpeakerFramingObject(profile, tracking);
    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);

    editor::TranscriptEngine engine;
    return engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc);
}

bool SpeakersTab::setSpeakerSkipped(const QString& speakerId, bool skipped)
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.trimmed().isEmpty()) {
        return false;
    }

    const QString targetSpeaker = speakerId.trimmed();
    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    bool changed = false;

    for (int segIndex = 0; segIndex < segments.size(); ++segIndex) {
        QJsonObject segmentObj = segments.at(segIndex).toObject();
        const QString segmentSpeaker =
            segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
            QJsonObject wordObj = words.at(wordIndex).toObject();
            QString wordSpeaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (wordSpeaker.isEmpty()) {
                wordSpeaker = segmentSpeaker;
            }
            if (wordSpeaker != targetSpeaker) {
                continue;
            }
            if (wordObj.value(QStringLiteral("word")).toString().trimmed().isEmpty()) {
                continue;
            }
            const bool previous = wordObj.value(QStringLiteral("skipped")).toBool(false);
            if (previous != skipped) {
                wordObj[QStringLiteral("skipped")] = skipped;
                words.replace(wordIndex, wordObj);
                changed = true;
            }
        }
        segmentObj[QStringLiteral("words")] = words;
        segments.replace(segIndex, segmentObj);
    }

    if (!changed) {
        return true;
    }

    root[QStringLiteral("segments")] = segments;
    m_loadedTranscriptDoc.setObject(root);
    editor::TranscriptEngine engine;
    return engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc);
}

bool SpeakersTab::selectedSpeakerReferenceObject(int referenceIndex,
                                                 QString* speakerIdOut,
                                                 QJsonObject* refOut) const
{
    if (referenceIndex != 1 && referenceIndex != 2) {
        return false;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty() || !m_loadedTranscriptDoc.isObject()) {
        return false;
    }
    const QJsonObject profiles =
        m_loadedTranscriptDoc.object().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QJsonObject framing = speakerFramingObject(profile);
    const QJsonObject refObj = framing.value(
        QString(referenceIndex == 1 ? kTranscriptSpeakerTrackingRef1Key
                                    : kTranscriptSpeakerTrackingRef2Key)).toObject();
    if (refObj.isEmpty()) {
        return false;
    }
    if (speakerIdOut) {
        *speakerIdOut = speakerId;
    }
    if (refOut) {
        *refOut = refObj;
    }
    return true;
}

bool SpeakersTab::adjustSelectedReferenceAvatarZoom(int referenceIndex, int wheelDelta)
{
    if (!activeCutMutable() || referenceIndex < 1 || referenceIndex > 2 || wheelDelta == 0) {
        return false;
    }
    QString speakerId;
    QJsonObject refObj;
    if (!selectedSpeakerReferenceObject(referenceIndex, &speakerId, &refObj)) {
        return false;
    }

    const qreal units = static_cast<qreal>(wheelDelta) / 120.0;
    if (std::abs(units) < 0.001) {
        return false;
    }
    const qreal currentBoxSize = qBound<qreal>(
        0.05, refObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(1.0 / 3.0), 1.0);
    // Scroll up zooms in (smaller source crop), scroll down zooms out.
    const qreal nextBoxSize = qBound<qreal>(0.05, currentBoxSize - (units * 0.02), 1.0);
    if (std::abs(nextBoxSize - currentBoxSize) < 0.0005) {
        return false;
    }

    const int64_t frame =
        qMax<int64_t>(0, refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
    const qreal xNorm = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    const qreal yNorm = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    if (!saveSpeakerTrackingReferenceAt(speakerId, referenceIndex, frame, xNorm, yNorm, nextBoxSize)) {
        return false;
    }

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

QPointF SpeakersTab::referenceNormPerPixelFromSourceFrame(const TimelineClip& clip,
                                                          const QJsonObject& refObj,
                                                          int avatarSize) const
{
    const int64_t sourceFrame30 =
        qMax<int64_t>(0, refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
    const qreal boxSizeNorm = qBound<qreal>(
        -1.0, refObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0), 1.0);
    const qreal sourceFps = resolvedSourceFps(clip);
    const int64_t decodeFrame = qMax<int64_t>(
        0, static_cast<int64_t>(std::floor((static_cast<qreal>(sourceFrame30) / kTimelineFps) * sourceFps)));

    editor::DecoderContext ctx(interactivePreviewMediaPathForClip(clip));
    if (!ctx.initialize()) {
        return QPointF(0.0, 0.0);
    }
    const editor::FrameHandle frame = ctx.decodeFrame(decodeFrame);
    const QImage image = frame.hasCpuImage() ? frame.cpuImage() : QImage();
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return QPointF(0.0, 0.0);
    }

    const int width = image.width();
    const int height = image.height();
    const int minSide = qMin(width, height);
    int side = qMax(40, minSide / 3);
    if (boxSizeNorm > 0.0) {
        side = qBound(40, static_cast<int>(std::round(boxSizeNorm * minSide)), minSide);
    }
    const int displaySize = qMax(1, avatarSize);
    const qreal sourcePxPerAvatarPx = static_cast<qreal>(side) / qMax<qreal>(1.0, displaySize);
    return QPointF(sourcePxPerAvatarPx / qMax<qreal>(1.0, width),
                   sourcePxPerAvatarPx / qMax<qreal>(1.0, height));
}

bool SpeakersTab::beginSelectedReferenceAvatarDrag(int referenceIndex, const QPoint& localPos)
{
    if (!activeCutMutable() || referenceIndex < 1 || referenceIndex > 2) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }
    QString speakerId;
    QJsonObject refObj;
    if (!selectedSpeakerReferenceObject(referenceIndex, &speakerId, &refObj)) {
        return false;
    }
    const QPointF normPerPixel = referenceNormPerPixelFromSourceFrame(*clip, refObj, 120);
    if (normPerPixel.x() <= 0.0 || normPerPixel.y() <= 0.0) {
        return false;
    }
    m_selectedAvatarDragActive = true;
    m_selectedAvatarDragReferenceIndex = referenceIndex;
    m_selectedAvatarDragSpeakerId = speakerId;
    m_selectedAvatarDragLastPos = localPos;
    m_selectedAvatarDragRefObj = refObj;
    m_selectedAvatarDragNormPerPixel = normPerPixel;
    return true;
}

void SpeakersTab::updateSelectedReferenceAvatarDrag(const QPoint& localPos)
{
    if (!m_selectedAvatarDragActive) {
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return;
    }
    const QPoint delta = localPos - m_selectedAvatarDragLastPos;
    m_selectedAvatarDragLastPos = localPos;

    const qreal currentX = qBound<qreal>(0.0, m_selectedAvatarDragRefObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    const qreal currentY = qBound<qreal>(0.0, m_selectedAvatarDragRefObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    const qreal nextX = qBound<qreal>(0.0, currentX + (delta.x() * m_selectedAvatarDragNormPerPixel.x()), 1.0);
    const qreal nextY = qBound<qreal>(0.0, currentY + (delta.y() * m_selectedAvatarDragNormPerPixel.y()), 1.0);
    m_selectedAvatarDragRefObj[QString(kTranscriptSpeakerLocationXKey)] = nextX;
    m_selectedAvatarDragRefObj[QString(kTranscriptSpeakerLocationYKey)] = nextY;

    QLabel* targetLabel = m_selectedAvatarDragReferenceIndex == 1
        ? m_widgets.selectedSpeakerRef1ImageLabel
        : m_widgets.selectedSpeakerRef2ImageLabel;
    if (targetLabel) {
        targetLabel->setPixmap(
            speakerReferenceAvatar(*clip, m_selectedAvatarDragSpeakerId, m_selectedAvatarDragRefObj, 120));
    }
}

void SpeakersTab::finishSelectedReferenceAvatarDrag(bool commit)
{
    if (!m_selectedAvatarDragActive) {
        return;
    }
    const QString speakerId = m_selectedAvatarDragSpeakerId;
    const int referenceIndex = m_selectedAvatarDragReferenceIndex;
    const QJsonObject refObj = m_selectedAvatarDragRefObj;

    m_selectedAvatarDragActive = false;
    m_selectedAvatarDragReferenceIndex = 0;
    m_selectedAvatarDragSpeakerId.clear();
    m_selectedAvatarDragLastPos = QPoint();
    m_selectedAvatarDragRefObj = QJsonObject();
    m_selectedAvatarDragNormPerPixel = QPointF();

    if (!commit || !activeCutMutable() || speakerId.isEmpty()) {
        refresh();
        return;
    }

    const int64_t frame =
        qMax<int64_t>(0, refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
    const qreal xNorm = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    const qreal yNorm = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    const qreal boxSizeNorm = qBound<qreal>(
        -1.0, refObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0), 1.0);
    if (!saveSpeakerTrackingReferenceAt(speakerId, referenceIndex, frame, xNorm, yNorm, boxSizeNorm)) {
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
    const QString speakerId = selectedSpeakerId();
    if (!speakerId.isEmpty()) {
        m_lastSelectedSpeakerIdHint = speakerId;
    }
    updateSpeakerTrackingStatusLabel();
    updateSelectedSpeakerPanel();
}

void SpeakersTab::onSpeakersTableContextMenuRequested(const QPoint& pos)
{
    if (!m_widgets.speakersTable || !m_loadedTranscriptDoc.isObject()) {
        return;
    }

    const int row = m_widgets.speakersTable->itemAt(pos)
        ? m_widgets.speakersTable->itemAt(pos)->row()
        : m_widgets.speakersTable->currentRow();
    if (row < 0) {
        return;
    }

    QTableWidgetItem* idItem = m_widgets.speakersTable->item(row, 1);
    if (!idItem) {
        return;
    }
    const QString speakerId = idItem->data(Qt::UserRole).toString().trimmed();
    if (speakerId.isEmpty()) {
        return;
    }

    if (m_widgets.speakersTable->currentRow() != row) {
        m_widgets.speakersTable->setCurrentCell(row, 1);
    }

    int wordCount = 0;
    int skippedCount = 0;
    const QJsonArray segments = m_loadedTranscriptDoc.object().value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segmentObj = segValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            QString wordSpeaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
            if (wordSpeaker.isEmpty()) {
                wordSpeaker = segmentSpeaker;
            }
            if (wordSpeaker != speakerId) {
                continue;
            }
            if (wordObj.value(QStringLiteral("word")).toString().trimmed().isEmpty()) {
                continue;
            }
            ++wordCount;
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                ++skippedCount;
            }
        }
    }

    QMenu menu(m_widgets.speakersTable);
    QAction* skipAction = menu.addAction(QStringLiteral("Skip Speaker"));
    QAction* unskipAction = menu.addAction(QStringLiteral("Unskip Speaker"));
    menu.addSeparator();
    const QJsonObject profiles =
        m_loadedTranscriptDoc.object().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QJsonObject tracking = speakerFramingObject(profile);
    const bool trackingEnabled = transcriptTrackingEnabled(tracking);
    const bool hasTrackingModel = transcriptTrackingHasPointstream(tracking);
    bool hasRef1 = false;
    bool hasRef2 = false;
    transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef1Key, &hasRef1);
    transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef2Key, &hasRef2);
    const bool hasAnyRef = hasRef1 || hasRef2;
    const bool canMutate = activeCutMutable();

    QAction* enableTrackingAction = menu.addAction(QStringLiteral("Enable Framing"));
    QAction* disableTrackingAction = menu.addAction(QStringLiteral("Disable Framing"));
    QMenu* autoTrackMenu = menu.addMenu(QStringLiteral("AutoTrack"));
    QAction* runAutoTrackAction = nullptr;
    QAction* redoAutoTrackAction = nullptr;
    QAction* deleteAutoTrackAction = nullptr;
    if (hasTrackingModel) {
        redoAutoTrackAction = autoTrackMenu->addAction(QStringLiteral("Redo AutoTrack"));
        deleteAutoTrackAction = autoTrackMenu->addAction(QStringLiteral("Delete AutoTrack"));
    } else {
        runAutoTrackAction = autoTrackMenu->addAction(QStringLiteral("AutoTrack"));
    }

    skipAction->setEnabled(canMutate && wordCount > 0 && skippedCount < wordCount);
    unskipAction->setEnabled(canMutate && wordCount > 0 && skippedCount > 0);
    enableTrackingAction->setEnabled(canMutate && hasTrackingModel && !trackingEnabled);
    disableTrackingAction->setEnabled(canMutate && trackingEnabled);
    if (runAutoTrackAction) {
        runAutoTrackAction->setEnabled(canMutate && hasAnyRef);
    }
    if (redoAutoTrackAction) {
        redoAutoTrackAction->setEnabled(canMutate && hasAnyRef);
    }
    if (deleteAutoTrackAction) {
        deleteAutoTrackAction->setEnabled(canMutate && hasTrackingModel);
    }

    QAction* chosen = menu.exec(m_widgets.speakersTable->viewport()->mapToGlobal(pos));
    if (!chosen) {
        return;
    }
    if (!activeCutMutable()) {
        QMessageBox::information(
            nullptr,
            QStringLiteral("Speakers"),
            QStringLiteral("Speaker actions are editable only on derived cuts (not Original)."));
        return;
    }
    if (wordCount <= 0) {
        if (chosen != enableTrackingAction &&
            chosen != disableTrackingAction &&
            chosen != runAutoTrackAction &&
            chosen != redoAutoTrackAction &&
            chosen != deleteAutoTrackAction) {
            return;
        }
    }

    if (chosen == runAutoTrackAction || chosen == redoAutoTrackAction) {
        if (!runAutoTrackForSpeaker(speakerId, true)) {
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
        return;
    }

    if (chosen == deleteAutoTrackAction) {
        if (!deleteSpeakerAutoTrackPointstream(speakerId)) {
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
        return;
    }

    if (chosen == enableTrackingAction || chosen == disableTrackingAction) {
        const bool enable = (chosen == enableTrackingAction);
        if (!setSpeakerTrackingEnabled(speakerId, enable)) {
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
        return;
    }

    const bool skip = (chosen == skipAction);
    const QString actionLabel = skip ? QStringLiteral("skip") : QStringLiteral("unskip");
    const auto confirmation = QMessageBox::question(
        nullptr,
        QStringLiteral("Confirm Speaker Skip"),
        QStringLiteral("Do you want to %1 all transcript words for speaker '%2' in this cut?")
            .arg(actionLabel, speakerId));
    if (confirmation != QMessageBox::Yes) {
        return;
    }

    if (!setSpeakerSkipped(speakerId, skip)) {
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

void SpeakersTab::onSpeakersTableItemClicked(QTableWidgetItem* item)
{
    if (m_updating || !item || !m_widgets.speakersTable) {
        return;
    }

    const int column = item->column();
    const QString clickedSpeakerId = item->data(Qt::UserRole).toString().trimmed();
    if (!clickedSpeakerId.isEmpty() && m_widgets.speakersTable->currentRow() != item->row()) {
        m_widgets.speakersTable->setCurrentCell(item->row(), 1);
    }
    if (column == 5 && activeCutMutable()) {
        const QString speakerId = clickedSpeakerId.isEmpty() ? selectedSpeakerId() : clickedSpeakerId;
        if (speakerId.isEmpty()) {
            updateSpeakerTrackingStatusLabel();
            return;
        }
        if (!cycleFramingModeForSpeaker(speakerId)) {
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
        return;
    }
    if ((column == 0 || column == 6 || column == 7) && activeCutMutable()) {
        const QString speakerId = clickedSpeakerId.isEmpty() ? selectedSpeakerId() : clickedSpeakerId;
        if (speakerId.isEmpty()) {
            updateSpeakerTrackingStatusLabel();
            return;
        }
        armReferencePickForSpeaker(speakerId, (column == 7) ? 2 : 1);
        return;
    }

    // Preserve in-place editing workflow for editable columns.
    if (column == 2 || column == 3 || column == 4) {
        return;
    }
    const QString speakerId = clickedSpeakerId.isEmpty() ? selectedSpeakerId() : clickedSpeakerId;
    if (!speakerId.isEmpty()) {
        // Selecting a speaker row starts the "locate face" workflow.
        // The first picked reference becomes the basis for future framing.
        armReferencePickForSpeaker(speakerId, 1);
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
    QVector<int64_t> speakerFrames = speakerSourceFrames(m_loadedTranscriptDoc.object(), speakerId);
    if (speakerFrames.isEmpty()) {
        return false;
    }

    int64_t chosenSource = -1;
    int chosenIndex = -1;
    if (direction > 0) {
        for (int i = 0; i < speakerFrames.size(); ++i) {
            if (speakerFrames.at(i) > currentSourceFrame) {
                chosenIndex = i;
                break;
            }
        }
        if (chosenIndex < 0) {
            chosenIndex = 0;
        }
    } else {
        for (int i = speakerFrames.size() - 1; i >= 0; --i) {
            if (speakerFrames.at(i) < currentSourceFrame) {
                chosenIndex = i;
                break;
            }
        }
        if (chosenIndex < 0) {
            chosenIndex = speakerFrames.size() - 1;
        }
    }

    chosenSource = speakerFrames.at(chosenIndex);
    int64_t timelineFrame = clip->startFrame + (chosenSource - clip->sourceInFrame);
    const int64_t clipEndFrame = clip->startFrame + qMax<int64_t>(0, clip->durationFrames - 1);
    timelineFrame = qBound<int64_t>(clip->startFrame, timelineFrame, clipEndFrame);

    // Ensure button presses visibly move when multiple segments exist.
    if (timelineFrame == currentTimeline && speakerFrames.size() > 1) {
        if (direction > 0) {
            chosenIndex = (chosenIndex + 1) % speakerFrames.size();
        } else {
            chosenIndex = (chosenIndex - 1 + speakerFrames.size()) % speakerFrames.size();
        }
        chosenSource = speakerFrames.at(chosenIndex);
        timelineFrame = clip->startFrame + (chosenSource - clip->sourceInFrame);
        timelineFrame = qBound<int64_t>(clip->startFrame, timelineFrame, clipEndFrame);
    }

    m_deps.seekToTimelineFrame(timelineFrame);
    return true;
}

bool SpeakersTab::seekToSpeakerRandomSentence(const QString& speakerId)
{
    if (speakerId.isEmpty() || !m_loadedTranscriptDoc.isObject() || !m_deps.seekToTimelineFrame) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }
    const QVector<int64_t> speakerFrames = speakerSourceFrames(m_loadedTranscriptDoc.object(), speakerId);
    if (speakerFrames.isEmpty()) {
        return false;
    }
    const int idx = QRandomGenerator::global()->bounded(speakerFrames.size());
    const int64_t chosenSource = speakerFrames.at(idx);
    int64_t timelineFrame = clip->startFrame + (chosenSource - clip->sourceInFrame);
    const int64_t clipEndFrame = clip->startFrame + qMax<int64_t>(0, clip->durationFrames - 1);
    timelineFrame = qBound<int64_t>(clip->startFrame, timelineFrame, clipEndFrame);
    m_deps.seekToTimelineFrame(timelineFrame);
    return true;
}

bool SpeakersTab::navigateSpeakerSentence(const QString& speakerId, SentenceNavAction action)
{
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return false;
    }
    m_lastSelectedSpeakerIdHint = speakerId;
    switch (action) {
    case SentenceNavAction::Previous:
        return seekToSpeakerSegmentRelative(speakerId, -1);
    case SentenceNavAction::Next:
        return seekToSpeakerSegmentRelative(speakerId, +1);
    case SentenceNavAction::Random:
        return seekToSpeakerRandomSentence(speakerId);
    }
    return false;
}

void SpeakersTab::onSpeakerPreviousSentenceClicked()
{
    const QString speakerId = selectedSpeakerId();
    navigateSpeakerSentence(speakerId, SentenceNavAction::Previous);
}

void SpeakersTab::onSpeakerNextSentenceClicked()
{
    const QString speakerId = selectedSpeakerId();
    navigateSpeakerSentence(speakerId, SentenceNavAction::Next);
}

void SpeakersTab::onSpeakerRandomSentenceClicked()
{
    const QString speakerId = selectedSpeakerId();
    navigateSpeakerSentence(speakerId, SentenceNavAction::Random);
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

bool SpeakersTab::cycleFramingModeForSpeaker(const QString& speakerId)
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.trimmed().isEmpty()) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    QJsonObject tracking = speakerFramingObject(profile);
    const bool hasPointstream = transcriptTrackingHasPointstream(tracking);
    bool hasRef1 = false;
    bool hasRef2 = false;
    transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef1Key, &hasRef1);
    transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef2Key, &hasRef2);
    const QString currentMode =
        tracking.value(QString(kTranscriptSpeakerTrackingModeKey)).toString(QStringLiteral("Manual")).trimmed();

    QString nextMode = QStringLiteral("Manual");
    if (currentMode.compare(QStringLiteral("manual"), Qt::CaseInsensitive) == 0) {
        nextMode = QStringLiteral("ReferencePoints");
    } else if (currentMode.compare(QStringLiteral("referencepoints"), Qt::CaseInsensitive) == 0) {
        nextMode = hasPointstream ? QStringLiteral("AutoTrack") : QStringLiteral("Manual");
    } else {
        nextMode = QStringLiteral("Manual");
    }

    tracking[QString(kTranscriptSpeakerTrackingModeKey)] = nextMode;
    if (nextMode == QStringLiteral("Manual")) {
        tracking[QString(kTranscriptSpeakerTrackingEnabledKey)] = false;
        tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] = QStringLiteral("manual");
    } else if (nextMode == QStringLiteral("ReferencePoints")) {
        tracking[QString(kTranscriptSpeakerTrackingEnabledKey)] = false;
        tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] = QStringLiteral("refs_only");
    }

    setSpeakerFramingObject(profile, tracking);
    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);

    editor::TranscriptEngine engine;
    return engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc);
}

bool SpeakersTab::runNativeAutoTrackForSpeaker(const TimelineClip& clip,
                                               const QString& speakerId,
                                               const QJsonObject& ref1,
                                               const QJsonObject& ref2,
                                               const QVector<QPair<int64_t, int64_t>>& activeWindows,
                                               int64_t startFrame,
                                               int64_t endFrame,
                                               int stepFrames,
                                               QJsonArray* keyframesOut,
                                               QString* errorOut)
{
    Q_UNUSED(speakerId);
    if (!keyframesOut) {
        return false;
    }
    *keyframesOut = QJsonArray();

    auto resolveMediaPath = [&](const TimelineClip& currentClip, QString* reasonOut) {
        QString candidate = interactivePreviewMediaPathForClip(currentClip);
        QFileInfo candidateInfo(candidate);
        const bool candidateIsSequenceDir =
            !candidate.trimmed().isEmpty() &&
            candidateInfo.exists() &&
            candidateInfo.isDir() &&
            isImageSequencePath(candidate);
        const bool interactiveInvalid =
            candidate.trimmed().isEmpty() ||
            !candidateInfo.exists() ||
            (candidateInfo.isDir() && !candidateIsSequenceDir);
        if (!interactiveInvalid) {
            return candidate;
        }

        const QString sourcePath = currentClip.filePath.trimmed();
        const QFileInfo sourceInfo(sourcePath);
        if (!sourcePath.isEmpty() &&
            sourceInfo.exists() &&
            (sourceInfo.isFile() || isImageSequencePath(sourcePath))) {
            if (reasonOut) {
                *reasonOut = QStringLiteral("interactive media path invalid (%1); using source (%2)")
                                 .arg(candidate, sourcePath);
            }
            return sourcePath;
        }
        if (reasonOut) {
            *reasonOut = QStringLiteral("no playable media for native autotrack");
        }
        return QString();
    };

    QString mediaReason;
    const QString mediaPath = resolveMediaPath(clip, &mediaReason);
    if (mediaPath.isEmpty()) {
        if (errorOut) {
            *errorOut = mediaReason.isEmpty() ? QStringLiteral("native autotrack media path not found") : mediaReason;
        }
        return false;
    }

    editor::DecoderContext decoder(mediaPath);
    if (!decoder.initialize()) {
        if (errorOut) {
            *errorOut = QStringLiteral("native autotrack failed to initialize decoder: %1").arg(mediaPath);
        }
        return false;
    }

    const auto frameFromRef = [](const QJsonObject& refObj) -> int64_t {
        return refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
    };
    const auto xFromRef = [](const QJsonObject& refObj) -> qreal {
        return qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    };
    const auto yFromRef = [](const QJsonObject& refObj) -> qreal {
        return qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    };
    const auto boxFromRef = [](const QJsonObject& refObj) -> qreal {
        return qBound<qreal>(0.01, refObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(0.12), 1.0);
    };

    const double sourceFps = clip.sourceFps > 0.0
        ? clip.sourceFps
        : (decoder.info().fps > 0.0 ? decoder.info().fps : 30.0);
    const int64_t ref1Frame = frameFromRef(ref1);
    const int64_t ref2Frame = frameFromRef(ref2);
    const int64_t refStart = qMin(ref1Frame, ref2Frame);
    const int64_t refEnd = qMax(ref1Frame, ref2Frame);

    const int64_t ref1SourceFrame = canonicalToSourceFrameForTracking(ref1Frame, sourceFps);
    const int64_t ref2SourceFrame = canonicalToSourceFrameForTracking(ref2Frame, sourceFps);
    const QImage ref1Image =
        toGray8(decoder.decodeFrame(ref1SourceFrame).cpuImage());
    const QImage ref2Image =
        toGray8(decoder.decodeFrame(ref2SourceFrame).cpuImage());
    if (ref1Image.isNull() || ref2Image.isNull()) {
        if (errorOut) {
            *errorOut = QStringLiteral("native autotrack failed to decode reference frame(s)");
        }
        return false;
    }

    QImage tmpl1;
    QImage tmpl2;
    if (!cropSquareGray(ref1Image, xFromRef(ref1), yFromRef(ref1), boxFromRef(ref1), &tmpl1, nullptr, nullptr, nullptr) ||
        !cropSquareGray(ref2Image, xFromRef(ref2), yFromRef(ref2), boxFromRef(ref2), &tmpl2, nullptr, nullptr, nullptr)) {
        if (errorOut) {
            *errorOut = QStringLiteral("native autotrack failed to create templates");
        }
        return false;
    }

    const int64_t start = qMin(startFrame, endFrame);
    const int64_t end = qMax(startFrame, endFrame);
    const int64_t spanRef = qMax<int64_t>(1, refEnd - refStart);
    const int64_t step = qMax<int64_t>(1, stepFrames);
    QVector<QPair<int64_t, int64_t>> windows = activeWindows;
    if (windows.isEmpty()) {
        windows.push_back(qMakePair(start, end));
    }
    std::sort(
        windows.begin(),
        windows.end(),
        [](const QPair<int64_t, int64_t>& a, const QPair<int64_t, int64_t>& b) {
            if (a.first == b.first) {
                return a.second < b.second;
            }
            return a.first < b.first;
        });
    QVector<QPair<int64_t, int64_t>> mergedWindows;
    for (const auto& window : windows) {
        const int64_t wStart = qMin(window.first, window.second);
        const int64_t wEnd = qMax(window.first, window.second);
        if (mergedWindows.isEmpty()) {
            mergedWindows.push_back(qMakePair(wStart, wEnd));
            continue;
        }
        auto& last = mergedWindows.last();
        if (wStart <= (last.second + step)) {
            last.second = qMax(last.second, wEnd);
        } else {
            mergedWindows.push_back(qMakePair(wStart, wEnd));
        }
    }
    int totalSteps = 0;
    for (const auto& window : mergedWindows) {
        totalSteps += qMax(1, static_cast<int>(((qMax<int64_t>(0, window.second - window.first)) / step) + 1));
    }
    totalSteps = qMax(1, totalSteps);
    const qreal minTrackConfidence = 0.24;
    const qreal minAcceptConfidence = 0.15;
    const int reacquireInterval = 12;
    const int hardLostThreshold = 5;

    QDialog progressDialog;
    progressDialog.setWindowTitle(QStringLiteral("AutoTrack Progress"));
    progressDialog.setWindowFlag(Qt::Window, true);
    progressDialog.setWindowModality(Qt::NonModal);
    progressDialog.resize(560, 420);
    auto* progressLayout = new QVBoxLayout(&progressDialog);
    progressLayout->setContentsMargins(12, 12, 12, 12);
    progressLayout->setSpacing(8);
    auto* progressStatus = new QLabel(QStringLiteral("Initializing native tracker..."), &progressDialog);
    progressStatus->setWordWrap(true);
    progressLayout->addWidget(progressStatus);
    auto* progressPreview = new QLabel(&progressDialog);
    progressPreview->setMinimumSize(480, 270);
    progressPreview->setAlignment(Qt::AlignCenter);
    progressPreview->setStyleSheet(
        QStringLiteral("QLabel { background: #060a10; border: 1px solid #263344; border-radius: 6px; color: #8ea5c2; }"));
    progressPreview->setText(QStringLiteral("Preparing preview..."));
    progressLayout->addWidget(progressPreview, 1);
    auto* progressBar = new QProgressBar(&progressDialog);
    progressBar->setRange(0, totalSteps);
    progressBar->setValue(0);
    progressLayout->addWidget(progressBar);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &progressDialog);
    progressLayout->addWidget(cancelButton, 0, Qt::AlignRight);
    bool canceledByUser = false;
    connect(cancelButton, &QPushButton::clicked, &progressDialog, [&]() {
        canceledByUser = true;
        progressStatus->setText(QStringLiteral("Canceling..."));
    });
    progressDialog.show();
    QApplication::processEvents();

    qreal lastX = -1.0;
    qreal lastY = -1.0;
    qreal lastBox = -1.0;
    qreal velocityX = 0.0;
    qreal velocityY = 0.0;
    int lostCount = 0;
    enum class TrackingState {
        Tracking,
        Reacquire,
        Lost
    };
    TrackingState state = TrackingState::Reacquire;
    QJsonArray out;
    int64_t processed = 0;
    for (const auto& window : mergedWindows) {
        const int64_t windowStart = qMin(window.first, window.second);
        const int64_t windowEnd = qMax(window.first, window.second);
        // Reset motion state between non-contiguous speaking windows.
        lastX = -1.0;
        lastY = -1.0;
        lastBox = -1.0;
        velocityX = 0.0;
        velocityY = 0.0;
        lostCount = 0;
        state = TrackingState::Reacquire;
        for (int64_t frame = windowStart; frame <= windowEnd; frame += step) {
        qreal predictedX = 0.5;
        qreal predictedY = 0.5;
        qreal predictedBox = 0.12;
        if (lastX >= 0.0 && lastY >= 0.0 && lastBox > 0.0) {
            const qreal stepFactor = qBound<qreal>(1.0, static_cast<qreal>(step) / 3.0, 20.0);
            predictedX = lastX + (velocityX * stepFactor);
            predictedY = lastY + (velocityY * stepFactor);
            predictedBox = lastBox;
        } else {
            const qreal t = qBound<qreal>(
                0.0, static_cast<qreal>(frame - refStart) / static_cast<qreal>(spanRef), 1.0);
            predictedX = xFromRef(ref1) + ((xFromRef(ref2) - xFromRef(ref1)) * t);
            predictedY = yFromRef(ref1) + ((yFromRef(ref2) - yFromRef(ref1)) * t);
            predictedBox = boxFromRef(ref1) + ((boxFromRef(ref2) - boxFromRef(ref1)) * t);
        }

        const int64_t sourceFrame = canonicalToSourceFrameForTracking(frame, sourceFps);
        const QImage frameGray = toGray8(decoder.decodeFrame(sourceFrame).cpuImage());
        if (frameGray.isNull()) {
            continue;
        }
        if (canceledByUser) {
            if (errorOut) {
                *errorOut = QStringLiteral("user canceled native autotrack");
            }
            return false;
        }

        struct Candidate {
            bool ok = false;
            qreal x = 0.0;
            qreal y = 0.0;
            qreal b = 0.12;
            qreal conf = 0.0;
            QString source;
            qreal score = -1.0;
        };
        auto evaluateCandidate = [&](const QImage& tmpl,
                                     const QString& sourceTag,
                                     qreal searchScale,
                                     int strideCap) {
            Candidate c;
            c.source = sourceTag;
            c.ok = trackTemplateSad(
                frameGray, predictedX, predictedY, predictedBox, tmpl, searchScale, strideCap,
                &c.x, &c.y, &c.b, &c.conf);
            if (!c.ok) {
                return c;
            }
            const qreal movePenalty =
                std::sqrt(std::pow(c.x - predictedX, 2.0) + std::pow(c.y - predictedY, 2.0)) * 0.35;
            c.score = c.conf - movePenalty;
            return c;
        };

        const bool periodicReacquire = (processed % reacquireInterval) == 0;
        const bool shouldReacquire = (state != TrackingState::Tracking) || periodicReacquire;
        const qreal narrowSearchScale = 1.8;
        const qreal wideSearchScale = 3.4;

        Candidate best;
        {
            const Candidate n1 = evaluateCandidate(
                tmpl1, QStringLiteral("autotrack_native_cpp_v2_template1"), narrowSearchScale, 3);
            const Candidate n2 = evaluateCandidate(
                tmpl2, QStringLiteral("autotrack_native_cpp_v2_template2"), narrowSearchScale, 3);
            if (n1.ok && (!best.ok || n1.score > best.score)) {
                best = n1;
            }
            if (n2.ok && (!best.ok || n2.score > best.score)) {
                best = n2;
            }
        }
        if (shouldReacquire && (!best.ok || best.conf < minTrackConfidence)) {
            const Candidate w1 = evaluateCandidate(
                tmpl1, QStringLiteral("autotrack_native_cpp_v2_reacquire1"), wideSearchScale, 5);
            const Candidate w2 = evaluateCandidate(
                tmpl2, QStringLiteral("autotrack_native_cpp_v2_reacquire2"), wideSearchScale, 5);
            if (w1.ok && (!best.ok || w1.score > best.score)) {
                best = w1;
            }
            if (w2.ok && (!best.ok || w2.score > best.score)) {
                best = w2;
            }
        }
        if (!best.ok) {
            continue;
        }

        qreal x = best.x;
        qreal y = best.y;
        qreal b = best.b;
        qreal c = best.conf;
        QString source = best.source;

        if (lastX >= 0.0 && lastY >= 0.0) {
            const qreal jumpDistance = std::sqrt(std::pow(x - lastX, 2.0) + std::pow(y - lastY, 2.0));
            const qreal maxJump = 0.10 + (0.02 * static_cast<qreal>(qMin(lostCount, 8)));
            if (jumpDistance > maxJump && c < 0.45) {
                x = (0.80 * lastX) + (0.20 * x);
                y = (0.80 * lastY) + (0.20 * y);
                b = (0.80 * lastBox) + (0.20 * b);
                source = QStringLiteral("autotrack_native_cpp_v2_jump_clamped");
            }
        }

        if (c < minAcceptConfidence && lastX >= 0.0 && lastY >= 0.0 && lastBox > 0.0) {
            x = lastX;
            y = lastY;
            b = lastBox;
            ++lostCount;
            state = (lostCount >= hardLostThreshold) ? TrackingState::Lost : TrackingState::Reacquire;
            c = (lostCount >= hardLostThreshold) ? 0.02 : 0.08;
            source = (lostCount >= hardLostThreshold)
                ? QStringLiteral("autotrack_native_cpp_v2_offscreen_hold")
                : QStringLiteral("autotrack_native_cpp_v2_hold");
        } else {
            lostCount = 0;
            state = (c >= minTrackConfidence) ? TrackingState::Tracking : TrackingState::Reacquire;
        }

        if (lastX >= 0.0 && lastY >= 0.0 && lastBox > 0.0) {
            const qreal smoothingAlpha = (c >= minTrackConfidence) ? 0.55 : 0.28;
            x = (lastX * (1.0 - smoothingAlpha)) + (x * smoothingAlpha);
            y = (lastY * (1.0 - smoothingAlpha)) + (y * smoothingAlpha);
            b = (lastBox * (1.0 - smoothingAlpha)) + (b * smoothingAlpha);
        }

        if (lastX >= 0.0 && lastY >= 0.0) {
            const qreal blend = 0.45;
            const qreal instVx = x - lastX;
            const qreal instVy = y - lastY;
            velocityX = (velocityX * (1.0 - blend)) + (instVx * blend);
            velocityY = (velocityY * (1.0 - blend)) + (instVy * blend);
        } else {
            velocityX = 0.0;
            velocityY = 0.0;
        }
        lastX = qBound<qreal>(0.0, x, 1.0);
        lastY = qBound<qreal>(0.0, y, 1.0);
        lastBox = qBound<qreal>(0.01, b, 1.0);
        QJsonObject point;
        point[QString(kTranscriptSpeakerTrackingFrameKey)] = static_cast<qint64>(frame);
        point[QString(kTranscriptSpeakerLocationXKey)] = lastX;
        point[QString(kTranscriptSpeakerLocationYKey)] = lastY;
        point[QString(kTranscriptSpeakerTrackingBoxSizeKey)] = lastBox;
        point[QString(kTranscriptSpeakerTrackingConfidenceKey)] = qBound<qreal>(0.0, c, 1.0);
        point[QString(kTranscriptSpeakerTrackingSourceKey)] = source;
        out.push_back(point);
        ++processed;

        const int stepIndex = qMax(0, qMin(totalSteps, static_cast<int>(processed)));
        progressBar->setValue(stepIndex);
        progressStatus->setText(
            QStringLiteral("Tracking frame %1/%2 | conf=%3 | mode=%4")
                .arg(stepIndex)
                .arg(totalSteps)
                .arg(QString::number(c, 'f', 3))
                .arg(source));

        QImage preview = frameGray.convertToFormat(QImage::Format_RGB32);
        {
            QPainter painter(&preview);
            painter.setRenderHint(QPainter::Antialiasing, true);
            const int pw = preview.width();
            const int ph = preview.height();
            const int minSide = qMax(1, qMin(pw, ph));
            const int cx = static_cast<int>(std::round(lastX * static_cast<qreal>(pw)));
            const int cy = static_cast<int>(std::round(lastY * static_cast<qreal>(ph)));
            const int side = qBound(12, static_cast<int>(std::round(lastBox * static_cast<qreal>(minSide))), minSide);
            const QRect boxRect(
                qBound(0, cx - (side / 2), qMax(0, pw - side)),
                qBound(0, cy - (side / 2), qMax(0, ph - side)),
                side,
                side);
            painter.setPen(QPen(QColor(QStringLiteral("#2fcf73")), 2));
            painter.drawRect(boxRect);
            painter.setPen(QPen(QColor(QStringLiteral("#ffdf5d")), 1));
            painter.drawLine(cx - 8, cy, cx + 8, cy);
            painter.drawLine(cx, cy - 8, cx, cy + 8);
        }
        progressPreview->setPixmap(QPixmap::fromImage(
            preview.scaled(
                progressPreview->size(),
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation)));
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        if (canceledByUser) {
            if (errorOut) {
                *errorOut = QStringLiteral("user canceled native autotrack");
            }
            return false;
        }
    }
        if (!out.isEmpty() && lastX >= 0.0 && lastY >= 0.0 && lastBox > 0.0) {
            const int64_t lastFrame = out.at(out.size() - 1).toObject().value(
                QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            if (lastFrame != windowEnd) {
                QJsonObject tail;
                tail[QString(kTranscriptSpeakerTrackingFrameKey)] = static_cast<qint64>(windowEnd);
                tail[QString(kTranscriptSpeakerLocationXKey)] = lastX;
                tail[QString(kTranscriptSpeakerLocationYKey)] = lastY;
                tail[QString(kTranscriptSpeakerTrackingBoxSizeKey)] = lastBox;
                tail[QString(kTranscriptSpeakerTrackingConfidenceKey)] = 0.5;
                tail[QString(kTranscriptSpeakerTrackingSourceKey)] = QStringLiteral("autotrack_native_cpp_v2_tail");
                out.push_back(tail);
            }
        }
    }

    if (out.isEmpty() || processed < 2) {
        progressDialog.close();
        if (errorOut) {
            *errorOut = QStringLiteral("native autotrack produced insufficient keyframes");
        }
        return false;
    }
    progressBar->setValue(totalSteps);
    progressStatus->setText(QStringLiteral("Done."));
    progressDialog.close();
    *keyframesOut = out;
    return true;
}

bool SpeakersTab::runDockerAutoTrackForSpeaker(const TimelineClip& clip,
                                               const QString& speakerId,
                                               const QJsonObject& ref1,
                                               const QJsonObject& ref2,
                                               int64_t startFrame,
                                               int64_t endFrame,
                                               int stepFrames,
                                               QJsonArray* keyframesOut,
                                               QString* errorOut)
{
    Q_UNUSED(speakerId);
    if (!keyframesOut) {
        return false;
    }
    *keyframesOut = QJsonArray();

    const QString dockerPath = QStandardPaths::findExecutable(QStringLiteral("docker"));
    if (dockerPath.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("docker not found in PATH");
        }
        return false;
    }

    const QString scriptPath =
        QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("speaker_autotrack.py"));
    if (!QFileInfo::exists(scriptPath)) {
        if (errorOut) {
            *errorOut = QStringLiteral("speaker_autotrack.py not found");
        }
        return false;
    }

    auto resolveDockerAutotrackMediaPath = [&](const TimelineClip& currentClip, QString* reasonOut) {
        QString candidate = interactivePreviewMediaPathForClip(currentClip);
        QFileInfo candidateInfo(candidate);
        const bool candidateIsSequenceDir =
            !candidate.trimmed().isEmpty() &&
            candidateInfo.exists() &&
            candidateInfo.isDir() &&
            isImageSequencePath(candidate);
        const bool interactiveInvalid =
            candidate.trimmed().isEmpty() ||
            !candidateInfo.exists() ||
            (candidateInfo.isDir() && !candidateIsSequenceDir);
        if (!interactiveInvalid) {
            return candidate;
        }

        const QString sourcePath = currentClip.filePath.trimmed();
        const QFileInfo sourceInfo(sourcePath);
        if (!sourcePath.isEmpty() &&
            sourceInfo.exists() &&
            sourceInfo.isFile() &&
            !isImageSequencePath(sourcePath)) {
            if (reasonOut) {
                *reasonOut = QStringLiteral("interactive media path was non-video (%1); using source file (%2)")
                                 .arg(candidate, sourcePath);
            }
            return sourcePath;
        }
        if (reasonOut) {
            *reasonOut = QStringLiteral("interactive media path is not a playable video or image sequence: %1")
                             .arg(candidate);
        }
        return QString();
    };

    QString mediaPathReason;
    const QString mediaPath = resolveDockerAutotrackMediaPath(clip, &mediaPathReason);
    const QFileInfo mediaInfo(mediaPath);
    const bool mediaIsSequenceDir =
        !mediaPath.isEmpty() &&
        mediaInfo.exists() &&
        mediaInfo.isDir() &&
        isImageSequencePath(mediaPath);
    if (mediaPath.isEmpty() ||
        !mediaInfo.exists() ||
        (!mediaInfo.isFile() && !mediaIsSequenceDir)) {
        if (errorOut) {
            if (mediaPathReason.isEmpty()) {
                *errorOut = QStringLiteral("autotrack requires a playable video or image sequence, but none was found");
            } else {
                *errorOut = mediaPathReason;
            }
        }
        return false;
    }

    QTemporaryDir outDir;
    if (!outDir.isValid()) {
        if (errorOut) {
            *errorOut = QStringLiteral("failed to create temporary output directory");
        }
        return false;
    }

    const auto refFieldDouble = [](const QJsonObject& refObj, const QLatin1String& key, double fallback) {
        return refObj.value(QString(key)).toDouble(fallback);
    };
    const auto refFieldFrame = [](const QJsonObject& refObj) {
        return refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
    };

    const QString dockerImage = qEnvironmentVariable(
        "SPEAKER_AUTOTRACK_DOCKER_IMAGE",
        QStringLiteral("speaker-autotrack:latest"));
    const QString dockerfilePath = qEnvironmentVariable(
        "SPEAKER_AUTOTRACK_DOCKERFILE",
        QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("speaker_autotrack.dockerfile")));
    const QString dockerBuildContext = qEnvironmentVariable(
        "SPEAKER_AUTOTRACK_DOCKER_CONTEXT",
        QDir::currentPath());
    const QString outputFile = QDir(outDir.path()).absoluteFilePath(QStringLiteral("speaker_track.json"));
    const QString preferGpuEnv =
        qEnvironmentVariable("SPEAKER_AUTOTRACK_PREFER_GPU", QStringLiteral("1")).trimmed().toLower();
    const bool preferGpuDefault =
        !(preferGpuEnv == QStringLiteral("0") ||
          preferGpuEnv == QStringLiteral("false") ||
          preferGpuEnv == QStringLiteral("no") ||
          preferGpuEnv == QStringLiteral("off"));
    struct PreflightSettings {
        bool preferGpu = true;
        bool autoBuildMissingImage = true;
        bool forceRebuildImage = false;
        bool confirmRunCommand = true;
    };
    static bool s_preflightInitialized = false;
    static PreflightSettings s_preflightDefaults;
    if (!s_preflightInitialized) {
        s_preflightDefaults.preferGpu = preferGpuDefault;
        s_preflightInitialized = true;
    }
    PreflightSettings preflight = s_preflightDefaults;

    const auto shellQuote = [](const QString& arg) {
        QString escaped = arg;
        escaped.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
        return QStringLiteral("'") + escaped + QStringLiteral("'");
    };
    const auto formatCommand = [&](const QString& program, const QStringList& args) {
        QStringList parts;
        parts.reserve(args.size() + 1);
        parts.push_back(shellQuote(program));
        for (const QString& arg : args) {
            parts.push_back(shellQuote(arg));
        }
        return parts.join(QStringLiteral(" "));
    };
    const QStringList inspectArgs = {
        QStringLiteral("image"),
        QStringLiteral("inspect"),
        dockerImage.trimmed()
    };
    const QStringList buildArgs = {
        QStringLiteral("build"),
        QStringLiteral("-f"),
        dockerfilePath,
        QStringLiteral("-t"),
        dockerImage.trimmed(),
        dockerBuildContext
    };

    auto buildRunArgs = [&](bool gpu) {
        QStringList args;
        args << QStringLiteral("run")
             << QStringLiteral("--rm");
        if (gpu) {
            args << QStringLiteral("--gpus") << QStringLiteral("all");
        }
        args << QStringLiteral("-v") << QStringLiteral("%1:/work/app:ro").arg(QFileInfo(scriptPath).absolutePath())
             << QStringLiteral("-v") << QStringLiteral("%1:/work/video:ro").arg(mediaInfo.absolutePath())
             << QStringLiteral("-v") << QStringLiteral("%1:/work/out").arg(outDir.path())
             << dockerImage.trimmed()
             << QStringLiteral("python")
             << QStringLiteral("/work/app/%1").arg(QFileInfo(scriptPath).fileName())
             << QStringLiteral("--video") << QStringLiteral("/work/video/%1").arg(mediaInfo.fileName())
             << QStringLiteral("--output") << QStringLiteral("/work/out/%1").arg(QFileInfo(outputFile).fileName())
             << QStringLiteral("--ref1-frame") << QString::number(refFieldFrame(ref1))
             << QStringLiteral("--ref1-x") << QString::number(refFieldDouble(ref1, kTranscriptSpeakerLocationXKey, 0.5), 'f', 8)
             << QStringLiteral("--ref1-y") << QString::number(refFieldDouble(ref1, kTranscriptSpeakerLocationYKey, 0.85), 'f', 8)
             << QStringLiteral("--ref1-box") << QString::number(qBound(0.01, refFieldDouble(ref1, kTranscriptSpeakerTrackingBoxSizeKey, 0.33), 1.0), 'f', 8)
             << QStringLiteral("--ref2-frame") << QString::number(refFieldFrame(ref2))
             << QStringLiteral("--ref2-x") << QString::number(refFieldDouble(ref2, kTranscriptSpeakerLocationXKey, 0.5), 'f', 8)
             << QStringLiteral("--ref2-y") << QString::number(refFieldDouble(ref2, kTranscriptSpeakerLocationYKey, 0.85), 'f', 8)
             << QStringLiteral("--ref2-box") << QString::number(qBound(0.01, refFieldDouble(ref2, kTranscriptSpeakerTrackingBoxSizeKey, 0.33), 1.0), 'f', 8)
             << QStringLiteral("--source-fps") << QString::number(clip.sourceFps > 0.0 ? clip.sourceFps : 30.0, 'f', 6)
             << QStringLiteral("--start-frame") << QString::number(qMax<int64_t>(0, startFrame))
             << QStringLiteral("--end-frame") << QString::number(qMax<int64_t>(0, endFrame))
             << QStringLiteral("--step") << QString::number(qMax(1, stepFrames));
        if (gpu) {
            args << QStringLiteral("--prefer-gpu");
        }
        return args;
    };

    {
        QDialog dialog;
        dialog.setWindowTitle(QStringLiteral("Docker AutoTrack Preflight"));
        dialog.resize(980, 620);

        auto* layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);

        auto* title = new QLabel(
            QStringLiteral("Review and adjust Docker AutoTrack settings before execution."),
            &dialog);
        title->setWordWrap(true);
        layout->addWidget(title);

        auto* preferGpuCheck = new QCheckBox(QStringLiteral("Prefer GPU execution (--gpus all), then fallback to CPU"), &dialog);
        preferGpuCheck->setChecked(preflight.preferGpu);
        layout->addWidget(preferGpuCheck);

        auto* autoBuildCheck = new QCheckBox(QStringLiteral("Auto-build image when missing"), &dialog);
        autoBuildCheck->setChecked(preflight.autoBuildMissingImage);
        layout->addWidget(autoBuildCheck);

        auto* forceRebuildCheck = new QCheckBox(QStringLiteral("Force rebuild image before run"), &dialog);
        forceRebuildCheck->setChecked(preflight.forceRebuildImage);
        layout->addWidget(forceRebuildCheck);

        auto* confirmRunCheck = new QCheckBox(QStringLiteral("Confirm the final docker run command before execution"), &dialog);
        confirmRunCheck->setChecked(preflight.confirmRunCommand);
        layout->addWidget(confirmRunCheck);

        auto* commandPreview = new QPlainTextEdit(&dialog);
        commandPreview->setReadOnly(true);
        commandPreview->setLineWrapMode(QPlainTextEdit::NoWrap);
        commandPreview->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { background: #05080c; color: #d6e2ee; border: 1px solid #24303c; "
            "border-radius: 8px; font-family: monospace; font-size: 12px; }"));
        layout->addWidget(commandPreview, 1);

        auto refreshPreview = [&]() {
            QStringList lines;
            lines << QStringLiteral("Image check:");
            lines << formatCommand(QStringLiteral("docker"), inspectArgs);
            lines << QString();
            lines << QStringLiteral("Image build:");
            lines << formatCommand(QStringLiteral("docker"), buildArgs);
            lines << QString();
            lines << QStringLiteral("Run command (GPU):");
            lines << formatCommand(QStringLiteral("docker"), buildRunArgs(true));
            lines << QString();
            lines << QStringLiteral("Run command (CPU):");
            lines << formatCommand(QStringLiteral("docker"), buildRunArgs(false));
            commandPreview->setPlainText(lines.join(QStringLiteral("\n")));
        };

        connect(forceRebuildCheck, &QCheckBox::toggled, &dialog, [&](bool checked) {
            if (checked && !autoBuildCheck->isChecked()) {
                autoBuildCheck->setChecked(true);
            }
        });
        connect(autoBuildCheck, &QCheckBox::toggled, &dialog, [&](bool checked) {
            if (!checked && forceRebuildCheck->isChecked()) {
                forceRebuildCheck->setChecked(false);
            }
        });
        refreshPreview();

        auto* buttons = new QHBoxLayout;
        buttons->addStretch(1);
        auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
        auto* proceedButton = new QPushButton(QStringLiteral("Proceed"), &dialog);
        buttons->addWidget(cancelButton);
        buttons->addWidget(proceedButton);
        layout->addLayout(buttons);
        QObject::connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
        QObject::connect(proceedButton, &QPushButton::clicked, &dialog, &QDialog::accept);

        if (dialog.exec() != QDialog::Accepted) {
            if (errorOut) {
                *errorOut = QStringLiteral("user canceled docker autotrack command");
            }
            return false;
        }

        preflight.preferGpu = preferGpuCheck->isChecked();
        preflight.autoBuildMissingImage = autoBuildCheck->isChecked();
        preflight.forceRebuildImage = forceRebuildCheck->isChecked();
        preflight.confirmRunCommand = confirmRunCheck->isChecked();
        s_preflightDefaults = preflight;
    }

    auto runDocker = [&](const QStringList& dockerArgs, int startTimeoutMs, QString* outputOut) -> bool {
        QProcess proc;
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start(dockerPath, dockerArgs);
        if (!proc.waitForStarted(startTimeoutMs)) {
            if (outputOut) {
                *outputOut = QStringLiteral("failed to start docker process");
            }
            return false;
        }
        proc.waitForFinished(-1);
        const QString output = QString::fromUtf8(proc.readAllStandardOutput());
        if (outputOut) {
            *outputOut = output;
        }
        return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
    };
    auto runDockerInConsole = [&](const QStringList& dockerArgs,
                                  int startTimeoutMs,
                                  const QString& title,
                                  QString* outputOut) -> bool {
        QDialog dialog;
        dialog.setWindowTitle(title);
        dialog.resize(920, 560);

        auto* layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);

        auto* titleLabel = new QLabel(&dialog);
        titleLabel->setWordWrap(true);
        layout->addWidget(titleLabel);

        auto* output = new QPlainTextEdit(&dialog);
        output->setReadOnly(true);
        output->setLineWrapMode(QPlainTextEdit::NoWrap);
        output->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { background: #05080c; color: #d6e2ee; border: 1px solid #24303c; "
            "border-radius: 8px; font-family: monospace; font-size: 12px; }"));
        layout->addWidget(output, 1);

        auto* autoScrollBox = new QCheckBox(QStringLiteral("Auto-scroll"), &dialog);
        autoScrollBox->setChecked(true);
        layout->addWidget(autoScrollBox);

        auto* inputRow = new QHBoxLayout;
        inputRow->setContentsMargins(0, 0, 0, 0);
        inputRow->setSpacing(8);
        auto* inputLabel = new QLabel(QStringLiteral("stdin"), &dialog);
        auto* inputLine = new QLineEdit(&dialog);
        inputLine->setPlaceholderText(QStringLiteral("Optional process input, then Send"));
        auto* sendButton = new QPushButton(QStringLiteral("Send"), &dialog);
        auto* closeButton = new QPushButton(QStringLiteral("Close"), &dialog);
        closeButton->setEnabled(false);
        inputRow->addWidget(inputLabel);
        inputRow->addWidget(inputLine, 1);
        inputRow->addWidget(sendButton);
        inputRow->addWidget(closeButton);
        layout->addLayout(inputRow);

        QProcess process(&dialog);
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.setWorkingDirectory(QDir::currentPath());

        QString collectedOutput;
        const auto appendOutput = [&](const QString& text) {
            if (text.isEmpty()) {
                return;
            }
            collectedOutput += text;
            if (autoScrollBox->isChecked()) {
                output->moveCursor(QTextCursor::End);
            }
            output->insertPlainText(text);
            if (autoScrollBox->isChecked()) {
                output->moveCursor(QTextCursor::End);
            }
        };

        connect(&process, &QProcess::readyReadStandardOutput, &dialog, [&]() {
            appendOutput(QString::fromLocal8Bit(process.readAllStandardOutput()));
        });
        connect(&process, &QProcess::started, &dialog, [&]() {
            appendOutput(QStringLiteral("$ %1\n").arg(formatCommand(QStringLiteral("docker"), dockerArgs)));
        });
        connect(&process, &QProcess::errorOccurred, &dialog, [&](QProcess::ProcessError error) {
            appendOutput(QStringLiteral("\n[process error] %1\n").arg(static_cast<int>(error)));
            closeButton->setEnabled(true);
        });
        connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &dialog,
                [&](int exitCode, QProcess::ExitStatus exitStatus) {
                    appendOutput(QStringLiteral("\n[process finished] exitCode=%1 status=%2\n")
                                     .arg(exitCode)
                                     .arg(exitStatus == QProcess::NormalExit
                                              ? QStringLiteral("normal")
                                              : QStringLiteral("crashed")));
                    closeButton->setEnabled(true);
                });
        connect(sendButton, &QPushButton::clicked, &dialog, [&]() {
            const QString text = inputLine->text();
            if (text.isEmpty()) {
                return;
            }
            process.write(text.toUtf8());
            process.write("\n");
            appendOutput(QStringLiteral("> %1\n").arg(text));
            inputLine->clear();
        });
        connect(inputLine, &QLineEdit::returnPressed, sendButton, &QPushButton::click);
        connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
        connect(&dialog, &QDialog::finished, &dialog, [&](int) {
            if (process.state() != QProcess::NotRunning) {
                process.kill();
                process.waitForFinished(1000);
            }
        });

        titleLabel->setText(
            QStringLiteral("docker command\n%1").arg(formatCommand(QStringLiteral("docker"), dockerArgs)));
        process.start(dockerPath, dockerArgs);
        if (!process.waitForStarted(startTimeoutMs)) {
            appendOutput(QStringLiteral("[process error] failed to start docker process\n"));
            closeButton->setEnabled(true);
        }

        dialog.exec();
        if (outputOut) {
            *outputOut = collectedOutput;
        }
        return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    };

    // Ensure image is available locally; build from local Dockerfile when missing.
    {
        bool imageReady = false;
        if (!preflight.forceRebuildImage) {
            QString inspectOutput;
            imageReady = runDocker(inspectArgs, 10000, &inspectOutput);
        }
        if (!imageReady) {
            if (!preflight.autoBuildMissingImage) {
                if (errorOut) {
                    *errorOut = QStringLiteral(
                                    "docker image %1 was not available and auto-build is disabled in preflight settings")
                                    .arg(dockerImage.trimmed());
                }
                return false;
            }
            const QFileInfo dockerfileInfo(dockerfilePath);
            if (!dockerfileInfo.exists()) {
                if (errorOut) {
                    *errorOut = QStringLiteral(
                                    "docker image %1 not found and dockerfile missing: %2\n"
                                    "Set SPEAKER_AUTOTRACK_DOCKER_IMAGE to an existing image or provide SPEAKER_AUTOTRACK_DOCKERFILE.")
                                    .arg(dockerImage.trimmed(), dockerfilePath);
                }
                return false;
            }
            QString buildOutput;
            if (!runDocker(
                    QStringList{
                        QStringLiteral("build"),
                        QStringLiteral("-f"),
                        dockerfileInfo.absoluteFilePath(),
                        QStringLiteral("-t"),
                        dockerImage.trimmed(),
                        dockerBuildContext},
                    10000,
                    &buildOutput)) {
                if (errorOut) {
                    *errorOut = QStringLiteral(
                                    "docker image build failed for %1 using %2\n%3")
                                    .arg(dockerImage.trimmed(), dockerfileInfo.absoluteFilePath(), buildOutput.trimmed());
                }
                return false;
            }
        }
    }
    if (preflight.confirmRunCommand) {
        const QStringList gpuArgs = buildRunArgs(true);
        const QStringList cpuArgs = buildRunArgs(false);
        QString promptText;
        if (preflight.preferGpu) {
            promptText = QStringLiteral(
                "Docker AutoTrack is about to run these commands.\n\n"
                "Primary (GPU preferred):\n%1\n\n"
                "Fallback (CPU):\n%2\n\n"
                "Proceed?")
                             .arg(formatCommand(QStringLiteral("docker"), gpuArgs),
                                  formatCommand(QStringLiteral("docker"), cpuArgs));
        } else {
            promptText = QStringLiteral(
                "Docker AutoTrack is about to run this command:\n\n%1\n\nProceed?")
                             .arg(formatCommand(QStringLiteral("docker"), cpuArgs));
        }
        const QMessageBox::StandardButton approved = QMessageBox::question(
            nullptr,
            QStringLiteral("Approve Docker AutoTrack Command"),
            promptText,
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (approved != QMessageBox::Yes) {
            if (errorOut) {
                *errorOut = QStringLiteral("user canceled docker autotrack command");
            }
            return false;
        }
    }

    QString output;
    if (preflight.preferGpu && !runDockerInConsole(
                         buildRunArgs(true),
                         10000,
                         QStringLiteral("Speaker AutoTrack (Docker GPU)"),
                         &output)) {
        // GPU-first policy: if GPU path fails, fall back to CPU automatically.
        output.clear();
    }
    if (output.isEmpty() && !runDockerInConsole(
                                  buildRunArgs(false),
                                  10000,
                                  QStringLiteral("Speaker AutoTrack (Docker CPU Fallback)"),
                                  &output)) {
        if (errorOut) {
            *errorOut = QStringLiteral("docker autotrack failed:\n%1")
                            .arg(output.trimmed());
        }
        return false;
    }

    QFile outJson(outputFile);
    if (!outJson.open(QIODevice::ReadOnly)) {
        if (errorOut) {
            *errorOut = QStringLiteral("autotrack output JSON not found");
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(outJson.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        if (errorOut) {
            *errorOut = QStringLiteral("autotrack output JSON invalid");
        }
        return false;
    }

    const QJsonArray arr = doc.array();
    QJsonArray keyframes;
    for (const QJsonValue& value : arr) {
        const QJsonObject in = value.toObject();
        if (in.isEmpty()) {
            continue;
        }
        const int64_t frame = in.value(QStringLiteral("frame")).toVariant().toLongLong();
        const double x = in.value(QStringLiteral("x")).toDouble(-1.0);
        const double y = in.value(QStringLiteral("y")).toDouble(-1.0);
        if (frame < 0 || x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) {
            continue;
        }
        QJsonObject out;
        out[QString(kTranscriptSpeakerTrackingFrameKey)] = static_cast<qint64>(frame);
        out[QString(kTranscriptSpeakerLocationXKey)] = x;
        out[QString(kTranscriptSpeakerLocationYKey)] = y;
        const double confidence = qBound(0.0, in.value(QStringLiteral("confidence")).toDouble(0.0), 1.0);
        out[QString(kTranscriptSpeakerTrackingConfidenceKey)] = confidence;
        const double boxSize = in.value(QStringLiteral("box_size")).toDouble(-1.0);
        if (boxSize > 0.0) {
            out[QString(kTranscriptSpeakerTrackingBoxSizeKey)] = qBound(0.01, boxSize, 1.0);
        }
        const QString source = in.value(QStringLiteral("source")).toString().trimmed();
        out[QString(kTranscriptSpeakerTrackingSourceKey)] =
            source.isEmpty() ? QStringLiteral("autotrack_docker_v1") : source;
        keyframes.push_back(out);
    }

    if (keyframes.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("autotrack produced no keyframes");
        }
        return false;
    }
    *keyframesOut = keyframes;
    return true;
}

bool SpeakersTab::runAutoTrackForSpeaker(const QString& speakerId, bool forceModelTracking)
{
    if (!activeCutMutable() || speakerId.trimmed().isEmpty()) {
        return false;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QJsonObject profile = profiles.value(speakerId).toObject();
    QJsonObject tracking = speakerFramingObject(profile);
    if (tracking.isEmpty()) {
        tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("ReferencePoints");
    }
    const QString requestedMode = forceModelTracking
        ? QStringLiteral("AutoTrack")
        : tracking.value(QString(kTranscriptSpeakerTrackingModeKey))
              .toString(QStringLiteral("AutoTrack"))
              .trimmed();
    struct AutoTrackPreflightSettings {
        bool useSpeechWindows = false;
    };
    static AutoTrackPreflightSettings s_autoTrackPreflight;

    bool hasRef1 = false;
    bool hasRef2 = false;
    const QJsonObject ref1 =
        transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef1Key, &hasRef1);
    const QJsonObject ref2 =
        transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef2Key, &hasRef2);
    if (!hasRef1 && !hasRef2) {
        QMessageBox::warning(
            nullptr,
            QStringLiteral("Auto-Frame Speakers"),
            QStringLiteral("Set at least one reference point (Ref 1 or Ref 2) before running auto-frame."));
        return false;
    }
    if (requestedMode.compare(QStringLiteral("AutoTrack"), Qt::CaseInsensitive) == 0) {
        QDialog preflightDialog;
        preflightDialog.setWindowTitle(QStringLiteral("AutoTrack Preflight"));
        preflightDialog.setWindowFlag(Qt::Window, true);
        preflightDialog.resize(520, 220);
        auto* layout = new QVBoxLayout(&preflightDialog);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);
        auto* infoLabel = new QLabel(
            QStringLiteral("Configure this AutoTrack run.\n\n"
                           "Speech windows limit tracking to when the speaker is speaking, expanded by ±2.0s."),
            &preflightDialog);
        infoLabel->setWordWrap(true);
        layout->addWidget(infoLabel);
        auto* windowsCheck =
            new QCheckBox(QStringLiteral("Use speaker speech windows (±2s)"), &preflightDialog);
        windowsCheck->setChecked(s_autoTrackPreflight.useSpeechWindows);
        layout->addWidget(windowsCheck);
        auto* buttons = new QHBoxLayout;
        buttons->addStretch(1);
        auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &preflightDialog);
        auto* proceedButton = new QPushButton(QStringLiteral("Proceed"), &preflightDialog);
        buttons->addWidget(cancelButton);
        buttons->addWidget(proceedButton);
        layout->addLayout(buttons);
        connect(cancelButton, &QPushButton::clicked, &preflightDialog, &QDialog::reject);
        connect(proceedButton, &QPushButton::clicked, &preflightDialog, &QDialog::accept);
        if (preflightDialog.exec() != QDialog::Accepted) {
            return false;
        }
        s_autoTrackPreflight.useSpeechWindows = windowsCheck->isChecked();
    }

    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    const int64_t clipSourceStart = selectedClip ? qMax<int64_t>(0, selectedClip->sourceInFrame) : -1;
    const int64_t clipSourceEnd = selectedClip
        ? qMax<int64_t>(clipSourceStart, selectedClip->sourceInFrame + qMax<int64_t>(0, selectedClip->durationFrames - 1))
        : -1;

    int64_t speakerRangeStart = -1;
    int64_t speakerRangeEnd = -1;
    QVector<QPair<int64_t, int64_t>> speakerWindows;
    const bool useSpeechWindows = s_autoTrackPreflight.useSpeechWindows;
    const int64_t speechPadFrames = static_cast<int64_t>(std::round(2.0 * static_cast<double>(kTimelineFps)));
    {
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
                if (wordObj.value(QStringLiteral("word")).toString().trimmed().isEmpty()) {
                    continue;
                }
                const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
                const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
                if (startSeconds < 0.0 || endSeconds < startSeconds) {
                    continue;
                }
                const int64_t startFrame = qMax<int64_t>(
                    0, static_cast<int64_t>(std::floor(startSeconds * static_cast<double>(kTimelineFps))));
                const int64_t endFrame = qMax<int64_t>(
                    startFrame, static_cast<int64_t>(std::floor(endSeconds * static_cast<double>(kTimelineFps))));
                if (selectedClip && (endFrame < clipSourceStart || startFrame > clipSourceEnd)) {
                    continue;
                }
                if (useSpeechWindows) {
                    int64_t winStart = qMax<int64_t>(0, startFrame - speechPadFrames);
                    int64_t winEnd = qMax<int64_t>(winStart, endFrame + speechPadFrames);
                    if (selectedClip) {
                        winStart = qMax<int64_t>(clipSourceStart, winStart);
                        winEnd = qMin<int64_t>(clipSourceEnd, winEnd);
                    }
                    if (winEnd >= winStart) {
                        speakerWindows.push_back(qMakePair(winStart, winEnd));
                    }
                }
                if (speakerRangeStart < 0 || startFrame < speakerRangeStart) {
                    speakerRangeStart = startFrame;
                }
                if (speakerRangeEnd < 0 || endFrame > speakerRangeEnd) {
                    speakerRangeEnd = endFrame;
                }
            }
        }
    }
    if (useSpeechWindows && !speakerWindows.isEmpty()) {
        std::sort(
            speakerWindows.begin(),
            speakerWindows.end(),
            [](const QPair<int64_t, int64_t>& a, const QPair<int64_t, int64_t>& b) {
                if (a.first == b.first) {
                    return a.second < b.second;
                }
                return a.first < b.first;
            });
        QVector<QPair<int64_t, int64_t>> merged;
        for (const auto& window : speakerWindows) {
            if (merged.isEmpty()) {
                merged.push_back(window);
                continue;
            }
            auto& last = merged.last();
            if (window.first <= (last.second + 1)) {
                last.second = qMax(last.second, window.second);
            } else {
                merged.push_back(window);
            }
        }
        speakerWindows = merged;
    }

    const auto createPoint = [](int64_t frame,
                                double x,
                                double y,
                                double confidence,
                                const QString& source,
                                double boxSize = -1.0) {
        QJsonObject p;
        p[QString(kTranscriptSpeakerTrackingFrameKey)] = static_cast<qint64>(frame);
        p[QString(kTranscriptSpeakerLocationXKey)] = qBound(0.0, x, 1.0);
        p[QString(kTranscriptSpeakerLocationYKey)] = qBound(0.0, y, 1.0);
        if (boxSize > 0.0) {
            p[QString(kTranscriptSpeakerTrackingBoxSizeKey)] = qBound(0.01, boxSize, 1.0);
        }
        p[QString(kTranscriptSpeakerTrackingConfidenceKey)] = qBound(0.0, confidence, 1.0);
        p[QString(kTranscriptSpeakerTrackingSourceKey)] = source;
        return p;
    };

    QJsonArray keyframes;
    bool usedNativeModel = false;
    bool usedDockerModel = false;
    QString modelError;
    const int64_t refStartFrame = hasRef1 && hasRef2
        ? qMin(ref1.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong(),
               ref2.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong())
        : (hasRef1
               ? ref1.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong()
               : ref2.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
    const int64_t refEndFrame = hasRef1 && hasRef2
        ? qMax(ref1.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong(),
               ref2.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong())
        : refStartFrame;
    int64_t trackStartFrame = qMax<int64_t>(0, refStartFrame);
    int64_t trackEndFrame = qMax<int64_t>(trackStartFrame, refEndFrame);
    if (!speakerWindows.isEmpty()) {
        trackStartFrame = qMin(trackStartFrame, speakerWindows.constFirst().first);
        trackEndFrame = qMax(trackEndFrame, speakerWindows.constLast().second);
    } else if (speakerRangeStart >= 0 && speakerRangeEnd >= 0) {
        trackStartFrame = qMax<int64_t>(0, qMin(trackStartFrame, speakerRangeStart));
        trackEndFrame = qMax<int64_t>(trackEndFrame, speakerRangeEnd);
    }
    if (selectedClip) {
        trackStartFrame = qMax<int64_t>(clipSourceStart, trackStartFrame);
        trackEndFrame = qMin<int64_t>(clipSourceEnd, trackEndFrame);
        if (trackEndFrame < trackStartFrame) {
            trackStartFrame = qMax<int64_t>(clipSourceStart, qMin<int64_t>(clipSourceEnd, refStartFrame));
            trackEndFrame = qMax<int64_t>(trackStartFrame, qMin<int64_t>(clipSourceEnd, refEndFrame));
        }
    }
    const int64_t trackSpan = qMax<int64_t>(1, trackEndFrame - trackStartFrame);
    const QJsonObject trackingConfig = transcriptSpeakerTrackingConfigSnapshot();
    const int configuredMaxStepFrames =
        qBound<int>(1, trackingConfig.value(QStringLiteral("auto_track_step_frames")).toInt(6), 120);
    const int stepFrames =
        qBound<int>(1, static_cast<int>(trackSpan / 300), configuredMaxStepFrames);
    if (requestedMode.compare(QStringLiteral("AutoTrack"), Qt::CaseInsensitive) == 0 && hasRef1 && hasRef2) {
        if (selectedClip) {
            usedNativeModel = runNativeAutoTrackForSpeaker(
                *selectedClip, speakerId, ref1, ref2, speakerWindows, trackStartFrame, trackEndFrame, stepFrames, &keyframes, &modelError);
            if (!usedNativeModel) {
                const QString allowDocker =
                    qEnvironmentVariable("SPEAKER_AUTOTRACK_ALLOW_DOCKER", QStringLiteral("0")).trimmed().toLower();
                const bool allowDockerFallback =
                    (allowDocker == QStringLiteral("1") ||
                     allowDocker == QStringLiteral("true") ||
                     allowDocker == QStringLiteral("yes") ||
                     allowDocker == QStringLiteral("on"));
                if (allowDockerFallback) {
                    QString dockerError;
                    usedDockerModel = runDockerAutoTrackForSpeaker(
                        *selectedClip, speakerId, ref1, ref2, trackStartFrame, trackEndFrame, stepFrames, &keyframes, &dockerError);
                    if (!dockerError.isEmpty()) {
                        if (modelError.isEmpty()) {
                            modelError = dockerError;
                        } else {
                            modelError += QStringLiteral("\n") + dockerError;
                        }
                    }
                }
            }
        } else {
            modelError = QStringLiteral("no selected clip for native autotrack");
        }
    }

    if (!usedNativeModel && !usedDockerModel) {
        if (modelError.trimmed().startsWith(QStringLiteral("user canceled"), Qt::CaseInsensitive)) {
            return false;
        }
        if (hasRef1 && hasRef2) {
            const int64_t frame1 = ref1.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            const int64_t frame2 = ref2.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            const double x1 = qBound(0.0, ref1.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
            const double y1 = qBound(0.0, ref1.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
            const double x2 = qBound(0.0, ref2.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
            const double y2 = qBound(0.0, ref2.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
            const double box1 = qBound(
                -1.0, ref1.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0), 1.0);
            const double box2 = qBound(
                -1.0, ref2.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0), 1.0);
            const bool hasBox1 = box1 > 0.0;
            const bool hasBox2 = box2 > 0.0;

            QVector<QPair<int64_t, int64_t>> linearWindows = speakerWindows;
            if (linearWindows.isEmpty()) {
                linearWindows.push_back(qMakePair(trackStartFrame, trackEndFrame));
            }
            const int64_t kStepFrames = qMax<int64_t>(1, stepFrames);
            for (const auto& window : linearWindows) {
                const int64_t startFrame = qMin(window.first, window.second);
                const int64_t endFrame = qMax(window.first, window.second);
                const int64_t span = qMax<int64_t>(1, endFrame - startFrame);
                for (int64_t frame = startFrame; frame <= endFrame; frame += kStepFrames) {
                    const double t = static_cast<double>(frame - startFrame) / static_cast<double>(span);
                    const double x = x1 + (x2 - x1) * t;
                    const double y = y1 + (y2 - y1) * t;
                    double boxSize = -1.0;
                    if (hasBox1 && hasBox2) {
                        boxSize = box1 + (box2 - box1) * t;
                    } else if (hasBox1) {
                        boxSize = box1;
                    } else if (hasBox2) {
                        boxSize = box2;
                    }
                    keyframes.push_back(createPoint(
                        frame, x, y, 0.70, QStringLiteral("autotrack_linear_v1"), boxSize));
                }
                const int64_t lastFrame = keyframes.isEmpty()
                    ? -1
                    : keyframes.at(keyframes.size() - 1)
                          .toObject()
                          .value(QString(kTranscriptSpeakerTrackingFrameKey))
                          .toVariant()
                          .toLongLong();
                if (keyframes.isEmpty() || lastFrame != endFrame) {
                    double boxSize = -1.0;
                    if (hasBox2) {
                        boxSize = box2;
                    } else if (hasBox1) {
                        boxSize = box1;
                    }
                    keyframes.push_back(createPoint(
                        endFrame, x2, y2, 0.70, QStringLiteral("autotrack_linear_v1"), boxSize));
                }
            }
            tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("AutoTrackLinear");
            tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] =
                QStringLiteral("completed_linear_v1_open_for_model_tracking");
        } else {
            const QJsonObject onlyRef = hasRef1 ? ref1 : ref2;
            const int64_t frame = onlyRef.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            const double x = qBound(0.0, onlyRef.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
            const double y = qBound(0.0, onlyRef.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
            const double boxSize =
                qBound(-1.0, onlyRef.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0), 1.0);
            keyframes.push_back(createPoint(
                frame, x, y, 0.60, QStringLiteral("autotrack_anchor_v1"), boxSize));
            tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("AnchorHold");
            tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] =
                QStringLiteral("completed_anchor_v1_open_for_model_tracking");
        }
        if (!modelError.isEmpty()) {
            QMessageBox::information(
                nullptr,
                QStringLiteral("AutoTrack Fallback"),
                QStringLiteral("Model AutoTrack was unavailable, so linear fallback was used.\n\n%1")
                    .arg(modelError));
        }
    } else {
        tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("AutoTrack");
        tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] = usedNativeModel
            ? QStringLiteral("completed_native_cpp_v1")
            : QStringLiteral("completed_docker_v1");
    }

    tracking[QString(kTranscriptSpeakerTrackingKeyframesKey)] = keyframes;
    tracking[QString(kTranscriptSpeakerTrackingEnabledKey)] = true;
    setSpeakerFramingObject(profile, tracking);
    profiles[speakerId] = profile;
    root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
    m_loadedTranscriptDoc.setObject(root);

    editor::TranscriptEngine engine;
    return engine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc);
}

void SpeakersTab::onSpeakerRunAutoTrackClicked()
{
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    if (!runAutoTrackForSpeaker(speakerId, true)) {
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

void SpeakersTab::onSpeakerGuideClicked()
{
    const QString guideText =
        QStringLiteral(
            "Speaker Framing Guide\n\n"
            "1. Select a speaker row. This arms Ref 1 automatically.\n"
            "2. Move the playhead to a frame where the speaker is visible.\n"
            "3. In Preview, hold Shift.\n"
            "4. Drag a square over the speaker's head and release.\n"
            "5. Click Auto-Frame immediately (single-reference mode).\n"
            "6. Optional: set Ref 2 on another frame, then Auto-Frame again for better quality.\n\n"
            "Tips\n"
            "- Square selection is required and enforced.\n"
            "- Ref buttons show [ARMED] when awaiting your pick.\n"
            "- Speaker metadata is editable only on derived cuts (not Original).");
    QMessageBox::information(nullptr, QStringLiteral("Speaker Framing Guide"), guideText);
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
    const int64_t frame = currentSourceFrameForClip(*clip);
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

bool SpeakersTab::handlePreviewBox(const QString& clipId,
                                   qreal xNorm,
                                   qreal yNorm,
                                   qreal boxSizeNorm)
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
    const int64_t frame = currentSourceFrameForClip(*clip);
    if (!saveSpeakerTrackingReferenceAt(
            speakerId,
            m_pendingReferencePick,
            frame,
            xNorm,
            yNorm,
            qBound<qreal>(0.01, boxSizeNorm, 1.0))) {
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
