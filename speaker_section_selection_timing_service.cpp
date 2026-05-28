#include "speaker_section_selection_timing_service.h"

#include <QDateTime>

void SpeakerSectionSelectionTimingService::begin(int row)
{
    m_timer.restart();
    m_lastMarkMs = 0;
    m_active = true;
    m_current = QJsonObject{
        {QStringLiteral("row"), row},
        {QStringLiteral("started_at_ms"), QDateTime::currentMSecsSinceEpoch()},
        {QStringLiteral("completed"), false}};
    m_currentSteps = QJsonObject{};
}

void SpeakerSectionSelectionTimingService::setSectionContext(const QString& speakerId, qint64 timelineFrame)
{
    if (!m_active) {
        return;
    }
    m_current[QStringLiteral("speaker_id")] = speakerId;
    m_current[QStringLiteral("timeline_frame")] = timelineFrame;
}

void SpeakerSectionSelectionTimingService::markStep(const QString& key)
{
    if (!m_active || key.trimmed().isEmpty()) {
        return;
    }
    const qint64 now = m_timer.elapsed();
    m_currentSteps[key] = now - m_lastMarkMs;
    m_lastMarkMs = now;
}

void SpeakerSectionSelectionTimingService::finish()
{
    if (!m_active) {
        return;
    }
    const qint64 totalMs = m_timer.elapsed();
    m_current[QStringLiteral("completed")] = true;
    m_current[QStringLiteral("total_ms")] = totalMs;
    m_current[QStringLiteral("steps")] = m_currentSteps;
    m_last = m_current;
    if (totalMs > m_maxTotal.value(QStringLiteral("total_ms")).toInteger(0)) {
        m_maxTotal = m_current;
    }
    m_active = false;
}

void SpeakerSectionSelectionTimingService::finishSkipped(const QString& reason)
{
    if (!m_active) {
        return;
    }
    m_current[QStringLiteral("completed")] = false;
    m_current[QStringLiteral("skipped_reason")] = reason;
    m_current[QStringLiteral("total_ms")] = m_timer.elapsed();
    m_current[QStringLiteral("steps")] = m_currentSteps;
    m_last = m_current;
    m_active = false;
}

QJsonObject SpeakerSectionSelectionTimingService::profileSnapshot() const
{
    return QJsonObject{
        {QStringLiteral("last"), m_last},
        {QStringLiteral("max_total"), m_maxTotal}};
}
