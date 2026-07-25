#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

#include <cstdint>

namespace editor {

struct PresentationFrameSample {
    QString streamId;
    int64_t requestedFrame = -1;
    int64_t presentedFrame = -1;
};

inline bool presentationStatusRequiresDraw(bool active,
                                           bool drawSuppressed,
                                           const QString& missingReason)
{
    if (!active) {
        return false;
    }
    if (!drawSuppressed) {
        return true;
    }

    // Hidden providers, zero-opacity layers, and pregraded clones are
    // intentionally suppressed. A missing matte is different: the layer was
    // supposed to draw and failed closed, so it is a presentation miss.
    return missingReason == QStringLiteral("mask_texture_unavailable");
}

inline int64_t presentedFrameForDrawOutcome(bool drawSubmitted,
                                            int64_t decodedFrame)
{
    return drawSubmitted ? decodedFrame : -1;
}

inline QString presentationStreamId(const QString& clipId,
                                    const QString& mediaOwnerClipId,
                                    bool layerScoped)
{
    if (layerScoped || mediaOwnerClipId.isEmpty()) {
        return clipId;
    }
    return mediaOwnerClipId;
}

class PresentationMissTracker {
public:
    int64_t recordPresentedFrame(const QVector<PresentationFrameSample>& samples)
    {
        QSet<QString> observedStreams;
        int64_t newMisses = 0;

        for (const PresentationFrameSample& sample : samples) {
            if (sample.streamId.isEmpty() ||
                sample.requestedFrame < 0 ||
                observedStreams.contains(sample.streamId)) {
                continue;
            }
            observedStreams.insert(sample.streamId);

            if (sample.requestedFrame == sample.presentedFrame) {
                m_activeMissTargetByStream.remove(sample.streamId);
                continue;
            }

            const auto activeMiss = m_activeMissTargetByStream.constFind(sample.streamId);
            if (activeMiss == m_activeMissTargetByStream.cend() ||
                activeMiss.value() != sample.requestedFrame) {
                m_activeMissTargetByStream.insert(sample.streamId, sample.requestedFrame);
                ++newMisses;
            }
        }

        return newMisses;
    }

    void endPresentationRun()
    {
        m_activeMissTargetByStream.clear();
    }

    void reset()
    {
        m_activeMissTargetByStream.clear();
    }

private:
    QHash<QString, int64_t> m_activeMissTargetByStream;
};

}  // namespace editor
