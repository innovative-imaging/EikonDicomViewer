#include "DicomInputHandler_Simple.h"
#include <QDebug>

DicomInputHandler::DicomInputHandler(QObject *parent)
    : QObject(parent)
{
    setupDefaultBindings();
}

void DicomInputHandler::setupDefaultBindings()
{
}

bool DicomInputHandler::processKeyEvent(QKeyEvent* event)
{
    if (!event) return false;
    
    Qt::Key key = static_cast<Qt::Key>(event->key());
    Qt::KeyboardModifiers modifiers = event->modifiers();
    
    handleKey(key, modifiers);
    return true; // We handled it
}

bool DicomInputHandler::processMouseEvent(QMouseEvent* event)
{
    Q_UNUSED(event)
    // Mouse handling can be added later if needed
    return false;
}

void DicomInputHandler::handleKey(Qt::Key key, Qt::KeyboardModifiers modifiers)
{
    // Handle Enter/Return for play/pause
    if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
                   (key == Qt::Key_Return ? "Return" : 
                    key == Qt::Key_Enter ? "Enter" : "Space");
        emit playPauseRequested();
        return;
    }
    
    // Handle Escape or Ctrl+R for reset
    if (key == Qt::Key_Escape || 
        (modifiers & Qt::ControlModifier && key == Qt::Key_R)) {
        emit resetAllRequested();
        return;
    }
    
    // Handle Ctrl modifier combinations
    if (modifiers & Qt::ControlModifier) {
        switch (key) {
        case Qt::Key_H:
            emit horizontalFlipRequested();
            return;
        case Qt::Key_V:
            emit verticalFlipRequested();
            return;
        case Qt::Key_I:
            emit invertImageRequested();
            return;
        }
    }
    
    // Handle arrow keys for frame and image navigation
    switch (key) {
    case Qt::Key_Left:
        // Only emit if we should handle frame navigation
        emit previousFrameRequested();
        return;
    case Qt::Key_Right:
        // Only emit if we should handle frame navigation  
        emit nextFrameRequested();
        return;
    case Qt::Key_Home:
        emit firstFrameRequested();
        return;
    case Qt::Key_End:
        emit lastFrameRequested();
        return;
    case Qt::Key_Up:
        emit previousImageRequested();
        return;
    case Qt::Key_Down:
        emit nextImageRequested();
        return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        emit zoomInRequested();
        return;
    case Qt::Key_Minus:
        emit zoomOutRequested();
        return;
    case Qt::Key_F:
        emit fitToWindowRequested();
        return;
    }
    
}
