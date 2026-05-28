#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QString>

class SpeakerSectionSelectionTimingService {
public:
    void begin(int row);
    void setSectionContext(const QString& speakerId, qint64 timelineFrame);
    void markStep(const QString& key);
    void finish();
    void finishSkipped(const QString& reason);

    QJsonObject profileSnapshot() const;

private:
    QElapsedTimer m_timer;
    qint64 m_lastMarkMs = 0;
    bool m_active = false;

    QJsonObject m_current;
    QJsonObject m_currentSteps;
    QJsonObject m_last;
    QJsonObject m_maxTotal;
};
