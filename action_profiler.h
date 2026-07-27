#pragma once

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVector>

#include <cstdint>

class UiActionProfiler
{
public:
    struct Context
    {
        Context() = default;
        bool playbackActive = false;
        qint64 frame = -1;
        qint64 sample = -1;
        qint64 stateRevision = -1;
        QString selectedTab;
        QString selectedClipId;
    };

    class Scope
    {
    public:
        Scope(UiActionProfiler* profiler,
              QString actionId,
              Context context,
              QJsonObject details = {});
        ~Scope();

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&& other) noexcept;
        Scope& operator=(Scope&& other) noexcept;

        void addDetail(const QString& key, const QJsonValue& value);

    private:
        UiActionProfiler* m_profiler = nullptr;
        QString m_actionId;
        Context m_context;
        QJsonObject m_details;
        QElapsedTimer m_timer;
        bool m_active = false;
    };

    [[nodiscard]] Scope scope(QString actionId,
                              Context context,
                              QJsonObject details = {});

    void record(const QString& actionId,
                qint64 elapsedMs,
                const Context& context,
                const QJsonObject& details = {});
    [[nodiscard]] QJsonObject snapshot() const;
    void reset();

private:
    struct Record
    {
        qint64 sequence = 0;
        qint64 utcMs = 0;
        QString actionId;
        qint64 elapsedMs = 0;
        Context context;
        QJsonObject details;
    };

    struct Summary
    {
        QString actionId;
        qint64 count = 0;
        qint64 slowCount = 0;
        qint64 severeCount = 0;
        qint64 lastMs = 0;
        qint64 maxMs = 0;
        qint64 totalMs = 0;
    };

    static QJsonObject contextToJson(const Context& context);
    static QJsonObject recordToJson(const Record& record);
    static QJsonObject summaryToJson(const Summary& summary);

    QVector<Record> m_records;
    QVector<Summary> m_summaries;
    qint64 m_sequence = 0;
    qint64 m_totalCount = 0;
    qint64 m_slowCount = 0;
    qint64 m_severeCount = 0;
    qint64 m_lastElapsedMs = 0;
    qint64 m_maxElapsedMs = 0;
};
