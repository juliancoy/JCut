#include "action_profiler.h"

#include <QDateTime>

#include <algorithm>
#include <utility>

namespace {
constexpr qsizetype kMaxUiActionRecords = 1024;
constexpr qint64 kSlowUiActionMs = 30;
constexpr qint64 kPlaybackSlowUiActionMs = 16;
constexpr qint64 kSevereUiActionMs = 100;
constexpr int kMaxLastRecordsInSnapshot = 80;
constexpr int kMaxSlowRecordsInSnapshot = 40;
constexpr int kMaxSummaryRowsInSnapshot = 40;
}

UiActionProfiler::Scope::Scope(UiActionProfiler* profiler,
                               QString actionId,
                               Context context,
                               QJsonObject details)
    : m_profiler(profiler)
    , m_actionId(std::move(actionId))
    , m_context(std::move(context))
    , m_details(std::move(details))
    , m_active(m_profiler && !m_actionId.isEmpty())
{
    if (m_active) {
        m_timer.start();
    }
}

UiActionProfiler::Scope::~Scope()
{
    if (m_active && m_profiler) {
        m_profiler->record(m_actionId, m_timer.elapsed(), m_context, m_details);
    }
}

UiActionProfiler::Scope::Scope(Scope&& other) noexcept
    : m_profiler(other.m_profiler)
    , m_actionId(std::move(other.m_actionId))
    , m_context(std::move(other.m_context))
    , m_details(std::move(other.m_details))
    , m_timer(other.m_timer)
    , m_active(other.m_active)
{
    other.m_profiler = nullptr;
    other.m_active = false;
}

UiActionProfiler::Scope& UiActionProfiler::Scope::operator=(Scope&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    if (m_active && m_profiler) {
        m_profiler->record(m_actionId, m_timer.elapsed(), m_context, m_details);
    }
    m_profiler = other.m_profiler;
    m_actionId = std::move(other.m_actionId);
    m_context = std::move(other.m_context);
    m_details = std::move(other.m_details);
    m_timer = other.m_timer;
    m_active = other.m_active;
    other.m_profiler = nullptr;
    other.m_active = false;
    return *this;
}

void UiActionProfiler::Scope::addDetail(const QString& key, const QJsonValue& value)
{
    if (!key.isEmpty()) {
        m_details.insert(key, value);
    }
}

UiActionProfiler::Scope UiActionProfiler::scope(QString actionId,
                                                Context context,
                                                QJsonObject details)
{
    return Scope(this, std::move(actionId), std::move(context), std::move(details));
}

void UiActionProfiler::record(const QString& actionId,
                              qint64 elapsedMs,
                              const Context& context,
                              const QJsonObject& details)
{
    if (actionId.isEmpty()) {
        return;
    }

    const qint64 slowThresholdMs =
        context.playbackActive ? kPlaybackSlowUiActionMs : kSlowUiActionMs;
    const bool slow = elapsedMs >= slowThresholdMs;
    const bool severe = elapsedMs >= kSevereUiActionMs;

    Record record;
    record.sequence = ++m_sequence;
    record.utcMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    record.actionId = actionId;
    record.elapsedMs = elapsedMs;
    record.context = context;
    record.details = details;
    m_records.push_back(std::move(record));
    while (m_records.size() > kMaxUiActionRecords) {
        m_records.removeFirst();
    }

    ++m_totalCount;
    m_lastElapsedMs = elapsedMs;
    m_maxElapsedMs = std::max(m_maxElapsedMs, elapsedMs);
    if (slow) {
        ++m_slowCount;
    }
    if (severe) {
        ++m_severeCount;
    }

    auto it = std::find_if(m_summaries.begin(),
                           m_summaries.end(),
                           [&actionId](const Summary& summary) {
                               return summary.actionId == actionId;
                           });
    if (it == m_summaries.end()) {
        Summary summary;
        summary.actionId = actionId;
        m_summaries.push_back(std::move(summary));
        it = m_summaries.end() - 1;
    }
    ++it->count;
    it->lastMs = elapsedMs;
    it->maxMs = std::max(it->maxMs, elapsedMs);
    it->totalMs += elapsedMs;
    if (slow) {
        ++it->slowCount;
    }
    if (severe) {
        ++it->severeCount;
    }
}

QJsonObject UiActionProfiler::contextToJson(const Context& context)
{
    QJsonObject object{
        {QStringLiteral("playback_active"), context.playbackActive},
        {QStringLiteral("frame"), context.frame},
        {QStringLiteral("sample"), context.sample},
        {QStringLiteral("state_revision"), context.stateRevision}
    };
    if (!context.selectedTab.isEmpty()) {
        object.insert(QStringLiteral("selected_tab"), context.selectedTab);
    }
    if (!context.selectedClipId.isEmpty()) {
        object.insert(QStringLiteral("selected_clip_id"), context.selectedClipId);
    }
    return object;
}

QJsonObject UiActionProfiler::recordToJson(const Record& record)
{
    QJsonObject object{
        {QStringLiteral("sequence"), record.sequence},
        {QStringLiteral("utc_ms"), record.utcMs},
        {QStringLiteral("action_id"), record.actionId},
        {QStringLiteral("elapsed_ms"), record.elapsedMs},
        {QStringLiteral("context"), contextToJson(record.context)}
    };
    if (!record.details.isEmpty()) {
        object.insert(QStringLiteral("details"), record.details);
    }
    return object;
}

QJsonObject UiActionProfiler::summaryToJson(const Summary& summary)
{
    return QJsonObject{
        {QStringLiteral("action_id"), summary.actionId},
        {QStringLiteral("count"), summary.count},
        {QStringLiteral("slow_count"), summary.slowCount},
        {QStringLiteral("severe_count"), summary.severeCount},
        {QStringLiteral("last_ms"), summary.lastMs},
        {QStringLiteral("max_ms"), summary.maxMs},
        {QStringLiteral("avg_ms"), summary.count > 0
             ? static_cast<double>(summary.totalMs) / static_cast<double>(summary.count)
             : 0.0}
    };
}

QJsonObject UiActionProfiler::snapshot() const
{
    QJsonArray lastActions;
    const qsizetype firstLast =
        std::max<qsizetype>(0, m_records.size() - kMaxLastRecordsInSnapshot);
    for (qsizetype i = firstLast; i < m_records.size(); ++i) {
        lastActions.push_back(recordToJson(m_records.at(i)));
    }

    QVector<Record> slowRecords;
    for (const Record& record : m_records) {
        const qint64 slowThresholdMs =
            record.context.playbackActive ? kPlaybackSlowUiActionMs : kSlowUiActionMs;
        if (record.elapsedMs >= slowThresholdMs) {
            slowRecords.push_back(record);
        }
    }
    std::sort(slowRecords.begin(), slowRecords.end(), [](const Record& left, const Record& right) {
        if (left.elapsedMs != right.elapsedMs) {
            return left.elapsedMs > right.elapsedMs;
        }
        return left.sequence > right.sequence;
    });
    QJsonArray slowActions;
    const qsizetype slowLimit =
        std::min<qsizetype>(kMaxSlowRecordsInSnapshot, slowRecords.size());
    for (qsizetype i = 0; i < slowLimit; ++i) {
        slowActions.push_back(recordToJson(slowRecords.at(i)));
    }

    QVector<Summary> summaries = m_summaries;
    std::sort(summaries.begin(), summaries.end(), [](const Summary& left, const Summary& right) {
        if (left.maxMs != right.maxMs) {
            return left.maxMs > right.maxMs;
        }
        if (left.slowCount != right.slowCount) {
            return left.slowCount > right.slowCount;
        }
        return left.actionId < right.actionId;
    });
    QJsonArray summaryArray;
    const qsizetype summaryLimit =
        std::min<qsizetype>(kMaxSummaryRowsInSnapshot, summaries.size());
    for (qsizetype i = 0; i < summaryLimit; ++i) {
        summaryArray.push_back(summaryToJson(summaries.at(i)));
    }

    return QJsonObject{
        {QStringLiteral("enabled"), true},
        {QStringLiteral("record_capacity"), static_cast<qint64>(kMaxUiActionRecords)},
        {QStringLiteral("record_count"), m_records.size()},
        {QStringLiteral("total_count"), m_totalCount},
        {QStringLiteral("slow_threshold_ms"), kSlowUiActionMs},
        {QStringLiteral("playback_slow_threshold_ms"), kPlaybackSlowUiActionMs},
        {QStringLiteral("severe_threshold_ms"), kSevereUiActionMs},
        {QStringLiteral("slow_count"), m_slowCount},
        {QStringLiteral("severe_count"), m_severeCount},
        {QStringLiteral("last_elapsed_ms"), m_lastElapsedMs},
        {QStringLiteral("max_elapsed_ms"), m_maxElapsedMs},
        {QStringLiteral("last_actions"), lastActions},
        {QStringLiteral("slow_actions"), slowActions},
        {QStringLiteral("summary"), summaryArray}
    };
}

void UiActionProfiler::reset()
{
    m_records.clear();
    m_summaries.clear();
    m_sequence = 0;
    m_totalCount = 0;
    m_slowCount = 0;
    m_severeCount = 0;
    m_lastElapsedMs = 0;
    m_maxElapsedMs = 0;
}
