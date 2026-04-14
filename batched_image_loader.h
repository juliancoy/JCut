#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QHash>
#include <QMutex>
#include <QWaitCondition>
#include <QThreadPool>
#include <QRunnable>
#include <QFuture>
#include <QFutureWatcher>
#include <QtConcurrent>

#include <memory>
#include <atomic>

class BatchedImageLoader : public QObject {
    Q_OBJECT
public:
    struct BatchRequest {
        QStringList filePaths;
        QVector<int> frameIndices;
        int batchId;
        std::atomic<bool> completed{false};
        QVector<QImage> results;
        QString error;
    };

    explicit BatchedImageLoader(int maxBatchSize = 16, int maxConcurrentBatches = 4);
    ~BatchedImageLoader();

    // Request a batch of images
    std::shared_ptr<BatchRequest> requestBatch(const QStringList& filePaths, 
                                               const QVector<int>& frameIndices);

    // Cancel a pending batch
    void cancelBatch(std::shared_ptr<BatchRequest> request);

    // Wait for batch completion (with timeout)
    bool waitForBatch(std::shared_ptr<BatchRequest> request, int timeoutMs = 5000);

    // Get batch result
    QVector<QImage> getBatchResult(std::shared_ptr<BatchRequest> request);

    // Clear all pending batches
    void clear();

    // Statistics
    struct Stats {
        int totalBatchesProcessed = 0;
        int totalImagesLoaded = 0;
        int failedBatches = 0;
        int cancelledBatches = 0;
        qint64 totalLoadTimeMs = 0;
        double averageBatchTimeMs = 0.0;
        double averageImageLoadTimeMs = 0.0;
    };

    Stats getStats() const;

private slots:
    void onBatchCompleted(int batchId);

private:
    class BatchWorker : public QRunnable {
    public:
        BatchWorker(std::shared_ptr<BatchRequest> request, 
                    std::function<void(int)> completionCallback);
        void run() override;

    private:
        std::shared_ptr<BatchRequest> m_request;
        std::function<void(int)> m_completionCallback;
    };

    QImage loadSingleImageOptimized(const QString& filePath);

    mutable QMutex m_mutex;
    QWaitCondition m_condition;
    QThreadPool m_threadPool;
    
    QHash<int, std::shared_ptr<BatchRequest>> m_pendingBatches;
    QHash<int, std::shared_ptr<BatchRequest>> m_completedBatches;
    
    int m_maxBatchSize;
    int m_maxConcurrentBatches;
    int m_nextBatchId = 0;
    
    Stats m_stats;
    
    // Memory-mapped file cache
    struct MappedFile {
        QByteArray data;
        qint64 lastAccessTime = 0;
    };
    QHash<QString, MappedFile> m_fileCache;
    const qint64 kMaxFileCacheSize = 100 * 1024 * 1024; // 100MB
    qint64 m_currentCacheSize = 0;
    
    QMutex m_cacheMutex;
    
    void addToCache(const QString& filePath, const QByteArray& data);
    QByteArray getFromCache(const QString& filePath);
    void cleanupCache();
};