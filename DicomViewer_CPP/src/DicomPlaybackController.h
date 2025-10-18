#pragma once

#include <QObject>
#include <QTimer>
#include <QPixmap>
#include <QMap>
#include <QMutex>

/**
 * @class DicomPlaybackController
 * @brief Professional DICOM multiframe playback controller
 * 
 * Manages playback state, frame navigation, and timing for DICOM image sequences.
 * Follows patterns used in commercial medical imaging viewers like OsiriX and Horos.
 */
class DicomPlaybackController : public QObject
{
    Q_OBJECT

public:
    enum PlaybackState {
        Stopped,        // No frames loaded or playback stopped
        Playing,        // Actively playing frames
        Paused,         // Playback paused by user
        Loading,        // Frames being loaded progressively
        Ready          // Frames loaded, ready to play
    };

    enum NavigationMode {
        Manual,         // User-controlled navigation (arrow keys, mouse)
        Automatic      // Timer-controlled automatic playback
    };

    enum AutoPlayPolicy {
        Never,              // Never auto-start playback
        OnFirstFrame,       // Start playback as soon as first frame is ready (current behavior)
        OnAllFramesLoaded   // Start playback when all frames are loaded
    };

    explicit DicomPlaybackController(QObject *parent = nullptr);
    ~DicomPlaybackController();
    
    // Auto-play policy accessors
    AutoPlayPolicy autoPlayPolicy() const { return m_autoPlayPolicy; }
    void setAutoPlayPolicy(AutoPlayPolicy policy) { m_autoPlayPolicy = policy; }

    // State queries
    PlaybackState playbackState() const { return m_playbackState; }
    NavigationMode navigationMode() const { return m_navigationMode; }
    int currentFrame() const { return m_currentFrame; }
    int totalFrames() const { return m_totalFrames; }
    double playbackSpeed() const { return m_playbackSpeed; }
    bool isPlaying() const { return m_playbackState == Playing; }
    bool isPaused() const { return m_playbackState == Paused; }
    bool hasFrames() const { return m_totalFrames > 0; }
    bool isMultiFrame() const { return m_totalFrames > 1; }

    // Configuration
    void setAutoPlayPolicy(AutoPlayPolicy policy) { m_autoPlayPolicy = policy; }
    AutoPlayPolicy autoPlayPolicy() const { return m_autoPlayPolicy; }
    void setLoopPlayback(bool loop) { m_loopPlayback = loop; }
    bool loopPlayback() const { return m_loopPlayback; }

signals:
    // Core playback signals
    void playbackStateChanged(PlaybackState oldState, PlaybackState newState);
    void currentFrameChanged(int frameIndex, int totalFrames);
    void playbackSpeedChanged(double fps);
    
    // Progress and loading signals
    void frameLoadingProgress(int loadedFrames, int totalFrames);
    void allFramesReady();
    
    // Navigation signals  
    void frameRequested(int frameIndex);
    void playbackStartRequested();
    void playbackStopRequested();

public slots:
    // Core playback control
    void play();
    void pause();
    void stop();
    void togglePlayback();
    
    // Frame navigation
    void nextFrame();
    void previousFrame();
    void goToFrame(int frameIndex);
    void goToFirstFrame();
    void goToLastFrame();
    
    // Configuration methods
    void setFrameRate(double fps);
    void setTotalFrames(int totalFrames);
    void setCurrentFrame(int frameIndex);
    
    // Speed and timing control
    void setPlaybackSpeed(double fps);
    void setFrameInterval(int milliseconds);
    void resetToDefaultSpeed();
    
    // Frame data management
    void setTotalFrames(int totalFrames);
    void onFrameReady(int frameIndex);
    void onAllFramesLoaded();
    void onLoadingStarted(int totalFrames);
    void clearFrames();

private slots:
    void onTimerTimeout();

private:
    // Internal state management
    void changePlaybackState(PlaybackState newState);
    void updateNavigationMode(NavigationMode mode);
    void startPlaybackTimer();
    void stopPlaybackTimer();
    void calculateOptimalFrameRate(int totalFrames);
    bool canNavigateToFrame(int frameIndex) const;
    
    // Playback state
    PlaybackState m_playbackState;
    NavigationMode m_navigationMode;
    AutoPlayPolicy m_autoPlayPolicy;
    
    // Frame management
    int m_currentFrame;
    int m_totalFrames;
    int m_loadedFrames;
    bool m_loopPlayback;
    AutoPlayPolicy m_autoPlayPolicy;
    
    // Timing control
    QTimer* m_playbackTimer;
    double m_playbackSpeed;        // frames per second
    int m_frameInterval;           // milliseconds between frames
    int m_defaultFrameInterval;    // default interval based on DICOM metadata
    
    // Thread safety
    mutable QMutex m_stateMutex;
    
    // Constants
    static constexpr double MIN_FPS = 0.5;
    static constexpr double MAX_FPS = 60.0;
    static constexpr double DEFAULT_FPS = 15.0;
};
