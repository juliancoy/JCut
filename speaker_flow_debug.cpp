#include "speaker_flow_debug.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

namespace speaker_flow_debug {

QString sanitizeToken(const QString& raw)
{
    QString token = raw.trimmed();
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
}

QString deriveProjectRootFromTranscriptPath(const QString& transcriptPath)
{
    const QRegularExpression re(QStringLiteral("(.*/projects/[^/]+)/.*"));
    const QRegularExpressionMatch m = re.match(transcriptPath);
    if (m.hasMatch()) {
        return m.captured(1);
    }
    return QDir::currentPath();
}

QString makeRunId()
{
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")) +
           QStringLiteral("-") +
           QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

QString latestRunId(const QString& clipDebugRoot)
{
    const QDir dir(clipDebugRoot);
    const QStringList runs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    return runs.isEmpty() ? QString() : runs.constFirst();
}

RunContext openLatestOrCreateRun(const QString& transcriptPath,
                                 const QString& clipId,
                                 const QString& videoStem)
{
    RunContext ctx;
    ctx.clipToken = sanitizeToken(clipId.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : clipId);
    ctx.videoStem = sanitizeToken(videoStem);
    ctx.projectRoot = deriveProjectRootFromTranscriptPath(transcriptPath);
    ctx.clipDebugRoot = QDir(ctx.projectRoot).absoluteFilePath(
        QStringLiteral("debug/speaker_flow/%1").arg(ctx.clipToken));
    QDir().mkpath(ctx.clipDebugRoot);
    ctx.runId = latestRunId(ctx.clipDebugRoot);
    if (ctx.runId.isEmpty()) {
        ctx.runId = makeRunId();
    }
    ctx.runDir = QDir(ctx.clipDebugRoot).absoluteFilePath(ctx.runId);
    QDir().mkpath(ctx.runDir);
    return ctx;
}

RunContext createNewRunFrom(const RunContext& base)
{
    RunContext ctx = base;
    ctx.runId = makeRunId();
    ctx.runDir = QDir(ctx.clipDebugRoot).absoluteFilePath(ctx.runId);
    QDir().mkpath(ctx.runDir);
    return ctx;
}

OverwriteAction promptOverwrite(QWidget* parent,
                                const QString& runDir,
                                const QString& stage,
                                const QStringList& files,
                                bool allowCreateNewRun,
                                QStringList* existingFilesOut)
{
    QStringList existing;
    for (const QString& file : files) {
        if (QFileInfo::exists(file)) {
            existing.push_back(file);
        }
    }
    if (existingFilesOut) {
        *existingFilesOut = existing;
    }
    if (existing.isEmpty()) {
        return OverwriteAction::Overwrite;
    }

    QMessageBox dialog(parent);
    dialog.setIcon(QMessageBox::Warning);
    dialog.setWindowTitle(QStringLiteral("Overwrite Debug Artefacts"));
    dialog.setText(
        QStringLiteral("Stage \"%1\" will overwrite %2 artefact(s).")
            .arg(stage)
            .arg(existing.size()));

    QString detail;
    const int previewCount = qMin(10, existing.size());
    for (int i = 0; i < previewCount; ++i) {
        detail += QStringLiteral("- %1\n").arg(QDir(runDir).relativeFilePath(existing.at(i)));
    }
    if (existing.size() > previewCount) {
        detail += QStringLiteral("- ... and %1 more\n").arg(existing.size() - previewCount);
    }
    dialog.setInformativeText(detail.trimmed());

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
        return OverwriteAction::Cancel;
    }
    if (newRunButton && dialog.clickedButton() == newRunButton) {
        return OverwriteAction::CreateNewRun;
    }
    return dialog.clickedButton() == overwriteButton
        ? OverwriteAction::Overwrite
        : OverwriteAction::Cancel;
}

void recordOverwriteDecision(const QString& decisionPath,
                             const QString& runDir,
                             const QString& stage,
                             const QStringList& files,
                             bool approved)
{
    QJsonObject root;
    QFile f(decisionPath);
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
        fileArray.push_back(QDir(runDir).relativeFilePath(file));
    }
    decision[QStringLiteral("files")] = fileArray;
    decisions.push_back(decision);
    root[QStringLiteral("decisions")] = decisions;
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        f.close();
    }
}

void persistIndex(const QString& indexPath,
                  const QString& runId,
                  const QString& clipId,
                  const QString& videoFilename,
                  const QString& transcriptPath,
                  const QString& stage,
                  const QString& status,
                  const QString& message,
                  const QStringList& artefacts)
{
    QJsonObject root;
    QFile f(indexPath);
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            root = doc.object();
        }
        f.close();
    }
    root[QStringLiteral("schema_version")] = QStringLiteral("1.0");
    root[QStringLiteral("run_id")] = runId;
    root[QStringLiteral("clip_id")] = clipId;
    root[QStringLiteral("video_filename")] = videoFilename;
    root[QStringLiteral("transcript_path")] = transcriptPath;

    QJsonObject stageStatus = root.value(QStringLiteral("stage_status")).toObject();
    QJsonObject stageObj;
    stageObj[QStringLiteral("status")] = status;
    stageObj[QStringLiteral("message")] = message;
    stageObj[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    stageStatus[stage] = stageObj;
    root[QStringLiteral("stage_status")] = stageStatus;

    const QString runDir = QFileInfo(indexPath).absolutePath();
    QJsonArray artefactArray = root.value(QStringLiteral("artefacts")).toArray();
    QSet<QString> dedupe;
    for (const QJsonValue& v : artefactArray) {
        dedupe.insert(v.toString());
    }
    for (const QString& absolutePath : artefacts) {
        const QString rel = QDir(runDir).relativeFilePath(absolutePath);
        if (!dedupe.contains(rel)) {
            artefactArray.push_back(rel);
            dedupe.insert(rel);
        }
    }
    root[QStringLiteral("artefacts")] = artefactArray;
    root[QStringLiteral("completed_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        f.close();
    }
}

} // namespace speaker_flow_debug

