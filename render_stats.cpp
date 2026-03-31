#include "render_internal.h"

namespace render_detail {

size_t qHash(const RenderAsyncFrameKey& key, size_t seed) {
    return qHashMulti(seed, key.path, key.frameNumber);
}

bool isHardwareEncoderLabel(const QString& codecLabel) {
    const QString lowered = codecLabel.toLower();
    return lowered.contains(QStringLiteral("nvenc")) ||
           lowered.contains(QStringLiteral("qsv")) ||
           lowered.contains(QStringLiteral("vaapi")) ||
           lowered.contains(QStringLiteral("videotoolbox")) ||
           lowered.contains(QStringLiteral("amf")) ||
           lowered.contains(QStringLiteral("omx")) ||
           lowered.contains(QStringLiteral("mediacodec"));
}

void recordRenderSkip(QJsonArray* skippedClips,
                      QJsonObject* skippedReasonCounts,
                      const TimelineClip& clip,
                      const QString& reason,
                      int64_t timelineFrame,
                      int64_t localFrame) {
    if (skippedClips) {
        QJsonObject obj{
            {QStringLiteral("id"), clip.id},
            {QStringLiteral("label"), clip.label},
            {QStringLiteral("reason"), reason},
            {QStringLiteral("timeline_frame"), static_cast<qint64>(timelineFrame)}
        };
        if (localFrame >= 0) {
            obj.insert(QStringLiteral("local_frame"), static_cast<qint64>(localFrame));
        }
        skippedClips->push_back(obj);
    }
    if (skippedReasonCounts) {
        skippedReasonCounts->insert(reason, skippedReasonCounts->value(reason).toInt(0) + 1);
    }
}

void accumulateClipStageStats(QHash<QString, RenderClipStageStats>* clipStageStats,
                              const TimelineClip& clip,
                              qint64 decodeMs,
                              qint64 textureMs,
                              qint64 compositeMs) {
    if (!clipStageStats) {
        return;
    }
    RenderClipStageStats& stats = (*clipStageStats)[clip.id];
    if (stats.id.isEmpty()) {
        stats.id = clip.id;
        stats.label = clip.label;
    }
    ++stats.frames;
    stats.decodeMs += decodeMs;
    stats.textureMs += textureMs;
    stats.compositeMs += compositeMs;
}

QJsonObject buildRenderStageTable(const QHash<QString, RenderClipStageStats>& clipStageStats,
                                  qint64 totalRenderStageMs,
                                  int64_t completedFrames) {
    QJsonArray columns{
        QStringLiteral("clip"),
        QStringLiteral("frames"),
        QStringLiteral("decode_ms"),
        QStringLiteral("decode_ms_per_frame"),
        QStringLiteral("texture_ms"),
        QStringLiteral("texture_ms_per_frame"),
        QStringLiteral("composite_ms"),
        QStringLiteral("composite_ms_per_frame"),
        QStringLiteral("stage_ms"),
        QStringLiteral("stage_ms_per_frame"),
        QStringLiteral("stage_share_pct")
    };

    QVector<RenderClipStageStats> rows = clipStageStats.values().toVector();
    std::sort(rows.begin(), rows.end(), [](const RenderClipStageStats& a, const RenderClipStageStats& b) {
        const qint64 aTotal = a.decodeMs + a.textureMs + a.compositeMs;
        const qint64 bTotal = b.decodeMs + b.textureMs + b.compositeMs;
        if (aTotal != bTotal) {
            return aTotal > bTotal;
        }
        return a.label < b.label;
    });

    QJsonArray jsonRows;
    for (const RenderClipStageStats& stats : rows) {
        const qint64 stageMs = stats.decodeMs + stats.textureMs + stats.compositeMs;
        const double frames = static_cast<double>(qMax<int64_t>(1, stats.frames));
        const double sharePct = totalRenderStageMs > 0
            ? (100.0 * static_cast<double>(stageMs) / static_cast<double>(totalRenderStageMs))
            : 0.0;
        jsonRows.push_back(QJsonObject{
            {QStringLiteral("id"), stats.id},
            {QStringLiteral("clip"), stats.label},
            {QStringLiteral("frames"), static_cast<qint64>(stats.frames)},
            {QStringLiteral("decode_ms"), stats.decodeMs},
            {QStringLiteral("decode_ms_per_frame"), static_cast<double>(stats.decodeMs) / frames},
            {QStringLiteral("texture_ms"), stats.textureMs},
            {QStringLiteral("texture_ms_per_frame"), static_cast<double>(stats.textureMs) / frames},
            {QStringLiteral("composite_ms"), stats.compositeMs},
            {QStringLiteral("composite_ms_per_frame"), static_cast<double>(stats.compositeMs) / frames},
            {QStringLiteral("stage_ms"), stageMs},
            {QStringLiteral("stage_ms_per_frame"), static_cast<double>(stageMs) / frames},
            {QStringLiteral("stage_share_pct"), sharePct}
        });
    }

    qint64 attributedStageMs = 0;
    for (const RenderClipStageStats& stats : rows) {
        attributedStageMs += stats.decodeMs + stats.textureMs + stats.compositeMs;
    }
    const qint64 overheadMs = qMax<qint64>(0, totalRenderStageMs - attributedStageMs);
    if (overheadMs > 0) {
        const double frames = static_cast<double>(qMax<int64_t>(1, completedFrames));
        const double sharePct = totalRenderStageMs > 0
            ? (100.0 * static_cast<double>(overheadMs) / static_cast<double>(totalRenderStageMs))
            : 0.0;
        jsonRows.push_back(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("__frame_overhead__")},
            {QStringLiteral("clip"), QStringLiteral("__frame_overhead__")},
            {QStringLiteral("frames"), static_cast<qint64>(completedFrames)},
            {QStringLiteral("decode_ms"), static_cast<qint64>(0)},
            {QStringLiteral("decode_ms_per_frame"), 0.0},
            {QStringLiteral("texture_ms"), static_cast<qint64>(0)},
            {QStringLiteral("texture_ms_per_frame"), 0.0},
            {QStringLiteral("composite_ms"), overheadMs},
            {QStringLiteral("composite_ms_per_frame"), static_cast<double>(overheadMs) / frames},
            {QStringLiteral("stage_ms"), overheadMs},
            {QStringLiteral("stage_ms_per_frame"), static_cast<double>(overheadMs) / frames},
            {QStringLiteral("stage_share_pct"), sharePct}
        });
    }

    return QJsonObject{
        {QStringLiteral("columns"), columns},
        {QStringLiteral("rows"), jsonRows}
    };
}

void recordWorstFrame(QVector<RenderFrameStageStats>* worstFrames,
                      const RenderFrameStageStats& stats,
                      int maxEntries) {
    if (!worstFrames) {
        return;
    }
    worstFrames->push_back(stats);
    std::sort(worstFrames->begin(), worstFrames->end(), [](const RenderFrameStageStats& a, const RenderFrameStageStats& b) {
        if (a.renderMs != b.renderMs) {
            return a.renderMs > b.renderMs;
        }
        return a.timelineFrame > b.timelineFrame;
    });
    if (worstFrames->size() > maxEntries) {
        worstFrames->resize(maxEntries);
    }
}

QJsonObject buildWorstFrameTable(const QVector<RenderFrameStageStats>& worstFrames) {
    QJsonArray columns{
        QStringLiteral("timeline_frame"),
        QStringLiteral("segment_index"),
        QStringLiteral("render_ms"),
        QStringLiteral("decode_ms"),
        QStringLiteral("texture_ms"),
        QStringLiteral("readback_ms"),
        QStringLiteral("convert_ms")
    };

    QJsonArray rows;
    for (const RenderFrameStageStats& stats : worstFrames) {
        rows.push_back(QJsonObject{
            {QStringLiteral("timeline_frame"), static_cast<qint64>(stats.timelineFrame)},
            {QStringLiteral("segment_index"), stats.segmentIndex},
            {QStringLiteral("render_ms"), stats.renderMs},
            {QStringLiteral("decode_ms"), stats.decodeMs},
            {QStringLiteral("texture_ms"), stats.textureMs},
            {QStringLiteral("readback_ms"), stats.readbackMs},
            {QStringLiteral("convert_ms"), stats.convertMs}
        });
    }

    return QJsonObject{
        {QStringLiteral("columns"), columns},
        {QStringLiteral("rows"), rows}
    };
}

} // namespace render_detail
