#pragma once

#include <QObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QSet>

/**
 * @class DicomInputHandler
 * @brief Professional input handling for DICOM viewers
 * 
 * Handles keyboard shortcuts, mouse actions, and gesture inputs for medical imaging applications.
 * Follows industry standard key bindings used in commercial DICOM viewers.
 */
class DicomInputHandler : public QObject
{
    Q_OBJECT

public:
    enum InputContext {
        GlobalContext,      // Always active shortcuts
        ImageContext,       // Active when image is displayed
        TreeContext,        // Active when tree has focus
        PlaybackContext    // Active during multiframe playback
    };

    enum KeyBinding {
        // Playback controls
        PlayPause,          // Space, Enter
        NextFrame,          // Right arrow, Page Down
        PreviousFrame,      // Left arrow, Page Up
        FirstFrame,         // Home
        LastFrame,          // End
        
        // Series/Image navigation
        NextImage,          // Down arrow, Ctrl+Right
        PreviousImage,      // Up arrow, Ctrl+Left
        NextSeries,         // Ctrl+Down
        PreviousSeries,     // Ctrl+Up
        
        // Transformations
        HorizontalFlip,     // Ctrl+H
        VerticalFlip,       // Ctrl+V
        InvertImage,        // Ctrl+I
        ResetAll,           // Esc, Ctrl+R
        
        // Zoom and windowing
        ZoomIn,             // Plus, Ctrl+Plus
        ZoomOut,            // Minus, Ctrl+Minus
        FitToWindow,        // Ctrl+0, F
        ResetWindowing     // Ctrl+W
    };

    explicit DicomInputHandler(QObject *parent = nullptr);
    ~DicomInputHandler() = default;

    // Context management
    void setActiveContext(InputContext context);
    InputContext activeContext() const { return m_activeContext; }
    void addContext(InputContext context);
    void removeContext(InputContext context);

    // Key binding management
    void setKeyBinding(KeyBinding action, const QKeySequence& sequence);
    QKeySequence getKeyBinding(KeyBinding action) const;
    void resetToDefaults();

    // Event processing
    bool processKeyEvent(QKeyEvent* event);
    bool processMouseEvent(QMouseEvent* event);
    bool processWheelEvent(QWheelEvent* event);

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
    void nextSeriesRequested();
    void previousSeriesRequested();
    
    // Transformation signals
    void horizontalFlipRequested();
    void verticalFlipRequested();
    void invertImageRequested();
    void resetAllRequested();
    
    // Zoom and windowing signals
    void zoomInRequested();
    void zoomOutRequested();
    void fitToWindowRequested();
    void resetWindowingRequested();
    
    // Mouse interaction signals
    void windowingStartRequested(const QPoint& startPos);
    void windowingUpdateRequested(const QPoint& currentPos);
    void windowingEndRequested();
    void panStartRequested(const QPoint& startPos);
    void panUpdateRequested(const QPoint& currentPos);
    void panEndRequested();

private:
    void initializeDefaultBindings();
    bool isKeySequenceMatch(const QKeySequence& sequence, QKeyEvent* event);
    bool handleKeyAction(KeyBinding action, QKeyEvent* event);
    
    // Input context state
    InputContext m_activeContext;
    QSet<InputContext> m_activeContexts;
    
    // Key bindings
    QMap<KeyBinding, QKeySequence> m_keyBindings;
    
    // Mouse interaction state
    bool m_windowingActive;
    bool m_panningActive;
    QPoint m_lastMousePos;
    Qt::MouseButtons m_activeButtons;
};
