#include "DicomInputHandler.h"
#include <QKeySequence>
#include <QDebug>

DicomInputHandler::DicomInputHandler(QObject *parent)
    : QObject(parent)
    , m_activeContext(GlobalContext)
    , m_windowingActive(false)
    , m_panningActive(false)
{
    initializeDefaultBindings();
    m_activeContexts.insert(GlobalContext);
    
}

void DicomInputHandler::setActiveContext(InputContext context)
{
    m_activeContext = context;
    m_activeContexts.clear();
    m_activeContexts.insert(GlobalContext); // Always include global context
    m_activeContexts.insert(context);
    
}

void DicomInputHandler::addContext(InputContext context)
{
    m_activeContexts.insert(context);
}

void DicomInputHandler::removeContext(InputContext context)
{
    if (context != GlobalContext) { // Never remove global context
        m_activeContexts.remove(context);
    }
}

void DicomInputHandler::setKeyBinding(KeyBinding action, const QKeySequence& sequence)
{
    m_keyBindings[action] = sequence;
}

QKeySequence DicomInputHandler::getKeyBinding(KeyBinding action) const
{
    return m_keyBindings.value(action, QKeySequence());
}

void DicomInputHandler::resetToDefaults()
{
    initializeDefaultBindings();
}

bool DicomInputHandler::processKeyEvent(QKeyEvent* event)
{
    if (!event) {
        return false;
    }
    
    // Create key sequence from the event
    QKeySequence eventSequence(event->key() | event->modifiers());
    
    // Check all active contexts for matching key bindings
    for (auto it = m_keyBindings.constBegin(); it != m_keyBindings.constEnd(); ++it) {
        if (it.value() == eventSequence) {
            return handleKeyAction(it.key(), event);
        }
    }
    
    // Handle some keys without modifiers for convenience
    switch (event->key()) {
    case Qt::Key_Space:
        if (m_activeContexts.contains(PlaybackContext) || m_activeContexts.contains(ImageContext)) {
            emit playPauseRequested();
            return true;
        }
        break;
        
    case Qt::Key_Left:
        if (m_activeContexts.contains(PlaybackContext) || m_activeContexts.contains(ImageContext)) {
            emit previousFrameRequested();
            return true;
        }
        break;
        
    case Qt::Key_Right:
        if (m_activeContexts.contains(PlaybackContext) || m_activeContexts.contains(ImageContext)) {
            emit nextFrameRequested();
            return true;
        }
        break;
        
    case Qt::Key_Up:
        if (m_activeContexts.contains(TreeContext)) {
            emit previousImageRequested();
            return true;
        }
        break;
        
    case Qt::Key_Down:
        if (m_activeContexts.contains(TreeContext)) {
            emit nextImageRequested();
            return true;
        }
        break;
    }
    
    return false;
}

bool DicomInputHandler::processMouseEvent(QMouseEvent* event)
{
    if (!event) {
        return false;
    }
    
    switch (event->type()) {
    case QEvent::MouseButtonPress:
        if (event->button() == Qt::RightButton) {
            // Start windowing
            m_windowingActive = true;
            m_lastMousePos = event->pos();
            emit windowingStartRequested(event->pos());
            return true;
        } else if (event->button() == Qt::MiddleButton) {
            // Start panning
            m_panningActive = true;
            m_lastMousePos = event->pos();
            emit panStartRequested(event->pos());
            return true;
        }
        break;
        
    case QEvent::MouseMove:
        if (m_windowingActive) {
            emit windowingUpdateRequested(event->pos());
            m_lastMousePos = event->pos();
            return true;
        } else if (m_panningActive) {
            emit panUpdateRequested(event->pos());
            m_lastMousePos = event->pos();
            return true;
        }
        break;
        
    case QEvent::MouseButtonRelease:
        if (event->button() == Qt::RightButton && m_windowingActive) {
            m_windowingActive = false;
            emit windowingEndRequested();
            return true;
        } else if (event->button() == Qt::MiddleButton && m_panningActive) {
            m_panningActive = false;
            emit panEndRequested();
            return true;
        }
        break;
        
    case QEvent::MouseButtonDblClick:
        if (event->button() == Qt::LeftButton) {
            emit fitToWindowRequested();
            return true;
        } else if (event->button() == Qt::RightButton) {
            emit resetWindowingRequested();
            return true;
        }
        break;
        
    default:
        break;
    }
    
    return false;
}

bool DicomInputHandler::processWheelEvent(QWheelEvent* event)
{
    if (!event) {
        return false;
    }
    
    // Mouse wheel for zooming
    if (event->angleDelta().y() > 0) {
        emit zoomInRequested();
    } else {
        emit zoomOutRequested();
    }
    
    return true;
}

bool DicomInputHandler::handleKeyAction(KeyBinding action, QKeyEvent* event)
{
    Q_UNUSED(event)
    
    switch (action) {
    // Playback controls
    case PlayPause:
        emit playPauseRequested();
        return true;
        
    case NextFrame:
        emit nextFrameRequested();
        return true;
        
    case PreviousFrame:
        emit previousFrameRequested();
        return true;
        
    case FirstFrame:
        emit firstFrameRequested();
        return true;
        
    case LastFrame:
        emit lastFrameRequested();
        return true;
        
    // Series/Image navigation
    case NextImage:
        emit nextImageRequested();
        return true;
        
    case PreviousImage:
        emit previousImageRequested();
        return true;
        
    case NextSeries:
        emit nextSeriesRequested();
        return true;
        
    case PreviousSeries:
        emit previousSeriesRequested();
        return true;
        
    // Transformations
    case HorizontalFlip:
        emit horizontalFlipRequested();
        return true;
        
    case VerticalFlip:
        emit verticalFlipRequested();
        return true;
        
    case InvertImage:
        emit invertImageRequested();
        return true;
        
    case ResetAll:
        emit resetAllRequested();
        return true;
        
    // Zoom and windowing
    case ZoomIn:
        emit zoomInRequested();
        return true;
        
    case ZoomOut:
        emit zoomOutRequested();
        return true;
        
    case FitToWindow:
        emit fitToWindowRequested();
        return true;
        
    case ResetWindowing:
        emit resetWindowingRequested();
        return true;
        
    default:
        return false;
    }
}

void DicomInputHandler::initializeDefaultBindings()
{
    // Playback controls
    m_keyBindings[PlayPause] = QKeySequence(Qt::Key_Return);
    m_keyBindings[NextFrame] = QKeySequence(Qt::Key_Right);
    m_keyBindings[PreviousFrame] = QKeySequence(Qt::Key_Left);
    m_keyBindings[FirstFrame] = QKeySequence(Qt::Key_Home);
    m_keyBindings[LastFrame] = QKeySequence(Qt::Key_End);
    
    // Series/Image navigation  
    m_keyBindings[NextImage] = QKeySequence(Qt::Key_Down);
    m_keyBindings[PreviousImage] = QKeySequence(Qt::Key_Up);
    m_keyBindings[NextSeries] = QKeySequence(Qt::ControlModifier | Qt::Key_Down);
    m_keyBindings[PreviousSeries] = QKeySequence(Qt::ControlModifier | Qt::Key_Up);
    
    // Transformations
    m_keyBindings[HorizontalFlip] = QKeySequence(Qt::ControlModifier | Qt::Key_H);
    m_keyBindings[VerticalFlip] = QKeySequence(Qt::ControlModifier | Qt::Key_V);
    m_keyBindings[InvertImage] = QKeySequence(Qt::ControlModifier | Qt::Key_I);
    m_keyBindings[ResetAll] = QKeySequence(Qt::Key_Escape);
    
    // Alternative reset binding
    // m_keyBindings[ResetAll] can also use Ctrl+R, but we'll keep Escape as primary
    
    // Zoom and windowing
    m_keyBindings[ZoomIn] = QKeySequence(Qt::Key_Plus);
    m_keyBindings[ZoomOut] = QKeySequence(Qt::Key_Minus);
    m_keyBindings[FitToWindow] = QKeySequence(Qt::ControlModifier | Qt::Key_0);
    m_keyBindings[ResetWindowing] = QKeySequence(Qt::ControlModifier | Qt::Key_W);
}

bool DicomInputHandler::isKeySequenceMatch(const QKeySequence& sequence, QKeyEvent* event)
{
    QKeySequence eventSequence(event->key() | event->modifiers());
    return sequence == eventSequence;
}
