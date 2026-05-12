#include "speakers_tab.h"
#include "speakers_tab_internal.h"

#include "facestream_assignment_services.h"
#include "facestream_runtime.h"
#include "facefind_window.h"
#include "identity_resolution.h"
#include "transcript_engine.h"

#include <QApplication>
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
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
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

bool confirmClusteringPreflight(QWidget* parent,
                                const QVector<facefind::Candidate>& candidates,
                                const QString& modelLabel,
                                const QString& summary)
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

    std::mt19937 rng(std::random_device{}());
    constexpr int kSampleTileCount = 8;
    QVector<int> currentSampleIndexes;

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("FaceStream Clustering Preflight"));
    dialog.setModal(true);
    dialog.resize(760, 520);
    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* intro = new QLabel(
        QStringLiteral("Review a random representative sample of cropped FaceStreams before identity clustering runs."),
        &dialog);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* details = new QLabel(
        QStringLiteral("%1\nModel: %2\nSample: %3 of %4 crop(s) (high-confidence biased)")
            .arg(summary.trimmed().isEmpty() ? QStringLiteral("Ready to cluster FaceStream tracks.") : summary.trimmed())
            .arg(modelLabel)
            .arg(qMin(kSampleTileCount, validIndexes.size()))
            .arg(validIndexes.size()),
        &dialog);
    details->setWordWrap(true);
    details->setStyleSheet(QStringLiteral(
        "QLabel { background: #142234; border: 1px solid #314459; border-radius: 8px; color: #d8e6f5; padding: 8px; }"));
    root->addWidget(details);

    auto* gridWidget = new QWidget(&dialog);
    auto* grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(8);

    const auto rebuildSample = [&]() {
        QVector<int> pool = validIndexes;
        std::sort(pool.begin(), pool.end(), [&](int lhs, int rhs) {
            return candidates.at(lhs).score > candidates.at(rhs).score;
        });

        const int preferredPoolSize = qMax(1, static_cast<int>(std::ceil(static_cast<double>(pool.size()) * 0.60)));
        pool.resize(preferredPoolSize);
        std::shuffle(pool.begin(), pool.end(), rng);
        pool.resize(qMin(kSampleTileCount, pool.size()));
        currentSampleIndexes = pool;
    };

    const auto redrawTiles = [&]() {
        while (QLayoutItem* item = grid->takeAt(0)) {
            if (QWidget* widget = item->widget()) {
                widget->deleteLater();
            }
            delete item;
        }

        for (int sampleIndex = 0; sampleIndex < currentSampleIndexes.size(); ++sampleIndex) {
            const facefind::Candidate& c = candidates.at(currentSampleIndexes.at(sampleIndex));
            auto* tile = new QWidget(gridWidget);
            auto* tileLayout = new QVBoxLayout(tile);
            tileLayout->setContentsMargins(6, 6, 6, 6);
            tileLayout->setSpacing(4);
            tile->setStyleSheet(QStringLiteral(
                "QWidget { background: #101c2b; border: 1px solid #2f4358; border-radius: 8px; }"));

            auto* imageLabel = new QLabel(tile);
            imageLabel->setMinimumSize(140, 120);
            imageLabel->setAlignment(Qt::AlignCenter);
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
            caption->setStyleSheet(
                QStringLiteral("QLabel { color: #b8c7d8; border: none; background: transparent; font-size: 11px; }"));
            tileLayout->addWidget(caption);

            grid->addWidget(tile, sampleIndex / 4, sampleIndex % 4);
        }
    };

    rebuildSample();
    redrawTiles();
    root->addWidget(gridWidget, 1);

    auto* actions = new QHBoxLayout;
    auto* randomizeButton = new QPushButton(QStringLiteral("Randomize"), &dialog);
    actions->addWidget(randomizeButton);
    actions->addStretch(1);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
    auto* proceedButton = new QPushButton(QStringLiteral("Proceed With Clustering"), &dialog);
    actions->addWidget(cancelButton);
    actions->addWidget(proceedButton);
    root->addLayout(actions);
    QObject::connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(proceedButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(randomizeButton, &QPushButton::clicked, &dialog, [&]() {
        rebuildSample();
        redrawTiles();
    });
    return dialog.exec() == QDialog::Accepted;
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
    dialog.setWindowTitle(QStringLiteral("Assign FaceStreams"));
    dialog.setText(QStringLiteral("Identity clustering requires ArcFace NCNN before reviewing this many FaceStream tracks."));
    QString details = QStringLiteral("Tracks: %1\n\nExpected files:\n%2\n%3")
        .arg(trackCount)
        .arg(paramPath, binPath);
    if (!reason.trimmed().isEmpty()) {
        details += QStringLiteral("\n\nReason:\n%1").arg(reason.trimmed());
    }
    dialog.setInformativeText(
        QStringLiteral("Download the model files now, then run Assign FaceStreams again.\n\n"
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
                                 QStringLiteral("Assign FaceStreams"),
                                 QStringLiteral("ArcFace NCNN model files were downloaded.\n\nRun Assign FaceStreams again."));
        return;
    }
    QMessageBox::warning(parent,
                         QStringLiteral("Assign FaceStreams"),
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

void SpeakersTab::onSpeakerPrecropFacesClicked()
{
    if (!activeCutMutable() || !m_loadedTranscriptDoc.isObject() || m_loadedTranscriptPath.isEmpty()) {
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
                             QStringLiteral("Assign FaceStreams"),
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

    const QString projectId = sanitizedToken(deriveProjectId(m_loadedTranscriptPath));
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
        const QRegularExpressionMatch m = re.match(m_loadedTranscriptPath);
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
        root[QStringLiteral("transcript_path")] = m_loadedTranscriptPath;
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
    if (transcriptEngine.loadFacestreamArtifact(m_loadedTranscriptPath, &artifactRoot)) {
        const QJsonObject byClip = artifactRoot.value(QStringLiteral("continuity_facestreams_by_clip")).toObject();
        const QJsonObject continuityRoot = byClip.value(clip->id.trimmed()).toObject();
        streams = jcut::facestream::continuityStreamsForRoot(
            continuityRoot,
            m_loadedTranscriptDoc.object());
        importedArtifactDir = continuityRoot.value(QStringLiteral("imported_from_artifact_dir")).toString();
        facestreamPath = continuityRoot.value(QStringLiteral("facestream_part")).toString();
        if (facestreamPath.isEmpty()) {
            facestreamPath = continuityRoot.value(QStringLiteral("facestream_bin")).toString();
        }
    }
    if (streams.isEmpty()) {
        const QJsonObject transcriptRoot = m_loadedTranscriptDoc.object();
        const QJsonObject speakerFlow = transcriptRoot.value(QStringLiteral("speaker_flow")).toObject();
        const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
        const QJsonObject clipFlow = clipsRoot.value(clip->id.trimmed()).toObject();
        const QJsonObject continuityRoot = clipFlow.value(QStringLiteral("continuity_facestreams")).toObject();
        streams = continuityRoot.value(QStringLiteral("streams")).toArray();
    }
    if (streams.isEmpty()) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Assign FaceStreams"),
                                 QStringLiteral("No generated FaceStream tracks were found for this clip. Generate FaceStream first."));
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
    if (streams.size() > maxManualSingletonReviewRows &&
        (!QFileInfo::exists(arcfaceParamPath) || !QFileInfo::exists(arcfaceBinPath))) {
        showArcFaceModelRequiredDialog(nullptr,
                                       streams.size(),
                                       QString(),
                                       arcfaceParamPath,
                                       arcfaceBinPath);
        return;
    }

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
    progressDialog.setWindowTitle(QStringLiteral("Assign FaceStreams"));
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
        QMessageBox::warning(nullptr, QStringLiteral("Assign FaceStreams"), cropResult.errorMessage);
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
                   QStringLiteral("Extracted one representative crop for each generated FaceStream track."));
    persistIndex(debugRun);
    if (candidates.isEmpty()) {
        progressDialog.close();
        setStageStatus(debugRun, kStageCropReview, QStringLiteral("warn"),
                       QStringLiteral("No candidates produced."));
        persistIndex(debugRun);
        QMessageBox::information(nullptr,
                                 QStringLiteral("Assign FaceStreams"),
                                 QStringLiteral("No usable FaceStream comparison crops could be extracted."));
        return;
    }

    QJsonObject root = m_loadedTranscriptDoc.object();
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
                                 QStringLiteral("Assign FaceStreams"),
                                 QStringLiteral("No transcript speaker IDs were available to assign FaceStream tracks."));
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
        const QJsonObject transcriptRoot = m_loadedTranscriptDoc.object();
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
        QJsonObject transcriptRoot = m_loadedTranscriptDoc.object();
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

    const QVector<facefind::Candidate> trackCandidates = candidates;
    progressDialog.close();
    if (!confirmClusteringPreflight(
            nullptr,
            trackCandidates,
            QStringLiteral("ArcFace MobileFaceNet NCNN"),
            QStringLiteral("%1 FaceStream representative crop(s) extracted from %2 generated track(s).")
                .arg(trackCandidates.size())
                .arg(streams.size()))) {
        setStageStatus(debugRun, kStageClustering, kStatusSkipped, kMsgCanceledClusteringPreflight);
        persistIndex(debugRun);
        return;
    }
    progressDialog.setLabelText(QStringLiteral("Preparing FaceStream identity clustering..."));
    progressDialog.setValue(qMin(progressDialog.maximum(), streams.size()));
    progressDialog.show();
    QApplication::processEvents(QEventLoop::AllEvents, 50);

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

        const QString identitySidecarPath = transcriptEngine.identityArtifactPath(m_loadedTranscriptPath);
        QJsonObject identityRoot;
        transcriptEngine.loadIdentityArtifact(m_loadedTranscriptPath, &identityRoot);
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
        clustersRoot[QStringLiteral("track_count")] = trackCandidates.size();
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
        if (transcriptEngine.saveIdentityArtifact(m_loadedTranscriptPath, identityRoot)) {
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
                           ? QStringLiteral("Auto-cluster ran: %1 tracks -> %2 identity clusters; %3 auto-merge pair(s), %4 review pair(s).")
                                 .arg(trackCandidates.size())
                                 .arg(clusterCandidates.size())
                                 .arg(clusterResult.autoClusterPairCount)
                                 .arg(clusterResult.reviewPairCount)
                           : QStringLiteral("Auto-cluster fallback: ArcFace unavailable; %1 tracks -> %2 singleton cluster(s).")
                                 .arg(trackCandidates.size())
                                 .arg(clusterCandidates.size()));
        persistIndex(debugRun);
        updateProgress(streams.size() + 3, QStringLiteral("FaceStream identity preparation complete."));
        clusterSummaryText = clusterResult.embeddingReady
            ? QStringLiteral("Auto-cluster ran: %1 FaceStream track(s) -> %2 identity cluster(s) using ArcFace NCNN. Auto-merge pairs: %3. Review pairs: %4. Thresholds: auto >= %5, review >= %6.")
                  .arg(trackCandidates.size())
                  .arg(clusterCandidates.size())
                  .arg(clusterResult.autoClusterPairCount)
                  .arg(clusterResult.reviewPairCount)
                  .arg(clusterResult.autoClusterThreshold, 0, 'f', 2)
                  .arg(clusterResult.reviewThreshold, 0, 'f', 2)
            : QStringLiteral("Auto-cluster fallback: ArcFace NCNN unavailable (%1). %2 FaceStream track(s) are shown as %3 singleton cluster(s).")
                  .arg(clusterResult.embeddingError.trimmed().isEmpty() ? QStringLiteral("unknown error")
                                                                        : clusterResult.embeddingError)
                  .arg(trackCandidates.size())
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
        QJsonArray overrides;
        QJsonArray auditLog;
        QJsonArray resolvedMap;
        QHash<int, facefind::Candidate> candidateByTrackId;
        for (const facefind::Candidate& c : std::as_const(trackCandidates)) {
            if (c.trackId < 0) {
                continue;
            }
            const auto existing = candidateByTrackId.constFind(c.trackId);
            if (existing == candidateByTrackId.constEnd() || c.score > existing->score) {
                candidateByTrackId.insert(c.trackId, c);
            }
        }

        for (const QJsonValue& value : assignmentTableRows) {
            const QJsonObject row = value.toObject();
            const QString decision = row.value(QStringLiteral("decision")).toString();
            if (decision != QStringLiteral("accepted")) {
                continue;
            }
            const QString resolvedSpeaker = row.value(QStringLiteral("resolved_speaker_id")).toString().trimmed();
            if (resolvedSpeaker.isEmpty()) {
                continue;
            }
            const QString manualOverride = row.value(QStringLiteral("manual_override")).toString().trimmed();
            QJsonArray trackIds = row.value(QStringLiteral("track_ids")).toArray();
            if (trackIds.isEmpty()) {
                const int fallbackTrackId = row.value(QStringLiteral("track_id")).toInt(-1);
                if (fallbackTrackId >= 0) {
                    trackIds.push_back(fallbackTrackId);
                }
            }
            for (const QJsonValue& trackValue : trackIds) {
                const int trackId = trackValue.toInt(-1);
                if (trackId < 0) {
                    continue;
                }
                QJsonObject overrideRow;
                overrideRow[QStringLiteral("track_id")] = trackId;
                overrideRow[QStringLiteral("cluster_id")] = row.value(QStringLiteral("cluster_id")).toString();
                overrideRow[QStringLiteral("identity_id")] = resolvedSpeaker;
                overrideRow[QStringLiteral("source")] =
                    manualOverride.isEmpty() ? QStringLiteral("auto_selected") : QStringLiteral("human_override");
                overrideRow[QStringLiteral("manual_override")] = !manualOverride.isEmpty();
                overrides.push_back(overrideRow);

                QJsonObject auditRow;
                auditRow[QStringLiteral("timestamp_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
                auditRow[QStringLiteral("action")] = QStringLiteral("track_identity_set");
                auditRow[QStringLiteral("track_id")] = trackId;
                auditRow[QStringLiteral("cluster_id")] = row.value(QStringLiteral("cluster_id")).toString();
                auditRow[QStringLiteral("identity_id")] = resolvedSpeaker;
                auditRow[QStringLiteral("source")] = overrideRow.value(QStringLiteral("source")).toString();
                auditLog.push_back(auditRow);

                QJsonObject resolvedRow;
                resolvedRow[QStringLiteral("track_id")] = trackId;
                resolvedRow[QStringLiteral("identity_id")] = resolvedSpeaker;
                resolvedRow[QStringLiteral("cluster_id")] = row.value(QStringLiteral("cluster_id")).toString();
                resolvedRow[QStringLiteral("resolution_source")] = overrideRow.value(QStringLiteral("source")).toString();
                resolvedMap.push_back(resolvedRow);

                const auto trackIt = candidateByTrackId.constFind(trackId);
                if (trackIt != candidateByTrackId.constEnd()) {
                    assignmentsBySpeaker[resolvedSpeaker].push_back(trackIt.value());
                }
            }
        }
        QJsonObject humanPayload;
        humanPayload[QStringLiteral("run_id")] = debugRun.runId;
        humanPayload[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        humanPayload[QStringLiteral("assignment_table_rows")] = assignmentTableRows;
        humanPayload[QStringLiteral("track_identity_overrides")] = overrides;
        humanPayload[QStringLiteral("audit_log")] = auditLog;

        QJsonObject resolvedPayload;
        resolvedPayload[QStringLiteral("run_id")] = debugRun.runId;
        resolvedPayload[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        resolvedPayload[QStringLiteral("track_identity_map")] = resolvedMap;
        QJsonObject identityRoot;
        transcriptEngine.loadIdentityArtifact(m_loadedTranscriptPath, &identityRoot);
        QJsonObject assignmentsByClip = identityRoot.value(QStringLiteral("identity_assignments_by_clip")).toObject();
        QJsonObject assignmentRoot;
        assignmentRoot[QStringLiteral("run_id")] = debugRun.runId;
        assignmentRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        assignmentRoot[QStringLiteral("assignment_table_rows")] = assignmentTableRows;
        assignmentRoot[QStringLiteral("track_identity_overrides")] = overrides;
        assignmentRoot[QStringLiteral("track_identity_map")] = resolvedMap;
        assignmentsByClip[clipFlowId] = assignmentRoot;
        identityRoot[QStringLiteral("schema")] = QStringLiteral("jcut_identity_v1");
        identityRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        identityRoot[QStringLiteral("identity_assignments_by_clip")] = assignmentsByClip;
        if (transcriptEngine.saveIdentityArtifact(m_loadedTranscriptPath, identityRoot)) {
            addArtefact(debugRun, transcriptEngine.identityArtifactPath(m_loadedTranscriptPath));
        }
        persistSpeakerFlowSnapshot(QJsonObject(), humanPayload, resolvedPayload);
    }

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
