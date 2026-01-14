#ifndef DICOMREADER_H
#define DICOMREADER_H

#include "dicomviewer.h"  // Include for LogLevel enum
#include <QString>
#include <QStringList>
#include <QMap>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QIcon>

#ifdef HAVE_DCMTK
#include "dcmtk/dcmdata/dcdicdir.h"
#include "dcmtk/dcmdata/dcdirrec.h"
#include "dcmtk/dcmdata/dcelem.h"
#include "dcmtk/dcmdata/dcdeftag.h"
#include "dcmtk/ofstd/ofstd.h"
#endif

class QFile;

struct DicomImageInfo {
    QString filePath;
    int instanceNumber = 0;
    int frameCount = 1;
    bool fileExists = true;
    bool isDirectory = false; // True if filePath points to a directory containing DICOM files
    QString displayName; // Optional display name (e.g., "SR DOC 1" for structured reports)
};

struct DicomSeriesInfo {
    QString seriesUID;
    QString seriesNumber;
    QString seriesDescription;
    QList<DicomImageInfo> images;
    QStringList reports;
};

struct DicomStudyInfo {
    QString studyUID;
    QString studyDate;
    QString studyDescription;
    QMap<QString, DicomSeriesInfo> series;
};

struct DicomPatientInfo {
    QString patientID;
    QString patientName;
    QMap<QString, DicomStudyInfo> studies;
};

class DicomReader
{
public:
    DicomReader();
    ~DicomReader();
    
    // Main methods
    bool loadDicomDir(const QString& dicomdirPath);
    void populateTreeWidget(QTreeWidget* treeWidget);
    
    // Getters
    int getTotalPatients() const { return m_patients.size(); }
    int getTotalImages() const { return m_totalImages; }
    QString getLastError() const { return m_lastError; }
    
    // Utility methods
    static QString cleanDicomText(const QString& text);
    static QString formatDate(const QString& dicomDate);
    
    // Copy monitoring support
    void refreshFileExistenceStatus();
    void startProactiveCopyMonitoring();
    double calculateProgress() const;
    
    // Get image info for a specific file
    DicomImageInfo getImageInfoForFile(const QString& filePath) const;
    
    // Get frame count from DICOM file
    int getFrameCountFromFile(const QString& filePath);
    
    // Update frame count for a specific file when it becomes available
    void updateFrameCountForFile(const QString& fileName);
    
    // Additional public methods for RDSR support
    void updateImageDisplayNameFromFile(DicomImageInfo& image);
    bool isRDSRFile(const QString& filePath) const;

private:
    QMap<QString, DicomPatientInfo> m_patients;
    int m_totalImages;
    QString m_lastError;
    QString m_basePath;
    
    // Private methods
    bool isDicomDir(const QString& filePath);
    void clearData();
    bool isStructuredReport(const QString& filePath);
    
private:
    
#ifdef HAVE_DCMTK
    bool parseWithDcmtk(const QString& dicomdirPath);
#endif
    
    bool parseDirectoryRecordSequenceSimple(QFile& file, quint32 sequenceLength);
    void parseDirectoryRecordSequence(QFile& file, quint32 sequenceLength, 
                                     QMap<QString, DicomPatientInfo>& patients,
                                     QString& currentPatientID, QString& currentStudyUID, QString& currentSeriesUID);
    QString readDicomString(QFile& file, quint32 length);
    quint32 readDicomInteger(QFile& file, quint32 length);
    QString extractSeriesDescriptionFromFile(const QString& filePath);
    bool parseWithSimpleMethod(const QString& dicomdirPath);
    QList<DicomImageInfo> expandDirectoryEntries(const QList<DicomImageInfo>& images);
    
    // Fallback basic parsing
    bool loadWithBasicParsing(const QString& dicomdirPath);
};

#endif // DICOMREADER_H
