#include "dicomreader.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QTreeWidgetItem>
#include <QColor>
#include <QDate>
#include <QtEndian>
#include <QRegularExpression>

#ifdef HAVE_DCMTK
#include "dcmtk/config/osconfig.h"
#include "dcmtk/dcmdata/dcdicdir.h"
#include "dcmtk/dcmdata/dcdirrec.h"
#include "dcmtk/dcmdata/dcdeftag.h"
#include "dcmtk/dcmdata/dcfilefo.h"
#include "dcmtk/ofstd/ofstd.h"
#endif

DicomReader::DicomReader()
    : m_totalImages(0)
{
}

DicomReader::~DicomReader()
{
}

void DicomReader::clearData()
{
    m_patients.clear();
    m_totalImages = 0;
    m_lastError.clear();
    m_basePath.clear();
}

QString DicomReader::cleanDicomText(const QString& text)
{
    if (text.isEmpty()) {
        return "N/A";
    }
    return QString(text).replace('^', ' ').trimmed();
}

QString DicomReader::formatDate(const QString& dicomDate)
{
    if (dicomDate.length() >= 8) {
        return QString("%1.%2.%3")
            .arg(dicomDate.mid(6, 2))  // DD
            .arg(dicomDate.mid(4, 2))  // MM
            .arg(dicomDate.mid(0, 4)); // YYYY
    }
    return dicomDate;
}

bool DicomReader::isDicomDir(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    
    // Skip the 128-byte preamble
    file.seek(128);
    
    // Read the DICM prefix
    QByteArray dicmPrefix = file.read(4);
    if (dicmPrefix != "DICM") {
        return false;
    }
    
    // For now, assume it's a DICOMDIR if it has the DICM prefix
    // More sophisticated checking could be done here
    return true;
}

bool DicomReader::loadDicomDir(const QString& dicomdirPath)
{
    clearData();
    
    if (!QFile::exists(dicomdirPath)) {
        m_lastError = "File does not exist: " + dicomdirPath;
        return false;
    }
    
    m_basePath = QFileInfo(dicomdirPath).absolutePath();
    
    // Check if it's a valid DICOM file
    if (!isDicomDir(dicomdirPath)) {
        m_lastError = "Not a valid DICOM file: " + dicomdirPath;
        return false;
    }
    
    return parseWithDcmtk(dicomdirPath);
}



void DicomReader::populateTreeWidget(QTreeWidget* treeWidget)
{
    if (!treeWidget) return;
    
    qDebug() << "##### populateTreeWidget() called - clearing and repopulating tree #####";
    
    treeWidget->clear();
    
    // Update header
    treeWidget->setHeaderLabel(QString("All patients (Patients: %1, Images: %2)")
                              .arg(getTotalPatients())
                              .arg(getTotalImages()));
    
    // Populate patients
    for (auto patientIt = m_patients.constBegin(); patientIt != m_patients.constEnd(); ++patientIt) {
        const DicomPatientInfo& patient = patientIt.value();
        
        QTreeWidgetItem* patientItem = new QTreeWidgetItem(QStringList() << patient.patientName);
        patientItem->setData(0, Qt::UserRole, QVariantList() << "patient" << patient.patientID);
        patientItem->setIcon(0, QIcon(":/icons/Doctor.png"));
        treeWidget->addTopLevelItem(patientItem);
        
        // Add studies
        for (auto studyIt = patient.studies.constBegin(); studyIt != patient.studies.constEnd(); ++studyIt) {
            const DicomStudyInfo& study = studyIt.value();
            
            QString formattedDate = formatDate(study.studyDate);
            QString studyDesc = study.studyDescription;
            QString displayText = QString("%1 (%2 series) - %3")
                                 .arg(studyDesc)
                                 .arg(study.series.size())
                                 .arg(formattedDate);
            
            QTreeWidgetItem* studyItem = new QTreeWidgetItem(QStringList() << displayText);
            studyItem->setData(0, Qt::UserRole, QVariantList() << "study" << study.studyUID);
            studyItem->setIcon(0, QIcon(":/icons/List.png"));
            patientItem->addChild(studyItem);
            
            // Add series
            for (auto seriesIt = study.series.constBegin(); seriesIt != study.series.constEnd(); ++seriesIt) {
                const DicomSeriesInfo& series = seriesIt.value();
                
                // Show ALL series, even those with 0 images
                QString seriesDesc = series.seriesDescription;
                QString seriesDisplayText = QString("%1 (%2 images)")
                                           .arg(seriesDesc)
                                           .arg(series.images.size());
                
                QTreeWidgetItem* seriesItem = new QTreeWidgetItem(QStringList() << seriesDisplayText);
                seriesItem->setData(0, Qt::UserRole, QVariantList() << "series" << series.seriesUID);
                seriesItem->setIcon(0, QIcon(":/icons/GeneralList.png"));
                studyItem->addChild(seriesItem);
                
                // Add images (sorted by instance number)
                QList<DicomImageInfo> sortedImages = series.images;
                std::sort(sortedImages.begin(), sortedImages.end(), 
                         [](const DicomImageInfo& a, const DicomImageInfo& b) {
                             return a.instanceNumber < b.instanceNumber;
                         });
                
                // Skip directory expansion for now - just use the actual DICOM files as listed
                // QList<DicomImageInfo> expandedImages = expandDirectoryEntries(sortedImages);
                
                int imageIndex = 0;  // Track image index for fallback naming
                for (const DicomImageInfo& image : sortedImages) {
                    imageIndex++;
                    // Generate proper display names that work even when files don't exist yet
                    QString displayName;
                    if (!image.displayName.isEmpty() && image.displayName.startsWith("SR DOC")) {
                        // Keep SR DOC names as they are meaningful
                        displayName = image.displayName;
                    } else {
                        // Extract meaningful filename from file path, even if file doesn't exist yet
                        QFileInfo pathInfo(image.filePath);
                        QString filename = pathInfo.fileName();
                        
                        // Debug: Show what we're getting - CRITICAL DEBUG
                        qDebug() << "************************ TREE POPULATION DEBUG ************************";
                        qDebug() << "[TREE FILENAME DEBUG]" 
                                 << "FilePath:" << image.filePath 
                                 << "Filename:" << filename 
                                 << "DisplayName:" << image.displayName
                                 << "FileExists:" << image.fileExists;
                        qDebug() << "************************ END TREE DEBUG ************************";
                        
                        // ALWAYS use the actual filename from the path if we have one
                        // Don't check file existence - we want to show the real filename even if file doesn't exist yet
                        if (!filename.isEmpty() && filename != "DICOMFiles" && filename != "DICOMDIR" && 
                            !filename.endsWith(".") && filename.length() > 3) {
                            // Use the actual DICOM filename - this should be the filename from DICOMDIR
                            displayName = filename;
                            qDebug() << "[TREE] Using actual filename:" << filename;
                        } else {
                            // Only fall back to generic names if we really don't have a valid filename
                            qDebug() << "[TREE] Falling back to generic name for invalid filename:" << filename;
                            if (image.instanceNumber > 0) {
                                displayName = QString("Image_%1").arg(image.instanceNumber, 3, 10, QChar('0'));
                            } else {
                                // Fallback to generic numbering using current index
                                displayName = QString("Image_%1").arg(imageIndex, 3, 10, QChar('0'));
                            }
                        }
                    }
                    
                    // Add frame count information for multiframe images
                    if (image.frameCount > 1) {
                        displayName += QString(" (%1 frames)").arg(image.frameCount);
                    }
                    
                    QTreeWidgetItem* imageItem = new QTreeWidgetItem(QStringList() << displayName);
                    
                    // Set UserRole data based on content type
                    // Only check display name from DICOMDIR parsing - don't check individual files
                    if (!image.displayName.isEmpty() && image.displayName.startsWith("SR DOC")) {
                        // This is a Structured Report document - mark as "report" type
                        imageItem->setData(0, Qt::UserRole, QVariantList() << "report" << image.filePath);
                    } else {
                        // This is an actual image - mark as "image" type
                        imageItem->setData(0, Qt::UserRole, QVariantList() << "image" << image.filePath);
                    }
                    
                    // Set icon based on file existence first, then file type and frame count
                    QString iconName;
                    QString tooltip;
                    
                    if (!image.fileExists) {
                        // File doesn't exist yet - show loading icon for all file types
                        iconName = "Loading.png";
                        if (!image.displayName.isEmpty() && image.displayName.startsWith("SR DOC")) {
                            tooltip = QString("Loading Structured Report (SR) Document\nFile is being copied from media...");
                        } else {
                            tooltip = QString("Loading %1\nFile is being copied from media...")
                                     .arg(image.frameCount > 1 ? "multiframe image" : "DICOM image");
                        }
                        imageItem->setForeground(0, QColor(180, 180, 180)); // Gray out text
                    } else if (!image.displayName.isEmpty() && image.displayName.startsWith("SR DOC")) {
                        // This is a Structured Report document that exists
                        iconName = "List.png"; // Use document/list icon for SR
                        tooltip = "Structured Report (SR) Document";
                    } else if (image.frameCount > 1) {
                        iconName = "AcquisitionHeader.png";
                        tooltip = QString("Multiframe DICOM image - %1 frames").arg(image.frameCount);
                    } else {
                        iconName = "Camera.png";
                        tooltip = "Single frame DICOM image";
                    }
                    
                    imageItem->setIcon(0, QIcon(":/icons/" + iconName));
                    imageItem->setToolTip(0, tooltip);
                    seriesItem->addChild(imageItem);
                }
            }
        }
    }
    

    
    // Ensure tree widget shows root decoration and proper indentation
    treeWidget->setRootIsDecorated(true);
    treeWidget->setIndentation(20);
    
    // Force expand all items to see the structure
    treeWidget->expandAll();
}

#ifdef HAVE_DCMTK
bool DicomReader::parseWithDcmtk(const QString& dicomdirPath)
{
    
    // Clear existing data
    clearData();
    
    // Set base path from DICOMDIR path
    QFileInfo fileInfo(dicomdirPath);
    m_basePath = fileInfo.dir().absolutePath();
    
    // Create DcmDicomDir object
    DcmDicomDir ddir(dicomdirPath.toLocal8Bit().constData());
    if (ddir.error().bad()) {
        m_lastError = QString("Failed to read DICOMDIR: %1").arg(ddir.error().text());
        return false;
    }


    DcmDirectoryRecord& root = ddir.getRootRecord();

    int patientCount = 0;
    int studyCount = 0;
    int seriesCount = 0;
    int imageCount = 0;
    
    // Track processed files to avoid duplicates when directories are referenced multiple times
    QSet<QString> processedFiles;

    // Walk PATIENT level
    for (DcmDirectoryRecord* pat = root.nextSub(nullptr); pat; pat = root.nextSub(pat)) {
        if (pat->getRecordType() != ERT_Patient) continue;
        patientCount++;
        
        OFString patientName, patientId;
        pat->findAndGetOFString(DCM_PatientName, patientName);
        pat->findAndGetOFString(DCM_PatientID, patientId);
        
        QString patientID = QString::fromStdString(patientId.c_str());
        QString patName = cleanDicomText(QString::fromStdString(patientName.c_str()));
        
        
        // Create patient
        DicomPatientInfo patientInfo;
        patientInfo.patientID = patientID;
        patientInfo.patientName = patName.isEmpty() ? patientID : patName;

        // STUDY level
        int patientStudyCount = 0; // Count studies within this patient
        for (DcmDirectoryRecord* study = pat->nextSub(nullptr); study; study = pat->nextSub(study)) {
            if (study->getRecordType() != ERT_Study) continue;
            studyCount++;
            patientStudyCount++;
            
            OFString studyUID, studyDesc, studyDate;
            study->findAndGetOFString(DCM_StudyInstanceUID, studyUID);
            study->findAndGetOFString(DCM_StudyDescription, studyDesc);
            study->findAndGetOFString(DCM_StudyDate, studyDate);
            
            QString sUID = QString::fromStdString(studyUID.c_str());
            QString rawStudyDesc = QString::fromStdString(studyDesc.c_str());
            QString sDesc = rawStudyDesc.isEmpty() ? QString("Study %1").arg(patientStudyCount) : QString(rawStudyDesc).replace('^', ' ').trimmed();
            QString sDate = QString::fromStdString(studyDate.c_str());
            
            
            // Create study
            DicomStudyInfo studyInfo;
            studyInfo.studyUID = sUID;
            studyInfo.studyDescription = sDesc;
            studyInfo.studyDate = sDate;

            // SERIES level
            int studySeriesCount = 0; // Count series within this study
            for (DcmDirectoryRecord* series = study->nextSub(nullptr); series; series = study->nextSub(series)) {
                if (series->getRecordType() != ERT_Series) continue;
                seriesCount++;
                studySeriesCount++;
                
                OFString seriesUID, seriesDesc, seriesNumber;
                series->findAndGetOFString(DCM_SeriesInstanceUID, seriesUID);
                OFCondition seriesDescResult = series->findAndGetOFString(DCM_SeriesDescription, seriesDesc);
                series->findAndGetOFString(DCM_SeriesNumber, seriesNumber);
                
                QString serUID = QString::fromStdString(seriesUID.c_str());
                QString rawSerDesc = QString::fromStdString(seriesDesc.c_str());
                QString serDesc = rawSerDesc.isEmpty() ? QString("Series %1").arg(studySeriesCount) : QString(rawSerDesc).replace('^', ' ').trimmed();
                QString serNum = QString::fromStdString(seriesNumber.c_str());
                
                
                // Create series
                DicomSeriesInfo seriesInfo;
                seriesInfo.seriesUID = serUID;
                seriesInfo.seriesDescription = serDesc;
                seriesInfo.seriesNumber = serNum;

                // INSTANCE level (IMAGE, SR DOC, etc.)
                int seriesInstanceCount = 0; // Reset instance counter for each series
                
                for (DcmDirectoryRecord* inst = series->nextSub(nullptr); inst; inst = series->nextSub(inst)) {
                    E_DirRecType recordType = inst->getRecordType();
                    
                    if (recordType == ERT_Image) {
                        OFString fileId, sopInstanceUID, instanceNumberStr;
                        if (inst->findAndGetOFString(DCM_ReferencedFileID, fileId).good()) {
                            // Get instance number and SOP Instance UID for proper identification
                            inst->findAndGetOFString(DCM_ReferencedSOPInstanceUIDInFile, sopInstanceUID);
                            inst->findAndGetOFString(DCM_InstanceNumber, instanceNumberStr);
                            
                            QString sopUID = QString::fromStdString(sopInstanceUID.c_str());
                            
                            // CRITICAL FIX: DCM_ReferencedFileID can be multi-valued (directory + filename)
                            // We need to get ALL components, not just the first one
                            QString fullRelativePath;
                            DcmElement *fileIdElement = nullptr;
                            if (inst->findAndGetElement(DCM_ReferencedFileID, fileIdElement).good() && fileIdElement) {
                                // Check if it's a multi-valued field
                                Uint32 numValues = fileIdElement->getVM();
                                qDebug() << "[DICOMDIR MULTI-VALUE] DCM_ReferencedFileID has" << numValues << "values";
                                
                                QStringList pathComponents;
                                for (Uint32 i = 0; i < numValues; i++) {
                                    OFString component;
                                    if (fileIdElement->getOFString(component, i).good()) {
                                        QString componentStr = QString::fromStdString(component.c_str());
                                        if (!componentStr.isEmpty()) {
                                            pathComponents.append(componentStr);
                                            qDebug() << "[DICOMDIR COMPONENT" << i << "]" << componentStr;
                                        }
                                    }
                                }
                                
                                // Join all components with path separator
                                if (!pathComponents.isEmpty()) {
                                    fullRelativePath = pathComponents.join("/");
                                } else {
                                    // Fallback to single value if multi-value parsing failed
                                    fullRelativePath = QString::fromStdString(fileId.c_str());
                                }
                            } else {
                                // Fallback to original method
                                fullRelativePath = QString::fromStdString(fileId.c_str());
                            }
                            
                            // Convert to proper path format
                            std::string pathStr = fullRelativePath.toStdString();
                            std::replace(pathStr.begin(), pathStr.end(), '\\', '/');
                            QString relativePath = QString::fromStdString(pathStr);
                            QString fullPath = m_basePath + "/" + relativePath;
                            fullPath = QDir::toNativeSeparators(fullPath);
                            
                            qDebug() << "[DICOMDIR DEBUG] FileID from DICOMDIR:" << QString::fromStdString(fileId.c_str())
                                     << "FullRelativePath:" << fullRelativePath
                                     << "RelativePath:" << relativePath << "FullPath:" << fullPath;
                            
                            // Always use the file reference from DICOMDIR, regardless of current file existence
                            // The DICOMDIR contains the authoritative list of what files should exist
                            if (!sopUID.isEmpty() && !processedFiles.contains(sopUID)) {
                                processedFiles.insert(sopUID);
                                seriesInstanceCount++;
                                imageCount++;
                                
                                DicomImageInfo imageInfo;
                                imageInfo.filePath = fullPath;  // Use the path exactly as specified in DICOMDIR
                                imageInfo.isDirectory = false;
                                
                                // CRITICAL: Debug what filename we'll extract from this path
                                QString extractedFilename = QFileInfo(fullPath).fileName();
                                qDebug() << ">>> DICOMDIR PARSE: fullPath=" << fullPath 
                                         << "extractedFilename=" << extractedFilename;
                                imageInfo.instanceNumber = !instanceNumberStr.empty() ? 
                                    atoi(instanceNumberStr.c_str()) : seriesInstanceCount;
                                imageInfo.frameCount = 1;
                                imageInfo.fileExists = QFile::exists(fullPath);
                                
                                // Try to get frame count from DICOMDIR record first
                                OFString numberOfFramesStr;
                                if (inst->findAndGetOFString(DCM_NumberOfFrames, numberOfFramesStr).good() && 
                                    !numberOfFramesStr.empty()) {
                                    int frameCount = atoi(numberOfFramesStr.c_str());
                                    if (frameCount > 0 && frameCount < 100000) { // Sanity check
                                        imageInfo.frameCount = frameCount;
                                        qDebug() << "[DICOMDIR FRAMES] File:" << extractedFilename 
                                                 << "frames from DICOMDIR:" << frameCount;
                                    }
                                } else if (imageInfo.fileExists) {
                                    // Fallback: get frame count from actual file if DICOMDIR doesn't have it
                                    imageInfo.frameCount = getFrameCountFromFile(fullPath);
                                    qDebug() << "[FILE FRAMES] File:" << extractedFilename 
                                             << "frames from file:" << imageInfo.frameCount;
                                }
                                
                                seriesInfo.images.append(imageInfo);
                                m_totalImages++;
                            }
                        }
                    }
                    else {
                        // Handle other record types (SR, etc.)
                        OFString fileId, sopInstanceUID, instanceNumberStr;
                        if (inst->findAndGetOFString(DCM_ReferencedFileID, fileId).good()) {
                            // Get instance number and SOP Instance UID for proper identification
                            inst->findAndGetOFString(DCM_ReferencedSOPInstanceUIDInFile, sopInstanceUID);
                            inst->findAndGetOFString(DCM_InstanceNumber, instanceNumberStr);
                            
                            QString sopUID = QString::fromStdString(sopInstanceUID.c_str());
                            
                            // CRITICAL FIX: DCM_ReferencedFileID can be multi-valued (directory + filename)
                            // Apply same fix as for images
                            QString fullRelativePath;
                            DcmElement *fileIdElement = nullptr;
                            if (inst->findAndGetElement(DCM_ReferencedFileID, fileIdElement).good() && fileIdElement) {
                                // Check if it's a multi-valued field
                                Uint32 numValues = fileIdElement->getVM();
                                qDebug() << "[DICOMDIR SR MULTI-VALUE] DCM_ReferencedFileID has" << numValues << "values";
                                
                                QStringList pathComponents;
                                for (Uint32 i = 0; i < numValues; i++) {
                                    OFString component;
                                    if (fileIdElement->getOFString(component, i).good()) {
                                        QString componentStr = QString::fromStdString(component.c_str());
                                        if (!componentStr.isEmpty()) {
                                            pathComponents.append(componentStr);
                                            qDebug() << "[DICOMDIR SR COMPONENT" << i << "]" << componentStr;
                                        }
                                    }
                                }
                                
                                // Join all components with path separator
                                if (!pathComponents.isEmpty()) {
                                    fullRelativePath = pathComponents.join("/");
                                } else {
                                    // Fallback to single value if multi-value parsing failed
                                    fullRelativePath = QString::fromStdString(fileId.c_str());
                                }
                            } else {
                                // Fallback to original method
                                fullRelativePath = QString::fromStdString(fileId.c_str());
                            }
                            
                            // Convert to proper path format
                            std::string pathStr = fullRelativePath.toStdString();
                            std::replace(pathStr.begin(), pathStr.end(), '\\', '/');
                            QString relativePath = QString::fromStdString(pathStr);
                            QString fullPath = m_basePath + "/" + relativePath;
                            fullPath = QDir::toNativeSeparators(fullPath);
                            
                            // Check if this path is a directory (same issue as images)
                            QFileInfo pathInfo(fullPath);
                            if (pathInfo.isDir()) {
                                // CRITICAL FIX: For directory references in SR documents, find the specific file
                                // that matches this SOP Instance UID, just like we do for images
                                
                                if (!sopUID.isEmpty()) {
                                    // Try to find the specific file that matches this SOP Instance UID
                                    QDir dir(fullPath);
                                    QStringList nameFilters;
                                    nameFilters << "*.dcm" << "*.DCM" << "*.dicom" << "*.DICOM" << "*";
                                    QFileInfoList fileList = dir.entryInfoList(nameFilters, QDir::Files | QDir::Readable, QDir::Name);
                                    
                                    // CRITICAL FIX: Always add the DICOMDIR entry regardless of file existence
                                    // The progressive loader will handle missing files
                                    
                                    // Use the SOP UID as the unique key to prevent duplicates
                                    if (!processedFiles.contains(sopUID)) {
                                        processedFiles.insert(sopUID);
                                        seriesInstanceCount++;
                                        imageCount++;
                                        
                                        // Determine record type name
                                        QString recordTypeStr;
                                        switch (recordType) {
                                            case ERT_root: recordTypeStr = "ROOT"; break;
                                            case ERT_Curve: recordTypeStr = "CURVE"; break;
                                            case ERT_FilmBox: recordTypeStr = "FILM BOX"; break;
                                            case ERT_FilmSession: recordTypeStr = "FILM SESSION"; break;
                                            case ERT_Image: recordTypeStr = "IMAGE"; break;
                                            case ERT_ImageBox: recordTypeStr = "IMAGE BOX"; break;
                                            case ERT_Interpretation: recordTypeStr = "INTERPRETATION"; break;
                                            case ERT_ModalityLut: recordTypeStr = "MODALITY LUT"; break;
                                            case ERT_Mrdr: recordTypeStr = "MRDR"; break;
                                            case ERT_Overlay: recordTypeStr = "OVERLAY"; break;
                                            case ERT_Patient: recordTypeStr = "PATIENT"; break;
                                            case ERT_PrintQueue: recordTypeStr = "PRINT QUEUE"; break;
                                            case ERT_Private: recordTypeStr = "PRIVATE"; break;
                                            case ERT_Results: recordTypeStr = "RESULTS"; break;
                                            case ERT_Series: recordTypeStr = "SERIES"; break;
                                            case ERT_Study: recordTypeStr = "STUDY"; break;
                                            case ERT_StudyComponent: recordTypeStr = "STUDY COMPONENT"; break;
                                            case ERT_Topic: recordTypeStr = "TOPIC"; break;
                                            case ERT_Visit: recordTypeStr = "VISIT"; break;
                                            case ERT_VoiLut: recordTypeStr = "VOI LUT"; break;
                                            default: 
                                                recordTypeStr = "SR DOC";
                                                break;
                                        }
                                        
                                        // Create the expected file path based on DICOMDIR reference
                                        QString expectedFilePath = fullPath;
                                        
                                        // If it's a directory reference, construct the likely filename
                                        if (pathInfo.isDir()) {
                                            QString baseDir = fullPath;
                                            QString sopUidFileName = sopUID + ".dcm";
                                            
                                            // Try to find an actual file, but don't require it to exist
                                            QDir dir(baseDir);
                                            QStringList nameFilters;
                                            nameFilters << "*.dcm" << "*.DCM" << "*.dicom" << "*.DICOM";
                                            QFileInfoList fileList = dir.entryInfoList(nameFilters, QDir::Files, QDir::Name);
                                            
                                            if (!fileList.isEmpty()) {
                                                // Use the first DICOM file found (will be checked for existence later)
                                                expectedFilePath = fileList.first().absoluteFilePath();
                                            } else {
                                                // No files found, construct expected path anyway
                                                expectedFilePath = baseDir + "/" + sopUidFileName;
                                            }
                                        }
                                        
                                        DicomImageInfo docInfo;
                                        docInfo.filePath = expectedFilePath;
                                        docInfo.isDirectory = false;
                                        docInfo.instanceNumber = !instanceNumberStr.empty() ? 
                                            atoi(instanceNumberStr.c_str()) : seriesInstanceCount;
                                        docInfo.frameCount = 1;
                                        docInfo.fileExists = QFile::exists(expectedFilePath);
                                        docInfo.displayName = QString("%1 %2").arg(recordTypeStr).arg(seriesInstanceCount);
                                        
                                        seriesInfo.images.append(docInfo);
                                        m_totalImages++;
                                    }
                                } else {
                                    // No SOP Instance UID to match against
                                }
                            } else {
                                // This is a direct file reference (not a directory)
                                if (!sopUID.isEmpty() && !processedFiles.contains(sopUID)) {
                                    processedFiles.insert(sopUID);
                                    seriesInstanceCount++;
                                    imageCount++;
                                    
                                    // Determine record type name
                                    QString recordTypeStr;
                                    switch (recordType) {
                                        case ERT_root: recordTypeStr = "ROOT"; break;
                                        case ERT_Curve: recordTypeStr = "CURVE"; break;
                                        case ERT_FilmBox: recordTypeStr = "FILM BOX"; break;
                                        case ERT_FilmSession: recordTypeStr = "FILM SESSION"; break;
                                        case ERT_Image: recordTypeStr = "IMAGE"; break;
                                        case ERT_ImageBox: recordTypeStr = "IMAGE BOX"; break;
                                        case ERT_Interpretation: recordTypeStr = "INTERPRETATION"; break;
                                        case ERT_ModalityLut: recordTypeStr = "MODALITY LUT"; break;
                                        case ERT_Mrdr: recordTypeStr = "MRDR"; break;
                                        case ERT_Overlay: recordTypeStr = "OVERLAY"; break;
                                        case ERT_Patient: recordTypeStr = "PATIENT"; break;
                                        case ERT_PrintQueue: recordTypeStr = "PRINT QUEUE"; break;
                                        case ERT_Private: recordTypeStr = "PRIVATE"; break;
                                        case ERT_Results: recordTypeStr = "RESULTS"; break;
                                        case ERT_Series: recordTypeStr = "SERIES"; break;
                                        case ERT_Study: recordTypeStr = "STUDY"; break;
                                        case ERT_StudyComponent: recordTypeStr = "STUDY COMPONENT"; break;
                                        case ERT_Topic: recordTypeStr = "TOPIC"; break;
                                        case ERT_Visit: recordTypeStr = "VISIT"; break;
                                        case ERT_VoiLut: recordTypeStr = "VOI LUT"; break;
                                        default: 
                                            recordTypeStr = "SR DOC";
                                            break;
                                    }
                                    
                                    DicomImageInfo docInfo;
                                    docInfo.filePath = fullPath;
                                    docInfo.isDirectory = false;
                                    docInfo.instanceNumber = !instanceNumberStr.empty() ? 
                                        atoi(instanceNumberStr.c_str()) : seriesInstanceCount;
                                    docInfo.frameCount = 1;
                                    docInfo.fileExists = QFile::exists(fullPath);
                                    docInfo.displayName = QString("%1 %2").arg(recordTypeStr).arg(seriesInstanceCount);
                                    
                                    seriesInfo.images.append(docInfo);
                                    m_totalImages++;
                                    
                                } else {
                                    // Duplicate SOP Instance UID, skip
                                }
                            }
                        }
                    }
                }
                
                // Only add series if it has at least one image or document
                if (!seriesInfo.images.isEmpty()) {
                    studyInfo.series[serUID] = seriesInfo;
                } else {
                }
            }
            
            // Only add study if it has at least one series
            if (!studyInfo.series.isEmpty()) {
                patientInfo.studies[sUID] = studyInfo;
            } else {
            }
        }
        
        // Only add patient if they have at least one study
        if (!patientInfo.studies.isEmpty()) {
            m_patients[patientID] = patientInfo;
        } else {
        }
    }
    
    
    return !m_patients.isEmpty();
}

int DicomReader::getFrameCountFromFile(const QString& filePath)
{
    try {
        // Use DCMTK to properly read the Number of Frames tag
        DcmFileFormat dcmFile;
        OFCondition status = dcmFile.loadFile(filePath.toLocal8Bit().constData());
        if (status.bad()) {
            return 1;
        }
        
        DcmDataset* dataset = dcmFile.getDataset();
        if (!dataset) {
            return 1;
        }
        
        // Try to get Number of Frames (0028,0008)
        OFString numberOfFramesStr;
        if (dataset->findAndGetOFString(DCM_NumberOfFrames, numberOfFramesStr).good()) {
            if (!numberOfFramesStr.empty()) {
                int frameCount = atoi(numberOfFramesStr.c_str());
                if (frameCount > 0 && frameCount < 100000) { // Sanity check
                    return frameCount;
                }
            }
        }
        
        // If no Number of Frames tag found, it's likely a single frame image
        return 1;
        
    } catch (const std::exception& e) {
        return 1;
    } catch (...) {
        return 1;
    }
} 

QList<DicomImageInfo> DicomReader::expandDirectoryEntries(const QList<DicomImageInfo>& images)
{
    QList<DicomImageInfo> expandedImages;
    
    for (const DicomImageInfo& image : images) {
        if (image.isDirectory) {
            // This entry points to a directory, enumerate actual DICOM files
            QDir dir(image.filePath);
            if (dir.exists()) {
                QStringList nameFilters;
                nameFilters << "*" << "*.dcm" << "*.DCM" << "*.dicom" << "*.DICOM";
                
                QFileInfoList fileList = dir.entryInfoList(nameFilters, QDir::Files | QDir::Readable, QDir::Name);
                
                int fileIndex = 1;
                for (const QFileInfo& fileInfo : fileList) {
                    // Skip obvious non-DICOM files
                    QString fileName = fileInfo.fileName();
                    if (fileName.endsWith(".txt", Qt::CaseInsensitive) ||
                        fileName.endsWith(".inf", Qt::CaseInsensitive) ||
                        fileName.endsWith(".log", Qt::CaseInsensitive) ||
                        fileName == "DICOMDIR") {
                        continue;
                    }
                    
                    DicomImageInfo expandedImage;
                    expandedImage.filePath = fileInfo.absoluteFilePath();
                    expandedImage.instanceNumber = image.instanceNumber + fileIndex - 1;
                    expandedImage.frameCount = getFrameCountFromFile(expandedImage.filePath);
                    expandedImage.fileExists = true;
                    expandedImage.isDirectory = false;
                    
                    // Create a better display name using the actual filename
                    QString baseName = fileInfo.baseName();
                    if (baseName.contains('.') && baseName.split('.').size() > 5) {
                        // UID-based filename, create readable name
                        expandedImage.displayName = QString("IMG_%1").arg(fileIndex, 3, 10, QChar('0'));
                    } else {
                        expandedImage.displayName = fileName;
                    }
                    
                    expandedImages.append(expandedImage);
                    fileIndex++;
                }
            }
        } else {
            // Regular file entry, keep as-is
            expandedImages.append(image);
        }
    }
    
    return expandedImages;
}

QString DicomReader::extractSeriesDescriptionFromFile(const QString& filePath)
{
#ifdef HAVE_DCMTK
    try {
        DcmFileFormat dcmFile;
        OFCondition status = dcmFile.loadFile(filePath.toLocal8Bit().constData());
        if (status.bad()) {
            return "No Series Description";
        }
        
        DcmDataset* dataset = dcmFile.getDataset();
        if (!dataset) {
            return "No Series Description";
        }
        
        OFString seriesDesc;
        if (dataset->findAndGetOFString(DCM_SeriesDescription, seriesDesc).good()) {
            QString result = QString::fromStdString(seriesDesc.c_str()).replace('^', ' ').trimmed();
            return result.isEmpty() ? "No Series Description" : result;
        }
        
        return "No Series Description";
    } catch (...) {
        return "No Series Description";
    }
#else
    Q_UNUSED(filePath)
    return "No Series Description";
#endif
}

void DicomReader::refreshFileExistenceStatus()
{
    // Update file existence status for all images in all patients/studies/series
    for (auto patientIt = m_patients.begin(); patientIt != m_patients.end(); ++patientIt) {
        DicomPatientInfo& patient = patientIt.value();
        for (auto studyIt = patient.studies.begin(); studyIt != patient.studies.end(); ++studyIt) {
            DicomStudyInfo& study = studyIt.value();
            for (auto seriesIt = study.series.begin(); seriesIt != study.series.end(); ++seriesIt) {
                DicomSeriesInfo& series = seriesIt.value();
                for (DicomImageInfo& image : series.images) {
                    bool wasExisting = image.fileExists;
                    
                    // Update file existence status
                    image.fileExists = QFile::exists(image.filePath);
                    
                    // If file just became available, read actual frame count from the DICOM file
                    if (image.fileExists && !wasExisting) {
                        int actualFrameCount = getFrameCountFromFile(image.filePath);
                        if (actualFrameCount > 0) {
                            qDebug() << "[FRAME COUNT UPDATE]" << QFileInfo(image.filePath).fileName() 
                                     << "- DICOMDIR frames:" << image.frameCount 
                                     << "- Actual frames:" << actualFrameCount;
                            image.frameCount = actualFrameCount;
                        }
                    }
                }
            }
        }
    }
}

void DicomReader::startProactiveCopyMonitoring()
{
    // Simplified implementation - just refresh file status
    // The main DicomViewer now handles robocopy detection and progress monitoring
    refreshFileExistenceStatus();
    qDebug() << "DicomReader: Refreshed file existence status for proactive copy monitoring";
}

double DicomReader::calculateProgress() const
{
    if (m_totalImages == 0) {
        return 0.0;
    }
    
    int existingFiles = 0;
    int totalFiles = 0;
    
    // Count existing files vs total files
    for (auto patientIt = m_patients.begin(); patientIt != m_patients.end(); ++patientIt) {
        const DicomPatientInfo& patient = patientIt.value();
        for (auto studyIt = patient.studies.begin(); studyIt != patient.studies.end(); ++studyIt) {
            const DicomStudyInfo& study = studyIt.value();
            for (auto seriesIt = study.series.begin(); seriesIt != study.series.end(); ++seriesIt) {
                const DicomSeriesInfo& series = seriesIt.value();
                for (const DicomImageInfo& image : series.images) {
                    totalFiles++;
                    if (image.fileExists) {
                        existingFiles++;
                    }
                }
            }
        }
    }
    
    // Calculate percentage
    if (totalFiles == 0) {
        return 100.0; // No files to copy
    }
    
    double progress = (static_cast<double>(existingFiles) / static_cast<double>(totalFiles)) * 100.0;
    return qBound(0.0, progress, 100.0); // Ensure progress is between 0 and 100
}

bool DicomReader::isStructuredReport(const QString& filePath)
{
    if (!QFile::exists(filePath)) {
        return false;
    }
    
#ifdef HAVE_DCMTK
    try {
        DcmFileFormat fileformat;
        OFCondition result = fileformat.loadFile(filePath.toLocal8Bit().constData());
        
        if (result.bad()) {
            return false;
        }
        
        DcmDataset* dataset = fileformat.getDataset();
        if (!dataset) {
            return false;
        }
        
        // Check SOP Class UID for known SR types
        OFString sopClassUID;
        if (dataset->findAndGetOFString(DCM_SOPClassUID, sopClassUID).good()) {
            QString sopUID = QString::fromStdString(sopClassUID.c_str());
            
            // Debug: Log the SOP Class UID we found
            qDebug() << "DICOM file SOP Class UID:" << sopUID;
            
            // Known SR SOP Class UIDs
            if (sopUID == "1.2.840.10008.5.1.4.1.1.88.67" || // X-Ray Radiation Dose SR
                sopUID == "1.2.840.10008.5.1.4.1.1.88.11" || // Basic Text SR
                sopUID == "1.2.840.10008.5.1.4.1.1.88.22" || // Enhanced SR
                sopUID == "1.2.840.10008.5.1.4.1.1.88.33" || // Comprehensive SR
                sopUID == "1.2.840.10008.5.1.4.1.1.88.40" || // Procedure Log SR
                sopUID == "1.2.840.10008.5.1.4.1.1.88.50" || // Mammography CAD SR
                sopUID == "1.2.840.10008.5.1.4.1.1.88.59") {  // Key Object Selection
                qDebug() << "File identified as SR document:" << filePath;
                return true;
            } else {
                qDebug() << "File identified as regular DICOM image:" << filePath;
                return false;
            }
        }
    } catch (...) {
        qDebug() << "Exception while checking DICOM file:" << filePath;
        return false;
    }
#endif
    return false;
}

void DicomReader::updateImageDisplayNameFromFile(DicomImageInfo& image)
{
    if (isStructuredReport(image.filePath)) {
        if (!image.displayName.startsWith("SR DOC")) {
            image.displayName = QString("SR DOC - X-Ray Radiation Dose Report");
        }
    } else {
        // For regular images, try to get better metadata if available
        if (image.displayName.isEmpty() || image.displayName.startsWith("Image_")) {
            QFileInfo pathInfo(image.filePath);
            image.displayName = pathInfo.fileName();
        }
    }
}

DicomImageInfo DicomReader::getImageInfoForFile(const QString& filePath) const
{
    QString targetFileName = QFileInfo(filePath).fileName();
    qDebug() << "[ICON DEBUG] Looking for image info for file:" << targetFileName << "from path:" << filePath;
    
    // Search through all patients, studies, series to find the file
    int itemCount = 0;
    for (const auto& patient : m_patients) {
        for (const auto& study : patient.studies) {
            for (const auto& series : study.series) {
                for (const auto& image : series.images) {
                    itemCount++;
                    QFileInfo imageFileInfo(image.filePath);
                    QString imageFileName = imageFileInfo.fileName();
                    
                    // Debug first few items
                    if (itemCount <= 3) {
                        qDebug() << "[ICON DEBUG]" << itemCount << "Checking:" << imageFileName << "frames:" << image.frameCount;
                    }
                    
                    // Match by filename (case insensitive)
                    if (imageFileName.compare(targetFileName, Qt::CaseInsensitive) == 0) {
                        qDebug() << "[ICON DEBUG] ✓ MATCH FOUND:" << imageFileName << "frames:" << image.frameCount;
                        return image;
                    }
                    
                    // Also try exact path match
                    if (image.filePath == filePath) {
                        qDebug() << "[ICON DEBUG] ✓ EXACT PATH MATCH:" << imageFileName << "frames:" << image.frameCount;
                        return image;
                    }
                }
            }
        }
    }
    
    qDebug() << "[ICON DEBUG] ✗ NO MATCH FOUND for:" << targetFileName << "- returning default (1 frame)";
    
    // Return default if not found
    DicomImageInfo defaultInfo;
    defaultInfo.filePath = filePath;
    defaultInfo.frameCount = 1;
    defaultInfo.fileExists = false;
    return defaultInfo;
}

void DicomReader::updateFrameCountForFile(const QString& fileName)
{
    qDebug() << "[FRAME COUNT UPDATE] Updating frame count for file:" << fileName;
    
    // Find the file in our data structures and update its frame count
    for (auto& patient : m_patients) {
        for (auto& study : patient.studies) {
            for (auto& series : study.series) {
                for (auto& image : series.images) {
                    // Extract filename from full path to compare
                    QString imageFileName = QFileInfo(image.filePath).fileName();
                    
                    if (imageFileName == fileName && image.fileExists) {
                        qDebug() << "[FRAME COUNT UPDATE] Found matching image, reading actual frame count...";
                        
                        // Re-read the actual frame count from the now-available file
                        int actualFrameCount = getFrameCountFromFile(image.filePath);
                        
                        if (actualFrameCount != image.frameCount) {
                            qDebug() << QString("[FRAME COUNT UPDATE] Updated %1 from %2 to %3 frames")
                                        .arg(fileName)
                                        .arg(image.frameCount)
                                        .arg(actualFrameCount);
                            
                            image.frameCount = actualFrameCount;
                        } else {
                            qDebug() << QString("[FRAME COUNT UPDATE] Frame count unchanged: %1 frames")
                                        .arg(actualFrameCount);
                        }
                        
                        return; // Found and updated, we're done
                    }
                }
            }
        }
    }
    
    qDebug() << "[FRAME COUNT UPDATE] File not found in data structures:" << fileName;
}

#endif // HAVE_DCMTK
