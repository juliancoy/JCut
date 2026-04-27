#pragma once

#include <QString>
#include <QStringList>

class QWidget;

namespace speaker_flow_debug {

struct RunContext {
    QString clipToken;
    QString videoStem;
    QString projectRoot;
    QString clipDebugRoot;
    QString runId;
    QString runDir;
};

enum class OverwriteAction {
    Cancel,
    Overwrite,
    CreateNewRun
};

QString sanitizeToken(const QString& raw);
QString deriveProjectRootFromTranscriptPath(const QString& transcriptPath);
QString makeRunId();
QString latestRunId(const QString& clipDebugRoot);
RunContext openLatestOrCreateRun(const QString& transcriptPath,
                                 const QString& clipId,
                                 const QString& videoStem);
RunContext createNewRunFrom(const RunContext& base);

OverwriteAction promptOverwrite(QWidget* parent,
                                const QString& runDir,
                                const QString& stage,
                                const QStringList& files,
                                bool allowCreateNewRun,
                                QStringList* existingFilesOut = nullptr);

void recordOverwriteDecision(const QString& decisionPath,
                             const QString& runDir,
                             const QString& stage,
                             const QStringList& files,
                             bool approved);

void persistIndex(const QString& indexPath,
                  const QString& runId,
                  const QString& clipId,
                  const QString& videoFilename,
                  const QString& transcriptPath,
                  const QString& stage,
                  const QString& status,
                  const QString& message,
                  const QStringList& artefacts);

} // namespace speaker_flow_debug

