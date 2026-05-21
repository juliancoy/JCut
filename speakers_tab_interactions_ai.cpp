#include "speakers_tab.h"
#include "speakers_tab_internal.h"

#include "facestream_assignment_services.h"
#include "facestream_artifact_utils.h"
#include "facestream_runtime.h"
#include "facestream_time_mapping.h"
#include "facefind_window.h"
#include "identity_resolution.h"
#include "transcript_engine.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QGridLayout>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QScopedPointer>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <random>
#include <vector>
#include <cmath>

namespace {
class ClickableTileWidget : public QFrame
{
public:
    std::function<void()> onClick;

    explicit ClickableTileWidget(QWidget* parent = nullptr)
        : QFrame(parent)
    {
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event && event->button() == Qt::LeftButton && onClick) {
            onClick();
            event->accept();
            return;
        }
        QFrame::mousePressEvent(event);
    }
};

struct AiProposalRow {
    QString targetId;
    QString field;
    QString currentValue;
    QString proposedValue;
    qreal confidence = 0.0;
    QString rationale;
};

bool confirmAiProposals(QWidget* parent,
                        const QString& title,
                        const QString& summary,
                        const QVector<AiProposalRow>& proposals)
{
    if (proposals.isEmpty()) {
        return false;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.setModal(true);
    dialog.resize(860, 460);
    auto* rootLayout = new QVBoxLayout(&dialog);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(8);
    auto* intro = new QLabel(summary, &dialog);
    intro->setWordWrap(true);
    rootLayout->addWidget(intro);

    auto* table = new QTableWidget(&dialog);
    table->setColumnCount(6);
    table->setHorizontalHeaderLabels(
        QStringList{QStringLiteral("Target"),
                    QStringLiteral("Field"),
                    QStringLiteral("Current"),
                    QStringLiteral("Proposed"),
                    QStringLiteral("Confidence"),
                    QStringLiteral("Rationale")});
    table->setRowCount(proposals.size());
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    for (int i = 0; i < proposals.size(); ++i) {
        const AiProposalRow& p = proposals[i];
        table->setItem(i, 0, new QTableWidgetItem(p.targetId));
        table->setItem(i, 1, new QTableWidgetItem(p.field));
        table->setItem(i, 2, new QTableWidgetItem(p.currentValue));
        table->setItem(i, 3, new QTableWidgetItem(p.proposedValue));
        table->setItem(i, 4, new QTableWidgetItem(QStringLiteral("%1%").arg(qRound(p.confidence * 100.0))));
        table->setItem(i, 5, new QTableWidgetItem(p.rationale));
    }
    rootLayout->addWidget(table, 1);

    auto* actions = new QHBoxLayout;
    actions->addStretch(1);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
    auto* applyButton = new QPushButton(QStringLiteral("Apply"), &dialog);
    actions->addWidget(cancelButton);
    actions->addWidget(applyButton);
    rootLayout->addLayout(actions);
    QObject::connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(applyButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    return dialog.exec() == QDialog::Accepted;
}

struct SeedTrackMatchDialogResult {
    bool accepted = false;
    QSet<int> chosenTrackIds;
};

SeedTrackMatchDialogResult reviewSeedTrackMatches(QWidget* parent,
                                                  const QString& speakerLabel,
                                                  int seedTrackId,
                                                  const QVector<jcut::facestream_assignment::SeedTrackMatch>& matches,
                                                  double autoMatchThreshold,
                                                  double reviewThreshold)
{
    SeedTrackMatchDialogResult result;
    if (matches.isEmpty()) {
        return result;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Assign Matching Continuity Tracks"));
    dialog.resize(980, 620);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    auto* intro = new QLabel(
        QStringLiteral("Review ArcFace identity matches for seed track T%1, then assign the checked tracks to %2.\n"
                       "Auto matches are prechecked. Review matches are shown unchecked.")
            .arg(seedTrackId)
            .arg(speakerLabel),
        &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* summary = new QLabel(
        QStringLiteral("Thresholds: auto >= %1, review >= %2")
            .arg(autoMatchThreshold, 0, 'f', 2)
            .arg(reviewThreshold, 0, 'f', 2),
        &dialog);
    summary->setWordWrap(true);
    summary->setStyleSheet(QStringLiteral(
        "QLabel { background: #142234; border: 1px solid #314459; border-radius: 8px; color: #d8e6f5; padding: 8px; }"));
    layout->addWidget(summary);

    auto* table = new QTableWidget(&dialog);
    table->setColumnCount(6);
    table->setHorizontalHeaderLabels(
        QStringList{
            QStringLiteral("Use"),
            QStringLiteral("Crop"),
            QStringLiteral("Track"),
            QStringLiteral("Cosine"),
            QStringLiteral("Decision"),
            QStringLiteral("Samples")});
    table->setRowCount(matches.size());
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);

    QVector<QCheckBox*> checkboxes;
    checkboxes.reserve(matches.size());
    for (int row = 0; row < matches.size(); ++row) {
        const auto& match = matches.at(row);
        auto* useCheck = new QCheckBox(table);
        const bool isSeed = match.trackId == seedTrackId;
        const bool isAuto = match.decision == QLatin1StringView("auto_match");
        useCheck->setChecked(isSeed || isAuto);
        useCheck->setEnabled(isSeed || match.decision == QLatin1StringView("auto_match") ||
                             match.decision == QLatin1StringView("review"));
        if (isSeed) {
            useCheck->setToolTip(QStringLiteral("Seed track is always included."));
        } else if (match.decision == QLatin1StringView("different")) {
            useCheck->setToolTip(QStringLiteral("Below the review threshold."));
        } else if (match.decision == QLatin1StringView("no_embedding")) {
            useCheck->setToolTip(QStringLiteral("No embedding could be computed for this track."));
        }
        table->setCellWidget(row, 0, useCheck);
        checkboxes.push_back(useCheck);

        auto* cropItem = new QTableWidgetItem;
        cropItem->setTextAlignment(Qt::AlignCenter);
        if (!match.representativeCandidate.cropPath.isEmpty()) {
            QPixmap crop(match.representativeCandidate.cropPath);
            if (!crop.isNull()) {
                cropItem->setIcon(QIcon(crop.scaled(84, 84, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            }
        }
        cropItem->setSizeHint(QSize(88, 88));
        table->setItem(row, 1, cropItem);
        table->setItem(row, 2, new QTableWidgetItem(QStringLiteral("T%1").arg(match.trackId)));
        table->setItem(
            row,
            3,
            new QTableWidgetItem(match.cosine < 0.0 ? QStringLiteral("—")
                                                    : QString::number(match.cosine, 'f', 3)));
        table->setItem(row, 4, new QTableWidgetItem(match.decision));
        table->setItem(row, 5, new QTableWidgetItem(
            QStringLiteral("%1 crop(s)").arg(match.cropSamples.size())));
        table->setRowHeight(row, 92);
    }
    layout->addWidget(table, 1);

    auto* actions = new QHBoxLayout;
    auto* autoOnlyButton = new QPushButton(QStringLiteral("Select Auto Matches"), &dialog);
    auto* allReviewButton = new QPushButton(QStringLiteral("Select Review Matches Too"), &dialog);
    actions->addWidget(autoOnlyButton);
    actions->addWidget(allReviewButton);
    actions->addStretch(1);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
    auto* applyButton = new QPushButton(QStringLiteral("Assign Checked Tracks"), &dialog);
    actions->addWidget(cancelButton);
    actions->addWidget(applyButton);
    layout->addLayout(actions);

    QObject::connect(autoOnlyButton, &QPushButton::clicked, &dialog, [&]() {
        for (int row = 0; row < matches.size(); ++row) {
            QCheckBox* check = checkboxes.value(row, nullptr);
            if (!check || !check->isEnabled()) {
                continue;
            }
            const bool isSeed = matches.at(row).trackId == seedTrackId;
            check->setChecked(isSeed || matches.at(row).decision == QLatin1StringView("auto_match"));
        }
    });
    QObject::connect(allReviewButton, &QPushButton::clicked, &dialog, [&]() {
        for (int row = 0; row < matches.size(); ++row) {
            QCheckBox* check = checkboxes.value(row, nullptr);
            if (!check || !check->isEnabled()) {
                continue;
            }
            const QString decision = matches.at(row).decision;
            check->setChecked(decision == QLatin1StringView("seed") ||
                              decision == QLatin1StringView("auto_match") ||
                              decision == QLatin1StringView("review"));
        }
    });
    QObject::connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(applyButton, &QPushButton::clicked, &dialog, &QDialog::accept);

    if (dialog.exec() != QDialog::Accepted) {
        return result;
    }
    for (int row = 0; row < matches.size(); ++row) {
        QCheckBox* check = checkboxes.value(row, nullptr);
        if (check && check->isChecked()) {
            result.chosenTrackIds.insert(matches.at(row).trackId);
        }
    }
    if (result.chosenTrackIds.isEmpty()) {
        return result;
    }
    result.accepted = true;
    return result;
}

bool confirmClusteringPreflight(QWidget* parent,
                                const QVector<facefind::Candidate>& candidates,
                                const QString& speakerLabel,
                                const QString& summary,
                                const QVector<int>& preferredSampleIndexes,
                                QSet<int>* selectedTrackIdsOut = nullptr)
{
    if (candidates.isEmpty()) {
        return false;
    }

    QVector<int> validIndexes;
    validIndexes.reserve(candidates.size());
    for (int i = 0; i < candidates.size(); ++i) {
        const facefind::Candidate& candidate = candidates.at(i);
        if (!candidate.cropPath.trimmed().isEmpty()) {
            validIndexes.push_back(i);
        }
    }
    if (validIndexes.isEmpty()) {
        return false;
    }

    QVector<int> currentSampleIndexes;
    QSet<int> selectedTrackIds;

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Add Continuity Tracks"));
    dialog.setModal(true);
    dialog.resize(760, 520);
    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* intro = new QLabel(
        QStringLiteral("Review the continuity tracks active at the current playhead, then add the selected tracks to %1.\n"
                       "Click a tile to select that track only. Ctrl/Cmd-click toggles multiple tracks.")
            .arg(speakerLabel),
        &dialog);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* details = new QLabel(
        QStringLiteral("%1\nShowing: %2 track(s)")
            .arg(summary.trimmed().isEmpty() ? QStringLiteral("Ready to add continuity tracks.") : summary.trimmed())
            .arg(currentSampleIndexes.size() > 0 ? currentSampleIndexes.size() : preferredSampleIndexes.size()),
        &dialog);
    details->setWordWrap(true);
    details->setStyleSheet(QStringLiteral(
        "QLabel { background: #142234; border: 1px solid #314459; border-radius: 8px; color: #d8e6f5; padding: 8px; }"));
    root->addWidget(details);

    auto* gridWidget = new QWidget(&dialog);
    auto* grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(8);
    auto* selectionSummaryLabel = new QLabel(&dialog);
    selectionSummaryLabel->setWordWrap(true);
    selectionSummaryLabel->setStyleSheet(QStringLiteral("color:#9fb3c8; font-size:11px;"));
    root->addWidget(selectionSummaryLabel);

    QPushButton* proceedButton = nullptr;
    QHash<int, ClickableTileWidget*> tileButtonsBySampleIndex;

    const auto updateSelectionUi = [&]() {
        for (auto it = tileButtonsBySampleIndex.begin(); it != tileButtonsBySampleIndex.end(); ++it) {
            const int sampleIndex = it.key();
            ClickableTileWidget* button = it.value();
            if (!button) {
                continue;
            }
            const facefind::Candidate& candidate = candidates.at(currentSampleIndexes.at(sampleIndex));
            const bool selected = selectedTrackIds.contains(candidate.trackId);
            button->setStyleSheet(selected
                ? QStringLiteral(
                    "QFrame { background: #18344f; border: 2px solid #4ade80; border-radius: 8px; }"
                    "QFrame:hover { background: #21425f; }")
                : QStringLiteral(
                    "QFrame { background: #101c2b; border: 1px solid #2f4358; border-radius: 8px; }"
                    "QFrame:hover { background: #162536; border-color: #4b6983; }"));
        }

        if (selectionSummaryLabel) {
            if (selectedTrackIds.isEmpty()) {
                selectionSummaryLabel->setText(QStringLiteral("Selection: none. Choose at least one track to add."));
            } else {
                selectionSummaryLabel->setText(
                    QStringLiteral("Selection: %1 track(s) will be added to %2.")
                        .arg(selectedTrackIds.size())
                        .arg(speakerLabel));
            }
        }
        if (proceedButton) {
            proceedButton->setEnabled(!selectedTrackIds.isEmpty());
            proceedButton->setText(QStringLiteral("Add %1 Selected Track(s)").arg(selectedTrackIds.size()));
        }
    };

    const auto redrawTiles = [&]() {
        tileButtonsBySampleIndex.clear();
        while (QLayoutItem* item = grid->takeAt(0)) {
            if (QWidget* widget = item->widget()) {
                widget->deleteLater();
            }
            delete item;
        }

        for (int sampleIndex = 0; sampleIndex < currentSampleIndexes.size(); ++sampleIndex) {
            const facefind::Candidate& c = candidates.at(currentSampleIndexes.at(sampleIndex));
            auto* tile = new ClickableTileWidget(gridWidget);
            tile->setMinimumSize(170, 165);
            tile->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
            auto* tileLayout = new QVBoxLayout(tile);
            tileLayout->setContentsMargins(6, 6, 6, 6);
            tileLayout->setSpacing(4);

            auto* imageLabel = new QLabel(tile);
            imageLabel->setMinimumSize(140, 120);
            imageLabel->setAlignment(Qt::AlignCenter);
            imageLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            QPixmap crop(c.cropPath);
            if (!crop.isNull()) {
                imageLabel->setPixmap(crop.scaled(140, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            } else {
                imageLabel->setText(QStringLiteral("missing crop"));
            }
            tileLayout->addWidget(imageLabel);

            auto* caption = new QLabel(
                QStringLiteral("T%1 | frame %2 | score %3")
                    .arg(c.trackId)
                    .arg(c.frame)
                    .arg(c.score, 0, 'f', 2),
                tile);
            caption->setAlignment(Qt::AlignCenter);
            caption->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            caption->setStyleSheet(
                QStringLiteral("QLabel { color: #b8c7d8; border: none; background: transparent; font-size: 11px; }"));
            tileLayout->addWidget(caption);

            tile->onClick = [&, sampleIndex]() {
                const facefind::Candidate& candidate = candidates.at(currentSampleIndexes.at(sampleIndex));
                const Qt::KeyboardModifiers modifiers = QApplication::keyboardModifiers();
                const bool additive =
                    modifiers.testFlag(Qt::ControlModifier) || modifiers.testFlag(Qt::MetaModifier);
                if (!additive) {
                    if (selectedTrackIds.size() == 1 && selectedTrackIds.contains(candidate.trackId)) {
                        selectedTrackIds.clear();
                    } else {
                        selectedTrackIds = QSet<int>{candidate.trackId};
                    }
                } else {
                    if (selectedTrackIds.contains(candidate.trackId)) {
                        selectedTrackIds.remove(candidate.trackId);
                    } else {
                        selectedTrackIds.insert(candidate.trackId);
                    }
                }
                updateSelectionUi();
            };
            tileButtonsBySampleIndex.insert(sampleIndex, tile);
            grid->addWidget(tile, sampleIndex / 4, sampleIndex % 4);
        }
        updateSelectionUi();
    };

    currentSampleIndexes = preferredSampleIndexes;
    if (currentSampleIndexes.isEmpty()) {
        currentSampleIndexes = validIndexes;
    }
    if (details) {
        details->setText(
            QStringLiteral("%1\nShowing: %2 track(s)")
                .arg(summary.trimmed().isEmpty() ? QStringLiteral("Ready to add continuity tracks.") : summary.trimmed())
                .arg(currentSampleIndexes.size()));
    }
    redrawTiles();
    root->addWidget(gridWidget, 1);

    auto* actions = new QHBoxLayout;
    actions->addStretch(1);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
    proceedButton = new QPushButton(QStringLiteral("Add Selected Tracks"), &dialog);
    actions->addWidget(cancelButton);
    actions->addWidget(proceedButton);
    root->addLayout(actions);
    QObject::connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(proceedButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    updateSelectionUi();
    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (accepted && selectedTrackIdsOut) {
        *selectedTrackIdsOut = selectedTrackIds;
    }
    return accepted;
}

bool downloadFileBlocking(QWidget* parent,
                          QNetworkAccessManager& network,
                          const QUrl& url,
                          const QString& outputPath,
                          const QByteArray& expectedSha256Hex,
                          QString* error)
{
    Q_UNUSED(parent);
    if (!url.isValid() || url.isEmpty()) {
        if (error) *error = QStringLiteral("Invalid download URL: %1").arg(url.toString());
        return false;
    }
    const QFileInfo outputInfo(outputPath);
    if (!QDir().mkpath(outputInfo.absolutePath())) {
        if (error) *error = QStringLiteral("Could not create model directory: %1").arg(outputInfo.absolutePath());
        return false;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = network.get(request);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(120000);
    loop.exec();

    const QScopedPointer<QNetworkReply, QScopedPointerDeleteLater> replyGuard(reply);
    if (!timeout.isActive()) {
        if (error) *error = QStringLiteral("Timed out downloading %1").arg(url.toString());
        return false;
    }
    timeout.stop();
    if (reply->error() != QNetworkReply::NoError) {
        if (error) *error = QStringLiteral("Failed to download %1: %2").arg(url.toString(), reply->errorString());
        return false;
    }
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpStatus >= 400) {
        if (error) *error = QStringLiteral("Failed to download %1: HTTP %2").arg(url.toString()).arg(httpStatus);
        return false;
    }
    const QByteArray payload = reply->readAll();
    if (payload.isEmpty()) {
        if (error) *error = QStringLiteral("Downloaded model file is empty: %1").arg(url.toString());
        return false;
    }
    const QByteArray actualSha256 =
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
    if (!expectedSha256Hex.isEmpty() && actualSha256 != expectedSha256Hex) {
        if (error) {
            *error = QStringLiteral("Downloaded model checksum mismatch for %1\nExpected: %2\nActual: %3")
                         .arg(url.toString(),
                              QString::fromLatin1(expectedSha256Hex),
                              QString::fromLatin1(actualSha256));
        }
        return false;
    }

    QSaveFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("Could not write model file %1: %2").arg(outputPath, output.errorString());
        return false;
    }
    if (output.write(payload) != payload.size()) {
        if (error) *error = QStringLiteral("Short write while saving model file: %1").arg(outputPath);
        return false;
    }
    if (!output.commit()) {
        if (error) *error = QStringLiteral("Could not commit model file %1: %2").arg(outputPath, output.errorString());
        return false;
    }
    return true;
}

bool downloadArcFaceModelFiles(QWidget* parent,
                               const QString& paramPath,
                               const QString& binPath,
                               QString* error)
{
    QProgressDialog progress(parent);
    progress.setWindowTitle(QStringLiteral("Download ArcFace NCNN Model"));
    progress.setLabelText(QStringLiteral("Downloading ArcFace MobileFaceNet NCNN model..."));
    progress.setCancelButton(nullptr);
    progress.setRange(0, 2);
    progress.setValue(0);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.show();
    QApplication::processEvents();

    QNetworkAccessManager network;
    static constexpr const char* kSourceRevision = "43631161ecef7567b9871bc711b9fb848629e9bc";
    static constexpr const char* kParamSha256 = "dfea20c1a5c2adef2126296f42771ecf33bfa9a50cbe0f7ca8864c7bd1a4bbeb";
    static constexpr const char* kBinSha256 = "9638dcfee63785b87a5ab0b0ed9f9dffe93e4b3718e5ea1b537dd5243a661b87";
    const QString sourceBase = QStringLiteral(
        "https://raw.githubusercontent.com/liguiyuan/mobilefacenet-ncnn/%1/models").arg(QString::fromLatin1(kSourceRevision));
    const QUrl paramUrl(sourceBase + QStringLiteral("/mobilefacenet.param"));
    const QUrl binUrl(sourceBase + QStringLiteral("/mobilefacenet.bin"));

    progress.setLabelText(QStringLiteral("Downloading arcface_mobilefacenet.param..."));
    if (!downloadFileBlocking(parent, network, paramUrl, paramPath, QByteArray(kParamSha256), error)) {
        return false;
    }
    progress.setValue(1);
    QApplication::processEvents();

    progress.setLabelText(QStringLiteral("Downloading arcface_mobilefacenet.bin..."));
    if (!downloadFileBlocking(parent, network, binUrl, binPath, QByteArray(kBinSha256), error)) {
        return false;
    }
    progress.setValue(2);
    QApplication::processEvents();
    return true;
}

void showArcFaceModelRequiredDialog(QWidget* parent,
                                    int trackCount,
                                    const QString& reason,
                                    const QString& paramPath,
                                    const QString& binPath)
{
    QMessageBox dialog(parent);
    dialog.setIcon(QMessageBox::Warning);
    dialog.setWindowTitle(QStringLiteral("Assign Speaker Identity"));
    dialog.setText(QStringLiteral("Identity clustering requires ArcFace NCNN before reviewing this many continuity tracks."));
    QString details = QStringLiteral("Tracks: %1\n\nExpected files:\n%2\n%3")
        .arg(trackCount)
        .arg(paramPath, binPath);
    if (!reason.trimmed().isEmpty()) {
        details += QStringLiteral("\n\nReason:\n%1").arg(reason.trimmed());
    }
    dialog.setInformativeText(
        QStringLiteral("Download the model files now, then run Assign Speaker Identity again.\n\n"
                       "Source: liguiyuan/mobilefacenet-ncnn at revision 43631161ecef7567b9871bc711b9fb848629e9bc."));
    dialog.setDetailedText(details);
    auto* downloadButton = dialog.addButton(QStringLiteral("Download Model"), QMessageBox::AcceptRole);
    dialog.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
    dialog.setDefaultButton(qobject_cast<QPushButton*>(downloadButton));
    dialog.exec();

    if (dialog.clickedButton() != downloadButton) {
        return;
    }

    QString error;
    if (downloadArcFaceModelFiles(parent, paramPath, binPath, &error)) {
        QMessageBox::information(parent,
                                 QStringLiteral("Assign Speaker Identity"),
                                 QStringLiteral("ArcFace NCNN model files were downloaded.\n\nRun Assign Speaker Identity again."));
        return;
    }
    QMessageBox::warning(parent,
                         QStringLiteral("Assign Speaker Identity"),
                         QStringLiteral("Failed to download ArcFace NCNN model files.\n\n%1").arg(error));
}

} // namespace

void SpeakersTab::onSpeakerEnableTrackingClicked()
{
    if (!activeCutMutable()) {
        return;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    if (!setSpeakerTrackingEnabled(speakerId, true)) {
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

void SpeakersTab::onSpeakerDisableTrackingClicked()
{
    if (!activeCutMutable()) {
        return;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
    if (!setSpeakerTrackingEnabled(speakerId, false)) {
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

void SpeakersTab::onSpeakerDeletePointstreamClicked()
{
    if (!activeCutMutable()) {
        return;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        updateSpeakerTrackingStatusLabel();
        return;
    }
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
}

void SpeakersTab::onSpeakerFaceStreamTableContextMenuRequested(const QPoint& pos)
{
    if (!m_widgets.speakerFaceStreamTable) {
        return;
    }
    const int row = m_widgets.speakerFaceStreamTable->rowAt(pos.y());
    if (row < 0) {
        return;
    }
    if (m_widgets.speakerFaceStreamTable->currentRow() != row) {
        m_widgets.speakerFaceStreamTable->setCurrentCell(row, 0);
    }
    const QString speakerId = selectedSpeakerId();
    QMenu menu(m_widgets.speakerFaceStreamTable);
    QAction* matchAction =
        menu.addAction(QStringLiteral("Find Matching Tracks for %1").arg(
            speakerId.isEmpty() ? QStringLiteral("Selected Speaker") : speakerDisplayLabel(speakerId)));
    matchAction->setEnabled(activeCutMutable() && !speakerId.isEmpty());
    QAction* chosen =
        menu.exec(m_widgets.speakerFaceStreamTable->viewport()->mapToGlobal(pos));
    if (chosen == matchAction) {
        onSpeakerFindMatchingTracksClicked();
    }
}

void SpeakersTab::onSpeakerFindMatchingTracksClicked()
{
    if (!activeCutMutable() || !m_transcriptSession.hasObjectDocument() ||
        m_transcriptSession.transcriptPath().trimmed().isEmpty()) {
        return;
    }
    const QString speakerId = selectedSpeakerId();
    if (speakerId.isEmpty()) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Find Matching Tracks"),
                                 QStringLiteral("Select a speaker first."));
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return;
    }
    if (!m_widgets.speakerFaceStreamTable) {
        return;
    }
    const int tableRow = m_widgets.speakerFaceStreamTable->currentRow();
    if (tableRow < 0) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Find Matching Tracks"),
                                 QStringLiteral("Select a continuity track first."));
        return;
    }
    QTableWidgetItem* streamItem = m_widgets.speakerFaceStreamTable->item(tableRow, 0);
    if (!streamItem) {
        return;
    }
    const int rowIndex = streamItem->data(Qt::UserRole + 1).toInt();
    if (rowIndex < 0 || rowIndex >= m_faceStreamPanelRows.size()) {
        return;
    }
    const QJsonObject seedStream = m_faceStreamPanelRows.at(rowIndex).toObject();
    const int seedTrackId = seedStream.value(QStringLiteral("track_id")).toInt(-1);
    if (seedTrackId < 0) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Find Matching Tracks"),
                             QStringLiteral("The selected row does not contain a valid continuity track."));
        return;
    }

    auto resolveMediaPath = [&](const TimelineClip& currentClip) {
        QString candidate = interactivePreviewMediaPathForClip(currentClip);
        const QFileInfo candidateInfo(candidate);
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
            return sourcePath;
        }
        return QString();
    };

    const QString mediaPath = resolveMediaPath(*clip);
    if (mediaPath.isEmpty()) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Find Matching Tracks"),
                             QStringLiteral("No playable media was found for this clip."));
        return;
    }

    const QJsonArray streams = continuityStreamsForClip(*clip);
    if (streams.isEmpty()) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Find Matching Tracks"),
                                 QStringLiteral("No continuity tracks are available for this clip."));
        return;
    }

    const QString arcfaceParamPath =
        jcut::identity_resolution::findArcFaceModelFile(QStringLiteral("arcface_mobilefacenet.param"));
    const QString arcfaceBinPath =
        jcut::identity_resolution::findArcFaceModelFile(QStringLiteral("arcface_mobilefacenet.bin"));
    if (!QFileInfo::exists(arcfaceParamPath) || !QFileInfo::exists(arcfaceBinPath)) {
        QMessageBox::warning(
            nullptr,
            QStringLiteral("Find Matching Tracks"),
            QStringLiteral("ArcFace model files are required for seeded identity matching.\n\nExpected:\n%1\n%2")
                .arg(arcfaceParamPath, arcfaceBinPath));
        return;
    }

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Find Matching Tracks"),
                             QStringLiteral("Could not create a temporary directory for identity crops."));
        return;
    }

    QProgressDialog progressDialog(
        QStringLiteral("Preparing seeded identity matching..."),
        QStringLiteral("Cancel"),
        0,
        qMax(1, streams.size() + 3),
        nullptr);
    progressDialog.setWindowTitle(QStringLiteral("Find Matching Tracks"));
    progressDialog.setWindowModality(Qt::ApplicationModal);
    progressDialog.setMinimumDuration(0);
    progressDialog.setAutoClose(false);
    progressDialog.setAutoReset(false);
    progressDialog.setValue(0);
    progressDialog.show();
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    bool canceled = false;
    auto updateProgress = [&](int value, const QString& message) {
        progressDialog.setLabelText(message);
        progressDialog.setValue(qBound(0, value, progressDialog.maximum()));
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        canceled = canceled || progressDialog.wasCanceled();
        return !canceled;
    };

    const QVector<RenderSyncMarker> renderSyncMarkers =
        m_speakerDeps.getRenderSyncMarkers ? m_speakerDeps.getRenderSyncMarkers() : QVector<RenderSyncMarker>{};
    const auto cropResult = jcut::facestream_assignment::extractRepresentativeCrops(
        jcut::facestream_assignment::CropExtractionRequest{
            *clip,
            renderSyncMarkers,
            mediaPath,
            tempDir.path(),
            QStringLiteral("seed_track_%1").arg(seedTrackId),
            streams},
        updateProgress);
    if (!cropResult.ok) {
        progressDialog.close();
        if (!cropResult.canceled) {
            QMessageBox::warning(nullptr,
                                 QStringLiteral("Find Matching Tracks"),
                                 cropResult.errorMessage.trimmed().isEmpty()
                                     ? QStringLiteral("Could not extract representative identity crops.")
                                     : cropResult.errorMessage);
        }
        return;
    }
    if (cropResult.candidates.isEmpty()) {
        progressDialog.close();
        QMessageBox::information(nullptr,
                                 QStringLiteral("Find Matching Tracks"),
                                 QStringLiteral("No usable identity comparison crops could be extracted."));
        return;
    }

    const auto matchResult = jcut::facestream_assignment::matchFaceTracksToSeed(
        jcut::facestream_assignment::SeedTrackMatchRequest{
            cropResult.candidates, seedTrackId, arcfaceParamPath, arcfaceBinPath},
        [&](int value, const QString& message) {
            return updateProgress(streams.size() + value, message);
        });
    progressDialog.close();
    if (!matchResult.ok) {
        if (!matchResult.cancelStageMessage.trimmed().isEmpty()) {
            return;
        }
        QMessageBox::warning(
            nullptr,
            QStringLiteral("Find Matching Tracks"),
            matchResult.embeddingError.trimmed().isEmpty()
                ? QStringLiteral("Seeded identity matching could not be completed.")
                : matchResult.embeddingError);
        return;
    }

    QVector<jcut::facestream_assignment::SeedTrackMatch> candidateMatches;
    candidateMatches.reserve(matchResult.matches.size());
    for (const auto& match : matchResult.matches) {
        if (match.decision == QLatin1StringView("seed") ||
            match.decision == QLatin1StringView("auto_match") ||
            match.decision == QLatin1StringView("review")) {
            candidateMatches.push_back(match);
        }
    }
    if (candidateMatches.size() <= 1) {
        QMessageBox::information(
            nullptr,
            QStringLiteral("Find Matching Tracks"),
            QStringLiteral("No additional continuity tracks cleared the review threshold for T%1.")
                .arg(seedTrackId));
        return;
    }

    const auto dialogResult = reviewSeedTrackMatches(
        nullptr,
        speakerDisplayLabel(speakerId),
        seedTrackId,
        candidateMatches,
        matchResult.autoMatchThreshold,
        matchResult.reviewThreshold);
    if (!dialogResult.accepted) {
        return;
    }

    QHash<int, QJsonObject> streamByTrackId;
    for (const QJsonValue& value : streams) {
        const QJsonObject streamObj = value.toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        if (trackId >= 0) {
            streamByTrackId.insert(trackId, streamObj);
        }
    }

    QJsonArray assignmentAnchors;
    for (const auto& match : candidateMatches) {
        if (!dialogResult.chosenTrackIds.contains(match.trackId)) {
            continue;
        }
        const QJsonObject streamObj = streamByTrackId.value(match.trackId);
        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
        if (streamObj.isEmpty() || keyframes.isEmpty()) {
            continue;
        }
        FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
        if (!parseFacestreamFrameDomainString(
                streamObj.value(QStringLiteral("frame_domain")).toString(),
                &frameDomain)) {
            continue;
        }
        const QJsonObject keyframe = keyframes.first().toObject();
        const int64_t streamFrame =
            qMax<int64_t>(0, keyframe.value(QStringLiteral("frame")).toVariant().toLongLong());
        const int64_t sourceFrame = mapFacestreamFrameToSourceFrame(
            *clip, streamFrame, frameDomain, renderSyncMarkers);
        const qreal xNorm = qBound<qreal>(
            0.0,
            keyframe.value(QStringLiteral("x")).toDouble(match.representativeCandidate.x),
            1.0);
        const qreal yNorm = qBound<qreal>(
            0.0,
            keyframe.value(QStringLiteral("y")).toDouble(match.representativeCandidate.y),
            1.0);
        const qreal boxNorm = qBound<qreal>(
            0.01,
            keyframe.value(QStringLiteral("box_size")).toDouble(match.representativeCandidate.box),
            1.0);
        QJsonObject row;
        row[QStringLiteral("track_id")] = match.trackId;
        row[QStringLiteral("stream_id")] =
            streamObj.value(QStringLiteral("stream_id")).toString(QStringLiteral("T%1").arg(match.trackId));
        row[QStringLiteral("source_frame")] = static_cast<qint64>(qMax<int64_t>(0, sourceFrame));
        row[QStringLiteral("x")] = xNorm;
        row[QStringLiteral("y")] = yNorm;
        row[QStringLiteral("box")] = boxNorm;
        assignmentAnchors.push_back(row);
    }

    if (assignmentAnchors.isEmpty()) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Find Matching Tracks"),
                             QStringLiteral("No valid continuity-track anchors were available to assign."));
        return;
    }
    if (!assignTrackAnchorsToSpeakerBatch(
            speakerId,
            assignmentAnchors,
            QStringLiteral("seed_identity_match_arcface"),
            QStringLiteral("seed_identity_match_arcface_set"))) {
        return;
    }

    QMessageBox::information(
        nullptr,
        QStringLiteral("Find Matching Tracks"),
        QStringLiteral("Assigned %1 continuity track(s) to %2 from seed track T%3.")
            .arg(assignmentAnchors.size())
            .arg(speakerDisplayLabel(speakerId))
            .arg(seedTrackId));
}

void SpeakersTab::onSpeakerPrecropFacesClicked()
{
    {
        const QString inlineTargetSpeakerId = selectedSpeakerId().trimmed();
        if (inlineTargetSpeakerId.isEmpty() || !m_widgets.speakerPlayheadFaceStreamsList) {
            QMessageBox::information(nullptr,
                                     QStringLiteral("Add Tracks"),
                                     QStringLiteral("Select a speaker and one or more playhead tracks first."));
            return;
        }
        const QList<QListWidgetItem*> selectedPlayheadItems =
            m_widgets.speakerPlayheadFaceStreamsList->selectedItems();
        if (selectedPlayheadItems.isEmpty()) {
            QMessageBox::information(nullptr,
                                     QStringLiteral("Add Tracks"),
                                     QStringLiteral("Select one or more tracks from the Tracks At Playhead list first."));
            return;
        }
        QJsonArray inlineAssignmentAnchors;
        for (QListWidgetItem* item : selectedPlayheadItems) {
            if (!item) {
                continue;
            }
            const int trackId = item->data(Qt::UserRole).toInt();
            if (trackId < 0) {
                continue;
            }
            QJsonObject anchor;
            anchor[QStringLiteral("track_id")] = trackId;
            anchor[QStringLiteral("stream_id")] = item->data(Qt::UserRole + 1).toString().trimmed();
            anchor[QStringLiteral("source_frame")] = item->data(Qt::UserRole + 2).toLongLong();
            anchor[QStringLiteral("x")] = item->data(Qt::UserRole + 3).toDouble();
            anchor[QStringLiteral("y")] = item->data(Qt::UserRole + 4).toDouble();
            anchor[QStringLiteral("box")] = item->data(Qt::UserRole + 5).toDouble();
            inlineAssignmentAnchors.push_back(anchor);
        }
        if (inlineAssignmentAnchors.isEmpty()) {
            QMessageBox::warning(nullptr,
                                 QStringLiteral("Add Tracks"),
                                 QStringLiteral("The selected playhead tracks did not contain usable assignment anchors."));
            return;
        }
        if (!assignTrackAnchorsToSpeakerBatch(
                inlineTargetSpeakerId,
                inlineAssignmentAnchors,
                QStringLiteral("playhead_sidebar_picker"),
                QStringLiteral("playhead_sidebar_picker_set"))) {
            return;
        }
        QMessageBox::information(nullptr,
                                 QStringLiteral("Add Tracks"),
                                 QStringLiteral("Assigned %1 continuity track(s) to %2.")
                                     .arg(inlineAssignmentAnchors.size())
                                     .arg(speakerDisplayLabel(inlineTargetSpeakerId)));
        return;
    }

    if (!activeCutMutable() || !m_transcriptSession.hasObjectDocument() ||
        m_transcriptSession.transcriptPath().isEmpty()) {
        return;
    }
    const QString targetSpeakerId = selectedSpeakerId().trimmed();
    if (targetSpeakerId.isEmpty()) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Add Tracks"),
                                 QStringLiteral("Select a speaker first, then add continuity tracks to that speaker."));
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return;
    }

    auto resolveMediaPath = [&](const TimelineClip& currentClip) {
        QString candidate = interactivePreviewMediaPathForClip(currentClip);
        const QFileInfo candidateInfo(candidate);
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
            return sourcePath;
        }
        return QString();
    };

    const QString mediaPath = resolveMediaPath(*clip);
    if (mediaPath.isEmpty()) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Add Tracks"),
                             QStringLiteral("No playable media was found for this clip."));
        return;
    }

    const auto sanitizedToken = [](const QString& raw) {
        QString token = raw;
        token = token.trimmed();
        if (token.isEmpty()) {
            return QStringLiteral("unknown");
        }
        for (QChar& ch : token) {
            const bool ok =
                (ch >= QLatin1Char('a') && ch <= QLatin1Char('z')) ||
                (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) ||
                (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) ||
                ch == QLatin1Char('.') ||
                ch == QLatin1Char('_') ||
                ch == QLatin1Char('-');
            if (!ok) {
                ch = QLatin1Char('_');
            }
        }
        while (token.contains(QStringLiteral("__"))) {
            token.replace(QStringLiteral("__"), QStringLiteral("_"));
        }
        token = token.left(96);
        return token.isEmpty() ? QStringLiteral("unknown") : token;
    };

    struct DebugRun {
        QString projectId;
        QString clipId;
        QString videoStem;
        QString runId;
        QString runDir;
        QString indexPath;
        QString overwriteDecisionPath;
        QJsonObject stageStatus;
        QJsonArray artefacts;
    };

    const auto nowRunId = []() {
        return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")) +
               QStringLiteral("-") +
               QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    };

    const auto deriveProjectId = [&](const QString& transcriptPath) {
        const QRegularExpression re(QStringLiteral(".*/projects/([^/]+)/.*"));
        const QRegularExpressionMatch m = re.match(transcriptPath);
        if (!m.hasMatch()) {
            return QString();
        }
        return m.captured(1).trimmed();
    };

    const QString projectId = sanitizedToken(deriveProjectId(m_transcriptSession.transcriptPath()));
    const QString clipId = sanitizedToken(clip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : clip->id);
    const QString videoStem = sanitizedToken(QFileInfo(mediaPath).completeBaseName());

    const auto makeDebugRun = [&](const QString& requestedRunId) -> DebugRun {
        DebugRun run;
        run.projectId = projectId;
        run.clipId = clipId;
        run.videoStem = videoStem;
        run.runId = requestedRunId.isEmpty() ? nowRunId() : requestedRunId;

        QString baseRoot;
        const QRegularExpression re(QStringLiteral("(.*/projects/[^/]+)/.*"));
        const QRegularExpressionMatch m = re.match(m_transcriptSession.transcriptPath());
        if (m.hasMatch()) {
            baseRoot = m.captured(1);
        } else {
            baseRoot = QDir::currentPath();
        }
        const QString debugRoot = QDir(baseRoot).absoluteFilePath(
            QStringLiteral("debug/speaker_flow/%1").arg(run.clipId));
        run.runDir = QDir(debugRoot).absoluteFilePath(run.runId);
        QDir().mkpath(run.runDir);
        run.indexPath = QDir(run.runDir).absoluteFilePath(QStringLiteral("index.json"));
        run.overwriteDecisionPath = QDir(run.runDir).absoluteFilePath(
            QStringLiteral("%1_overwrite_decision.json").arg(run.videoStem));
        return run;
    };

    DebugRun debugRun = makeDebugRun(QString());

    const auto addArtefact = [&](DebugRun& run, const QString& absolutePath) {
        const QString relative = QDir(run.runDir).relativeFilePath(absolutePath);
        run.artefacts.push_back(relative);
    };
    const auto setStageStatus = [&](DebugRun& run, const QString& stage, const QString& status, const QString& message) {
        QJsonObject statusObj;
        statusObj[QStringLiteral("status")] = status;
        statusObj[QStringLiteral("message")] = message;
        statusObj[QStringLiteral("updated_at_utc")] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        run.stageStatus[stage] = statusObj;
    };
    const auto persistIndex = [&](const DebugRun& run) {
        QJsonObject root;
        root[QStringLiteral("schema_version")] = QStringLiteral("1.0");
        root[QStringLiteral("run_id")] = run.runId;
        root[QStringLiteral("project_id")] = run.projectId;
        root[QStringLiteral("clip_id")] = run.clipId;
        root[QStringLiteral("video_filename")] = QFileInfo(mediaPath).fileName();
        root[QStringLiteral("transcript_path")] = m_transcriptSession.transcriptPath();
        root[QStringLiteral("completed_at_utc")] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        root[QStringLiteral("stage_status")] = run.stageStatus;
        root[QStringLiteral("artefacts")] = run.artefacts;
        QFile file(run.indexPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            file.close();
        }
    };
    const auto recordOverwriteDecision = [&](const DebugRun& run,
                                             const QString& stage,
                                             const QStringList& files,
                                             bool approved) {
        QJsonObject root;
        QFile f(run.overwriteDecisionPath);
        if (f.exists() && f.open(QIODevice::ReadOnly)) {
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
            if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
                root = doc.object();
            }
            f.close();
        }
        QJsonArray decisions = root.value(QStringLiteral("decisions")).toArray();
        QJsonObject decision;
        decision[QStringLiteral("stage")] = stage;
        decision[QStringLiteral("approved_by_user")] = approved;
        decision[QStringLiteral("timestamp_utc")] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        QJsonArray fileArray;
        for (const QString& file : files) {
            fileArray.push_back(QDir(run.runDir).relativeFilePath(file));
        }
        decision[QStringLiteral("files")] = fileArray;
        decisions.push_back(decision);
        root[QStringLiteral("decisions")] = decisions;
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            f.close();
        }
    };

    const auto ensureWritableArtefacts = [&](const QString& stage,
                                             const QStringList& files,
                                             bool allowCreateNewRun,
                                             bool* createNewRunOut) -> bool {
        if (createNewRunOut) {
            *createNewRunOut = false;
        }
        QStringList existing;
        for (const QString& file : files) {
            if (QFileInfo::exists(file)) {
                existing.push_back(file);
            }
        }
        if (existing.isEmpty()) {
            return true;
        }

        QMessageBox dialog;
        dialog.setIcon(QMessageBox::Warning);
        dialog.setWindowTitle(QStringLiteral("Overwrite Debug Artefacts"));
        QString details;
        const int previewCount = qMin(10, existing.size());
        for (int i = 0; i < previewCount; ++i) {
            details += QStringLiteral("- %1\n").arg(QDir(debugRun.runDir).relativeFilePath(existing.at(i)));
        }
        if (existing.size() > previewCount) {
            details += QStringLiteral("- ... and %1 more\n").arg(existing.size() - previewCount);
        }
        dialog.setText(
            QStringLiteral("Stage \"%1\" will overwrite %2 artefact(s).")
                .arg(stage)
                .arg(existing.size()));
        dialog.setInformativeText(details.trimmed());
        QPushButton* overwriteButton =
            dialog.addButton(QStringLiteral("Overwrite"), QMessageBox::AcceptRole);
        QPushButton* cancelButton =
            dialog.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
        QPushButton* newRunButton = nullptr;
        if (allowCreateNewRun) {
            newRunButton = dialog.addButton(QStringLiteral("Create New Run Instead"), QMessageBox::ActionRole);
            dialog.setDefaultButton(newRunButton);
        } else {
            dialog.setDefaultButton(overwriteButton);
        }
        dialog.exec();

        if (dialog.clickedButton() == cancelButton) {
            recordOverwriteDecision(debugRun, stage, existing, false);
            return false;
        }
        if (newRunButton && dialog.clickedButton() == newRunButton) {
            if (createNewRunOut) {
                *createNewRunOut = true;
            }
            return false;
        }
        if (dialog.clickedButton() != overwriteButton) {
            recordOverwriteDecision(debugRun, stage, existing, false);
            return false;
        }

        recordOverwriteDecision(debugRun, stage, existing, true);
        for (const QString& file : existing) {
            QFileInfo info(file);
            if (info.isDir()) {
                QDir(file).removeRecursively();
            } else {
                QFile::remove(file);
            }
        }
        return true;
    };

    const int stepFrames = 0;
    int maxCandidates = 0;
    QVector<facefind::Candidate> candidates;
    editor::TranscriptEngine transcriptEngine;
    QJsonObject artifactRoot;
    QJsonArray streams;
    QString importedArtifactDir;
    QString facestreamPath;
    if (transcriptEngine.loadFacestreamArtifact(m_transcriptSession.transcriptPath(), &artifactRoot)) {
        const QJsonObject continuityRoot = continuityRootForClip(artifactRoot, clip->id);
        streams = jcut::facestream::continuityStreamsForRoot(
            continuityRoot,
            m_transcriptSession.rootObject());
        importedArtifactDir = continuityRoot.value(QStringLiteral("imported_from_artifact_dir")).toString();
        facestreamPath = continuityRoot.value(QStringLiteral("facestream_part")).toString();
        if (facestreamPath.isEmpty()) {
            facestreamPath = continuityRoot.value(QStringLiteral("facestream_bin")).toString();
        }
    }
    if (streams.isEmpty()) {
        const QJsonObject transcriptRoot = m_transcriptSession.rootObject();
        const QJsonObject speakerFlow = transcriptRoot.value(QStringLiteral("speaker_flow")).toObject();
        const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
        const QJsonObject clipFlow = clipsRoot.value(clip->id.trimmed()).toObject();
        const QJsonObject continuityRoot = clipFlow.value(QStringLiteral("continuity_facestreams")).toObject();
        streams = continuityRoot.value(QStringLiteral("streams")).toArray();
    }
    if (streams.isEmpty()) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Assign Speaker Identity"),
                                 QStringLiteral("No generated continuity tracks were found for this clip. Generate FaceStream first."));
        return;
    }
    const int maxManualSingletonReviewRows = 200;
    const QString kStageCrop = QStringLiteral("stage_3_facestream_crop");
    const QString kStageCropReview = QStringLiteral("stage_4_facestream_crop");
    const QString kStageClustering = QStringLiteral("stage_5_identity_clustering");
    const QString kStageAssignment = QStringLiteral("stage_6_assignment");
    const QString kStatusSkipped = QStringLiteral("skipped");
    const QString kMsgCanceledCropExtraction = QStringLiteral("User canceled FaceStream crop extraction.");
    const QString kMsgCanceledClusteringPreflight = QStringLiteral("User canceled clustering preflight.");
    const QString kMsgCanceledAssignmentDialog = QStringLiteral("User canceled assignment dialog.");
    const QString arcfaceParamPath =
        jcut::identity_resolution::findArcFaceModelFile(QStringLiteral("arcface_mobilefacenet.param"));
    const QString arcfaceBinPath =
        jcut::identity_resolution::findArcFaceModelFile(QStringLiteral("arcface_mobilefacenet.bin"));
    QString cropsDir = QDir(debugRun.runDir).absoluteFilePath(
        QStringLiteral("%1_facestream_track_crops").arg(debugRun.videoStem));
    QString outputJsonPath = QDir(debugRun.runDir).absoluteFilePath(
        QStringLiteral("%1_facestream_track_candidates.json").arg(debugRun.videoStem));
    bool createNewRun = false;
    if (!ensureWritableArtefacts(
            kStageCrop,
            QStringList{outputJsonPath, cropsDir},
            true,
            &createNewRun)) {
        if (createNewRun) {
            debugRun = makeDebugRun(QString());
            cropsDir = QDir(debugRun.runDir).absoluteFilePath(
                QStringLiteral("%1_facestream_track_crops").arg(debugRun.videoStem));
            outputJsonPath = QDir(debugRun.runDir).absoluteFilePath(
                QStringLiteral("%1_facestream_track_candidates.json").arg(debugRun.videoStem));
        } else {
            setStageStatus(debugRun, kStageCrop, kStatusSkipped,
                           QStringLiteral("Canceled due to overwrite prompt."));
            persistIndex(debugRun);
            return;
        }
    }
    QDir().mkpath(cropsDir);

    QProgressDialog progressDialog(
        QStringLiteral("Preparing FaceStream assignment..."),
        QStringLiteral("Cancel"),
        0,
        qMax(1, streams.size() + 3),
        nullptr);
    progressDialog.setWindowTitle(QStringLiteral("Add Tracks"));
    progressDialog.setWindowModality(Qt::ApplicationModal);
    progressDialog.setMinimumDuration(0);
    progressDialog.setAutoClose(false);
    progressDialog.setAutoReset(false);
    progressDialog.setValue(0);
    progressDialog.show();
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    bool assignmentCanceled = false;
    auto updateProgress = [&](int value, const QString& message) {
        progressDialog.setLabelText(message);
        progressDialog.setValue(qBound(0, value, progressDialog.maximum()));
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        assignmentCanceled = assignmentCanceled || progressDialog.wasCanceled();
        return !assignmentCanceled;
    };
    const QVector<RenderSyncMarker> renderSyncMarkers =
        m_speakerDeps.getRenderSyncMarkers ? m_speakerDeps.getRenderSyncMarkers() : QVector<RenderSyncMarker>{};

    const auto cropResult = jcut::facestream_assignment::extractRepresentativeCrops(
        jcut::facestream_assignment::CropExtractionRequest{
            *clip, renderSyncMarkers, mediaPath, cropsDir, debugRun.videoStem, streams},
        updateProgress);
    if (!cropResult.ok) {
        progressDialog.close();
        if (cropResult.canceled) {
            setStageStatus(debugRun, kStageCrop, kStatusSkipped, kMsgCanceledCropExtraction);
            persistIndex(debugRun);
            return;
        }
        QMessageBox::warning(nullptr, QStringLiteral("Add Tracks"), cropResult.errorMessage);
        return;
    }
    QJsonArray candidateRows = cropResult.candidateRows;
    candidates = cropResult.candidates;
    maxCandidates = candidates.size();
    QJsonObject candidateRoot;
    candidateRoot[QStringLiteral("schema_version")] = QStringLiteral("1.0");
    candidateRoot[QStringLiteral("source")] = QStringLiteral("generated_facestream_tracks");
    candidateRoot[QStringLiteral("media_path")] = mediaPath;
    candidateRoot[QStringLiteral("imported_artifact_dir")] = importedArtifactDir;
    candidateRoot[QStringLiteral("facestream_part")] = facestreamPath;
    candidateRoot[QStringLiteral("created_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    candidateRoot[QStringLiteral("candidates")] = candidateRows;
    QFile candidateFile(outputJsonPath);
    if (candidateFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        candidateFile.write(QJsonDocument(candidateRoot).toJson(QJsonDocument::Indented));
        candidateFile.close();
        addArtefact(debugRun, outputJsonPath);
    }
    addArtefact(debugRun, cropsDir);
    setStageStatus(debugRun, kStageCrop, QStringLiteral("ok"),
                   QStringLiteral("Extracted representative continuity-track crops for identity clustering."));
    persistIndex(debugRun);
    if (candidates.isEmpty()) {
        progressDialog.close();
        setStageStatus(debugRun, kStageCropReview, QStringLiteral("warn"),
                       QStringLiteral("No candidates produced."));
        persistIndex(debugRun);
        QMessageBox::information(nullptr,
                                 QStringLiteral("Add Tracks"),
                                 QStringLiteral("No usable identity comparison crops could be extracted."));
        return;
    }

    QJsonObject root = m_transcriptSession.rootObject();
    const QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    QStringList speakerIds;
    for (auto it = profiles.constBegin(); it != profiles.constEnd(); ++it) {
        const QString speakerId = it.key().trimmed();
        if (!speakerId.isEmpty()) {
            speakerIds.push_back(speakerId);
        }
    }
    if (speakerIds.isEmpty() && m_widgets.speakersTable) {
        for (int row = 0; row < m_widgets.speakersTable->rowCount(); ++row) {
            QTableWidgetItem* idItem = m_widgets.speakersTable->item(row, 1);
            if (!idItem) {
                continue;
            }
            const QString speakerId = idItem->data(Qt::UserRole).toString().trimmed();
            if (!speakerId.isEmpty()) {
                speakerIds.push_back(speakerId);
            }
        }
        speakerIds.removeDuplicates();
    }
    std::sort(speakerIds.begin(), speakerIds.end(), [](const QString& a, const QString& b) {
        return a.localeAwareCompare(b) < 0;
    });
    if (speakerIds.isEmpty()) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Add Tracks"),
                                 QStringLiteral("No transcript speaker IDs were available to assign the continuity tracks."));
        return;
    }

    auto suggestedSpeakerForFrame = [&](int64_t timelineFrame) -> QString {
        const double timeSeconds = static_cast<double>(timelineFrameToSeconds(timelineFrame));
        const QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
        QString nearestSpeaker;
        double nearestDistance = std::numeric_limits<double>::max();
        for (const QJsonValue& segValue : segments) {
            const QJsonObject segObj = segValue.toObject();
            const QString segmentSpeaker =
                segObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
            const QJsonArray words = segObj.value(QStringLiteral("words")).toArray();
            for (const QJsonValue& wordValue : words) {
                const QJsonObject wordObj = wordValue.toObject();
                if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                    continue;
                }
                QString speaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
                if (speaker.isEmpty()) {
                    speaker = segmentSpeaker;
                }
                if (speaker.isEmpty()) {
                    continue;
                }
                const double start = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
                const double end = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
                if (start < 0.0 || end < 0.0) {
                    continue;
                }
                if (timeSeconds >= start && timeSeconds <= end) {
                    return speaker;
                }
                const double distance = qMin(std::abs(timeSeconds - start), std::abs(timeSeconds - end));
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    nearestSpeaker = speaker;
                }
            }
        }
        return nearestSpeaker;
    };

    const QString clipFlowId =
        (clip && !clip->id.trimmed().isEmpty()) ? clip->id.trimmed() : QStringLiteral("unknown_clip");
    QHash<int, QString> persistedIdentityByTrackId;
    {
        const QJsonObject transcriptRoot = m_transcriptSession.rootObject();
        const QJsonObject speakerFlow = transcriptRoot.value(QStringLiteral("speaker_flow")).toObject();
        const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
        const QJsonObject clipFlowRoot = clipsRoot.value(clipFlowId).toObject();
        const QJsonObject resolvedCurrent = clipFlowRoot.value(QStringLiteral("resolved_current")).toObject();
        const QJsonArray resolvedMap = resolvedCurrent.value(QStringLiteral("track_identity_map")).toArray();
        for (const QJsonValue& value : resolvedMap) {
            const QJsonObject row = value.toObject();
            const int trackId = row.value(QStringLiteral("track_id")).toInt(-1);
            const QString identityId = row.value(QStringLiteral("identity_id")).toString().trimmed();
            if (trackId >= 0 && !identityId.isEmpty()) {
                persistedIdentityByTrackId.insert(trackId, identityId);
            }
        }
    }

    auto persistSpeakerFlowSnapshot = [&](const QJsonObject& machinePayload,
                                          const QJsonObject& humanPayload,
                                          const QJsonObject& resolvedPayload) {
        QJsonObject transcriptRoot = m_transcriptSession.rootObject();
        QJsonObject speakerFlow = transcriptRoot.value(QStringLiteral("speaker_flow")).toObject();
        speakerFlow[QStringLiteral("schema_version")] = QStringLiteral("1.0");

        QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
        QJsonObject clipRoot = clipsRoot.value(clipFlowId).toObject();
        clipRoot[QStringLiteral("clip_id")] = clipFlowId;
        clipRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

        if (!machinePayload.isEmpty()) {
            QJsonObject machineRuns = clipRoot.value(QStringLiteral("machine_runs")).toObject();
            machineRuns[debugRun.runId] = machinePayload;
            clipRoot[QStringLiteral("machine_runs")] = machineRuns;
            clipRoot[QStringLiteral("latest_machine_run_id")] = debugRun.runId;
        }
        if (!humanPayload.isEmpty()) {
            QJsonObject humanRuns = clipRoot.value(QStringLiteral("human_runs")).toObject();
            humanRuns[debugRun.runId] = humanPayload;
            clipRoot[QStringLiteral("human_runs")] = humanRuns;
            clipRoot[QStringLiteral("latest_human_run_id")] = debugRun.runId;
        }
        if (!resolvedPayload.isEmpty()) {
            clipRoot[QStringLiteral("resolved_current")] = resolvedPayload;
        }
        clipsRoot[clipFlowId] = clipRoot;
        speakerFlow[QStringLiteral("clips")] = clipsRoot;
        transcriptRoot[QStringLiteral("speaker_flow")] = speakerFlow;
        updateLoadedTranscriptDocument([&](QJsonObject& root) {
            root = transcriptRoot;
            return true;
        });
        saveLoadedTranscriptDocument();
    };

    {
        QJsonArray machineCandidates;
        QJsonArray machineSuggestions;
        for (int i = 0; i < candidates.size(); ++i) {
            const facefind::Candidate& c = candidates.at(i);
            QJsonObject row;
            row[QStringLiteral("candidate_index")] = i;
            row[QStringLiteral("frame")] = static_cast<qint64>(c.frame);
            row[QStringLiteral("x")] = c.x;
            row[QStringLiteral("y")] = c.y;
            row[QStringLiteral("box")] = c.box;
            row[QStringLiteral("score")] = c.score;
            row[QStringLiteral("track_id")] = c.trackId;
            row[QStringLiteral("crop_path")] = c.cropPath;
            machineCandidates.push_back(row);

            QJsonObject s;
            s[QStringLiteral("candidate_index")] = i;
            s[QStringLiteral("track_id")] = c.trackId;
            s[QStringLiteral("suggested_identity_id")] = suggestedSpeakerForFrame(c.frame);
            s[QStringLiteral("source")] = QStringLiteral("timing_nearest");
            machineSuggestions.push_back(s);
        }
        QJsonObject machinePayload;
        machinePayload[QStringLiteral("run_id")] = debugRun.runId;
        machinePayload[QStringLiteral("created_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        machinePayload[QStringLiteral("detection_source")] = QStringLiteral("generated_facestream_tracks");
        machinePayload[QStringLiteral("candidates")] = machineCandidates;
        machinePayload[QStringLiteral("suggestions")] = machineSuggestions;
        machinePayload[QStringLiteral("candidate_count")] = candidates.size();
        machinePayload[QStringLiteral("max_candidates")] = maxCandidates;
        machinePayload[QStringLiteral("step_frames")] = stepFrames;
        persistSpeakerFlowSnapshot(machinePayload, QJsonObject(), QJsonObject());
    }

    QVector<facefind::Candidate> trackCandidates = candidates;
    progressDialog.close();
    QSet<int> selectedTrackIds;
    QVector<int> playheadSampleIndexes;
    bool usedPlayheadTrackFallback = false;
    QHash<int, QJsonObject> previewAnchorByTrackId;
    {
        const int64_t playheadTimelineFrame =
            m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : clip->startFrame;
        const int64_t playheadSourceFrame =
            sourceFrameForClipAtTimelinePosition(*clip, static_cast<qreal>(playheadTimelineFrame), renderSyncMarkers);

        QHash<int, int> bestCandidateIndexByTrackId;
        auto registerCandidateIndex = [&](int trackId, int candidateIndex) {
            if (trackId < 0 || candidateIndex < 0 || candidateIndex >= candidates.size()) {
                return;
            }
            const auto existing = bestCandidateIndexByTrackId.constFind(trackId);
            if (existing == bestCandidateIndexByTrackId.constEnd()) {
                bestCandidateIndexByTrackId.insert(trackId, candidateIndex);
                return;
            }
            const facefind::Candidate& current = candidates.at(candidateIndex);
            const facefind::Candidate& prior = candidates.at(existing.value());
            const qint64 currentDistance = std::llabs(current.frame - playheadTimelineFrame);
            const qint64 priorDistance = std::llabs(prior.frame - playheadTimelineFrame);
            if (currentDistance < priorDistance ||
                (currentDistance == priorDistance && current.score > prior.score)) {
                bestCandidateIndexByTrackId.insert(trackId, candidateIndex);
            }
        };

        for (int candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
            const int trackId = candidates.at(candidateIndex).trackId;
            registerCandidateIndex(trackId, candidateIndex);
        }

        QVector<int> activeTrackIds;
        activeTrackIds.reserve(streams.size());
        QHash<int, QString> streamIdByTrackId;
        for (const QJsonValue& value : streams) {
            const QJsonObject streamObj = value.toObject();
            const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
            streamIdByTrackId.insert(
                trackId,
                streamObj.value(QStringLiteral("stream_id")).toString(QStringLiteral("T%1").arg(trackId)));
            const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
            if (trackId < 0 || keyframes.isEmpty()) {
                continue;
            }
            FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
            if (!parseFacestreamFrameDomainString(
                    streamObj.value(QStringLiteral("frame_domain")).toString(),
                    &frameDomain)) {
                continue;
            }
            int64_t minSourceFrame = std::numeric_limits<int64_t>::max();
            int64_t maxSourceFrame = std::numeric_limits<int64_t>::min();
            for (const QJsonValue& keyframeValue : keyframes) {
                const QJsonObject keyframeObj = keyframeValue.toObject();
                const int64_t streamFrame =
                    keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong();
                const int64_t sourceFrame =
                    mapFacestreamFrameToSourceFrame(*clip, streamFrame, frameDomain, renderSyncMarkers);
                minSourceFrame = qMin(minSourceFrame, sourceFrame);
                maxSourceFrame = qMax(maxSourceFrame, sourceFrame);
            }
            if (minSourceFrame <= playheadSourceFrame && playheadSourceFrame <= maxSourceFrame) {
                activeTrackIds.push_back(trackId);
            }
        }

        usedPlayheadTrackFallback = activeTrackIds.isEmpty();
        const QVector<int> trackIdsForPreview = activeTrackIds.isEmpty()
            ? bestCandidateIndexByTrackId.keys().toVector()
            : activeTrackIds;
        playheadSampleIndexes.reserve(trackIdsForPreview.size());
        for (int trackId : trackIdsForPreview) {
            const auto it = bestCandidateIndexByTrackId.constFind(trackId);
            if (it != bestCandidateIndexByTrackId.constEnd()) {
                playheadSampleIndexes.push_back(it.value());
                if (it.value() >= 0 && it.value() < candidateRows.size()) {
                    QJsonObject anchor = candidateRows.at(it.value()).toObject();
                    anchor[QStringLiteral("stream_id")] = streamIdByTrackId.value(
                        trackId,
                        QStringLiteral("T%1").arg(trackId));
                    previewAnchorByTrackId.insert(trackId, anchor);
                }
            }
        }
        std::sort(playheadSampleIndexes.begin(), playheadSampleIndexes.end(), [&](int lhs, int rhs) {
            const facefind::Candidate& a = candidates.at(lhs);
            const facefind::Candidate& b = candidates.at(rhs);
            const qint64 distA = std::llabs(a.frame - playheadTimelineFrame);
            const qint64 distB = std::llabs(b.frame - playheadTimelineFrame);
            if (distA != distB) {
                return distA < distB;
            }
            return a.trackId < b.trackId;
        });
    }
    if (!confirmClusteringPreflight(
            nullptr,
            trackCandidates,
            speakerDisplayLabel(targetSpeakerId),
            usedPlayheadTrackFallback
                ? QStringLiteral("No continuity tracks span the current playhead. Showing the nearest representative crop for each track (%1 total generated track(s)).")
                      .arg(streams.size())
                : QStringLiteral("%1 active continuity track(s) shown from playhead context (%2 total generated track(s)).")
                      .arg(playheadSampleIndexes.size())
                .arg(streams.size()),
            playheadSampleIndexes,
            &selectedTrackIds)) {
        setStageStatus(debugRun, kStageClustering, kStatusSkipped, kMsgCanceledClusteringPreflight);
        persistIndex(debugRun);
        return;
    }
    QJsonArray assignmentAnchors;
    for (int trackId : std::as_const(selectedTrackIds)) {
        const auto it = previewAnchorByTrackId.constFind(trackId);
        if (it != previewAnchorByTrackId.constEnd()) {
            assignmentAnchors.push_back(it.value());
        }
    }
    if (assignmentAnchors.isEmpty()) {
        setStageStatus(debugRun, kStageAssignment, QStringLiteral("warn"),
                       QStringLiteral("No continuity-track anchors were selected."));
        persistIndex(debugRun);
        return;
    }
    if (!assignTrackAnchorsToSpeakerBatch(
            targetSpeakerId,
            assignmentAnchors,
            QStringLiteral("playhead_track_picker"),
            QStringLiteral("playhead_track_picker_set"))) {
        setStageStatus(debugRun, kStageAssignment, QStringLiteral("warn"),
                       QStringLiteral("Selected continuity-track anchors could not be assigned."));
        persistIndex(debugRun);
        return;
    }
    setStageStatus(debugRun, kStageAssignment, QStringLiteral("ok"),
                   QStringLiteral("Selected continuity tracks were assigned directly to the chosen speaker."));
    persistIndex(debugRun);
    QMessageBox::information(nullptr,
                             QStringLiteral("Add Tracks"),
                             QStringLiteral("Assigned %1 continuity track(s) to %2.")
                                 .arg(assignmentAnchors.size())
                                 .arg(speakerDisplayLabel(targetSpeakerId)));
    return;

    QVector<facefind::Candidate> clusterCandidates;
    QJsonArray clusterRows;
    QJsonArray clusterDiagnosticsRows;
    QString clusterSummaryText;
    bool embeddingReadyForDialog = false;
    QString embeddingErrorForDialog;
    {
        const auto clusterResult = jcut::facestream_assignment::clusterFaceTracks(
            jcut::facestream_assignment::ClusterRequest{trackCandidates, arcfaceParamPath, arcfaceBinPath},
            [&](int value, const QString& message) {
                return updateProgress(streams.size() + value, message);
            });

        if (!clusterResult.ok) {
            setStageStatus(debugRun, kStageClustering, kStatusSkipped,
                           clusterResult.cancelStageMessage);
            persistIndex(debugRun);
            progressDialog.close();
            return;
        }

        clusterCandidates = clusterResult.clusterCandidates;
        clusterRows = clusterResult.clusterRows;
        clusterDiagnosticsRows = clusterResult.clusterDiagnosticsRows;
        embeddingReadyForDialog = clusterResult.embeddingReady;
        embeddingErrorForDialog = clusterResult.embeddingError;

        const QString identitySidecarPath = transcriptEngine.identityArtifactPath(m_transcriptSession.transcriptPath());
        QJsonObject identityRoot;
        transcriptEngine.loadIdentityArtifact(m_transcriptSession.transcriptPath(), &identityRoot);
        QJsonObject clustersRoot;
        clustersRoot[QStringLiteral("run_id")] = debugRun.runId;
        clustersRoot[QStringLiteral("created_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        clustersRoot[QStringLiteral("source")] = QStringLiteral("generated_facestream_track_crops");
        clustersRoot[QStringLiteral("sidecar_path")] = identitySidecarPath;
        clustersRoot[QStringLiteral("facestream_part")] = facestreamPath;
        clustersRoot[QStringLiteral("embedding_model")] = QStringLiteral("arcface_mobilefacenet_ncnn");
        clustersRoot[QStringLiteral("embedding_available")] = clusterResult.embeddingReady;
        clustersRoot[QStringLiteral("embedding_error")] =
            clusterResult.embeddingReady ? QString() : clusterResult.embeddingError;
        clustersRoot[QStringLiteral("track_count")] = streams.size();
        clustersRoot[QStringLiteral("crop_sample_count")] = trackCandidates.size();
        clustersRoot[QStringLiteral("embedded_track_count")] = clusterResult.embeddedTrackCount;
        clustersRoot[QStringLiteral("cluster_count")] = clusterCandidates.size();
        clustersRoot[QStringLiteral("auto_cluster_pair_count")] = clusterResult.autoClusterPairCount;
        clustersRoot[QStringLiteral("review_pair_count")] = clusterResult.reviewPairCount;
        clustersRoot[QStringLiteral("auto_cluster_threshold")] = clusterResult.autoClusterThreshold;
        clustersRoot[QStringLiteral("review_threshold")] = clusterResult.reviewThreshold;
        clustersRoot[QStringLiteral("clusters")] = clusterRows;

        QJsonObject diagnosticsRoot;
        diagnosticsRoot[QStringLiteral("run_id")] = debugRun.runId;
        diagnosticsRoot[QStringLiteral("pairs")] = clusterDiagnosticsRows;
        clustersRoot[QStringLiteral("diagnostics")] = diagnosticsRoot;

        QJsonObject clustersByClip = identityRoot.value(QStringLiteral("identity_clusters_by_clip")).toObject();
        clustersByClip[clipFlowId] = clustersRoot;
        identityRoot[QStringLiteral("schema")] = QStringLiteral("jcut_identity_v1");
        identityRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        identityRoot[QStringLiteral("identity_clusters_by_clip")] = clustersByClip;
        if (transcriptEngine.saveIdentityArtifact(m_transcriptSession.transcriptPath(), identityRoot)) {
            addArtefact(debugRun, identitySidecarPath);
        } else {
            setStageStatus(debugRun,
                           kStageClustering,
                           QStringLiteral("error"),
                           QStringLiteral("Failed to write identity sidecar: %1").arg(identitySidecarPath));
            persistIndex(debugRun);
            return;
        }
        setStageStatus(debugRun,
                       kStageClustering,
                       clusterResult.embeddingReady ? QStringLiteral("ok") : QStringLiteral("warn"),
                       clusterResult.embeddingReady
                           ? QStringLiteral("Auto-cluster ran: %1 crop sample(s) across %2 tracks -> %3 identity clusters; %4 auto-merge pair(s), %5 review pair(s).")
                                 .arg(trackCandidates.size())
                                 .arg(streams.size())
                                 .arg(clusterCandidates.size())
                                 .arg(clusterResult.autoClusterPairCount)
                                 .arg(clusterResult.reviewPairCount)
                           : QStringLiteral("Auto-cluster fallback: ArcFace unavailable; %1 tracks -> %2 singleton cluster(s).")
                                 .arg(streams.size())
                                 .arg(clusterCandidates.size()));
        persistIndex(debugRun);
        updateProgress(streams.size() + 3, QStringLiteral("FaceStream identity preparation complete."));
        clusterSummaryText = clusterResult.embeddingReady
            ? QStringLiteral("Auto-cluster ran: %1 FaceStream crop sample(s) across %2 track(s) -> %3 identity cluster(s) using ArcFace NCNN. Auto-merge pairs: %4. Review pairs: %5. Thresholds: auto >= %6, review >= %7.")
                  .arg(trackCandidates.size())
                  .arg(streams.size())
                  .arg(clusterCandidates.size())
                  .arg(clusterResult.autoClusterPairCount)
                  .arg(clusterResult.reviewPairCount)
                  .arg(clusterResult.autoClusterThreshold, 0, 'f', 2)
                  .arg(clusterResult.reviewThreshold, 0, 'f', 2)
            : QStringLiteral("Auto-cluster fallback: ArcFace NCNN unavailable (%1). %2 FaceStream track(s) are shown as %3 singleton cluster(s).")
                  .arg(clusterResult.embeddingError.trimmed().isEmpty() ? QStringLiteral("unknown error")
                                                                        : clusterResult.embeddingError)
                  .arg(streams.size())
                  .arg(clusterCandidates.size());
    }
    if (!clusterCandidates.isEmpty()) {
        candidates = clusterCandidates;
    }
    progressDialog.close();

    if (!embeddingReadyForDialog && trackCandidates.size() > maxManualSingletonReviewRows) {
        showArcFaceModelRequiredDialog(
            nullptr,
            trackCandidates.size(),
            embeddingErrorForDialog.trimmed().isEmpty() ? QStringLiteral("unknown error") : embeddingErrorForDialog,
            jcut::identity_resolution::findArcFaceModelFile(QStringLiteral("arcface_mobilefacenet.param")),
            jcut::identity_resolution::findArcFaceModelFile(QStringLiteral("arcface_mobilefacenet.bin")));
        refresh();
        return;
    }

    QStringList autoSuggestedSpeakerIds;
    autoSuggestedSpeakerIds.reserve(candidates.size());
    for (const facefind::Candidate& candidate : std::as_const(candidates)) {
        autoSuggestedSpeakerIds.push_back(suggestedSpeakerForFrame(candidate.frame));
    }

    QStringList suggestedSpeakerIds;
    suggestedSpeakerIds.reserve(candidates.size());
    QStringList defaultSourceLabels;
    defaultSourceLabels.reserve(candidates.size());
    for (const facefind::Candidate& candidate : std::as_const(candidates)) {
        QString persistedIdentity;
        const QVector<int> memberTracks = candidate.clusterTrackIds.isEmpty()
            ? QVector<int>{candidate.trackId}
            : candidate.clusterTrackIds;
        for (int trackId : memberTracks) {
            const QString candidatePersisted = persistedIdentityByTrackId.value(trackId).trimmed();
            if (!candidatePersisted.isEmpty()) {
                persistedIdentity = candidatePersisted;
                break;
            }
        }
        if (!persistedIdentity.isEmpty()) {
            suggestedSpeakerIds.push_back(persistedIdentity);
            defaultSourceLabels.push_back(QStringLiteral("Persisted (Human)"));
            continue;
        }
        suggestedSpeakerIds.push_back(suggestedSpeakerForFrame(candidate.frame));
        defaultSourceLabels.push_back(QStringLiteral("Auto (Timing)"));
    }

    QHash<QString, QString> speakerLabels;
    speakerLabels.reserve(speakerIds.size());
    for (const QString& speakerId : speakerIds) {
        const QString name =
            profiles.value(speakerId).toObject().value(QString(kTranscriptSpeakerNameKey)).toString().trimmed();
        speakerLabels.insert(
            speakerId,
            (name.isEmpty() || name == speakerId) ? speakerId : QStringLiteral("%1 (%2)").arg(speakerId, name));
    }

    const facefind::AssignmentDialogResult dialogResult =
        facefind::showFaceFindWindow(
            nullptr,
            candidates,
            speakerIds,
            speakerLabels,
            suggestedSpeakerIds,
            autoSuggestedSpeakerIds,
            defaultSourceLabels,
            clusterSummaryText);
    if (!dialogResult.accepted) {
        setStageStatus(debugRun, kStageAssignment, kStatusSkipped, kMsgCanceledAssignmentDialog);
        persistIndex(debugRun);
        return;
    }

    const QJsonArray assignmentTableRows = dialogResult.assignmentTableRows;
    QHash<QString, QVector<facefind::Candidate>> assignmentsBySpeaker;

    {
        const QString timestampUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        const auto resolution =
            jcut::facestream_assignment::resolveTrackIdentityAssignments(
                assignmentTableRows,
                trackCandidates,
                timestampUtc);
        const QJsonArray overrides = resolution.overrides;
        const QJsonArray auditLog = resolution.auditLog;
        const QJsonArray resolvedMap = resolution.resolvedMap;
        assignmentsBySpeaker = resolution.assignmentsBySpeaker;
        QJsonObject humanPayload;
        humanPayload[QStringLiteral("run_id")] = debugRun.runId;
        humanPayload[QStringLiteral("updated_at_utc")] = timestampUtc;
        humanPayload[QStringLiteral("assignment_table_rows")] = assignmentTableRows;
        humanPayload[QStringLiteral("track_identity_overrides")] = overrides;
        humanPayload[QStringLiteral("audit_log")] = auditLog;

        QJsonObject resolvedPayload;
        resolvedPayload[QStringLiteral("run_id")] = debugRun.runId;
        resolvedPayload[QStringLiteral("updated_at_utc")] = timestampUtc;
        resolvedPayload[QStringLiteral("track_identity_map")] = resolvedMap;
        QJsonObject identityRoot;
        transcriptEngine.loadIdentityArtifact(m_transcriptSession.transcriptPath(), &identityRoot);
        QJsonObject assignmentsByClip = identityRoot.value(QStringLiteral("identity_assignments_by_clip")).toObject();
        QJsonObject assignmentRoot;
        assignmentRoot[QStringLiteral("run_id")] = debugRun.runId;
        assignmentRoot[QStringLiteral("updated_at_utc")] = timestampUtc;
        assignmentRoot[QStringLiteral("assignment_table_rows")] = assignmentTableRows;
        assignmentRoot[QStringLiteral("track_identity_overrides")] = overrides;
        assignmentRoot[QStringLiteral("track_identity_map")] = resolvedMap;
        assignmentsByClip[clipFlowId] = assignmentRoot;
        identityRoot[QStringLiteral("schema")] = QStringLiteral("jcut_identity_v1");
        identityRoot[QStringLiteral("updated_at_utc")] = timestampUtc;
        identityRoot[QStringLiteral("identity_assignments_by_clip")] = assignmentsByClip;
        if (transcriptEngine.saveIdentityArtifact(m_transcriptSession.transcriptPath(), identityRoot)) {
            addArtefact(debugRun, transcriptEngine.identityArtifactPath(m_transcriptSession.transcriptPath()));
        }
        persistSpeakerFlowSnapshot(QJsonObject(), humanPayload, resolvedPayload);
    }

    m_speakersTableRefreshSignature.clear();
    m_faceStreamPanelRefreshSignature.clear();

    if (assignmentsBySpeaker.isEmpty()) {
        setStageStatus(debugRun, QStringLiteral("stage_6_assignment"), QStringLiteral("warn"),
                       QStringLiteral("No assignments accepted."));
        persistIndex(debugRun);
        return;
    }

    const QString assignmentTablePath = QDir(debugRun.runDir).absoluteFilePath(
        QStringLiteral("%1_assignment_table.json").arg(debugRun.videoStem));
    const QString assignmentDecisionsPath = QDir(debugRun.runDir).absoluteFilePath(
        QStringLiteral("%1_assignment_decisions.json").arg(debugRun.videoStem));
    bool unusedCreateNewRun = false;
    if (!ensureWritableArtefacts(
            QStringLiteral("stage_6_assignment"),
            QStringList{assignmentTablePath, assignmentDecisionsPath},
            false,
            &unusedCreateNewRun)) {
        setStageStatus(debugRun, QStringLiteral("stage_6_assignment"), QStringLiteral("skipped"),
                       QStringLiteral("Canceled due to overwrite prompt."));
        persistIndex(debugRun);
        return;
    }
    {
        QJsonObject assignmentTableRoot;
        assignmentTableRoot[QStringLiteral("run_id")] = debugRun.runId;
        assignmentTableRoot[QStringLiteral("rows")] = assignmentTableRows;
        QFile f(assignmentTablePath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(assignmentTableRoot).toJson(QJsonDocument::Indented));
            f.close();
        }
    }
    {
        QJsonObject decisionsRoot;
        decisionsRoot[QStringLiteral("run_id")] = debugRun.runId;
        QJsonArray decisions;
        for (auto it = assignmentsBySpeaker.constBegin(); it != assignmentsBySpeaker.constEnd(); ++it) {
            QJsonObject d;
            d[QStringLiteral("speaker_id")] = it.key();
            QJsonArray assigned;
            for (const facefind::Candidate& c : it.value()) {
                QJsonObject row;
                row[QStringLiteral("frame")] = static_cast<qint64>(c.frame);
                row[QStringLiteral("x")] = c.x;
                row[QStringLiteral("y")] = c.y;
                row[QStringLiteral("box")] = c.box;
                row[QStringLiteral("score")] = c.score;
                row[QStringLiteral("track_id")] = c.trackId;
                assigned.push_back(row);
            }
            d[QStringLiteral("candidates")] = assigned;
            decisions.push_back(d);
        }
        decisionsRoot[QStringLiteral("assignments")] = decisions;
        QFile f(assignmentDecisionsPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(decisionsRoot).toJson(QJsonDocument::Indented));
            f.close();
        }
    }
    addArtefact(debugRun, assignmentTablePath);
    addArtefact(debugRun, assignmentDecisionsPath);
    setStageStatus(debugRun, QStringLiteral("stage_6_assignment"), QStringLiteral("ok"),
                   QStringLiteral("Cluster assignment completed."));
    persistIndex(debugRun);

    emit transcriptDocumentChanged();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    persistIndex(debugRun);
    refresh();
}
