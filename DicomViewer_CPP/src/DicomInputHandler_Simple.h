#pragma once

#include <QObject>
#include <QKeyEvent>
#include <QMouseEvent>

/**
 * Simplified DICOM input handler
 * Handles keyboard shortcuts for medical imaging applications
 */
class DicomInputHandler : public QObject
{
    Q_OBJECT

public:
    explicit DicomInputHandler(QObject *parent = nullptr);
    
    // Process input events
    bool processKeyEvent(QKeyEvent* event);
    bool processMouseEvent(QMouseEvent* event);

signals:
    // Playback control signals
    void playPauseRequested();
    void nextFrameRequested();
    void previousFrameRequested();
    void firstFrameRequested();
    void lastFrameRequested();
    
    // Navigation signals
    void nextImageRequested();
    void previousImageRequested();
    
    // Transform signals
    void horizontalFlipRequested();
    void verticalFlipRequested();
    void invertImageRequested();
    void resetAllRequested();
    
    // Zoom signals
    void zoomInRequested();
    void zoomOutRequested();
    void fitToWindowRequested();

private:
    void setupDefaultBindings();
    void handleKey(Qt::Key key, Qt::KeyboardModifiers modifiers);
};
