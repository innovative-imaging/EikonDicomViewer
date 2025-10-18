#pragma once

#include <QtCore/QThread>
#include <QtCore/QMutex>
#include <QtGui/QPixmap>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QMap>
#include <QtCore/QReadWriteLock>
#include "DicomFrameProcessor.h"

#ifdef HAVE_DCMTK
#include "dcmtk/dcmdata/dcfilefo.h"
#include "dcmtk/dcmdata/dcdeftag.h"
#include "dcmtk/dcmimgle/dcmimage.h"
#endif

class ProgressiveFrameLoader : public QThread
{
    Q_OBJECT

public:
    explicit ProgressiveFrameLoader(const QString& filePath, QObject* parent = nullptr);
    ~ProgressiveFrameLoader();
    
    void stop();
    bool isStopped() const;

signals:
    // Emitted when a single frame is ready (lightweight - no data transfer)
    void frameReady(int frameNumber);
    
    // Emitted when all frames have been loaded
    void allFramesLoaded(int totalFrames);
    
    // Emitted with first frame info for overlay setup
    void firstFrameInfo(QString patientName, QString patientId, int totalFrames);
    
    // Emitted when an error occurs
    void errorOccurred(const QString& errorMessage);
    
    // Emitted to update loading progress
    void loadingProgress(int currentFrame, int totalFrames);

public:
    // Thread-safe access to cached frames (eliminates signal data transfer)
    QPixmap getFramePixmap(int frameIndex) const;
    QByteArray getFrameOriginalData(int frameIndex) const;
    bool isFrameReady(int frameIndex) const;

protected:
    void run() override;

private:
    struct DicomMetadata {
        QString patientName;
        QString patientId;
        QString studyDescription;
        QString seriesDescription;
        double windowCenter = 0.0;
        double windowWidth = 0.0;
        int totalFrames = 1;
        unsigned long imageWidth = 0;
        unsigned long imageHeight = 0;
    };
    
    // Frame cache structure for thread-safe access
    struct FrameData {
        QPixmap pixmap;
        QByteArray originalData;
        bool isReady = false;
    };
    
    // Private methods
    bool loadDicomMetadata();
    QPixmap processFrame(int frameIndex);
    QByteArray extractOriginalPixelData(int frameIndex);
    void cacheFrame(int frameIndex, const QPixmap& pixmap, const QByteArray& originalData);
    
    // Member variables
    QString m_filePath;
    mutable QMutex m_mutex;
    bool m_stopped;
    DicomMetadata m_metadata;
    DicomFrameProcessor* m_frameProcessor;  // Use DicomFrameProcessor for GDCM support
    
    // Thread-safe frame cache (eliminates 350ms signal transfer delay)
    mutable QReadWriteLock m_frameCacheLock;
    QMap<int, FrameData> m_frameCache;
    
#ifdef HAVE_DCMTK
    DcmFileFormat* m_dcmFile;
    DcmDataset* m_dataset;
#endif
};
