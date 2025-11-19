#pragma once

#include <QImage>
#include <QString>
#include <QMap>
#include <memory>
#include <chrono>
#include <algorithm>

#ifdef HAVE_DCMTK
#include "dcmtk/config/osconfig.h"
#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmdata/dcfilefo.h"
#include "dcmtk/dcmdata/dcdeftag.h"
#include "dcmtk/dcmdata/dcpixel.h"
#include "dcmtk/dcmdata/dcpixseq.h"
#include "dcmtk/dcmdata/dcpxitem.h"
#include "dcmtk/dcmimgle/dcmimage.h"
#endif

#ifdef HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

#ifdef HAVE_GDCM
#include "gdcmImageReader.h"
#include "gdcmImage.h"
#include "gdcmDataSet.h"
#include "gdcmFile.h"
#include "gdcmPhotometricInterpretation.h"
#include "gdcmPixelFormat.h"
#include "gdcmAttribute.h"
#endif

/**
 * @brief High-performance DICOM frame processor with direct pixel data access
 * 
 * This class provides efficient access to DICOM multi-frame image data by:
 * - Loading DICOM file once and keeping it in memory
 * - Providing direct pointer access to individual frame pixel data
 * - Applying window/level adjustments without copying pixel data
 * - Supporting both 8-bit and 16-bit DICOM images
 */
class DicomFrameProcessor
{
public:
    DicomFrameProcessor();
    ~DicomFrameProcessor();

    /**
     * @brief Load DICOM file and prepare for frame access
     * @param filePath Path to the DICOM file
     * @return true if successful, false otherwise
     */
    bool loadDicomFile(const QString& filePath);

    /**
     * @brief Get direct access to pixel data for a specific frame
     * @param frameNumber Frame number (0-based)
     * @return true if successful, false otherwise
     */
    bool getFramePixelData(unsigned long frameNumber);

    /**
     * @brief Get a specific frame as QImage with proper DCMTK handling
     * @param frameNumber Frame number (0-based)
     * @return QImage ready for display, or null QImage on error
     */
    QImage getFrameAsQImage(unsigned long frameNumber);

    /**
     * @brief Apply windowing and create QImage for display
     * @param windowCenter Window center value
     * @param windowWidth Window width value
     * @return QImage ready for display
     */
    QImage applyWindowingAndCreateQImage(double windowCenter, double windowWidth);

    /**
     * @brief Create QImage with default DICOM windowing values
     * @return QImage ready for display
     */
    QImage createQImageWithDefaultWindowing();

    // Getters for DICOM properties
    unsigned long getNumberOfFrames() const { return m_numberOfFrames; }
    unsigned int getWidth() const { return m_cols; }
    unsigned int getHeight() const { return m_rows; }
    double getDefaultWindowCenter() const { return m_defaultWindowCenter; }
    double getDefaultWindowWidth() const { return m_defaultWindowWidth; }
    unsigned long getCurrentFrame() const { return m_currentFrame; }
    
    // Get DICOM tag value as string
    QString getDicomTagValue(const QString& tag) const;
    
    // Check if processor is ready
    bool isValid() const { return m_fileFormat != nullptr && m_rawPixelData != nullptr; }

private:
#ifdef HAVE_DCMTK
    DcmFileFormat* m_fileFormat;
#endif
    
    unsigned char* m_rawPixelData;
    QString m_currentFilePath;
    
    // Image properties
    unsigned int m_rows;
    unsigned int m_cols;
    unsigned int m_bitsAllocated;
    unsigned int m_bitsStored;
    unsigned int m_highBit;
    unsigned int m_pixelRepresentation; // 0 = unsigned, 1 = signed
    unsigned long m_numberOfFrames;
    unsigned long m_currentFrame;
    
    // Windowing parameters
    double m_defaultWindowCenter;
    double m_defaultWindowWidth;
    double m_rescaleSlope;
    double m_rescaleIntercept;
    
    // Performance mode flags
    bool m_useGdcmMode;
    
    // Frame-level decompression cache to avoid repeated decompression
    struct CachedFrame {
        QImage image;
        qint64 timestamp;
        bool isValid;
        CachedFrame() : timestamp(0), isValid(false) {}
    };
    
    mutable QMap<unsigned long, CachedFrame> m_frameCache;
    static constexpr int MAX_CACHED_FRAMES = 20; // Limit cache size
    
    // Batch decompression for GDCM mode
    bool m_batchDecompressed;
    QVector<QImage> m_preDecompressedFrames;

#ifdef HAVE_TURBOJPEG
    // Hybrid performance mode members
    tjhandle m_tjInstance;
    bool m_useHybridMode;
    bool m_isCompressedJpeg;
    
    struct JpegFrameInfo {
        unsigned char* jpegData;
        unsigned long jpegSize;
        bool extracted;
        
        JpegFrameInfo() : jpegData(nullptr), jpegSize(0), extracted(false) {}
        ~JpegFrameInfo() { if (jpegData) delete[] jpegData; }
    };
    
    std::vector<std::unique_ptr<JpegFrameInfo>> m_jpegFrames;
    
    /**
     * @brief Extract JPEG data from DICOM pixel sequence for hybrid mode
     */
    bool extractJpegFrames();
    
    /**
     * @brief Decompress JPEG data using TurboJPEG for maximum performance
     */
    bool decompressJpegFrame(int frameNumber, unsigned char** outputBuffer, 
                            unsigned long* outputSize);
#endif

#ifdef HAVE_GDCM
    // GDCM performance mode members
    gdcm::ImageReader* m_gdcmReader;
    gdcm::Image* m_gdcmImage;
    std::vector<char> m_gdcmPixelBuffer;
    
    /**
     * @brief Initialize GDCM reader for accelerated decompression
     */
    bool initializeGdcm(const QString& filePath);
    
    /**
     * @brief Decompress frame using GDCM for JPEG Lossless acceleration
     */
    bool decompressGdcmFrame(unsigned long frameNumber, unsigned char** outputBuffer, 
                            unsigned long* outputSize);
#endif

    /**
     * @brief Apply window/level to a single pixel value
     */
    inline unsigned char applyWindowLevel(double pixelValue, double minValue, 
                                          double maxValue, double range);

    /**
     * @brief Extract DICOM metadata needed for processing
     */
    bool extractMetadata();
    
    /**
     * @brief Pre-decompress all frames for optimal GDCM performance
     */
    bool preDecompressAllFrames();
};
