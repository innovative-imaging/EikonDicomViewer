#include "DicomFrameManager.h"
#include <QDebug>
#include <QMutexLocker>
#include <QDateTime>
#include <QThread>
#include <QThreadPool>
#include <QRunnable>
#include <QSet>
#include <QTimer>
#include <chrono>
#include <algorithm>

// Background frame loading task for multi-threaded decompression
class FrameLoadingTask : public QRunnable
{
public:
    FrameLoadingTask(DicomFrameManager* manager, int frameIndex, const QString& filePath)
        : m_manager(manager), m_frameIndex(frameIndex), m_filePath(filePath)
    {
        setAutoDelete(true);
    }
    
    void run() override
    {
        try {
            // Check if manager is still valid before doing work
            if (!m_manager) {
                return;
            }
            
            // This would connect to your DicomFrameProcessor to actually load the frame
            // For now, simulate work - in real implementation, call processor->getFrameAsQImage()
            QThread::msleep(10); // Simulate decompression time
            
            // Create a dummy pixmap for testing
            QPixmap dummyPixmap(512, 512);
            dummyPixmap.fill(Qt::gray);
            
            QByteArray dummyData;
            
            // Check again if manager is still valid before emitting
            if (m_manager) {
                // Emit result back to manager (thread-safe)
                QMetaObject::invokeMethod(m_manager, "onFrameLoaded",
                                        Qt::QueuedConnection,
                                        Q_ARG(int, m_frameIndex),
                                        Q_ARG(QPixmap, dummyPixmap),
                                        Q_ARG(QByteArray, dummyData));
            }
        } catch (const std::exception& e) {
        } catch (...) {
        }
    }
    
private:
    DicomFrameManager* m_manager;
    int m_frameIndex;
    QString m_filePath;
};

DicomFrameManager::DicomFrameManager(QObject *parent)
    : QObject(parent)
    , m_loadingStrategy(AdaptiveLoading)
    , m_cachePolicy(AdaptiveCache)
    , m_maxCacheSize(DEFAULT_MAX_CACHE_SIZE)
    , m_maxMemoryUsage(DEFAULT_MAX_MEMORY_MB * 1024 * 1024) // Convert MB to bytes
    , m_totalFrames(0)
    , m_currentFrame(0)
    , m_cacheHits(0)
    , m_cacheMisses(0)
    , m_currentMemoryUsage(0)
    , m_preloadWatcher(nullptr)
    , m_loadingThread(nullptr)
{
    // Initialize future watcher for async operations
    m_preloadWatcher = new QFutureWatcher<void>(this);
    connect(m_preloadWatcher, &QFutureWatcher<void>::finished,
            this, &DicomFrameManager::onPreloadingFinished);
    
    // Initialize thread pool for background frame loading
    m_threadPool = QThreadPool::globalInstance();
    m_threadPool->setMaxThreadCount(qMax(2, QThread::idealThreadCount() / 2)); // Use half available cores
    
}

DicomFrameManager::~DicomFrameManager()
{
    clearCache();
    
    if (m_preloadWatcher) {
        m_preloadWatcher->cancel();
        m_preloadWatcher->waitForFinished();
    }
}

bool DicomFrameManager::hasFrame(int frameIndex) const
{
    QMutexLocker locker(&m_cacheMutex);
    return m_frameCache.contains(frameIndex);
}

DicomFrameManager::FrameInfo DicomFrameManager::getFrame(int frameIndex) const
{
    QMutexLocker locker(&m_cacheMutex);
    
    auto it = m_frameCache.constFind(frameIndex);
    if (it != m_frameCache.constEnd()) {
        m_cacheHits++;
        
        // Use high-performance timing for LRU - avoid expensive QDateTime calls
        const_cast<DicomFrameManager*>(this)->updateFrameAccessFast(frameIndex);
        
        return it.value();
    } else {
        m_cacheMisses++;
        return FrameInfo(); // Return empty frame info
    }
}

QPixmap DicomFrameManager::getFramePixmap(int frameIndex) const
{
    FrameInfo frameInfo = getFrame(frameIndex);
    return frameInfo.pixmap;
}

int DicomFrameManager::getLoadedFrameCount() const
{
    QMutexLocker locker(&m_cacheMutex);
    return m_frameCache.size();
}

QList<int> DicomFrameManager::getAvailableFrames() const
{
    QMutexLocker locker(&m_cacheMutex);
    return m_frameCache.keys();
}

void DicomFrameManager::clearCache()
{
    QMutexLocker locker(&m_cacheMutex);
    
    
    // Cancel any pending background tasks to prevent crashes
    if (m_threadPool) {
        m_threadPool->clear(); // Clear pending tasks
    }
    
    m_frameCache.clear();
    m_frameAccessTimes.clear();
    m_loadingRequests.clear();
    m_failedLoads.clear();
    m_currentMemoryUsage = 0;
    m_totalFrames = 0;
    m_currentFrame = 0;
    
    // Reset statistics
    m_cacheHits = 0;
    m_cacheMisses = 0;
    
    emit cacheUpdated(0, 0);
}

void DicomFrameManager::removeFrame(int frameIndex)
{
    QMutexLocker locker(&m_cacheMutex);
    
    if (m_frameCache.contains(frameIndex)) {
        FrameInfo frameInfo = m_frameCache.take(frameIndex);
        m_frameAccessTimes.remove(frameIndex);
        
        // Update memory usage
        qint64 frameSize = calculateFrameMemorySize(frameInfo.pixmap);
        m_currentMemoryUsage -= frameSize;
        
        emit frameCacheEvicted(frameIndex);
        emit cacheUpdated(m_frameCache.size(), m_currentMemoryUsage);
        
    }
}

qint64 DicomFrameManager::getCacheMemoryUsage() const
{
    QMutexLocker locker(&m_cacheMutex);
    return m_currentMemoryUsage;
}

double DicomFrameManager::getCacheHitRatio() const
{
    QMutexLocker locker(&m_cacheMutex);
    
    int totalAccesses = m_cacheHits + m_cacheMisses;
    if (totalAccesses == 0) {
        return 0.0;
    }
    
    return static_cast<double>(m_cacheHits) / totalAccesses;
}

void DicomFrameManager::loadFrames(const QString& dicomPath)
{
    m_currentDicomPath = dicomPath;
    
    // Clear existing cache
    clearCache();
    
    emit loadingStarted(m_totalFrames);
}

void DicomFrameManager::preloadFrameRange(int startFrame, int endFrame)
{
    if (startFrame < 0 || endFrame >= m_totalFrames || startFrame > endFrame) {
        return;
    }
    
    QList<int> framesToLoad;
    
    for (int i = startFrame; i <= endFrame; i++) {
        if (!hasFrame(i) && !m_loadingRequests.contains(i) && !m_failedLoads.contains(i)) {
            framesToLoad.append(i);
            m_loadingRequests.insert(i);
        }
    }
    
    if (!framesToLoad.isEmpty()) {
        startPreloading(framesToLoad);
    }
}

void DicomFrameManager::requestFrame(int frameIndex, bool highPriority)
{
    QMutexLocker locker(&m_cacheMutex);
    
    if (frameIndex < 0 || frameIndex >= m_totalFrames) {
        return;
    }
    
    if (!hasFrame(frameIndex) && !m_loadingRequests.contains(frameIndex)) {
        m_loadingRequests.insert(frameIndex);
        
        // Create background loading task for thread pool execution
        FrameLoadingTask* task = new FrameLoadingTask(this, frameIndex, m_currentDicomPath);
        
        if (highPriority) {
            // High priority frames get executed immediately
            m_threadPool->start(task, QThread::HighPriority);
        } else {
            // Normal priority for preload frames
            m_threadPool->start(task, QThread::NormalPriority);
        }
        
                 << (highPriority ? "HIGH" : "NORMAL") << ")";
    }
}

void DicomFrameManager::cancelLoading()
{
    if (m_preloadWatcher && m_preloadWatcher->isRunning()) {
        m_preloadWatcher->cancel();
    }
    
    m_loadingRequests.clear();
}

void DicomFrameManager::onFrameLoaded(int frameIndex, const QPixmap& pixmap, const QByteArray& originalData)
{
    QMutexLocker locker(&m_cacheMutex);
    
    if (frameIndex < 0 || frameIndex >= m_totalFrames) {
        return;
    }
    
    // Create frame info
    FrameInfo frameInfo(frameIndex, pixmap, originalData);
    frameInfo.loadTime = QDateTime::currentMSecsSinceEpoch();
    
    // Add to cache
    addFrameToCache(frameIndex, frameInfo);
    
    // Remove from loading requests
    m_loadingRequests.remove(frameIndex);
    
    // Emit signals
    emit frameReady(frameIndex, pixmap, originalData);
    
    int loadedCount = m_frameCache.size();
    emit loadingProgress(loadedCount, m_totalFrames);
    
    
    // Check if all frames are loaded
    if (loadedCount >= m_totalFrames) {
        emit allFramesLoaded(m_totalFrames);
        emit loadingCompleted();
    }
}

void DicomFrameManager::onLoadingStarted(int totalFrames)
{
    QMutexLocker locker(&m_cacheMutex);
    
    m_totalFrames = totalFrames;
    clearCache(); // This also resets counters
    
    emit loadingStarted(totalFrames);
}

void DicomFrameManager::onAllFramesLoaded()
{
    QMutexLocker locker(&m_cacheMutex);
    
    emit allFramesLoaded(m_totalFrames);
    emit loadingCompleted();
    
}

void DicomFrameManager::setCurrentFrame(int frameIndex)
{
    if (frameIndex >= 0 && frameIndex < m_totalFrames) {
        m_currentFrame = frameIndex;
        
        // Trigger preloading around current frame if using adaptive strategy
        if (m_loadingStrategy == AdaptiveLoading || m_loadingStrategy == PreemptiveLoading) {
            preloadAroundFrame(frameIndex);
        }
    }
}

void DicomFrameManager::optimizeCache()
{
    QMutexLocker locker(&m_cacheMutex);
    
    // Enforce memory limits
    enforceMemoryLimits();
    
    // Clean up old frames if cache is too large
    cleanupCache();
    
    emit cacheUpdated(m_frameCache.size(), m_currentMemoryUsage);
}

void DicomFrameManager::preloadAroundFrame(int centerFrame, int radius)
{
    if (m_totalFrames <= 1) {
        return;
    }
    
    QList<int> framesToPreload;
    
    // Enhanced preloading strategy: prioritize forward direction for playback
    // Load more frames ahead than behind for smooth playback
    int backwardRadius = radius / 3;
    int forwardRadius = radius;
    
    int startFrame = qMax(0, centerFrame - backwardRadius);
    int endFrame = qMin(m_totalFrames - 1, centerFrame + forwardRadius);
    
    // Priority loading: load closer frames first, prioritize forward frames
    QMap<int, int> frameDistances;
    
    for (int i = startFrame; i <= endFrame; i++) {
        if (!hasFrame(i) && !m_loadingRequests.contains(i)) {
            int distance = qAbs(i - centerFrame);
            // Give forward frames priority (lower distance value)
            if (i > centerFrame) {
                distance = static_cast<int>(distance * 0.8); // 20% priority boost
            }
            frameDistances[distance] = i;
        }
    }
    
    // Convert to list sorted by priority
    for (auto it = frameDistances.constBegin(); it != frameDistances.constEnd(); ++it) {
        framesToPreload.append(it.value());
    }
    
    if (!framesToPreload.isEmpty()) {
                 << "- range:" << startFrame << "to" << endFrame << "(forward-biased)";
        
        for (int frameIndex : framesToPreload) {
            requestFrame(frameIndex, frameIndex <= centerFrame + 2); // High priority for very close frames
        }
    }
}

void DicomFrameManager::onPreloadingFinished()
{
}

void DicomFrameManager::onFrameProcessingFinished()
{
}

void DicomFrameManager::addFrameToCache(int frameIndex, const FrameInfo& frameInfo)
{
    // Check if we need to evict frames first
    if (m_frameCache.size() >= m_maxCacheSize) {
        evictLeastRecentlyUsed();
    }
    
    // Calculate memory usage
    qint64 frameSize = calculateFrameMemorySize(frameInfo.pixmap);
    
    // Check memory limits
    if (m_currentMemoryUsage + frameSize > m_maxMemoryUsage) {
        enforceMemoryLimits();
        
        // If still not enough space, don't cache this frame
        if (m_currentMemoryUsage + frameSize > m_maxMemoryUsage) {
            return;
        }
    }
    
    // Add frame to cache
    m_frameCache[frameIndex] = frameInfo;
    m_currentMemoryUsage += frameSize;
    updateFrameAccess(frameIndex);
    
    emit cacheUpdated(m_frameCache.size(), m_currentMemoryUsage);
}

void DicomFrameManager::evictLeastRecentlyUsed()
{
    if (m_frameCache.isEmpty()) {
        return;
    }
    
    // Find least recently used frame using iterator for better performance
    auto oldestIt = std::min_element(m_frameAccessTimes.constBegin(), 
                                   m_frameAccessTimes.constEnd(),
                                   [](const auto& a, const auto& b) {
                                       return a.value() < b.value();
                                   });
    
    if (oldestIt != m_frameAccessTimes.constEnd()) {
        int lruFrame = oldestIt.key();
        removeFrame(lruFrame);
    }
}

void DicomFrameManager::updateFrameAccess(int frameIndex)
{
    m_frameAccessTimes[frameIndex] = QDateTime::currentMSecsSinceEpoch();
}

void DicomFrameManager::updateFrameAccessFast(int frameIndex)
{
    // Use high-resolution clock for better performance (10x faster than QDateTime)
    static auto startTime = std::chrono::steady_clock::now();
    auto currentTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime).count();
    m_frameAccessTimes[frameIndex] = elapsed;
}

bool DicomFrameManager::shouldEvictFrame(int frameIndex) const
{
    Q_UNUSED(frameIndex)
    // TODO: Implement intelligent eviction logic based on:
    // - Distance from current frame
    // - Access frequency
    // - Loading priority
    return true;
}

void DicomFrameManager::cleanupCache()
{
    // Remove frames that are far from current position if cache is getting full
    if (m_frameCache.size() > m_maxCacheSize * 0.8) { // 80% threshold
        
        QList<int> framesToEvict;
        const int keepRadius = DEFAULT_PRELOAD_RADIUS;
        
        for (auto it = m_frameCache.constBegin(); it != m_frameCache.constEnd(); ++it) {
            int frameIndex = it.key();
            int distance = qAbs(frameIndex - m_currentFrame);
            
            if (distance > keepRadius) {
                framesToEvict.append(frameIndex);
            }
        }
        
        // Sort by distance and evict furthest frames first
        std::sort(framesToEvict.begin(), framesToEvict.end(), [this](int a, int b) {
            return qAbs(a - m_currentFrame) > qAbs(b - m_currentFrame);
        });
        
        // Evict some frames to free space
        int framesToRemove = qMin(framesToEvict.size(), m_frameCache.size() / 4); // Remove 25%
        for (int i = 0; i < framesToRemove; i++) {
            removeFrame(framesToEvict[i]);
        }
        
    }
}

QList<int> DicomFrameManager::calculatePreloadFrames(int currentFrame) const
{
    QList<int> frames;
    
    // Simple strategy: load frames around current position
    const int radius = DEFAULT_PRELOAD_RADIUS;
    
    for (int i = currentFrame - radius; i <= currentFrame + radius; i++) {
        if (i >= 0 && i < m_totalFrames && !hasFrame(i)) {
            frames.append(i);
        }
    }
    
    return frames;
}

void DicomFrameManager::startPreloading(const QList<int>& frameIndices)
{
    if (frameIndices.isEmpty()) {
        return;
    }
    
    // For now, just mark frames as requested
    // In a full implementation, this would start async loading
    for (int frameIndex : frameIndices) {
        m_loadingRequests.insert(frameIndex);
    }
    
}

bool DicomFrameManager::isFrameLoadingRequested(int frameIndex) const
{
    return m_loadingRequests.contains(frameIndex);
}

qint64 DicomFrameManager::calculateFrameMemorySize(const QPixmap& pixmap) const
{
    if (pixmap.isNull()) {
        return 0;
    }
    
    // Estimate memory usage: width * height * depth (in bytes)
    QSize size = pixmap.size();
    int depth = pixmap.depth(); // bits per pixel
    
    return static_cast<qint64>(size.width()) * size.height() * (depth / 8);
}

qint64 DicomFrameManager::getAvailableMemory() const
{
    // TODO: Implement system memory detection
    // For now, return a reasonable default
    return 512 * 1024 * 1024; // 512 MB
}

void DicomFrameManager::enforceMemoryLimits()
{
    while (m_currentMemoryUsage > m_maxMemoryUsage && !m_frameCache.isEmpty()) {
        evictLeastRecentlyUsed();
    }
}

void DicomFrameManager::setImageInfo(const QString& imageId, int totalFrames)
{
    QMutexLocker locker(&m_cacheMutex);
    
    m_currentImageId = imageId;
    m_totalFrames = totalFrames;
    
    // Clear cache when switching to new image
    clearCache();
    
}
