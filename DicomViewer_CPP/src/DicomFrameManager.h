#pragma once

#include <QObject>
#include <QPixmap>
#include <QMap>
#include <QMutex>
#include <QThread>
#include <QThreadPool>
#include <QRunnable>
#include <QByteArray>
#include <QFuture>
#include <QFutureWatcher>
#include <QSet>
#include <QTimer>

/**
 * @class DicomFrameManager
 * @brief Professional frame caching and loading manager for DICOM images
 * 
 * Manages frame loading, caching, and memory optimization for multiframe DICOM images.
 * Implements intelligent preloading and memory management strategies used in professional viewers.
 */
class DicomFrameManager : public QObject
{
    Q_OBJECT

public:
    enum LoadingStrategy {
        SequentialLoading,    // Load frames in order (1, 2, 3, ...)
        AdaptiveLoading,      // Load based on playback direction and speed
        PreemptiveLoading,    // Load frames around current position
        OnDemandLoading      // Load only when requested
    };

    enum CachePolicy {
        UnlimitedCache,      // Keep all frames in memory
        LimitedCache,        // Keep N frames in memory
        AdaptiveCache,       // Adjust cache based on available memory
        StreamingCache      // Only keep current and next few frames
    };

    struct FrameInfo {
        int frameIndex;
        QPixmap pixmap;
        QByteArray originalData;
        qint64 loadTime;
        bool isTransformed;
        
        FrameInfo() : frameIndex(-1), loadTime(0), isTransformed(false) {}
        FrameInfo(int index, const QPixmap& pm, const QByteArray& data = QByteArray()) 
            : frameIndex(index), pixmap(pm), originalData(data), loadTime(0), isTransformed(false) {}
    };

    explicit DicomFrameManager(QObject *parent = nullptr);
    ~DicomFrameManager();

    // Configuration
    void setLoadingStrategy(LoadingStrategy strategy) { m_loadingStrategy = strategy; }
    LoadingStrategy loadingStrategy() const { return m_loadingStrategy; }
    void setCachePolicy(CachePolicy policy) { m_cachePolicy = policy; }
    CachePolicy cachePolicy() const { return m_cachePolicy; }
    void setMaxCacheSize(int maxFrames) { m_maxCacheSize = maxFrames; }
    int maxCacheSize() const { return m_maxCacheSize; }
    void setImageInfo(const QString& imageId, int totalFrames);

    // Frame access
    bool hasFrame(int frameIndex) const;
    FrameInfo getFrame(int frameIndex) const;
    QPixmap getFramePixmap(int frameIndex) const;
    int getTotalFrames() const { return m_totalFrames; }
    int getLoadedFrameCount() const;
    QList<int> getAvailableFrames() const;

    // Cache management
    void clearCache();
    void removeFrame(int frameIndex);
    qint64 getCacheMemoryUsage() const;
    double getCacheHitRatio() const;

signals:
    // Frame loading signals
    void frameReady(int frameIndex, const QPixmap& pixmap, const QByteArray& originalData);
    void frameLoadingStarted(int frameIndex);
    void frameLoadingFailed(int frameIndex, const QString& error);
    
    // Progress signals
    void loadingProgress(int loadedFrames, int totalFrames);
    void allFramesLoaded(int totalFrames);
    void loadingStarted(int totalFrames);
    void loadingCompleted();
    
    // Cache management signals
    void cacheUpdated(int cachedFrames, qint64 memoryUsage);
    void frameCacheEvicted(int frameIndex);

public slots:
    // Primary loading interface
    void loadFrames(const QString& dicomPath);
    void preloadFrameRange(int startFrame, int endFrame);
    void requestFrame(int frameIndex, bool highPriority = false);
    void cancelLoading();
    
    // Frame data input (from progressive loader)
    void onFrameLoaded(int frameIndex, const QPixmap& pixmap, const QByteArray& originalData = QByteArray());
    void onLoadingStarted(int totalFrames);
    void onAllFramesLoaded();
    
    // Cache management
    void setCurrentFrame(int frameIndex);
    void optimizeCache();
    void preloadAroundFrame(int centerFrame, int radius = 5);

private slots:
    void onPreloadingFinished();
    void onFrameProcessingFinished();

private:
    // Cache management
    void addFrameToCache(int frameIndex, const FrameInfo& frameInfo);
    void evictLeastRecentlyUsed();
    void updateFrameAccess(int frameIndex);
    void updateFrameAccessFast(int frameIndex);  // High-performance timing version
    bool shouldEvictFrame(int frameIndex) const;
    void cleanupCache();
    
    // Loading optimization
    QList<int> calculatePreloadFrames(int currentFrame) const;
    void startPreloading(const QList<int>& frameIndices);
    bool isFrameLoadingRequested(int frameIndex) const;
    
    // Memory management
    qint64 calculateFrameMemorySize(const QPixmap& pixmap) const;
    qint64 getAvailableMemory() const;
    void enforceMemoryLimits();
    
    // Frame storage and access
    mutable QMutex m_cacheMutex;
    QMap<int, FrameInfo> m_frameCache;
    QMap<int, qint64> m_frameAccessTimes;  // For LRU eviction
    
    // Loading state
    QString m_currentDicomPath;
    int m_totalFrames;
    int m_currentFrame;
    QSet<int> m_loadingRequests;
    QSet<int> m_failedLoads;
    
    // Configuration
    LoadingStrategy m_loadingStrategy;
    CachePolicy m_cachePolicy;
    int m_maxCacheSize;
    qint64 m_maxMemoryUsage;
    
    // Statistics
    mutable int m_cacheHits;
    mutable int m_cacheMisses;
    qint64 m_currentMemoryUsage;
    
    // Async loading support
    QFutureWatcher<void>* m_preloadWatcher;
    QThread* m_loadingThread;
    QThreadPool* m_threadPool;
    
    // Constants
    static constexpr int DEFAULT_MAX_CACHE_SIZE = 100;
    static constexpr qint64 DEFAULT_MAX_MEMORY_MB = 512;
    static constexpr int DEFAULT_PRELOAD_RADIUS = 5;
};
