#include "speakers_tab.h"
#include "speakers_tab_internal.h"

#include "decoder_context.h"
#include "transcript_engine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QEvent>
#include <QDialog>
#include <QDoubleSpinBox>
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
        m_widgets.speakersTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
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
            QStringLiteral("Generate BoxStream from one or two references."));
        connect(m_widgets.speakerRunAutoTrackButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerRunAutoTrackClicked);
    }
    if (m_widgets.speakerBoxstreamSettingsButton) {
        m_widgets.speakerBoxstreamSettingsButton->setToolTip(
            QStringLiteral("Open BoxStream-specific runtime smoothing options."));
        connect(m_widgets.speakerBoxstreamSettingsButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerBoxStreamSettingsClicked);
    }
    if (m_widgets.speakerTrackingChipButton) {
        m_widgets.speakerTrackingChipButton->setToolTip(
            QStringLiteral("Click to toggle Subtitle Face Tracking for the selected speaker."));
        connect(m_widgets.speakerTrackingChipButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerTrackingChipClicked);
    }
    if (m_widgets.speakerStabilizeChipButton) {
        m_widgets.speakerStabilizeChipButton->setToolTip(
            QStringLiteral("Click to toggle Face Stabilize for the selected clip."));
        connect(m_widgets.speakerStabilizeChipButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerStabilizeChipClicked);
    }
    if (m_widgets.speakerGuideButton) {
        m_widgets.speakerGuideButton->setToolTip(
            QStringLiteral("Open a quick guide for speaker reference picking and BoxStream generation."));
        connect(m_widgets.speakerGuideButton, &QPushButton::clicked,
                this, &SpeakersTab::onSpeakerGuideClicked);
    }
    if (m_widgets.speakerFramingTargetXSpin) {
        connect(m_widgets.speakerFramingTargetXSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &SpeakersTab::onSpeakerFramingTargetChanged);
    }
    if (m_widgets.speakerFramingTargetYSpin) {
        connect(m_widgets.speakerFramingTargetYSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &SpeakersTab::onSpeakerFramingTargetChanged);
    }
    if (m_widgets.speakerFramingTargetBoxSpin) {
        connect(m_widgets.speakerFramingTargetBoxSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &SpeakersTab::onSpeakerFramingTargetChanged);
    }
    if (m_widgets.speakerFramingZoomEnabledCheckBox) {
        connect(m_widgets.speakerFramingZoomEnabledCheckBox, &QCheckBox::toggled,
                this, &SpeakersTab::onSpeakerFramingZoomEnabledChanged);
    }
    if (m_widgets.speakerApplyFramingToClipCheckBox) {
        connect(m_widgets.speakerApplyFramingToClipCheckBox, &QCheckBox::toggled,
                this, &SpeakersTab::onSpeakerApplyFramingToClipChanged);
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
    m_lastSelectionSeekSpeakerId.clear();
    m_lastSelectionSeekClipId.clear();
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
        updateSelectedSpeakerPanel();
        updateSpeakerTrackingStatusLabel();
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
        updateSelectedSpeakerPanel();
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
        updateSelectedSpeakerPanel();
        updateSpeakerTrackingStatusLabel();
        return;
    }

    m_loadedTranscriptDoc = transcriptDoc;

    if (m_widgets.speakersInspectorClipLabel) {
        m_widgets.speakersInspectorClipLabel->setText(QStringLiteral("Speakers\n%1").arg(clip->label));
    }
    if (m_widgets.speakersInspectorDetailsLabel) {
        m_widgets.speakersInspectorDetailsLabel->setText(QString());
    }

    refreshSpeakersTable(transcriptDoc.object(), preferredSpeakerId);
    if (m_widgets.speakersTable) {
        m_widgets.speakersTable->setEnabled(activeCutMutable());
    }

    m_updating = false;
    updateSelectedSpeakerPanel();
    updateSpeakerTrackingStatusLabel();
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
        const uint hueHash = qHash(id);
        const QColor speakerHueTint = QColor::fromHsv(static_cast<int>(hueHash % 360), 160, 92, 105);
        avatarItem->setBackground(QBrush(speakerHueTint));
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
        return QStringLiteral("Subtitle Face Tracking: Off (no references)");
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
    QString summary = QStringLiteral("Subtitle Face Tracking: %1 (%2) | Ref1=%3 | Ref2=%4")
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
    updateSpeakerFramingTargetControls();

    const bool mutableCut = activeCutMutable();
    const QString speakerId = selectedSpeakerId();
    const bool canEdit = mutableCut && !speakerId.isEmpty();
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    const bool canEditClipFraming = mutableCut && m_speakerDeps.updateClipById && selectedClip;
    bool hasRef1 = false;
    bool hasRef2 = false;
    if (m_widgets.speakerRefsChipLabel) {
        m_widgets.speakerRefsChipLabel->setText(QStringLiteral("Refs: 0/2"));
    }
    if (m_widgets.speakerPointstreamChipLabel) {
        m_widgets.speakerPointstreamChipLabel->setText(QStringLiteral("BoxStream: None"));
    }
    if (m_widgets.speakerTrackingChipButton) {
        m_widgets.speakerTrackingChipButton->setText(QStringLiteral("Tracking: OFF"));
        m_widgets.speakerTrackingChipButton->setChecked(false);
        m_widgets.speakerTrackingChipButton->setEnabled(false);
    }
    if (m_widgets.speakerStabilizeChipButton) {
        m_widgets.speakerStabilizeChipButton->setText(QStringLiteral("Face Stabilize: OFF"));
        m_widgets.speakerStabilizeChipButton->setChecked(false);
        m_widgets.speakerStabilizeChipButton->setEnabled(false);
    }

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
        m_widgets.speakerRunAutoTrackButton->setText(QStringLiteral("Generate BoxStream"));
    }
    if (m_widgets.speakerBoxstreamSettingsButton) {
        m_widgets.speakerBoxstreamSettingsButton->setEnabled(canEdit);
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
    const bool canEditFramingTargets = canEditClipFraming;
    if (m_widgets.speakerFramingTargetXSpin) {
        m_widgets.speakerFramingTargetXSpin->setEnabled(canEditFramingTargets);
    }
    if (m_widgets.speakerFramingTargetYSpin) {
        m_widgets.speakerFramingTargetYSpin->setEnabled(canEditFramingTargets);
    }
    if (m_widgets.speakerFramingTargetBoxSpin) {
        const bool zoomEnabled =
            m_widgets.speakerFramingZoomEnabledCheckBox &&
            m_widgets.speakerFramingZoomEnabledCheckBox->isChecked();
        m_widgets.speakerFramingTargetBoxSpin->setEnabled(canEditFramingTargets && zoomEnabled);
    }
    if (m_widgets.speakerFramingZoomEnabledCheckBox) {
        m_widgets.speakerFramingZoomEnabledCheckBox->setEnabled(canEditFramingTargets);
    }
    if (m_widgets.speakerApplyFramingToClipCheckBox) {
        m_widgets.speakerApplyFramingToClipCheckBox->setEnabled(false);
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
        m_widgets.speakerTrackingStatusLabel->setText(QString());
        return;
    }
    if (speakerId.isEmpty()) {
        m_widgets.speakerTrackingStatusLabel->setText(QString());
        return;
    }
    if (!m_loadedTranscriptDoc.isObject()) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("Transcript not loaded."));
        return;
    }

    const QJsonObject profiles =
        m_loadedTranscriptDoc.object().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QJsonObject tracking = speakerFramingObject(profile);
    transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef1Key, &hasRef1);
    transcriptTrackingReferencePoint(tracking, kTranscriptSpeakerTrackingRef2Key, &hasRef2);
    const bool trackingEnabled = transcriptTrackingEnabled(tracking);
    const bool hasPointstream = transcriptTrackingHasPointstream(tracking);
    const int refCount = static_cast<int>(hasRef1) + static_cast<int>(hasRef2);
    if (m_widgets.speakerRefsChipLabel) {
        m_widgets.speakerRefsChipLabel->setText(QStringLiteral("Refs: %1/2").arg(refCount));
    }
    if (m_widgets.speakerPointstreamChipLabel) {
        m_widgets.speakerPointstreamChipLabel->setText(
            hasPointstream ? QStringLiteral("BoxStream: Ready") : QStringLiteral("BoxStream: None"));
    }
    if (m_widgets.speakerTrackingChipButton) {
        m_widgets.speakerTrackingChipButton->setText(
            trackingEnabled ? QStringLiteral("Tracking: ON") : QStringLiteral("Tracking: OFF"));
        m_widgets.speakerTrackingChipButton->setChecked(trackingEnabled);
        m_widgets.speakerTrackingChipButton->setEnabled(canEdit && hasPointstream);
    }
    if (selectedClip) {
        const bool hasRuntimeBinding = !selectedClip->speakerFramingSpeakerId.trimmed().isEmpty();
        const bool hasFramingData =
            hasRuntimeBinding || !selectedClip->speakerFramingKeyframes.isEmpty();
        const QString clipFramingState = selectedClip->speakerFramingEnabled
            ? QStringLiteral("ON")
            : QStringLiteral("OFF");
        if (m_widgets.speakerClipFramingStatusLabel) {
            m_widgets.speakerClipFramingStatusLabel->setText(
                QStringLiteral("Face Stabilize: %1 | %2")
                    .arg(clipFramingState)
                    .arg(hasRuntimeBinding
                        ? QStringLiteral("Runtime BoxStream")
                        : QStringLiteral("%1 keys").arg(selectedClip->speakerFramingKeyframes.size())));
        }
        if (m_widgets.speakerStabilizeChipButton) {
            const bool canToggleStabilize = canEditFramingTargets && hasFramingData;
            m_widgets.speakerStabilizeChipButton->setText(
                selectedClip->speakerFramingEnabled
                    ? QStringLiteral("Face Stabilize: ON")
                    : QStringLiteral("Face Stabilize: OFF"));
            m_widgets.speakerStabilizeChipButton->setChecked(selectedClip->speakerFramingEnabled);
            m_widgets.speakerStabilizeChipButton->setEnabled(canToggleStabilize);
            if (!selectedClip->speakerFramingEnabled && !hasFramingData) {
                m_widgets.speakerStabilizeChipButton->setToolTip(
                    QStringLiteral("Face Stabilize needs a BoxStream speaker binding. "
                                   "Generate BoxStream with this clip selected."));
            } else if (hasRuntimeBinding) {
                m_widgets.speakerStabilizeChipButton->setToolTip(
                    QStringLiteral("Face Stabilize uses runtime BoxStream transforms for speaker %1.")
                        .arg(selectedClip->speakerFramingSpeakerId));
            } else {
                m_widgets.speakerStabilizeChipButton->setToolTip(
                    QStringLiteral("Click to toggle Face Stabilize for the selected clip."));
            }
        }
        if (m_widgets.speakerApplyFramingToClipCheckBox) {
            const bool canToggleStabilizeFromCheckbox =
                canEditFramingTargets && (selectedClip->speakerFramingEnabled || hasFramingData);
            m_widgets.speakerApplyFramingToClipCheckBox->setEnabled(canToggleStabilizeFromCheckbox);
        }
    }
    if (!selectedClip && m_widgets.speakerClipFramingStatusLabel) {
        m_widgets.speakerClipFramingStatusLabel->setText(QStringLiteral("Face Stabilize: OFF | 0 keys"));
    }
    if (!selectedClip && m_widgets.speakerStabilizeChipButton) {
        m_widgets.speakerStabilizeChipButton->setText(QStringLiteral("Face Stabilize: OFF"));
        m_widgets.speakerStabilizeChipButton->setChecked(false);
        m_widgets.speakerStabilizeChipButton->setEnabled(false);
    }

    if (m_widgets.speakerRunAutoTrackButton) {
        const bool hasAnyRef = hasRef1 || hasRef2;
        m_widgets.speakerRunAutoTrackButton->setEnabled(canEdit && hasAnyRef);
        if (!hasAnyRef) {
            m_widgets.speakerRunAutoTrackButton->setText(QStringLiteral("Generate BoxStream (need Ref 1)"));
        } else if (hasRef1 && hasRef2) {
            m_widgets.speakerRunAutoTrackButton->setText(QStringLiteral("Generate BoxStream (2 refs)"));
        } else {
            m_widgets.speakerRunAutoTrackButton->setText(QStringLiteral("Generate BoxStream (1 ref mode)"));
        }
    }

    if (m_pendingReferencePick == 1 || m_pendingReferencePick == 2) {
        m_widgets.speakerTrackingStatusLabel->setText(
            QStringLiteral("[ARMED] Ref %1").arg(m_pendingReferencePick));
        return;
    }

    m_widgets.speakerTrackingStatusLabel->setText(
        QStringLiteral("Refs: %1/2 | BoxStream: %2 | Tracking: %3 | Face Stabilize: %4")
            .arg(refCount)
            .arg(hasPointstream ? QStringLiteral("Ready") : QStringLiteral("None"))
            .arg(trackingEnabled ? QStringLiteral("ON") : QStringLiteral("OFF"))
            .arg((selectedClip && selectedClip->speakerFramingEnabled) ? QStringLiteral("ON") : QStringLiteral("OFF")));
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
