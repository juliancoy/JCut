#pragma once

#include "preview_interaction_state.h"

#include <QHash>
#include <QString>
#include <QVector>

namespace jcut::direct_vulkan_preview {

struct MediaOwnerHandoffPlanEntry {
    QString mediaOwnerClipId;
    int providerStatusIndex = -1;
    QVector<int> consumerStatusIndices;
};

inline bool mediaOwnerPayloadMatches(
    const VulkanPreviewClipFrameStatus& provider,
    const VulkanPreviewClipFrameStatus& consumer)
{
    if (provider.frame != consumer.frame ||
        provider.frameSize != consumer.frameSize ||
        provider.presentedSourceFrame != consumer.presentedSourceFrame ||
        provider.frame.hasCpuImage() != consumer.frame.hasCpuImage() ||
        provider.frame.hasHardwareFrame() != consumer.frame.hasHardwareFrame() ||
        provider.frame.hasGpuTexture() != consumer.frame.hasGpuTexture() ||
        provider.externalVulkanFrame != consumer.externalVulkanFrame) {
        return false;
    }
    if (!provider.externalVulkanFrame) {
        return true;
    }
    return provider.externalPhysicalDevice == consumer.externalPhysicalDevice &&
           provider.externalDevice == consumer.externalDevice &&
           provider.externalQueue == consumer.externalQueue &&
           provider.externalQueueFamilyIndex == consumer.externalQueueFamilyIndex &&
           provider.externalImage == consumer.externalImage &&
           provider.externalImageView == consumer.externalImageView &&
           provider.externalImageMemory == consumer.externalImageMemory &&
           provider.externalImageLayout == consumer.externalImageLayout &&
           provider.externalImageFormat == consumer.externalImageFormat &&
           provider.externalReadySemaphoreFd == consumer.externalReadySemaphoreFd;
}

// Build a deterministic owner-first upload plan for the visible layers in a
// preview frame. A hidden media parent is authoritative when its status is
// available, but a virtual child can carry the parent's cloned decode status
// when the surface has omitted that hidden status from the final draw list.
inline QVector<MediaOwnerHandoffPlanEntry> mediaOwnerHandoffPlan(
    const QVector<VulkanPreviewClipFrameStatus>& statuses)
{
    QVector<MediaOwnerHandoffPlanEntry> plan;
    QHash<QString, int> planIndexByOwner;

    for (int statusIndex = 0; statusIndex < statuses.size(); ++statusIndex) {
        const VulkanPreviewClipFrameStatus& status = statuses.at(statusIndex);
        if (!status.active || status.drawSuppressed) {
            continue;
        }
        const QString mediaOwnerClipId = status.mediaOwnerClipId.trimmed().isEmpty()
            ? status.clipId.trimmed()
            : status.mediaOwnerClipId.trimmed();
        if (mediaOwnerClipId.isEmpty() || status.clipId.trimmed().isEmpty()) {
            continue;
        }

        auto planIndexIt = planIndexByOwner.constFind(mediaOwnerClipId);
        int planIndex = -1;
        if (planIndexIt == planIndexByOwner.cend()) {
            planIndex = plan.size();
            planIndexByOwner.insert(mediaOwnerClipId, planIndex);
            MediaOwnerHandoffPlanEntry entry;
            entry.mediaOwnerClipId = mediaOwnerClipId;
            entry.providerStatusIndex = statusIndex;
            plan.push_back(entry);
        } else {
            planIndex = planIndexIt.value();
        }
        plan[planIndex].consumerStatusIndices.push_back(statusIndex);
    }

    // Prefer the real media-owner status even when it is draw-suppressed. Its
    // decode payload remains the canonical provider for all visible children.
    for (MediaOwnerHandoffPlanEntry& entry : plan) {
        for (int statusIndex = 0; statusIndex < statuses.size(); ++statusIndex) {
            const VulkanPreviewClipFrameStatus& status = statuses.at(statusIndex);
            if (status.active && status.clipId.trimmed() == entry.mediaOwnerClipId) {
                entry.providerStatusIndex = statusIndex;
                break;
            }
        }
    }

    return plan;
}

} // namespace jcut::direct_vulkan_preview
