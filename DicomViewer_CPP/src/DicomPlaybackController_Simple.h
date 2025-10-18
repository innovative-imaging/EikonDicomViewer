#pragma once

#include <QObject>
#include <QTimer>
#include <QMutex>

/**
 * Simplified professional DICOM playback controller
 * Focuses on core functionality without complex dependencies
 */
class DicomPlaybackController : public QObject
{
    Q_OBJECT

public:
    enum PlaybackState {
        Stopped,
        Playing, 
        Paused,
        Ready
    };

    enum AutoPlayPolicy {
        Never,
        OnFirstFrame,
        OnAllFramesLoaded
    };

    explicit DicomPlaybackController(QObject *parent = nullptr);
    ~DicomPlaybackController();

    // State queries
    PlaybackState state() const { return m_state; }
    bool isPlaying() const { return m_state == Playing; }
    int currentFrame() const { return m_currentFrame; }
    int totalFrames() const { return m_totalFrames; }
    
    // Configuration
    AutoPlayPolicy autoPlayPolicy() const { return m_autoPlayPolicy; }
    void setAutoPlayPolicy(AutoPlayPolicy policy) { m_autoPlayPolicy = policy; }
    void setFrameRate(double fps);
    void setTotalFrames(int totalFrames);
    void setCurrentFrame(int frameIndex);

signals:
    void playbackStateChanged(PlaybackState oldState, PlaybackState newState);
    void currentFrameChanged(int frameIndex, int totalFrames);
    void frameRequested(int frameIndex);

public slots:
    void play();
    void pause();
    void stop();
    void togglePlayback();
    void nextFrame();
    void previousFrame();
    void goToFirstFrame();
    void goToLastFrame();

private slots:
    void onTimerTimeout();

private:
    void changeState(PlaybackState newState);
    
    // Core state
    PlaybackState m_state;
    int m_currentFrame;
    int m_totalFrames;
    AutoPlayPolicy m_autoPlayPolicy;
    
    // Timing
    QTimer* m_playbackTimer;
    double m_frameRate;
    
    // Thread safety
    mutable QMutex m_mutex;
};
