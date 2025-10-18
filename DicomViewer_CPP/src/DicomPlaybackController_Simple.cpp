#include "DicomPlaybackController_Simple.h"
#include <QDebug>
#include <QMutexLocker>
#include <chrono>

DicomPlaybackController::DicomPlaybackController(QObject *parent)
    : QObject(parent)
    , m_state(Stopped)
    , m_currentFrame(0)
    , m_totalFrames(0)
    , m_autoPlayPolicy(Never)
    , m_playbackTimer(new QTimer(this))
    , m_frameRate(15.0)
{
    m_playbackTimer->setSingleShot(false);
    connect(m_playbackTimer, &QTimer::timeout, this, &DicomPlaybackController::onTimerTimeout);
    
    // Default interval for 15 FPS
    m_playbackTimer->setInterval(static_cast<int>(1000.0 / m_frameRate));
}

DicomPlaybackController::~DicomPlaybackController()
{
    if (m_playbackTimer && m_playbackTimer->isActive()) {
        m_playbackTimer->stop();
    }
}

void DicomPlaybackController::setFrameRate(double fps)
{
    if (fps > 0 && fps <= 60) {
        QMutexLocker locker(&m_mutex);
        m_frameRate = fps;
        int interval = static_cast<int>(1000.0 / fps);
        m_playbackTimer->setInterval(interval);
    }
}

void DicomPlaybackController::setTotalFrames(int totalFrames)
{
    QMutexLocker locker(&m_mutex);
    m_totalFrames = totalFrames;
    
    if (m_currentFrame >= totalFrames && totalFrames > 0) {
        m_currentFrame = 0;
        emit currentFrameChanged(m_currentFrame, m_totalFrames);
    }
    
}

void DicomPlaybackController::setCurrentFrame(int frameIndex)
{
    QMutexLocker locker(&m_mutex);
    
    if (frameIndex >= 0 && frameIndex < m_totalFrames) {
        m_currentFrame = frameIndex;
        emit currentFrameChanged(m_currentFrame, m_totalFrames);
        emit frameRequested(m_currentFrame);
    }
}

void DicomPlaybackController::play()
{
    QMutexLocker locker(&m_mutex);
    
    if (m_totalFrames <= 1) {
        return;
    }
    
    if (m_state != Playing) {
        PlaybackState oldState = m_state;
        m_state = Playing;
        
        m_playbackTimer->start();
        
        locker.unlock();
        emit playbackStateChanged(oldState, m_state);
    }
}

void DicomPlaybackController::pause()
{
    QMutexLocker locker(&m_mutex);
    
    if (m_state == Playing) {
        PlaybackState oldState = m_state;
        m_state = Paused;
        
        m_playbackTimer->stop();
        
        locker.unlock();
        emit playbackStateChanged(oldState, m_state);
    }
}

void DicomPlaybackController::stop()
{
    QMutexLocker locker(&m_mutex);
    
    if (m_state != Stopped) {
        PlaybackState oldState = m_state;
        m_state = Stopped;
        
        m_playbackTimer->stop();
        m_currentFrame = 0;
        
        locker.unlock();
        emit playbackStateChanged(oldState, m_state);
        emit currentFrameChanged(m_currentFrame, m_totalFrames);
        emit frameRequested(m_currentFrame);
    }
}

void DicomPlaybackController::togglePlayback()
{
    if (isPlaying()) {
        pause();
    } else {
        play();
    }
}

void DicomPlaybackController::nextFrame()
{
    QMutexLocker locker(&m_mutex);
    
    if (m_totalFrames <= 1) return;
    
    // Pause if currently playing
    if (m_state == Playing) {
        m_playbackTimer->stop();
        changeState(Paused);
    }
    
    int nextFrame = (m_currentFrame + 1) % m_totalFrames;
    m_currentFrame = nextFrame;
    
    locker.unlock();
    emit currentFrameChanged(m_currentFrame, m_totalFrames);
    emit frameRequested(m_currentFrame);
}

void DicomPlaybackController::previousFrame()
{
    QMutexLocker locker(&m_mutex);
    
    if (m_totalFrames <= 1) return;
    
    // Pause if currently playing
    if (m_state == Playing) {
        m_playbackTimer->stop();
        changeState(Paused);
    }
    
    int prevFrame = (m_currentFrame - 1 + m_totalFrames) % m_totalFrames;
    m_currentFrame = prevFrame;
    
    locker.unlock();
    emit currentFrameChanged(m_currentFrame, m_totalFrames);
    emit frameRequested(m_currentFrame);
}

void DicomPlaybackController::goToFirstFrame()
{
    setCurrentFrame(0);
}

void DicomPlaybackController::goToLastFrame()
{
    if (m_totalFrames > 0) {
        setCurrentFrame(m_totalFrames - 1);
    }
}

void DicomPlaybackController::onTimerTimeout()
{
    // Fast path: avoid mutex when not playing
    if (m_state != Playing || m_totalFrames <= 1) {
        return;
    }
    
    QMutexLocker locker(&m_mutex);
    
    // Double-check state after acquiring mutex
    if (m_state == Playing && m_totalFrames > 1) {
        static std::chrono::steady_clock::time_point lastFpsLog;
        static int frameCount = 0;
        static bool fpsTimerInitialized = false;
        
        // Initialize FPS timer on first run
        if (!fpsTimerInitialized) {
            lastFpsLog = std::chrono::steady_clock::now();
            fpsTimerInitialized = true;
        }
        
        int nextFrame = (m_currentFrame + 1) % m_totalFrames;
        m_currentFrame = nextFrame;
        frameCount++;
        
        // Optimized FPS logging every 5 seconds (reduced frequency for better performance)
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastFpsLog);
        
        if (elapsed.count() >= 5000) { // 5 seconds
            double actualFPS = frameCount * 1000.0 / elapsed.count();
            // FPS debug calculation
            lastFpsLog = currentTime;
            frameCount = 0;
        }
        
        // Emit signals without mutex to reduce lock time
        locker.unlock();
        emit currentFrameChanged(m_currentFrame, m_totalFrames);
        emit frameRequested(m_currentFrame);
    }
}

void DicomPlaybackController::changeState(PlaybackState newState)
{
    if (m_state != newState) {
        PlaybackState oldState = m_state;
        m_state = newState;
        emit playbackStateChanged(oldState, newState);
    }
}
