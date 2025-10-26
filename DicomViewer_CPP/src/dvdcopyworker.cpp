#include "dvdcopyworker.h"
#include <QLoggingCategory>
#include <QDateTime>
#include <QRegularExpression>
#include <QTextStream>

// Uncomment to enable DVD speed throttling (simulates real DVD/CD read speeds)
#define ENABLE_DVD_SPEED_THROTTLING

// Uncomment to enable verbose console logging
#define ENABLE_DVD_COPY_LOGGING

Q_LOGGING_CATEGORY(dvdCopy, "dvd.copy")

// Helper function to ensure console output
void debugLog(const QString& message) {
#ifdef ENABLE_DVD_COPY_LOGGING
    qDebug() << message;
    // Also output to console directly
    qWarning() << "[DVD]" << message;
#endif
}

DvdCopyWorker::DvdCopyWorker(const QString& destPath, QObject* parent)
    : QObject(parent), m_destPath(destPath), m_robocopyProcess(nullptr), m_progressTimer(nullptr), m_lastLogPosition(0),
      m_currentFileIndex(0)
{
    debugLog("=== DVD Copy Worker Initialized ===");
    debugLog("Destination Path: " + m_destPath);
    debugLog("Timestamp: " + QDateTime::currentDateTime().toString());
    
    // Create progress monitoring timer
    m_progressTimer = new QTimer(this);
    connect(m_progressTimer, &QTimer::timeout, this, &DvdCopyWorker::checkFileProgress);
}

DvdCopyWorker::~DvdCopyWorker()
{
#ifdef ENABLE_DVD_COPY_LOGGING
    qCDebug(dvdCopy) << "=== DVD Copy Worker Destroyed ===";
#endif
    if (m_robocopyProcess && m_robocopyProcess->state() == QProcess::Running) {
#ifdef ENABLE_DVD_COPY_LOGGING
        qCDebug(dvdCopy) << "Waiting for active robocopy process to complete...";
#endif
        // Try to terminate gracefully first
        m_robocopyProcess->terminate();
        if (!m_robocopyProcess->waitForFinished(3000)) {
            // If it doesn't finish in 3 seconds, force kill
            qCDebug(dvdCopy) << "Force killing robocopy process";
            m_robocopyProcess->kill();
            m_robocopyProcess->waitForFinished(1000);
        }
        m_robocopyProcess->deleteLater();
    }
}

void DvdCopyWorker::startDvdDetectionAndCopy()
{
    debugLog("=== Starting DVD Detection and Copy Process ===");
    
    // Step 1: Detect DVD drives
    emit statusChanged("Detecting DVD drives...");
    debugLog("Step 1: Detecting DVD drives...");
    QString dvdPath = findDvdWithDicomFiles();
    
    if (dvdPath.isEmpty()) {
        debugLog("ERROR: No DVD with DICOMFILES folder found");
        emit workerError("No DVD with DICOMFILES folder found");
        return;
    }
    
    debugLog("SUCCESS: DVD detected at: " + dvdPath);
    emit dvdDetected(dvdPath);
    
    // DVD detected successfully - main application will decide which copy method to use
    debugLog("DVD detection complete. Waiting for copy method selection...");
}

void DvdCopyWorker::onRobocopyOutput()
{
    if (!m_robocopyProcess) return;
    
    QByteArray data = m_robocopyProcess->readAllStandardOutput();
    QString output = QString::fromUtf8(data);
    
#ifdef ENABLE_DVD_COPY_LOGGING
    if (!output.trimmed().isEmpty()) {
        // Log raw robocopy output for debugging
        QStringList lines = output.split('\n');
        for (const QString& line : lines) {
            QString trimmed = line.trimmed();
            if (!trimmed.isEmpty()) {
                qCDebug(dvdCopy) << "[ROBOCOPY]" << trimmed;
            }
        }
    }
#endif
    
    parseRobocopyOutput(output);
}

void DvdCopyWorker::onRobocopyError()
{
    if (!m_robocopyProcess) return;
    
    QByteArray data = m_robocopyProcess->readAllStandardError();
    QString output = QString::fromUtf8(data);
    
#ifdef ENABLE_DVD_COPY_LOGGING
    if (!output.trimmed().isEmpty()) {
        // Log robocopy error output (which might contain progress info)
        QStringList lines = output.split('\n');
        for (const QString& line : lines) {
            QString trimmed = line.trimmed();
            if (!trimmed.isEmpty()) {
                qCDebug(dvdCopy) << "[ROBOCOPY ERR]" << trimmed;
            }
        }
    }
#endif
    
    // Progress information might come through stderr, so parse this too
    parseRobocopyOutput(output);
}

void DvdCopyWorker::onRobocopyFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QString statusStr = (exitStatus == QProcess::NormalExit) ? "Normal" : "Crashed";
    bool success = (exitCode == 0 || exitCode == 1); // Robocopy returns 1 for successful copy
    
    // Mark the last file as complete when robocopy finishes
    if (!m_currentFileName.isEmpty()) {
        debugLog(QString("[FILE COMPLETE] %1 - 100%% (robocopy finished)").arg(m_currentFileName));
        emit fileProgress(m_currentFileName, 100);
    }
    
#ifdef ENABLE_DVD_COPY_LOGGING
    qCDebug(dvdCopy) << "=== Robocopy Process Finished ===";
    qCDebug(dvdCopy) << "Exit Code:" << exitCode;
    qCDebug(dvdCopy) << "Exit Status:" << statusStr;
    qCDebug(dvdCopy) << "Success:" << (success ? "YES" : "NO");
    
    // Explain robocopy exit codes
    switch (exitCode) {
        case 0: qCDebug(dvdCopy) << "Robocopy Result: No files copied (no change needed)"; break;
        case 1: qCDebug(dvdCopy) << "Robocopy Result: Files copied successfully"; break;
        case 2: qCDebug(dvdCopy) << "Robocopy Result: Extra files or directories detected"; break;
        case 4: qCDebug(dvdCopy) << "Robocopy Result: Mismatched files or directories"; break;
        case 8: qCDebug(dvdCopy) << "Robocopy Result: Failed copies occurred"; break;
        case 16: qCDebug(dvdCopy) << "Robocopy Result: Serious error - no files copied"; break;
        default: qCDebug(dvdCopy) << "Robocopy Result: Unknown exit code"; break;
    }
#endif
    
    qDebug() << "DVD Robocopy finished with exit code:" << exitCode;
    emit copyCompleted(success);
    
    if (m_robocopyProcess) {
        m_robocopyProcess->deleteLater();
        m_robocopyProcess = nullptr;
    }
}

QString DvdCopyWorker::findDvdWithDicomFiles()
{
    QStringList drivesToCheck = {"D:", "E:", "F:", "G:", "H:"};
    
#ifdef ENABLE_DVD_COPY_LOGGING
    qCDebug(dvdCopy) << "--- Scanning for DVD drives with DICOM files ---";
    qCDebug(dvdCopy) << "Drives to check:" << drivesToCheck;
#endif
    
    for (const QString& drive : drivesToCheck) {
        QString dicomPath = drive + "/DicomFiles";
        QDir dir(dicomPath);
        
#ifdef ENABLE_DVD_COPY_LOGGING
        qCDebug(dvdCopy) << "Checking drive:" << drive;
        qCDebug(dvdCopy) << "Looking for path:" << dicomPath;
#endif
        
        if (dir.exists()) {
#ifdef ENABLE_DVD_COPY_LOGGING
            qCDebug(dvdCopy) << "DicomFiles directory exists on" << drive;
#endif
            QStringList filters;
            filters << "*.dcm" << "*.DCM" << "*";
            QStringList files = dir.entryList(filters, QDir::Files);
            
#ifdef ENABLE_DVD_COPY_LOGGING
            qCDebug(dvdCopy) << "Found" << files.count() << "files in DicomFiles directory";
            if (files.count() > 0) {
                qCDebug(dvdCopy) << "Sample files:" << files.mid(0, qMin(5, files.count()));
            }
#endif
            
            if (!files.isEmpty()) {
#ifdef ENABLE_DVD_COPY_LOGGING
                qCDebug(dvdCopy) << "SUCCESS: Found valid DVD with DicomFiles at:" << drive;
#endif
                qDebug() << "Found DVD with DicomFiles at:" << drive;
                return drive;
            }
        } else {
#ifdef ENABLE_DVD_COPY_LOGGING
            qCDebug(dvdCopy) << "DicomFiles directory does not exist on" << drive;
#endif
        }
    }
    
#ifdef ENABLE_DVD_COPY_LOGGING
    qCDebug(dvdCopy) << "No DVD drives with DicomFiles found";
#endif
    return QString();
}

void DvdCopyWorker::startRobocopy(const QString& dvdPath)
{
    qDebug() << "[DVD COPY WORKER] startRobocopy method called with dvdPath:" << dvdPath;
    
    QString sourceDir = dvdPath + "/DicomFiles";
    QString destDir = m_destPath;
    
#ifdef ENABLE_DVD_COPY_LOGGING
    qCDebug(dvdCopy) << "=== Starting Robocopy Operation ===";
    qCDebug(dvdCopy) << "Source Directory:" << sourceDir;
    qCDebug(dvdCopy) << "Destination Directory:" << destDir;
#endif
    
    // Create destination directory
    bool dirCreated = QDir().mkpath(destDir);
#ifdef ENABLE_DVD_COPY_LOGGING
    qCDebug(dvdCopy) << "Destination directory created:" << (dirCreated ? "SUCCESS" : "FAILED");
#endif
    
    // Set up robocopy process
    m_robocopyProcess = new QProcess(this);
    
    connect(m_robocopyProcess, &QProcess::readyReadStandardOutput, 
            this, &DvdCopyWorker::onRobocopyOutput);
    connect(m_robocopyProcess, &QProcess::readyReadStandardError,
            this, &DvdCopyWorker::onRobocopyError);
    connect(m_robocopyProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &DvdCopyWorker::onRobocopyFinished);
    
    // Create log file path
    QString logFilePath = QDir::temp().absoluteFilePath("robocopy_progress.log");
    m_logFilePath = logFilePath;
    m_lastLogPosition = 0;  // Reset log position
    
    // Clear any existing log file
    QFile::remove(logFilePath);
    
    // Build robocopy command with log file output
    QStringList arguments;
    arguments << sourceDir << destDir;
    arguments << "/E";           // Copy subdirectories including empty ones
    arguments << "/Z";           // Restartable mode
    arguments << "/R:1";         // 1 retry on failure
    arguments << "/W:0";         // 0 wait time between retries
    arguments << "/MT:1";        // Single-threaded for DVD compatibility
    arguments << "/V";           // Verbose output
    arguments << "/TEE";         // Output to console AND log file
    arguments << "/LOG:" + logFilePath;  // Log to file
    
#ifdef ENABLE_DVD_SPEED_THROTTLING
    // Add bandwidth throttling to simulate DVD/CD read speeds
    // /IoRate:n[KMG] = Requested i/o rate, in n [KMG] bytes per second
    // Using realistic DVD speed: ~1.4 MB/s (1x DVD read speed)
    arguments << "/IoRate:1420K";   // Throttle to ~1.4 MB/s (1x DVD speed)
    arguments << "/IoMaxSize:128K"; // Use 128K I/O chunks
    arguments << "/Threshold:64K";  // Only throttle files > 64K
    
    debugLog("DVD Speed Simulation: ~1.4MB/s (1x DVD speed)");
#else
    debugLog("DVD speed throttling DISABLED - using maximum speed");
#endif
    
    // Log complete command for debugging
    QString fullCommand = QString("robocopy %1").arg(arguments.join(" "));
    debugLog("Command: \"" + fullCommand + "\"");
    debugLog("Log file path: " + logFilePath);
    debugLog("Log file exists before start: " + QString(QFile::exists(logFilePath) ? "YES" : "NO"));
    debugLog("DVD Speed Simulation: ~1.4MB/s (1x DVD speed)");
    debugLog("=========================");
    
    qDebug() << "[ROBOCOPY] Starting process with command: robocopy" << arguments.join(" ");
    m_robocopyProcess->start("robocopy", arguments);
    
    // Wait a moment for the process to start and verify it's running
    if (m_robocopyProcess->waitForStarted(2000)) {
        qDebug() << "[ROBOCOPY] Process started successfully, PID:" << m_robocopyProcess->processId();
#ifdef ENABLE_DVD_COPY_LOGGING
        qCDebug(dvdCopy) << "Robocopy process started, waiting for output...";
#endif
        emit copyStarted();
        
        // Start monitoring file progress
        startProgressMonitoring();
    } else {
        qDebug() << "[ERROR] Failed to start robocopy process:" << m_robocopyProcess->errorString();
        emit workerError("Failed to start robocopy process: " + m_robocopyProcess->errorString());
    }
}

void DvdCopyWorker::parseRobocopyOutput(const QString& output)
{
    QStringList lines = output.split('\n');
    
    for (const QString& line : lines) {
        QString trimmedLine = line.trimmed();
        
        if (trimmedLine.isEmpty()) continue;
        
        // Enhanced logging of robocopy output
        debugLog(QString("[DVD COPY] %1").arg(trimmedLine));
        
        // Look for progress indicators in various formats
        if (trimmedLine.contains('%')) {
            // Pattern 1: "X% - Y% (elapsed: Z.Zs)" format
            QRegularExpression progressRangeRe(R"((\d{1,3})%\s*-\s*(\d{1,3})%\s*\(elapsed:\s*([\d.]+)s\))");
            QRegularExpressionMatch rangeMatch = progressRangeRe.match(trimmedLine);
            if (rangeMatch.hasMatch()) {
                int endPercent = rangeMatch.captured(2).toInt();
                double elapsed = rangeMatch.captured(3).toDouble();
                
                debugLog(QString("[PROGRESS] Range: %1%% - %2%% (elapsed: %3s)")
                        .arg(rangeMatch.captured(1)).arg(endPercent).arg(elapsed));
                
                if (!m_currentFileName.isEmpty()) {
                    emit fileProgress(m_currentFileName, endPercent);
                }
                continue;
            }
            
            // Pattern 2: "X%    filename.dcm" format (robocopy per-file progress)
            QRegularExpression fileProgressRe(R"((\d{1,3})%\s+(.+\.dcm))");
            QRegularExpressionMatch fileProgressMatch = fileProgressRe.match(trimmedLine);
            if (fileProgressMatch.hasMatch()) {
                int percent = fileProgressMatch.captured(1).toInt();
                QString fileName = fileProgressMatch.captured(2);
                
                debugLog(QString("[FILE PROGRESS] %1%% - %2").arg(percent).arg(fileName));
                
                // Update current file and emit progress
                m_currentFileName = fileName;
                emit fileProgress(fileName, percent);
                continue;
            }
            
            // Pattern 3: Simple "X%" format (fallback)
            QRegularExpression simpleRe(R"((\d{1,3})%)");
            QRegularExpressionMatch simpleMatch = simpleRe.match(trimmedLine);
            if (simpleMatch.hasMatch()) {
                int percent = simpleMatch.captured(1).toInt();
                
                debugLog(QString("[PROGRESS] Simple: %1%%").arg(percent));
                
                if (!m_currentFileName.isEmpty()) {
                    emit fileProgress(m_currentFileName, percent);
                }
            }
        }
        
        // Detect file completion by looking for the next file starting
        // When a new file starts, mark the previous file as complete
        static QString s_previousFileName;
        if (trimmedLine.contains("Starting:") && trimmedLine.contains(".dcm")) {
            if (!s_previousFileName.isEmpty()) {
                debugLog(QString("[FILE COMPLETE] %1 - 100%% (new file detected)").arg(s_previousFileName));
                emit fileProgress(s_previousFileName, 100);
            }
            // The new file starting will be handled in the file detection section below
        }
        
        // Look for file being copied - enhanced pattern matching
        if (trimmedLine.contains("New File") || 
            trimmedLine.contains("Newer") ||
            trimmedLine.contains("Starting:") ||
            trimmedLine.contains(".dcm")) {
            
            // Extract filename from various robocopy output formats
            QString fileName;
            
            // Pattern for "Starting: filename.dcm (size)"
            QRegularExpression startingRe(R"(Starting:\s*([^\s(]+\.dcm)\s*\((\d+)\s*KB\))");
            QRegularExpressionMatch startingMatch = startingRe.match(trimmedLine);
            if (startingMatch.hasMatch()) {
                fileName = startingMatch.captured(1);
                QString sizeKB = startingMatch.captured(2);
                
                debugLog(QString("[FILE START] %1 (%2 KB)").arg(fileName).arg(sizeKB));
                
                // Update static variable to track previous file
                static QString s_previousFileName;
                s_previousFileName = m_currentFileName;
                
                m_currentFileName = fileName;
                emit fileProgress(fileName, 0);
                continue;
            }
            
            // Fallback: look for .dcm files in the line
            QRegularExpression dcmRe(R"(([^\s]+\.dcm))");
            QRegularExpressionMatch dcmMatch = dcmRe.match(trimmedLine);
            if (dcmMatch.hasMatch()) {
                fileName = dcmMatch.captured(1);
                
                debugLog(QString("[FILE DETECTED] %1").arg(fileName));
                
                m_currentFileName = fileName;
                emit fileProgress(fileName, 0);
            }
        }
        
        // Look for summary information
        if (trimmedLine.contains("Total") && trimmedLine.contains("Copied")) {
            debugLog(QString("[SUMMARY] %1").arg(trimmedLine));
        }
        
        // Look for error messages
        if (trimmedLine.contains("ERROR") || trimmedLine.contains("FAILED")) {
            debugLog(QString("[ERROR] %1").arg(trimmedLine));
        }
        
        // Look for completion indicators
        if (trimmedLine.contains("Bytes/sec") || trimmedLine.contains("MegaBytes/min")) {
            debugLog(QString("[SPEED] %1").arg(trimmedLine));
        }
    }
}

void DvdCopyWorker::startProgressMonitoring()
{
    // Get list of expected files from H drive (source)
    QDir sourceDir("H:/DicomFiles");
    
    m_expectedFiles.clear();
    m_completedFiles.clear();
    
    QStringList filters;
    filters << "*.dcm" << "*.DCM" << "*.dicom" << "*.DICOM";
    
    QFileInfoList sourceFiles = sourceDir.entryInfoList(filters, QDir::Files);
    for (const QFileInfo& fileInfo : sourceFiles) {
        m_expectedFiles.append(fileInfo.fileName());
    }
    
    debugLog(QString("Monitoring progress for %1 files").arg(m_expectedFiles.size()));
    
    // Start timer to check progress every 500ms
    if (m_progressTimer) {
        m_progressTimer->start(500);
    }
}

void DvdCopyWorker::checkFileProgress()
{
    // Debug log file monitoring
    if (!m_logFilePath.isEmpty()) {
        bool logExists = QFile::exists(m_logFilePath);
        debugLog(QString("Checking log file: %1 (exists: %2)").arg(m_logFilePath).arg(logExists ? "YES" : "NO"));
        
        if (logExists) {
            QFileInfo logInfo(m_logFilePath);
            debugLog(QString("Log file size: %1 bytes, position: %2").arg(logInfo.size()).arg(m_lastLogPosition));
        }
    }
    
    // First check log file for new entries
    if (!m_logFilePath.isEmpty() && QFile::exists(m_logFilePath)) {
        QFile logFile(m_logFilePath);
        if (logFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            // Seek to last read position
            logFile.seek(m_lastLogPosition);
            
            QTextStream stream(&logFile);
            QString newContent = stream.readAll();
            
            if (!newContent.isEmpty()) {
                // Update last position
                m_lastLogPosition = logFile.pos();
                
                // Process new log entries
                QStringList lines = newContent.split('\n');
                for (const QString& line : lines) {
                    QString trimmed = line.trimmed();
                    if (trimmed.isEmpty()) continue;
                    
                    debugLog(QString("[LOG] %1").arg(trimmed));
                    
                    // Look for completed file entries
                    // Robocopy logs completed files in format like: "New File     123.dcm"
                    if (trimmed.contains("New File") || trimmed.contains("100%")) {
                        // Extract filename from log line
                        QStringList parts = trimmed.split(QRegularExpression("\\s+"));
                        for (const QString& part : parts) {
                            if (part.endsWith(".dcm", Qt::CaseInsensitive) || 
                                part.endsWith(".dicom", Qt::CaseInsensitive)) {
                                
                                QString fileName = part;
                                
                                // If this file just completed, emit progress signal
                                if (!m_completedFiles.contains(fileName)) {
                                    m_completedFiles.append(fileName);
                                    
                                    debugLog(QString("File completed (from log): %1").arg(fileName));
                                    emit fileProgress(fileName, 100);
                                }
                                break;
                            }
                        }
                    }
                }
            }
            
            logFile.close();
        }
    }
    
    if (m_expectedFiles.isEmpty()) return;
    
    QDir destDir(m_destPath);
    int completedCount = 0;
    
    // Check each expected file
    for (const QString& fileName : m_expectedFiles) {
        QString filePath = destDir.absoluteFilePath(fileName);
        
        if (QFile::exists(filePath)) {
            completedCount++;
            
            // If this file just completed, emit progress signal
            if (!m_completedFiles.contains(fileName)) {
                m_completedFiles.append(fileName);
                
                debugLog(QString("File completed (filesystem): %1").arg(fileName));
                emit fileProgress(fileName, 100);
            }
        }
    }
    
    // Calculate overall progress
    int totalFiles = m_expectedFiles.size();
    if (totalFiles > 0) {
        int progressPercent = (completedCount * 100) / totalFiles;
        
        // Update UI with overall progress
        QString progressText = QString("Copying: %1% (%2/%3 files)")
                             .arg(progressPercent)
                             .arg(completedCount)
                             .arg(totalFiles);
        
        // Only emit if progress changed significantly
        static int lastProgressPercent = -1;
        if (progressPercent != lastProgressPercent) {
            emit overallProgress(progressPercent, progressText);
            lastProgressPercent = progressPercent;
            
            debugLog(QString("Progress update: %1").arg(progressText));
        }
    }
    
    // Stop monitoring when all files are complete
    if (completedCount >= totalFiles) {
        debugLog("All files completed - stopping progress monitoring");
        if (m_progressTimer) {
            m_progressTimer->stop();
        }
    }
}

void DvdCopyWorker::startSequentialRobocopy(const QString& dvdPath, const QStringList& orderedFiles)
{
    qDebug() << "[SEQUENTIAL COPY] Starting sequential copy with" << orderedFiles.size() << "files";
    qDebug() << "[SEQUENTIAL COPY] DVD Path:" << dvdPath;
    qDebug() << "[SEQUENTIAL COPY] Method called successfully!";
    
    // CRITICAL: Stop any existing robocopy process to prevent conflicts
    if (m_robocopyProcess && m_robocopyProcess->state() == QProcess::Running) {
        qDebug() << "[SEQUENTIAL COPY] Stopping existing robocopy process for sequential copying";
        debugLog("Stopping bulk robocopy to start sequential copying");
        
        m_robocopyProcess->terminate();
        if (!m_robocopyProcess->waitForFinished(3000)) {
            m_robocopyProcess->kill();
            m_robocopyProcess->waitForFinished(1000);
        }
        
        // Clean up existing process
        if (m_robocopyProcess) {
            m_robocopyProcess->deleteLater();
            m_robocopyProcess = nullptr;
        }
    }
    
    m_dvdSourcePath = dvdPath;
    m_filesToCopy = orderedFiles;
    m_currentFileIndex = 0;
    m_completedFiles.clear();
    
    debugLog("=== Sequential Robocopy Operation ===");
    debugLog("Source Directory: " + dvdPath + "/DicomFiles");
    debugLog("Destination Directory: " + m_destPath);
    debugLog(QString("Files to copy: %1").arg(orderedFiles.size()));
    
    // Log first few files for verification
    for (int i = 0; i < qMin(5, orderedFiles.size()); i++) {
        debugLog(QString("File %1: %2").arg(i+1).arg(orderedFiles[i]));
    }
    
    if (orderedFiles.isEmpty()) {
        qDebug() << "[ERROR] No files to copy";
        emit workerError("No files to copy");
        return;
    }
    
    // Create destination directory
    bool dirCreated = QDir().mkpath(m_destPath);
    debugLog("Destination directory created: " + QString(dirCreated ? "SUCCESS" : "FAILED"));
    
    // Start copying the first file
    emit copyStarted();
    copyNextFile();
}

void DvdCopyWorker::copyNextFile()
{
    qDebug() << "[COPY NEXT FILE] Method called - current index:" << m_currentFileIndex;
    
    // Safety checks
    if (m_filesToCopy.isEmpty()) {
        qDebug() << "[ERROR] copyNextFile called with empty file list";
        emit workerError("No files to copy");
        return;
    }
    
    if (m_currentFileIndex >= m_filesToCopy.size()) {
        // All files completed
        debugLog("=== All files copied successfully ===");
        emit copyCompleted(true);
        return;
    }
    
    if (m_currentFileIndex < 0) {
        qDebug() << "[ERROR] Invalid file index:" << m_currentFileIndex;
        emit workerError("Invalid file index");
        return;
    }
    
    QString fileName = m_filesToCopy[m_currentFileIndex];
    m_currentFileName = fileName;
    
    // Emit overall progress
    int overallPercent = (m_currentFileIndex * 100) / m_filesToCopy.size();
    QString progressText = QString("File %1 of %2: %3")
                          .arg(m_currentFileIndex + 1)
                          .arg(m_filesToCopy.size())
                          .arg(fileName);
    
    emit overallProgress(overallPercent, progressText);
    
    qDebug() << QString("[SEQUENTIAL] Copying file %1/%2: %3")
                .arg(m_currentFileIndex + 1)
                .arg(m_filesToCopy.size())
                .arg(fileName);
    
    // Start copying this specific file
    startSingleFileRobocopy(fileName);
}

void DvdCopyWorker::startSingleFileRobocopy(const QString& fileName)
{
    qDebug() << "[SINGLE FILE ROBOCOPY] Starting copy of:" << fileName;
    
    // Safety checks
    if (fileName.isEmpty()) {
        qDebug() << "[ERROR] startSingleFileRobocopy called with empty filename";
        emit workerError("Empty filename provided for copy");
        return;
    }
    
    if (m_dvdSourcePath.isEmpty()) {
        qDebug() << "[ERROR] DVD source path is empty";
        emit workerError("DVD source path not set");
        return;
    }
    
    if (m_destPath.isEmpty()) {
        qDebug() << "[ERROR] Destination path is empty";
        emit workerError("Destination path not set");
        return;
    }
    
    QString sourceDir = m_dvdSourcePath + "/DicomFiles";
    QString destDir = m_destPath;
    
    debugLog(QString("Starting copy of: %1").arg(fileName));
    debugLog("Source: " + sourceDir);
    debugLog("Dest: " + destDir);
    
    // Create new process for this file
    if (m_robocopyProcess) {
        m_robocopyProcess->deleteLater();
    }
    
    m_robocopyProcess = new QProcess(this);
    
    connect(m_robocopyProcess, &QProcess::readyReadStandardOutput, 
            this, &DvdCopyWorker::onRobocopyOutput);
    connect(m_robocopyProcess, &QProcess::readyReadStandardError,
            this, &DvdCopyWorker::onRobocopyError);
    connect(m_robocopyProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
                // Safety check - ensure object is still valid
                if (!this || !m_robocopyProcess) {
                    qDebug() << "[WARNING] Object destroyed during robocopy completion";
                    return;
                }
                
                // Handle single file completion
                bool success = (exitCode == 0 || exitCode == 1);
                
                debugLog(QString("File copy finished: %1 (exit code: %2)").arg(m_currentFileName).arg(exitCode));
                
                if (success) {
                    // Mark current file as 100% complete
                    emit fileProgress(m_currentFileName, 100);
                    m_completedFiles.append(m_currentFileName);
                    
                    // Move to next file
                    m_currentFileIndex++;
                    
                    // Start next file after a short delay (helps with DVD drive)
                    // Use QueuedConnection for safer threading
                    QTimer::singleShot(100, this, [this]() {
                        if (this) {  // Additional safety check
                            copyNextFile();
                        }
                    });
                } else {
                    debugLog(QString("ERROR copying file: %1").arg(m_currentFileName));
                    emit workerError(QString("Failed to copy file: %1").arg(m_currentFileName));
                }
                
                // Clean up current process
                if (m_robocopyProcess) {
                    m_robocopyProcess->deleteLater();
                    m_robocopyProcess = nullptr;
                }
            }, Qt::QueuedConnection);
    
    // Build robocopy command for single file
    QStringList arguments;
    arguments << sourceDir;      // Source directory (QProcess handles spaces automatically)
    arguments << destDir;        // Destination directory (QProcess handles spaces automatically)  
    arguments << fileName;       // Copy only this specific file (QProcess handles spaces automatically)
    arguments << "/Z";                                 // Restartable mode
    arguments << "/R:1";                               // 1 retry on failure
    arguments << "/W:0";                               // 0 wait time between retries
    arguments << "/V";                                 // Verbose output
    
#ifdef ENABLE_DVD_SPEED_THROTTLING
    // Add bandwidth throttling for DVD compatibility
    arguments << "/IoRate:1420K";    // ~1.4 MB/s (1x DVD speed)
    arguments << "/IoMaxSize:128K";  // Use 128K I/O chunks
    arguments << "/Threshold:64K";   // Only throttle files > 64K
    
    debugLog("DVD Speed Simulation: ~1.4MB/s (1x DVD speed)");
#endif
    
    QString fullCommand = QString("robocopy %1").arg(arguments.join(" "));
    debugLog("Command: \"" + fullCommand + "\"");
    
    // Emit initial progress for this file
    emit fileProgress(fileName, 0);
    
    // Start the robocopy process
    qDebug() << "[SINGLE FILE] Starting robocopy for:" << fileName;
    m_robocopyProcess->start("robocopy", arguments);
    
    if (!m_robocopyProcess->waitForStarted(2000)) {
        qDebug() << "[ERROR] Failed to start single file robocopy:" << m_robocopyProcess->errorString();
        emit workerError("Failed to start robocopy for file: " + fileName);
    }
}

void DvdCopyWorker::emitWorkerReady()
{
    qDebug() << "[WORKER READY] DvdCopyWorker is ready to receive signals";
    emit workerReady();
}