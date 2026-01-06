#include "DicomFrameProcessor.h"
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <chrono>

DicomFrameProcessor::DicomFrameProcessor()
    : m_rawPixelData(nullptr)
    , m_currentFrame(0)
    , m_rows(0)
    , m_cols(0)
    , m_bitsAllocated(8)
    , m_bitsStored(8)
    , m_highBit(7)
    , m_pixelRepresentation(0)
    , m_numberOfFrames(1)
    , m_defaultWindowCenter(0.0)     // Medical imaging default instead of 8-bit display default
    , m_defaultWindowWidth(2000.0)   // Medical imaging default instead of 8-bit display default
    , m_rescaleSlope(1.0)
    , m_rescaleIntercept(0.0)
    , m_useGdcmMode(false)
    , m_batchDecompressed(false)
#ifdef HAVE_DCMTK
    , m_fileFormat(nullptr)
#endif
#ifdef HAVE_GDCM
    , m_gdcmReader(nullptr)
    , m_gdcmImage(nullptr)
#endif
{
}

DicomFrameProcessor::~DicomFrameProcessor()
{
#ifdef HAVE_DCMTK
    delete m_fileFormat;
#endif

#ifdef HAVE_GDCM
    delete m_gdcmReader;
    delete m_gdcmImage;
#endif
}
    
bool DicomFrameProcessor::loadDicomFile(const QString& filePath)
{
#ifdef HAVE_DCMTK
    auto loadStart = std::chrono::high_resolution_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(loadStart.time_since_epoch()).count();
    
    try {
        // Clean up any existing data
        delete m_fileFormat;
        m_fileFormat = nullptr;
        m_rawPixelData = nullptr;
        
        // CRITICAL: Clean up batch decompression state when loading new file
        m_batchDecompressed = false;
        m_preDecompressedFrames.clear();
        
        // Clear frame cache when loading new file
        m_frameCache.clear();
        
#ifdef HAVE_GDCM
        // Clean up GDCM resources properly
        delete m_gdcmReader;
        m_gdcmReader = nullptr;
        delete m_gdcmImage;
        m_gdcmImage = nullptr;
        m_gdcmPixelBuffer.clear();
#endif
        
        // Store the current file path for frame access
        m_currentFilePath = filePath;
        
        // Load DICOM file
        m_fileFormat = new DcmFileFormat();
        OFCondition status = m_fileFormat->loadFile(filePath.toLocal8Bit().constData());
        
        if (status.bad()) {
            delete m_fileFormat;
            m_fileFormat = nullptr;
            return false;
        }
        
        // Extract metadata
        if (!extractMetadata()) {
            delete m_fileFormat;
            m_fileFormat = nullptr;
            return false;
        }
        
        // Check transfer syntax for debugging and TurboJPEG compatibility
        auto syntaxStart = std::chrono::high_resolution_clock::now();
        auto syntaxTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(syntaxStart.time_since_epoch()).count();
        
        OFString transferSyntax;
        if (m_fileFormat->getMetaInfo()->findAndGetOFString(DCM_TransferSyntaxUID, transferSyntax).good()) {
            
            // Check specific JPEG formats and select optimal decoder
            if (transferSyntax == "1.2.840.10008.1.2.4.70") {
#ifdef HAVE_GDCM
                auto gdcmInitStart = std::chrono::high_resolution_clock::now();
                auto gdcmTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(gdcmInitStart.time_since_epoch()).count();
                
                if (initializeGdcm(filePath)) {
                    m_useGdcmMode = true;
                } else {
                    m_useGdcmMode = false;
                }
#else
                m_useGdcmMode = false;
#endif
            } else if (transferSyntax == "1.2.840.10008.1.2.4.50") {
#ifdef HAVE_TURBOJPEG
#endif
            } else if (transferSyntax == "1.2.840.10008.1.2.4.51") {
#ifdef HAVE_TURBOJPEG
#endif
            } else if (transferSyntax.find("1.2.840.10008.1.2.4") != OFString_npos) {
#ifdef HAVE_GDCM
#endif
            } else if (transferSyntax == "1.2.840.10008.1.2") {
            } else if (transferSyntax == "1.2.840.10008.1.2.1") {
            } else {
            }
        } else {
            m_useGdcmMode = false;
        }
        
        auto loadEnd = std::chrono::high_resolution_clock::now();
        auto loadDuration = std::chrono::duration_cast<std::chrono::milliseconds>(loadEnd - loadStart).count();
        auto endTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(loadEnd.time_since_epoch()).count();
        
        // For GDCM mode with multiple frames, pre-decompress all frames for optimal performance
        if (m_useGdcmMode && m_numberOfFrames > 1) {
            if (preDecompressAllFrames()) {
            }
        }
        
        return true;
        
    } catch (const std::exception& e) {
        delete m_fileFormat;
        m_fileFormat = nullptr;
        return false;
    } catch (...) {
        delete m_fileFormat;
        m_fileFormat = nullptr;
        return false;
    }
#else
    Q_UNUSED(filePath)
    return false;
#endif
}
    
QImage DicomFrameProcessor::getFrameAsQImage(unsigned long frameNumber)
{
#ifdef HAVE_DCMTK
    auto totalStart = std::chrono::high_resolution_clock::now();
    
    if (!m_fileFormat || frameNumber >= m_numberOfFrames) {
        return QImage();
    }
    
    // Check batch decompressed frames first (fastest path)
    if (m_batchDecompressed && 
        frameNumber < static_cast<unsigned long>(m_preDecompressedFrames.size()) &&
        frameNumber < m_numberOfFrames) {
        
        // Additional safety check for valid frame
        const QImage& batchFrame = m_preDecompressedFrames[frameNumber];
        if (!batchFrame.isNull()) {
            auto batchTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(totalStart.time_since_epoch()).count();
            return batchFrame;
        } else {
        }
    }
    
    // Check frame cache second for performance
    if (m_frameCache.contains(frameNumber)) {
        const CachedFrame& cachedFrame = m_frameCache[frameNumber];
        if (cachedFrame.isValid) {
            auto cacheTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(totalStart.time_since_epoch()).count();
            return cachedFrame.image;
        }
    }
    
    try {
        auto frameTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(totalStart.time_since_epoch()).count();
        
        // **CRITICAL FIX**: Use GDCM decompression when GDCM mode is active
        if (m_useGdcmMode) {
#ifdef HAVE_GDCM
            
            unsigned char* frameBuffer = nullptr;
            unsigned long outputSize = 0;
            if (decompressGdcmFrame(frameNumber, &frameBuffer, &outputSize)) {
                // Create QImage from GDCM-decompressed data
                QImage frameImage(m_cols, m_rows, QImage::Format_Grayscale8);
                memcpy(frameImage.bits(), frameBuffer, m_cols * m_rows);
                
                auto totalEnd = std::chrono::high_resolution_clock::now();
                auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart).count();
                auto totalTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd.time_since_epoch()).count();
                
                return frameImage;
            } else {
                // Fall through to DCMTK processing
            }
#else
            return QImage();
#endif
        }
        
        // DCMTK processing (original code or GDCM fallback)
        auto stepStart = std::chrono::high_resolution_clock::now();
        // Use DicomImage constructor that takes file path for better compressed data handling
        // This creates a fresh DicomImage instance for each frame, ensuring proper decompression
        QString currentPath = m_currentFilePath;
        
        DicomImage* dicomImage = nullptr;
        
        if (m_numberOfFrames > 1) {
            // For multi-frame images, specify the frame number
            dicomImage = new DicomImage(currentPath.toLocal8Bit().constData(), 
                                      CIF_AcrNemaCompatibility, frameNumber, frameNumber + 1);
        } else {
            // For single frame images
            dicomImage = new DicomImage(currentPath.toLocal8Bit().constData(), CIF_AcrNemaCompatibility);
        }
        
        auto createTime = std::chrono::high_resolution_clock::now();
        auto createDuration = std::chrono::duration_cast<std::chrono::milliseconds>(createTime - stepStart).count();
        auto createTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(createTime.time_since_epoch()).count();
        
        if (dicomImage == nullptr) {
            return QImage();
        }
        
        EI_Status status = dicomImage->getStatus();
        if (status != EIS_Normal) {
            delete dicomImage;
            return QImage();
        }
        
        auto statusTime = std::chrono::high_resolution_clock::now();
        auto statusDuration = std::chrono::duration_cast<std::chrono::milliseconds>(statusTime - createTime).count();
        auto statusTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(statusTime.time_since_epoch()).count();
        
        // Verify the image dimensions match our metadata
        unsigned long imageWidth = dicomImage->getWidth();
        unsigned long imageHeight = dicomImage->getHeight();
        
        if (imageWidth != m_cols || imageHeight != m_rows) {
            // Image dimension mismatch
            delete dicomImage;
            return QImage();
        }
        
        // SKIP DCMTK windowing - get raw 8-bit data without windowing
        // We'll apply windowing at display level using the image pipeline
        // if (dicomImage->setWindow(m_defaultWindowCenter, m_defaultWindowWidth) == 0) {
        // }
        
        auto windowTime = std::chrono::high_resolution_clock::now();
        auto windowDuration = std::chrono::duration_cast<std::chrono::milliseconds>(windowTime - statusTime).count();
        auto windowTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(windowTime.time_since_epoch()).count();
        
        // Get the processed pixel data as 8-bit grayscale - THIS IS THE DECOMPRESSION STEP
        auto decompressionStart = std::chrono::high_resolution_clock::now();
        const void* pixelData = dicomImage->getOutputData(8 /* bits per sample */);
        auto decompressionEnd = std::chrono::high_resolution_clock::now();
        auto decompressionDuration = std::chrono::duration_cast<std::chrono::milliseconds>(decompressionEnd - decompressionStart).count();
        auto decompressionTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(decompressionEnd.time_since_epoch()).count();
        
        if (pixelData == nullptr) {
            delete dicomImage;
            return QImage();
        }
        
        // Create QImage from the processed data - OPTIMIZED: use direct buffer when possible
        auto qimageStart = std::chrono::high_resolution_clock::now();
        const unsigned char* srcData = static_cast<const unsigned char*>(pixelData);
        
        // Try to create QImage directly from buffer to avoid memcpy (zero-copy optimization)
        QImage frameImage;
        if (dicomImage->getOutputDataSize(8) == imageWidth * imageHeight) {
            // Direct buffer usage - no memory copy needed
            frameImage = QImage(srcData, imageWidth, imageHeight, imageWidth, QImage::Format_Grayscale8);
            // Create a deep copy to ensure data persistence after DicomImage deletion
            frameImage = frameImage.copy();
        } else {
            // Fallback to memcpy if sizes don't match
            frameImage = QImage(imageWidth, imageHeight, QImage::Format_Grayscale8);
            memcpy(frameImage.bits(), srcData, imageWidth * imageHeight);
        }
        
        auto qimageEnd = std::chrono::high_resolution_clock::now();
        auto qimageDuration = std::chrono::duration_cast<std::chrono::milliseconds>(qimageEnd - qimageStart).count();
        auto qimageTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(qimageEnd.time_since_epoch()).count();
        
        m_currentFrame = frameNumber;
        
        // Cache the processed frame for future use
        if (m_frameCache.size() >= MAX_CACHED_FRAMES) {
            // Remove oldest cached frame
            auto oldestIt = std::min_element(m_frameCache.begin(), m_frameCache.end(),
                [](const CachedFrame& a, const CachedFrame& b) {
                    return a.timestamp < b.timestamp;
                });
            if (oldestIt != m_frameCache.end()) {
                m_frameCache.erase(oldestIt);
            }
        }
        
        CachedFrame& cache = m_frameCache[frameNumber];
        cache.image = frameImage.copy(); // Deep copy for cache safety
        cache.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(totalStart.time_since_epoch()).count();
        cache.isValid = true;
        
        delete dicomImage;
        
        auto totalEnd = std::chrono::high_resolution_clock::now();
        auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart).count();
        auto finalTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd.time_since_epoch()).count();
        
        return frameImage;
        
    } catch (const std::exception& e) {
        return QImage();
    } catch (...) {
        return QImage();
    }
#else
    Q_UNUSED(frameNumber)
    return QImage();
#endif
}
    
QImage DicomFrameProcessor::applyWindowingAndCreateQImage(double windowCenter, double windowWidth)
{
    if (!m_rawPixelData) {
        return QImage();
    }
    
    QImage image(m_cols, m_rows, QImage::Format_Grayscale8);
    unsigned char* outputPixels = image.bits();
    
    // Calculate window bounds
    double minValue = windowCenter - windowWidth / 2.0;
    double maxValue = windowCenter + windowWidth / 2.0;
    double range = maxValue - minValue;
    
    if (range == 0) range = 1.0; // Avoid division by zero
    
    unsigned long pixelCount = m_cols * m_rows;
    
#ifdef HAVE_DCMTK
    if (m_bitsAllocated == 8) {
        // 8-bit pixel data
        Uint8* pixels = m_rawPixelData;
        
        for (unsigned long i = 0; i < pixelCount; ++i) {
            double pixelValue = pixels[i] * m_rescaleSlope + m_rescaleIntercept;
            outputPixels[i] = applyWindowLevel(pixelValue, minValue, maxValue, range);
        }
    } 
    else if (m_bitsAllocated == 16) {
        // 16-bit pixel data
        if (m_pixelRepresentation == 0) {
            // Unsigned 16-bit
            Uint16* pixels = reinterpret_cast<Uint16*>(m_rawPixelData);
            
            for (unsigned long i = 0; i < pixelCount; ++i) {
                double pixelValue = pixels[i] * m_rescaleSlope + m_rescaleIntercept;
                outputPixels[i] = applyWindowLevel(pixelValue, minValue, maxValue, range);
            }
        } else {
            // Signed 16-bit
            Sint16* pixels = reinterpret_cast<Sint16*>(m_rawPixelData);
            
            for (unsigned long i = 0; i < pixelCount; ++i) {
                double pixelValue = pixels[i] * m_rescaleSlope + m_rescaleIntercept;
                outputPixels[i] = applyWindowLevel(pixelValue, minValue, maxValue, range);
            }
        }
    }
#endif
    
    return image;
}

QImage DicomFrameProcessor::createQImageWithDefaultWindowing()
{
    return applyWindowingAndCreateQImage(m_defaultWindowCenter, m_defaultWindowWidth);
}
    
bool DicomFrameProcessor::extractMetadata()
{
#ifdef HAVE_DCMTK
    try {
        DcmDataset* dataset = m_fileFormat->getDataset();
        if (!dataset) {
            return false;
        }
        
        // Get image parameters
        Uint16 tempValue;
        if (dataset->findAndGetUint16(DCM_Rows, tempValue).good()) {
            m_rows = tempValue;
        } else {
            return false;
        }
        
        if (dataset->findAndGetUint16(DCM_Columns, tempValue).good()) {
            m_cols = tempValue;
        } else {
            return false;
        }
        
        if (dataset->findAndGetUint16(DCM_BitsAllocated, tempValue).good()) {
            m_bitsAllocated = tempValue;
        } else {
            m_bitsAllocated = 16; // Default
        }
        
        if (dataset->findAndGetUint16(DCM_BitsStored, tempValue).good()) {
            m_bitsStored = tempValue;
        } else {
            m_bitsStored = m_bitsAllocated;
        }
        
        if (dataset->findAndGetUint16(DCM_HighBit, tempValue).good()) {
            m_highBit = tempValue;
        } else {
            m_highBit = m_bitsStored - 1;
        }
        
        if (dataset->findAndGetUint16(DCM_PixelRepresentation, tempValue).good()) {
            m_pixelRepresentation = tempValue;
        } else {
            m_pixelRepresentation = 0; // Default unsigned
        }
        
        // Get number of frames
        OFString numFramesStr;
        m_numberOfFrames = 1;
        if (dataset->findAndGetOFString(DCM_NumberOfFrames, numFramesStr).good()) {
            if (!numFramesStr.empty()) {
                m_numberOfFrames = atoi(numFramesStr.c_str());
            }
        }
        
        // Get default window center/width from DICOM tags
        OFString wcStr, wwStr;
        if (dataset->findAndGetOFString(DCM_WindowCenter, wcStr).good()) {
            if (!wcStr.empty()) {
                m_defaultWindowCenter = atof(wcStr.c_str());
            }
        } else {
            // Use medical imaging defaults instead of 8-bit display defaults
            m_defaultWindowCenter = 0.0;      // Center for general medical images
        }
        
        if (dataset->findAndGetOFString(DCM_WindowWidth, wwStr).good()) {
            if (!wwStr.empty()) {
                m_defaultWindowWidth = atof(wwStr.c_str());
            }
        } else {
            // Use medical imaging defaults instead of 8-bit display defaults  
            m_defaultWindowWidth = 2000.0;    // Width for general medical data
        }
        
        // Always use DICOM window/level values as specified in the file
        // These values are set by medical imaging professionals and should be respected
        
        // Get rescale slope and intercept (for CT, etc.)
        OFString slopeStr, interceptStr;
        m_rescaleSlope = 1.0;
        m_rescaleIntercept = 0.0;
        
        if (dataset->findAndGetOFString(DCM_RescaleSlope, slopeStr).good()) {
            if (!slopeStr.empty()) {
                m_rescaleSlope = atof(slopeStr.c_str());
            }
        }
        
        if (dataset->findAndGetOFString(DCM_RescaleIntercept, interceptStr).good()) {
            if (!interceptStr.empty()) {
                m_rescaleIntercept = atof(interceptStr.c_str());
            }
        }
        
        return true;
        
    } catch (const std::exception& e) {
        return false;
    } catch (...) {
        return false;
    }
#else
    return false;
#endif
}

inline unsigned char DicomFrameProcessor::applyWindowLevel(double pixelValue, double minValue, 
                                                          double maxValue, double range)
{
    if (pixelValue <= minValue) {
        return 0;
    } else if (pixelValue >= maxValue) {
        return 255;
    } else {
        return static_cast<unsigned char>(((pixelValue - minValue) / range) * 255.0);
    }
}

#ifdef HAVE_GDCM
bool DicomFrameProcessor::initializeGdcm(const QString& filePath)
{
    auto gdcmStart = std::chrono::high_resolution_clock::now();
    auto startTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(gdcmStart.time_since_epoch()).count();
    
    try {
        // Clean up any existing GDCM objects
        auto cleanupStart = std::chrono::high_resolution_clock::now();
        delete m_gdcmReader;
        delete m_gdcmImage;
        auto cleanupEnd = std::chrono::high_resolution_clock::now();
        auto cleanupDuration = std::chrono::duration_cast<std::chrono::milliseconds>(cleanupEnd - cleanupStart).count();
        
        // Initialize GDCM reader
        auto readerStart = std::chrono::high_resolution_clock::now();
        auto readerTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(readerStart.time_since_epoch()).count();
        
        m_gdcmReader = new gdcm::ImageReader();
        m_gdcmReader->SetFileName(filePath.toLocal8Bit().constData());
        
        // Try to read the file
        auto readStart = std::chrono::high_resolution_clock::now();
        bool readResult = m_gdcmReader->Read();
        auto readEnd = std::chrono::high_resolution_clock::now();
        auto readDuration = std::chrono::duration_cast<std::chrono::milliseconds>(readEnd - readStart).count();
        auto readTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(readEnd.time_since_epoch()).count();
        
        
        if (!readResult) {
            delete m_gdcmReader;
            m_gdcmReader = nullptr;
            return false;
        }
        
        // Get the image object
        auto imageStart = std::chrono::high_resolution_clock::now();
        auto imageTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(imageStart.time_since_epoch()).count();
        
        m_gdcmImage = new gdcm::Image(m_gdcmReader->GetImage());
        
        // Verify this is a compressed JPEG format that GDCM can handle
        auto verifyStart = std::chrono::high_resolution_clock::now();
        const gdcm::PhotometricInterpretation& pi = m_gdcmImage->GetPhotometricInterpretation();
        const gdcm::PixelFormat& pf = m_gdcmImage->GetPixelFormat();
        
        // Check if GDCM can decompress this format
        unsigned int numFrames = m_gdcmImage->GetNumberOfDimensions() > 2 ? 
                                m_gdcmImage->GetDimension(2) : 1;
        
        auto verifyEnd = std::chrono::high_resolution_clock::now();
        auto verifyDuration = std::chrono::duration_cast<std::chrono::milliseconds>(verifyEnd - verifyStart).count();
        auto verifyTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(verifyEnd.time_since_epoch()).count();
        
        
        // Don't pre-allocate pixel buffer - use lazy loading for better initial performance
        auto bufferStart = std::chrono::high_resolution_clock::now();
        size_t bufferSize = m_gdcmImage->GetBufferLength();
        // m_gdcmPixelBuffer.resize(bufferSize);  // Remove pre-allocation
        auto bufferEnd = std::chrono::high_resolution_clock::now();
        auto bufferDuration = std::chrono::duration_cast<std::chrono::milliseconds>(bufferEnd - bufferStart).count();
        auto bufferTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(bufferEnd.time_since_epoch()).count();
        
        
        auto gdcmEnd = std::chrono::high_resolution_clock::now();
        auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(gdcmEnd - gdcmStart).count();
        auto endTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(gdcmEnd.time_since_epoch()).count();
        
        return true;
        
    } catch (const std::exception& e) {
        auto errorTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        delete m_gdcmReader;
        delete m_gdcmImage;
        m_gdcmReader = nullptr;
        m_gdcmImage = nullptr;
        return false;
    } catch (...) {
        auto errorTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        delete m_gdcmReader;
        delete m_gdcmImage;
        m_gdcmReader = nullptr;
        m_gdcmImage = nullptr;
        return false;
    }
}

bool DicomFrameProcessor::decompressGdcmFrame(unsigned long frameNumber, 
                                            unsigned char** outputBuffer, 
                                            unsigned long* outputSize)
{
    if (!m_gdcmImage || !m_gdcmReader) {
        return false;
    }
    
    try {
        auto gdcmStart = std::chrono::high_resolution_clock::now();
        auto stepStart = std::chrono::high_resolution_clock::now();
        // Calculate frame size
        const gdcm::PixelFormat& pf = m_gdcmImage->GetPixelFormat();
        unsigned int bytesPerPixel = pf.GetBitsAllocated() / 8;
        *outputSize = m_rows * m_cols * bytesPerPixel;
        
        // Allocate output buffer
        *outputBuffer = new unsigned char[*outputSize];
        
        auto allocTime = std::chrono::high_resolution_clock::now();
        auto allocDuration = std::chrono::duration_cast<std::chrono::milliseconds>(allocTime - stepStart).count();
        
        // Extract the specific frame using GDCM
        auto decompressStart = std::chrono::high_resolution_clock::now();
        if (m_numberOfFrames > 1) {
            // Multi-frame: lazy decompression approach
            size_t frameSize = *outputSize;
            
            // Check if we need to decompress the entire sequence
            if (m_gdcmPixelBuffer.empty()) {
                auto decompressionStart = std::chrono::high_resolution_clock::now();
                
                // Allocate buffer for all frames
                m_gdcmPixelBuffer.resize(m_gdcmImage->GetBufferLength());
                
                // Decompress all frames (GDCM limitation for JPEG Lossless)
                if (!m_gdcmImage->GetBuffer(&m_gdcmPixelBuffer[0])) {
                    delete[] *outputBuffer;
                    *outputBuffer = nullptr;
                    return false;
                }
                
                auto decompressionEnd = std::chrono::high_resolution_clock::now();
                auto decompressionDuration = std::chrono::duration_cast<std::chrono::milliseconds>(decompressionEnd - decompressionStart).count();
            }
            
            auto copyStart = std::chrono::high_resolution_clock::now();
            // Copy the specific frame from the pre-decompressed buffer
            char* frameData = &m_gdcmPixelBuffer[frameNumber * frameSize];
            memcpy(*outputBuffer, frameData, frameSize);
            auto copyEnd = std::chrono::high_resolution_clock::now();
            auto copyDuration = std::chrono::duration_cast<std::chrono::milliseconds>(copyEnd - copyStart).count();
        } else {
            // Single frame: direct decompression
            if (!m_gdcmImage->GetBuffer(reinterpret_cast<char*>(*outputBuffer))) {
                delete[] *outputBuffer;
                *outputBuffer = nullptr;
                return false;
            }
        }
        auto decompressEnd = std::chrono::high_resolution_clock::now();
        auto decompressDuration = std::chrono::duration_cast<std::chrono::milliseconds>(decompressEnd - decompressStart).count();
        
        auto gdcmEnd = std::chrono::high_resolution_clock::now();
        auto gdcmTotalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(gdcmEnd - gdcmStart).count();
        
        return true;
        
    } catch (const std::exception& e) {
        if (*outputBuffer) {
            delete[] *outputBuffer;
            *outputBuffer = nullptr;
        }
        return false;
    } catch (...) {
        if (*outputBuffer) {
            delete[] *outputBuffer;
            *outputBuffer = nullptr;
        }
        return false;
    }
}
#endif

bool DicomFrameProcessor::preDecompressAllFrames()
{
#ifdef HAVE_GDCM
    if (!m_useGdcmMode || m_numberOfFrames <= 1) {
        return false;
    }
    
    // Safety check: ensure GDCM resources are valid
    if (!m_gdcmReader || !m_gdcmImage) {
        return false;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    try {
        // Clear and prepare storage
        m_preDecompressedFrames.clear();
        m_preDecompressedFrames.resize(m_numberOfFrames);
        
        // Safety check: validate frame count
        if (m_numberOfFrames > 1000) { // Reasonable safety limit
            return false;
        }
        
        // Decompress all frames at once using GDCM's batch capability
        for (unsigned long i = 0; i < m_numberOfFrames; ++i) {
            // Safety check: ensure we don't go out of bounds
            if (i >= static_cast<unsigned long>(m_preDecompressedFrames.size())) {
                return false;
            }
            
            unsigned char* frameBuffer = nullptr;
            unsigned long outputSize = 0;
            
            try {
                if (decompressGdcmFrame(i, &frameBuffer, &outputSize)) {
                    // Validate frame buffer and size
                    if (!frameBuffer || outputSize < m_cols * m_rows) {
                        if (frameBuffer) delete[] frameBuffer;
                        return false;
                    }
                    
                    // Convert to QImage and store
                    QImage frameImage(m_cols, m_rows, QImage::Format_Grayscale8);
                    memcpy(frameImage.bits(), frameBuffer, m_cols * m_rows);
                    m_preDecompressedFrames[i] = frameImage.copy();
                    
                    // Clean up
                    delete[] frameBuffer;
                    frameBuffer = nullptr;
                } else {
                    return false;
                }
            } catch (const std::exception& e) {
                if (frameBuffer) delete[] frameBuffer;
                return false;
            } catch (...) {
                if (frameBuffer) delete[] frameBuffer;
                return false;
            }
        }
        
        m_batchDecompressed = true;
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        // Batch decompression completed
        
        return true;
        
    } catch (const std::exception& e) {
        return false;
    }
#else
    return false;
#endif
}

QString DicomFrameProcessor::getDicomTagValue(const QString& tag) const
{
#ifdef HAVE_DCMTK
    if (!m_fileFormat) return QString();
    
    DcmDataset* dataset = m_fileFormat->getDataset();
    if (!dataset) return QString();
    
    // Parse tag string (e.g., "0020,0013")
    QStringList parts = tag.split(',');
    if (parts.size() != 2) return QString();
    
    bool ok1, ok2;
    unsigned int group = parts[0].toUInt(&ok1, 16);
    unsigned int element = parts[1].toUInt(&ok2, 16);
    
    if (!ok1 || !ok2) return QString();
    
    DcmTag dcmTag(group, element);
    DcmElement* dcmElement = nullptr;
    
    if (dataset->findAndGetElement(dcmTag, dcmElement).good() && dcmElement) {
        OFString value;
        if (dcmElement->getOFString(value, 0).good()) {
            return QString::fromLatin1(value.c_str());
        }
    }
    
    return QString();
#else
    Q_UNUSED(tag)
    return QString();
#endif
}
