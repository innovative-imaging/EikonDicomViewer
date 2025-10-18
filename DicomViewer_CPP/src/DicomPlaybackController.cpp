#include "DicomPlaybackController.h"
#include <QDebug>
#include <QMutexLocker>
#include <QApplication>

DicomPlaybackController::DicomPlaybackController(QObject *parent)
    : QObject(parent)
    , m_playbackState(Stopped)
    , m_navigationMode(Manual)
    , m_autoPlayPolicy(OnFirstFrame)
    , m_currentFrame(0)
    , m_totalFrames(0)
    , m_loadedFrames(0)
    , m_loopPlayback(true)
    , m_autoPlayPolicy(Never)
    , m_playbackTimer(nullptr)
    , m_playbackSpeed(DEFAULT_FPS)
    , m_frameInterval(0)
    , m_defaultFrameInterval(0)
{
    // Create playback timer
    m_playbackTimer = new QTimer(this);
    m_playbackTimer->setSingleShot(false);
    connect(m_playbackTimer, &QTimer::timeout, this, &DicomPlaybackController::onTimerTimeout);
    
    // Initialize with default frame interval
    setPlaybackSpeed(DEFAULT_FPS);
    
}

DicomPlaybackController::~DicomPlaybackController()
{
    if (m_playbackTimer && m_playbackTimer->isActive()) {
        m_playbackTimer->stop();
    }
}

void DicomPlaybackController::play()
{
    QMutexLocker locker(&m_stateMutex);
    
    if (m_totalFrames <= 1) {
        return;
    }
    
    if (m_playbackState == Playing) {
        return;
    }
    
    PlaybackState oldState = m_playbackState;
    
    // Start playback if we have at least some frames loaded
    if (m_loadedFrames > 0 || m_playbackState == Loading) {
        startPlaybackTimer();
        changePlaybackState(Playing);
        updateNavigationMode(Automatic);
        
        emit playbackStartRequested();
    } else {
    }
}

void DicomPlaybackController::pause()
{
    QMutexLocker locker(&m_stateMutex);
    
    if (m_playbackState != Playing) {
        return;
    }
    
    PlaybackState oldState = m_playbackState;
    stopPlaybackTimer();
    changePlaybackState(Paused);
    updateNavigationMode(Manual);
    
}

void DicomPlaybackController::stop()
{
    QMutexLocker locker(&m_stateMutex);
    
    PlaybackState oldState = m_playbackState;
    stopPlaybackTimer();
    changePlaybackState(Stopped);
    updateNavigationMode(Manual);
    
    // Reset to first frame
    if (m_currentFrame != 0) {
        m_currentFrame = 0;
        emit currentFrameChanged(m_currentFrame, m_totalFrames);
        emit frameRequested(m_currentFrame);
    }
    
    emit playbackStopRequested();
}

void DicomPlaybackController::togglePlayback()
{
    if (m_playbackState == Playing) {
        pause();
    } else if (m_playbackState == Paused || m_playbackState == Ready || m_playbackState == Stopped) {
        play();
    }
}

void DicomPlaybackController::nextFrame()
{
    QMutexLocker locker(&m_stateMutex);
    
    if (m_totalFrames <= 1) {
        return;
    }
    
    // If currently playing, pause first (manual navigation interrupts automatic playback)
    if (m_playbackState == Playing) {
        stopPlaybackTimer();
        changePlaybackState(Paused);
        updateNavigationMode(Manual);
    }
    
    int nextFrame = (m_currentFrame + 1) % m_totalFrames;
    
    if (canNavigateToFrame(nextFrame)) {
        m_currentFrame = nextFrame;
        emit currentFrameChanged(m_currentFrame, m_totalFrames);
        emit frameRequested(m_currentFrame);
        
    } else {
    }
}

void DicomPlaybackController::previousFrame()
{
    QMutexLocker locker(&m_stateMutex);
    
    if (m_totalFrames <= 1) {
        return;
    }
    
    // If currently playing, pause first (manual navigation interrupts automatic playback)
    if (m_playbackState == Playing) {
        stopPlaybackTimer();
        changePlaybackState(Paused);
        updateNavigationMode(Manual);
    }
    
    int prevFrame = (m_currentFrame - 1 + m_totalFrames) % m_totalFrames;
    
    if (canNavigateToFrame(prevFrame)) {
        m_currentFrame = prevFrame;
        emit currentFrameChanged(m_currentFrame, m_totalFrames);
        emit frameRequested(m_currentFrame);
        
    } else {
    }
}

void DicomPlaybackController::seekToFrame(int frameIndex)
{
    QMutexLocker locker(&m_stateMutex);
    
    if (frameIndex < 0 || frameIndex >= m_totalFrames) {
        return;
    }
    
    // Seeking always pauses automatic playback
    if (m_playbackState == Playing) {
        stopPlaybackTimer();
        changePlaybackState(Paused);
        updateNavigationMode(Manual);
    }
    
    if (canNavigateToFrame(frameIndex)) {
        m_currentFrame = frameIndex;
        emit currentFrameChanged(m_currentFrame, m_totalFrames);
        emit frameRequested(m_currentFrame);
        
    } else {
    }
}

void DicomPlaybackController::goToFirstFrame()
{
    seekToFrame(0);
}

void DicomPlaybackController::goToLastFrame()
{
    seekToFrame(m_totalFrames - 1);
}

void DicomPlaybackController::setPlaybackSpeed(double fps)
{
    QMutexLocker locker(&m_stateMutex);
    
    // Clamp FPS to reasonable bounds
    fps = qBound(MIN_FPS, fps, MAX_FPS);
    
    if (qAbs(fps - m_playbackSpeed) < 0.01) {
        return; // No significant change
    }
    
    m_playbackSpeed = fps;
    m_frameInterval = static_cast<int>(1000.0 / fps);
    
    // Update timer if currently playing
    if (m_playbackTimer && m_playbackTimer->isActive()) {
        m_playbackTimer->setInterval(m_frameInterval);
    }
    
    emit playbackSpeedChanged(m_playbackSpeed);
}

void DicomPlaybackController::setFrameInterval(int milliseconds)
{
    if (milliseconds > 0) {
        double fps = 1000.0 / milliseconds;
        setPlaybackSpeed(fps);
    }
}

void DicomPlaybackController::resetToDefaultSpeed()
{
    if (m_defaultFrameInterval > 0) {
        setFrameInterval(m_defaultFrameInterval);
    } else {
        setPlaybackSpeed(DEFAULT_FPS);
    }
}

void DicomPlaybackController::setTotalFrames(int totalFrames)
{
    QMutexLocker locker(&m_stateMutex);
    
    if (totalFrames != m_totalFrames) {
        m_totalFrames = totalFrames;
        m_currentFrame = 0;
        m_loadedFrames = 0;
        
        // Calculate optimal frame rate based on frame count
        calculateOptimalFrameRate(totalFrames);
        
        // Update state based on frame count
        if (totalFrames <= 1) {
            changePlaybackState(Ready);
        } else {
            changePlaybackState(Loading);
        }
        
        emit currentFrameChanged(m_currentFrame, m_totalFrames);
    }
}

void DicomPlaybackController::onFrameReady(int frameIndex)
{
    QMutexLocker locker(&m_stateMutex);
    
    if (frameIndex >= 0 && frameIndex < m_totalFrames) {
        m_loadedFrames = qMax(m_loadedFrames, frameIndex + 1);
        
        // Auto-start playback when first frame is ready (if policy allows)
        if (frameIndex == 0 && m_autoPlayPolicy == OnFirstFrame && m_totalFrames > 1) {
            if (m_playbackState == Loading) {
                // Start playback immediately for multiframe images
                locker.unlock(); // Unlock before calling other methods
                play();
                locker.relock();
            }
        }
        
        emit frameLoadingProgress(m_loadedFrames, m_totalFrames);
    }
}

void DicomPlaybackController::onAllFramesLoaded()
{
    QMutexLocker locker(&m_stateMutex);
    
    m_loadedFrames = m_totalFrames;
    
    // Update state based on total frames
    if (m_totalFrames <= 1) {
        changePlaybackState(Ready);
    } else if (m_playbackState == Loading) {
        changePlaybackState(Ready);
        
        // Auto-start playback if policy allows
        if (m_autoPlayPolicy == OnLoad) {
            locker.unlock(); // Unlock before calling other methods  
            play();
            locker.relock();
        }
    }
    
    emit allFramesReady();
}

void DicomPlaybackController::onLoadingStarted(int totalFrames)
{
    QMutexLocker locker(&m_stateMutex);
    
    setTotalFrames(totalFrames);
    changePlaybackState(Loading);
    
}

void DicomPlaybackController::clearFrames()
{
    QMutexLocker locker(&m_stateMutex);
    
    stopPlaybackTimer();
    changePlaybackState(Stopped);
    updateNavigationMode(Manual);
    
    m_currentFrame = 0;
    m_totalFrames = 0;
    m_loadedFrames = 0;
    
}

void DicomPlaybackController::onTimerTimeout()
{
    QMutexLocker locker(&m_stateMutex);
    
    if (m_playbackState != Playing || m_totalFrames <= 1) {
        return;
    }
    
    int nextFrame = m_currentFrame + 1;
    
    // Handle looping
    if (nextFrame >= m_totalFrames) {
        if (m_loopPlayback) {
            nextFrame = 0;
        } else {
            // End of sequence reached
            stopPlaybackTimer();
            changePlaybackState(Paused);
            updateNavigationMode(Manual);
            return;
        }
    }
    
    // Check if next frame is available
    if (canNavigateToFrame(nextFrame)) {
        m_currentFrame = nextFrame;
        emit currentFrameChanged(m_currentFrame, m_totalFrames);
        emit frameRequested(m_currentFrame);
    } else {
        // Next frame not ready - pause playback temporarily
        stopPlaybackTimer();
        changePlaybackState(Paused);
        updateNavigationMode(Manual);
    }
}

void DicomPlaybackController::changePlaybackState(PlaybackState newState)
{
    if (newState != m_playbackState) {
        PlaybackState oldState = m_playbackState;
        m_playbackState = newState;
        emit playbackStateChanged(oldState, newState);
    }
}

void DicomPlaybackController::updateNavigationMode(NavigationMode mode)
{
    if (mode != m_navigationMode) {
        m_navigationMode = mode;
    }
}

void DicomPlaybackController::startPlaybackTimer()
{
    if (m_playbackTimer) {
        m_playbackTimer->setInterval(m_frameInterval);
        m_playbackTimer->start();
    }
}

void DicomPlaybackController::stopPlaybackTimer()
{
    if (m_playbackTimer && m_playbackTimer->isActive()) {
        m_playbackTimer->stop();
    }
}

void DicomPlaybackController::calculateOptimalFrameRate(int totalFrames)
{
    // Use intelligent defaults based on frame count (matches Python implementation)
    double optimalFps;
    
    if (totalFrames > 100) {
        optimalFps = 30.0;
    } else if (totalFrames > 50) {
        optimalFps = 25.0;
    } else {
        optimalFps = 15.0;
    }
    
    // Don't override user-set speed unless we're at default
    if (qAbs(m_playbackSpeed - DEFAULT_FPS) < 0.01) {
        setPlaybackSpeed(optimalFps);
    }
}

bool DicomPlaybackController::canNavigateToFrame(int frameIndex) const
{
    // For now, assume all frames are available if loading is complete
    // In a more sophisticated implementation, this would check the frame cache
    return frameIndex >= 0 && frameIndex < m_totalFrames && 
           (m_playbackState == Ready || m_playbackState == Playing || m_playbackState == Paused || frameIndex <= m_loadedFrames);
}

// Configuration methods implementation
void DicomPlaybackController::setFrameRate(double fps)
{
    if (fps > 0) {
        setPlaybackSpeed(fps);
    }
}

void DicomPlaybackController::setTotalFrames(int totalFrames)
{
    QMutexLocker locker(&m_stateMutex);
    
    m_totalFrames = totalFrames;
    
    // Reset to first frame if current frame is out of bounds
    if (m_currentFrame >= totalFrames) {
        m_currentFrame = 0;
        emit currentFrameChanged(m_currentFrame, m_totalFrames);
    }
    
    // Calculate optimal frame rate based on total frames
    calculateOptimalFrameRate(totalFrames);
    
}

void DicomPlaybackController::setCurrentFrame(int frameIndex)
{
    QMutexLocker locker(&m_stateMutex);
    
    if (frameIndex >= 0 && frameIndex < m_totalFrames) {
        int oldFrame = m_currentFrame;
        m_currentFrame = frameIndex;
        
        emit currentFrameChanged(frameIndex, m_totalFrames);
        emit frameRequested(frameIndex);
        
    }
}
