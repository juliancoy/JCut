#pragma once

#include "frame_payload_core.h"
#include "qt_compat.h"  // Qt 6.4/GCC 13 compatibility
#include <QExplicitlySharedDataPointer>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>
#include <cstdint>

class QRhiTexture;
class QRhi;
struct AVFrame;

namespace editor {

// ============================================================================
// FrameData - Internal shared data for FrameHandle
// ============================================================================
class FrameData : public QSharedData {
public:
    FrameData() = default;
    ~FrameData();
    
    // Prevent copying (use shared data pattern)
    FrameData(const FrameData&) = delete;
    FrameData& operator=(const FrameData&) = delete;
    
    jcut::core::FramePayloadCore payload;

    size_t memoryUsage() const;
};

// ============================================================================
// FrameHandle - RAII wrapper for decoded frames
// 
// Thread-safe, reference-counted handle to a decoded frame.
// Owns CPU/hardware payload lifetime; GPU textures remain render-thread owned.
// ============================================================================
class FrameHandle {
public:
    FrameHandle();
    FrameData* data() const { return d.data(); }
    
    // Creation helpers
    static FrameHandle createCpuFrame(const QImage& image, int64_t frameNum, const QString& path);
    // Legacy Qt-editor compatibility. Implemented in the Qt-only QRhi adapter
    // translation unit and intentionally absent from the ImGui runtime target.
    static FrameHandle createGpuFrame(QRhiTexture* texture, int64_t frameNum, const QString& path);
    static FrameHandle createHardwareFrame(const AVFrame* frame,
                                           int64_t frameNum,
                                           const QString& path,
                                           int swPixelFormat);
    
    // Validity
    bool isNull() const { return d.constData() == nullptr; }
    explicit operator bool() const { return !isNull(); }
    
    // Accessors
    int64_t frameNumber() const { return d ? d->payload.frameNumber() : -1; }
    QString sourcePath() const {
        return d ? QString::fromStdString(d->payload.sourcePath()) : QString();
    }
    QSize size() const {
        if (!d) {
            return QSize();
        }
        const jcut::core::SizeI payloadSize = d->payload.size();
        return QSize(payloadSize.width, payloadSize.height);
    }
    QRectF validTextureRectNormalized() const {
        if (!d) {
            return QRectF(0.0, 0.0, 1.0, 1.0);
        }
        const jcut::core::RectF rect = d->payload.validTextureRectNormalized();
        return QRectF(rect.x, rect.y, rect.width, rect.height);
    }
    bool hasCpuImage() const { return d && d->payload.hasCpuPayload(); }
    bool hasGpuTexture() const { return d && d->payload.hasOpaqueGpuTexture(); }
    bool hasHardwareFrame() const { return d && d->payload.hasHardwareFrame(); }
    
    QImage cpuImage() const;
    QRhiTexture* gpuTexture() const {
        return d
            ? static_cast<QRhiTexture*>(d->payload.opaqueGpuTexture())
            : nullptr;
    }
    const AVFrame* hardwareFrame() const {
        return d ? d->payload.hardwareFrame() : nullptr;
    }
    int hardwarePixelFormat() const {
        return d ? d->payload.hardwarePixelFormat() : -1;
    }
    int hardwareSwPixelFormat() const {
        return d ? d->payload.hardwareSoftwarePixelFormat() : -1;
    }
    
    size_t memoryUsage() const { return d ? d->memoryUsage() : 0; }
    size_t cpuMemoryUsage() const;
    size_t gpuMemoryUsage() const;

    // Legacy Qt-editor compatibility; see createGpuFrame(). No in-repository
    // ImGui/runtime caller depends on this private-QRhi upload path.
    void uploadToGpu(QRhi* rhi);
    bool isGpuUploadPending() const { return false; }
    
    // Comparison for caching
    bool operator==(const FrameHandle& other) const;
    bool operator!=(const FrameHandle& other) const { return !(*this == other); }
    
private:
    QExplicitlySharedDataPointer<FrameData> d;
};

// ============================================================================
// FrameCacheKey - For hash-based frame lookup
// ============================================================================
struct FrameCacheKey {
    QString path;
    int64_t frameNumber;
    
    bool operator==(const FrameCacheKey& other) const {
        return frameNumber == other.frameNumber && path == other.path;
    }
};

} // namespace editor

// Make FrameCacheKey hashable
namespace std {
template<>
struct hash<editor::FrameCacheKey> {
    size_t operator()(const editor::FrameCacheKey& key) const {
        return qHash(key.path) ^ std::hash<int64_t>{}(key.frameNumber);
    }
};
} // namespace std

// Note: Q_DECLARE_METATYPE causes issues with Qt 6.4/GCC 13
// Use qRegisterMetaType<editor::FrameHandle>() in .cpp file instead
// Q_DECLARE_METATYPE(editor::FrameHandle)
