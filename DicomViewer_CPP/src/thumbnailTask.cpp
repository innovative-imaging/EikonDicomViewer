#include "thumbnailTask.h"
#include "dicomviewer.h"
#include "DicomFrameProcessor.h"

#include <QtCore/QFileInfo>
#include <QtCore/QFile>
#include <QtCore/QMutexLocker>
#include <QtWidgets/QTreeWidgetItem>
#include <QtGui/QPainter>
#include <QtGui/QFont>
#include <QtGui/QFontMetrics>

ThumbnailTask::ThumbnailTask(const QString& filePath, DicomViewer* viewer, QObject* parent)
    : QObject(parent), QRunnable(), m_filePath(filePath), m_viewer(viewer)
{
    setAutoDelete(true); // Task will be automatically deleted when finished
}

void ThumbnailTask::run() 
{
    try {
        // Check if file is accessible before trying to generate thumbnail
        QFileInfo fileInfo(m_filePath);
        if (!fileInfo.exists()) {
            logMessage("WARN", QString("Skipping thumbnail generation for missing file: %1").arg(m_filePath));
            emit taskCompleted(m_filePath, QPixmap(), "1");
            return;
        }
        
        // For thumbnail generation, we only need read access - allow read-only files
        QFile testFile(m_filePath);
        if (!testFile.open(QIODevice::ReadOnly)) {
            logMessage("WARN", QString("Skipping thumbnail generation for inaccessible file: %1").arg(m_filePath));
            emit taskCompleted(m_filePath, QPixmap(), "1");
            return;
        }
        testFile.close();
        
        // Check file readiness before processing
        {
            QMutexLocker fileLocker(&m_viewer->m_fileStatesMutex);
            QString filename = fileInfo.fileName();
            if (m_viewer->m_copyInProgress && !m_viewer->m_fileReadyStates.value(filename, false)) {
                logMessage("DEBUG", QString("Skipping file not ready: %1").arg(filename));
                emit taskCompleted(m_filePath, QPixmap(), "1");
                return;
            }
        }
        
        // For DVD autorun scenario: Check if file is still being copied
        QString filename = fileInfo.fileName();
        bool fileIsCompleted = m_viewer->m_fullyCompletedFiles.contains(filename);
        
        if (m_viewer->m_copyInProgress && !fileIsCompleted) {
            logMessage("DEBUG", QString("Skipping thumbnail generation for file still being copied: %1").arg(filename));
            emit taskCompleted(m_filePath, QPixmap(), "1");
            return;
        }
        
        // Generate thumbnail using the viewer's logic
        QPixmap thumbnail;
        QString instanceNumber = "1";
        
        // Determine item type from tree widget
        QString itemType = "image"; // Default
        QTreeWidgetItemIterator it(m_viewer->m_dicomTree);
        while (*it) {
            QVariantList userData = (*it)->data(0, Qt::UserRole).toList();
            if (userData.size() >= 2 && userData[1].toString() == m_filePath) {
                itemType = userData[0].toString();
                break;
            }
            ++it;
        }
        
        if (itemType == "report") {
            // Generate special thumbnail for reports
            try {
                thumbnail = m_viewer->createReportThumbnail(m_filePath);
                instanceNumber = "RPT";
            } catch (...) {
                logMessage("ERROR", QString("Error creating report thumbnail for: %1").arg(m_filePath));
                emit taskCompleted(m_filePath, QPixmap(), "1");
                return;
            }
        } else if (m_viewer->m_dicomReader) {
            // Protect all DCMTK operations with mutex
            QMutexLocker dcmtkLocker(&m_viewer->m_dcmtkAccessMutex);
            
            // Generate DICOM thumbnail using viewer's existing logic
            QPixmap originalPixmap;
            try {
                originalPixmap = m_viewer->convertDicomFrameToPixmap(m_filePath, 0);
            } catch (...) {
                logMessage("ERROR", QString("Error converting DICOM frame to pixmap for: %1").arg(m_filePath));
                emit taskCompleted(m_filePath, QPixmap(), "1");
                return;
            }
            
            if (!originalPixmap.isNull()) {
                // Create thumbnail using viewer's existing thumbnail creation logic
                thumbnail = createDicomThumbnail(originalPixmap, m_filePath, instanceNumber);
            }
        }
        
        // Emit completion signal
        emit taskCompleted(m_filePath, thumbnail, instanceNumber);
        
    } catch (const std::exception& e) {
        logMessage("ERROR", QString("Error generating thumbnail for %1: %2").arg(m_filePath).arg(e.what()));
        emit taskCompleted(m_filePath, QPixmap(), "1");
    }
}

QPixmap ThumbnailTask::createDicomThumbnail(const QPixmap& originalPixmap, const QString& filePath, QString& instanceNumber) 
{
    // Scale image to fit the new smaller thumbnail size
    QPixmap scaledPixmap = originalPixmap.scaled(180, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    
    QPixmap finalThumbnail(190, 150);
    finalThumbnail.fill(QColor(42, 42, 42));
    
    QPainter painter(&finalThumbnail);
    painter.setRenderHint(QPainter::Antialiasing);
    
    // Center and draw the scaled image
    QRect imageRect((finalThumbnail.width() - scaledPixmap.width()) / 2, 15,
                   scaledPixmap.width(), scaledPixmap.height());
    painter.drawPixmap(imageRect, scaledPixmap);
    
    // Extract instance number and frame count from DICOM metadata
    int frameCount = 1;
    
    try {
        DicomFrameProcessor tempProcessor;
        if (tempProcessor.loadDicomFile(filePath)) {
            frameCount = static_cast<int>(tempProcessor.getNumberOfFrames());
            
            // Get actual Instance Number from DICOM tag (0020,0013)
            QString dicomInstanceNumber = tempProcessor.getDicomTagValue("0020,0013");
            if (!dicomInstanceNumber.isEmpty()) {
                instanceNumber = dicomInstanceNumber;
            }
        }
    } catch (...) {
        logMessage("WARN", QString("Error reading DICOM metadata for thumbnail, using defaults: %1").arg(filePath));
    }
    
    // Create top overlay bar with background
    painter.fillRect(0, 0, finalThumbnail.width(), 20, QColor(0, 0, 0, 180));
    
    // Draw frame count at top-left
    painter.setPen(QColor(255, 255, 255));
    painter.setFont(QFont("Arial", 11, QFont::Bold));
    painter.drawText(5, 14, QString("%1").arg(frameCount));
    
    // Draw instance number in center
    QString centerText = instanceNumber;
    QFontMetrics fm(painter.font());
    int textWidth = fm.horizontalAdvance(centerText);
    painter.drawText((finalThumbnail.width() - textWidth) / 2, 14, centerText);
    
    // Load and draw appropriate PNG icon at top-right
    QString iconPath = (frameCount > 1) 
        ? "DicomViewer_CPP/resources/icons/AcquisitionHeader.png"  // Multi-frame
        : "DicomViewer_CPP/resources/icons/Camera.png";           // Single frame
    
    QPixmap iconPixmap(iconPath);
    if (iconPixmap.isNull()) {
        // Try absolute path if relative doesn't work
        QString absoluteIconPath = (frameCount > 1) 
            ? "d:/Repos/EikonDicomViewer/DicomViewer_CPP/resources/icons/AcquisitionHeader.png"
            : "d:/Repos/EikonDicomViewer/DicomViewer_CPP/resources/icons/Camera.png";
        iconPixmap.load(absoluteIconPath);
    }
    
    if (!iconPixmap.isNull()) {
        // Scale icon to proper size (16x16 for top overlay)
        QPixmap scaledIcon = iconPixmap.scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        painter.drawPixmap(finalThumbnail.width() - 20, 2, scaledIcon);
        logMessage("DEBUG", QString("Icon loaded successfully: %1").arg(iconPath));
    } else {
        // Fallback: draw a simple text icon
        painter.setPen(QColor(100, 149, 237));
        painter.setFont(QFont("Arial", 10, QFont::Bold));
        QString fallbackIcon = (frameCount > 1) ? "M" : "S";
        painter.drawText(finalThumbnail.width() - 18, 14, fallbackIcon);
        logMessage("WARN", QString("Icon failed to load, using fallback: %1").arg(iconPath));
    }
    
    painter.end();
    return finalThumbnail;
}

void ThumbnailTask::logMessage(const QString& level, const QString& message) 
{
    if (m_viewer) {
        m_viewer->logMessage(level, message);
    }
}