#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace editor {

struct PlaybackStageMetric {
    qint64 attempts = 0;
    qint64 successes = 0;
    qint64 sourceUnavailable = 0;
    QString lastState;
    QString lastReason;
    qint64 lastUpdatedMs = 0;
};

inline void accumulatePlaybackStageMetric(PlaybackStageMetric* metric,
                                          qint64 attempts,
                                          qint64 successes,
                                          qint64 sourceUnavailable,
                                          const QString& state,
                                          const QString& reason = QString())
{
    if (!metric) {
        return;
    }
    if (attempts <= 0 && successes <= 0 && sourceUnavailable <= 0 &&
        state.trimmed().isEmpty() && reason.trimmed().isEmpty()) {
        return;
    }
    metric->attempts += attempts;
    metric->successes += qMax<qint64>(0, successes);
    metric->sourceUnavailable += qMax<qint64>(0, sourceUnavailable);
    metric->lastState = state;
    metric->lastReason = reason;
    metric->lastUpdatedMs = QDateTime::currentMSecsSinceEpoch();
}

inline QJsonObject playbackStageMetricToJson(const PlaybackStageMetric& metric,
                                             const QString& owner = QString())
{
    QJsonObject json{
        {QStringLiteral("attempts"), metric.attempts},
        {QStringLiteral("successes"), metric.successes},
        {QStringLiteral("source_unavailable"), metric.sourceUnavailable},
        {QStringLiteral("last_state"), metric.lastState},
        {QStringLiteral("last_reason"), metric.lastReason},
        {QStringLiteral("last_updated_ms"), metric.lastUpdatedMs}
    };
    if (!owner.trimmed().isEmpty()) {
        json.insert(QStringLiteral("owner"), owner.trimmed());
    }
    return json;
}

inline void mergePlaybackStageMetricObjects(QJsonObject* target, const QJsonObject& source)
{
    if (!target) {
        return;
    }
    for (auto it = source.constBegin(); it != source.constEnd(); ++it) {
        target->insert(it.key(), it.value());
    }
}

} // namespace editor
