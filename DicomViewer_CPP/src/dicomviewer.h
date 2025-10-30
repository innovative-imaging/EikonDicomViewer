#pragma once

#include <QtWidgets/QMainWindow>
#include <QtWidgets/QWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>
#include <QtWidgets/QLabel>
#include <QtWidgets/QFrame>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QSpacerItem>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsPixmapItem>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QStatusBar>
#include <QtCore/QProcess>
#include <QtCore/QTimer>
#include <QtCore/QThread>
#include <QtCore/QMutex>
#include <QtCore/QTextStream>
#include <QtMultimedia/QMediaRecorder>
#include <QtMultimedia/QMediaCaptureSession>
#include <QtCore/QUrl>
#include "dicomreader.h"
#include "progressiveframeloader.h"
#include "DicomFrameProcessor.h"
#include "DicomPlaybackController_Simple.h"
#include "DicomInputHandler_Simple.h"

#include <QtGui/QAction>
#include <QtGui/QIcon>
#include <QtGui/QPixmap>
#include <QtGui/QPainter>
#include <QtGui/QFont>
#include <QtGui/QColor>
#include <QtWidgets/QMenu>
#include <QtGui/QMouseEvent>
#include <QtGui/QCursor>
#include <QtGui/QPainterPath>
#include <QtGui/QBrush>

#include <QtCore/Qt>
#include <QtCore/QSize>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QWheelEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QShowEvent>
#include <QtGui/QCloseEvent>
#include <QtGui/QContextMenuEvent>

// Include save dialog headers  
#include "saveimagedialog.h"
#include "saverundialog.h"

// Forward declarations
class DvdCopyWorker;

// DVD Copy Worker - Background thread worker for DVD detection and copying
class ImageProcessingPipeline
{
public:
    ImageProcessingPipeline();
    
    // Pipeline configuration
    void setHorizontalFlipEnabled(bool enabled);
    void setVerticalFlipEnabled(bool enabled);
    void setInvertEnabled(bool enabled);
    void setWindowLevel(double windowCenter, double windowWidth);
    void setWindowLevelEnabled(bool enabled);
    void setBitsStored(int bitsStored);
    void resetAllTransformations();
    
    // Main pipeline execution
    QImage processImage(const QImage& sourceImage) const;
    
    // Individual pipeline stages (decompression handled by DCMTK/GDCM/libjpeg)
    QImage horizontalFlipStage(const QImage& input) const;
    QImage verticalFlipStage(const QImage& input) const;
    QImage invertStage(const QImage& input) const;
    QImage windowLevelStage(const QImage& input) const;
    
    // State queries
    bool isHorizontalFlipEnabled() const { return m_hFlipEnabled; }
    bool isVerticalFlipEnabled() const { return m_vFlipEnabled; }
    bool isInvertEnabled() const { return m_invertEnabled; }
    bool isWindowLevelEnabled() const { return m_windowLevelEnabled; }
    double getWindowCenter() const { return m_windowCenter; }
    double getWindowWidth() const { return m_windowWidth; }
    int getBitsStored() const { return m_bitsStored; }
    bool hasAnyTransformations() const;
    
private:
    bool m_hFlipEnabled;
    bool m_vFlipEnabled;
    bool m_invertEnabled;
    bool m_windowLevelEnabled;
    double m_windowCenter;
    double m_windowWidth;
    int m_bitsStored;
};

class DicomViewer : public QMainWindow
{
    Q_OBJECT

public:
    DicomViewer(QWidget *parent = nullptr);
    ~DicomViewer();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

signals:
    void requestSequentialRobocopyStart(const QString& dvdPath, const QStringList& orderedFiles);
    void ffmpegCopyCompleted(bool success);

private slots:
    // Simplified framework integration slots
    void onPlaybackStateChanged(DicomPlaybackController::PlaybackState oldState, DicomPlaybackController::PlaybackState newState);
    void onCurrentFrameChanged(int frameIndex, int totalFrames);
    void onFrameRequested(int frameIndex);
    
    // DVD copy worker slots
    void onWorkerReady();
    void onDvdDetected(const QString& dvdPath);
    void onCopyStarted();
    void onFileProgress(const QString& fileName, int progress);
    void onOverallProgress(int percentage, const QString& statusText);
    void onCopyCompleted(bool success);
    void onWorkerError(const QString& error);
    
    // Essential input handler slots
    void onPlayPauseRequested();
    void onNextFrameRequested();
    void onPreviousFrameRequested();
    void onNextImageRequested();
    void onPreviousImageRequested();
    void onHorizontalFlipRequested();
    void onVerticalFlipRequested();
    void onInvertImageRequested();
    void onResetAllRequested();
    
    // Legacy navigation slots (will be refactored)
    void nextFrame();
    void previousFrame();
    void automaticNextFrame(); // For timer-driven playback
    void togglePlayback();
    void nextImage();
    void previousImage();
    
    // Progressive loading slots
    void onFrameReady(int frameNumber);
    void onAllFramesLoaded(int totalFrames);
    void onProgressiveTimerTimeout(); // For FPS-controlled progressive display
    void onFirstFrameInfo(const QString& patientName, const QString& patientId, int totalFrames);
    void onLoadingError(const QString& errorMessage);
    void onLoadingProgress(int currentFrame, int totalFrames);
    
    // Copy monitoring slots
    void onCopyProgressTimeout();
    
    // FFmpeg copy completion slot
    void onFfmpegCopyCompleted(bool success);
    
    // Image transformation slots
    void horizontalFlip();
    void verticalFlip();
    void invertImage();
    void resetTransformations();
    
    // Window/Level slots
    void setWindowLevelPreset(const QString& presetName);
    void showWindowLevelDialog();
    void resetWindowLevel();
    void toggleWindowLevelMode();
    
    // Zoom slots
    void zoomIn();
    void zoomOut();
    void fitToWindow();
    
    // File operations
    void openDicomDir();
    void loadDicomDir(const QString& dicomdirPath);
    void saveImage();
    void saveRun();
    
    // Save functionality implementation
    void performImageExport(const SaveImageDialog::ExportSettings& settings);
    void performVideoExport(const SaveRunDialog::ExportSettings& settings);
    bool createMP4Video(const QString& frameDir, const QString& outputPath, int framerate);
    
    // Tree widget slots
    void onTreeItemSelected(QTreeWidgetItem *current, QTreeWidgetItem *previous);
    void expandFirstItems();

private:
    // Framework setup methods
    void initializeFramework();
    void connectFrameworkSignals();
    void configurePlaybackController();
    void configureInputHandler();
    
    // UI setup methods
    void createToolbars();
    void createCentralWidget();
    void createCloseButton();
    QWidget* createImageWidget();
    void createOverlayLabels(QWidget* parent);
    void installEventFilters();
    
    // Display methods
    void updateOverlayPositions();
    void updateOverlayInfo();
    void positionOverlays();
    void updateImageDisplay();
    void updateZoomOverlay();
    void updateCursorMode();
    void updatePlayButtonIcon(const QString& iconFilename);
    
    // DICOM info panel methods
    void toggleDicomInfo();
    void createDicomInfoPanel();
    void populateDicomInfo(const QString& filePath);
    
    // Status bar methods
    void createStatusBar();
    void updateStatusBar(const QString& message, int progress = -1);
    
    // Progressive loading methods
    void displayCachedFrame(int frameIndex);
    void clearFrameCache();
    void setTransformationActionsEnabled(bool enabled);
    
    // Framework helper methods (simplified)
    
    // DICOM image loading methods
    void loadDicomImage(const QString& filePath);
    QPixmap convertDicomFrameToPixmap(const QString& filePath, int frameIndex = 0);
    void setupMultiframePlayback(const QString& filePath);
    
    // Image processing methods - Pipeline Architecture
    void processThroughPipeline();
    void resetZoomToFit();
    
    // Windowing methods
    void startWindowing(const QPoint& pos);
    void updateWindowing(const QPoint& pos);
    void endWindowing();
    void applyWindowLevel(double center, double width);
    
    // Utility methods
    void autoLoadDicomdir();
    double calculateFitToWindowZoom();
    QString findFfmpegExecutable();  // Find ffmpeg.exe in local directory or DVD drive
    
    // FFmpeg copy methods
    void checkInitialFfmpegAvailability();
    
    // Logging methods
    void initializeLogging();
    void logMessage(const QString& level, const QString& message);
    bool copyFfmpegExe();  // Copy ffmpeg executable after DICOM files are copied
    
    // Tree navigation methods
    QTreeWidgetItem* findNextSelectableItem(QTreeWidgetItem* currentItem);
    QTreeWidgetItem* findPreviousSelectableItem(QTreeWidgetItem* currentItem);
    void selectFirstImageItem();
    void selectLastImageItem();
    bool isSelectableItem(QTreeWidgetItem* item);
    bool isImageItem(QTreeWidgetItem* item);
    QTreeWidgetItem* findFirstSelectableChild(QTreeWidgetItem* parent);
    QTreeWidgetItem* findFirstImageChild(QTreeWidgetItem* parent);
    QTreeWidgetItem* findLastSelectableChild(QTreeWidgetItem* parent);
    
    // DICOM metadata methods
    void extractDicomMetadata(const QString& filePath);
    QString cleanDicomText(const QString& text);
    
    // Member variables for UI components
    QWidget* m_centralWidget;
    QToolBar* m_topToolbar;
    QPushButton* m_closeButton;
    QAction* m_playAction; // Play/Pause button action
    QAction* m_windowLevelToggleAction; // Window/Level mode toggle button action
    QAction* m_saveImageAction; // Save image button action
    QAction* m_saveRunAction; // Save run button action
    
    // Left sidebar
    QFrame* m_leftSidebar;
    QTreeWidget* m_dicomTree;
    
    // Main content area
    QStackedWidget* m_mainStack;
    QWidget* m_imageWidget;
    QTextEdit* m_reportArea;
    
    // Image display components
    QLabel* m_imageLabel;
    QGraphicsView* m_graphicsView;
    QGraphicsScene* m_graphicsScene;
    QGraphicsPixmapItem* m_pixmapItem;
    
    // Overlay labels
    QLabel* m_overlayTopLeft;
    QLabel* m_overlayTopRight;
    QLabel* m_overlayBottomLeft;
    QLabel* m_overlayBottomRight;
    
    // DICOM info panel
    bool m_dicomInfoVisible;
    QWidget* m_dicomInfoWidget;
    QTextEdit* m_dicomInfoTextEdit;
    
    // Professional framework components (simplified)
    DicomPlaybackController* m_playbackController;
    DicomInputHandler* m_inputHandler;
    
    // Legacy state variables (will be moved to controllers)
    int m_currentFrame;
    int m_totalFrames;
    int m_currentDisplayedFrame;  // Track which frame is actually displayed
    bool m_isPlaying;
    bool m_playbackPausedForFrame;  // Track if playback was paused waiting for a frame
    QTimer* m_playbackTimer;
    QString m_currentImagePath;
    
    // Progressive loading variables
    ProgressiveFrameLoader* m_progressiveLoader;
    DicomFrameProcessor* m_frameProcessor;
    bool m_isLoadingProgressively;
    bool m_allFramesCached;
    QMap<int, QPixmap> m_frameCache;
    QMap<int, QByteArray> m_originalPixelCache;
    
    // Progressive display timing control
    QTimer* m_progressiveTimer;
    qint64 m_lastProgressiveDisplayTime;
    int m_targetProgressiveFPS;
    
    // Image processing pipeline
    ImageProcessingPipeline* m_imagePipeline;
    bool m_transformationsEnabled;
    QMap<QString, QAction*> m_transformationActions;
    
    // Zoom properties
    double m_zoomFactor;
    double m_minZoomFactor;
    double m_maxZoomFactor;
    double m_zoomIncrement;
    QPixmap m_currentPixmap;
    QPixmap m_originalPixmap;  // Store the original unmodified pixmap
    
    // Window/Level properties
    bool m_windowingActive;
    bool m_windowLevelModeEnabled;  // Toggle for enabling/disabling window/level mode
    QPoint m_windowingStartPos;
    double m_originalWindowCenter;
    double m_originalWindowWidth;
    double m_currentWindowCenter;
    double m_currentWindowWidth;
    double m_windowingSensitivity;
    
    // Icon path
    QString m_iconPath;
    
    // DICOM metadata for overlays
    QString m_currentPatientId;
    QString m_currentPatientName;
    QString m_currentPatientSex;
    QString m_currentPatientAge;
    QString m_currentStudyDescription;
    QString m_currentSeriesDescription;
    QString m_currentPerformingPhysician;
    QString m_currentInstitutionName;
    QString m_currentAcquisitionDate;
    QString m_currentAcquisitionTime;
    double m_currentPositionerPrimaryAngle;
    double m_currentPositionerSecondaryAngle;
    double m_currentXRayTubeCurrent;
    double m_currentKVP;
    bool m_hasPositionerAngles;
    bool m_hasTechnicalParams;
    
    // Original pixel data for transformations
    QByteArray m_originalPixelData;
    
    // DICOM reader
    DicomReader* m_dicomReader;
    
    // DVD copy management system (now handled by DvdCopyWorker)
    QTimer* m_copyProgressTimer;  // Still used for periodic tree updates
    QString m_dvdSourcePath;      // Still used for tracking source path
    QString m_localDestPath;      // Still used for destination path
    bool m_copyInProgress;        // Still used for copy state tracking
    int m_currentCopyProgress;    // Still used for progress tracking
    bool m_dvdDetectionInProgress; // Prevent multiple simultaneous DVD detection
    bool m_ffmpegCopyCompleted;   // Track if ffmpeg copy has completed
    QStringList m_completedFiles; // Track files that have reached 100% completion
    QSet<QString> m_fullyCompletedFiles; // Track files that are fully accessible (100% + file exists)
    
    // Background DVD detection and copy worker
    QThread* m_dvdWorkerThread;
    DvdCopyWorker* m_dvdWorker;
    bool m_workerReady;           // Track if worker thread is ready to receive signals
    
    // Pending sequential copy data
    QString m_pendingDvdPath;
    QStringList m_pendingOrderedFiles;
    bool m_firstImageAutoSelected;  // Track if we've auto-selected the first completed image
    
    // Copy progress UI components
    QWidget* m_progressWidget;
    QLabel* m_progressLabel;
    QProgressBar* m_progressBar;
    
    // Status bar components
    QStatusBar* m_statusBar;
    QLabel* m_statusLabel;
    QProgressBar* m_statusProgressBar;
    
    // Helper methods for transformations
    bool hasOriginalPixelData() const;
    void refreshCurrentFrameDisplay();
    QByteArray flipVerticallyInternal(const QByteArray& data);
    QByteArray flipHorizontallyInternal(const QByteArray& data);
    QByteArray invertPixelDataInternal(const QByteArray& data);
    void convertToDisplayFormat(const QByteArray& transformedData);
    void applyWindowingToTransformedData(const QByteArray& data, double center, double width);
    void applyTransformationsToAllCachedFrames();
    
    // Helper method to register transformation actions for enable/disable
    void registerTransformationAction(const QString& name, QAction* action);
    
    // DVD copy management methods
    void detectAndStartDvdCopy();
    QString findDvdWithDicomFiles();
    void handleMissingFile(const QString& path);
    void parseRobocopyOutput(const QString& output);
    qint64 getExpectedFileSize(const QString& filePath);
    bool hasActuallyMissingFiles();
    QStringList getOrderedFileList();
    
    // Background DVD worker methods
    void initializeDvdWorker();
    void updateTreeItemWithProgress(const QString& fileName, int progress);
    void updateSpecificTreeItemProgress(const QString& fileName, int progress);
    void autoSelectFirstCompletedImage();  // Auto-select first completed image for better UX
    
    // RDSR (Radiation Dose Structured Report) methods
    void displayReport(const QString& filePath);
    QString formatSRReport(const QString& filePath);
    bool isRadiationDoseSR(const QString& filePath);
    QString formatRadiationDoseReport(const QString& filePath);
    QString formatRDSRHeader(const QString& filePath);
    QString formatRDSRProcedureInfo(const QString& filePath);
    QString formatAccumulatedDoseData(const QString& filePath);
    QString formatIrradiationEvents(const QString& filePath);
    QString formatDateTime(const QString& dtString);
    QString formatMeasurement(const QString& name, const QString& value, const QString& unit = QString(), int indent = 0);
    QString getCodeSequenceValue(const QString& filePath, const QString& tagPath);
    QString extractDoseValue(const QString& filePath, const QString& conceptName);
    QString extractEventData(const QString& filePath, int eventIndex);
    QString formatRadiationEvent(const QString& eventData, int eventNum);
    QString formatFilterInfo(const QString& filterData, int indent = 2);
    
private:
    // Logging members
    QString m_logFilePath;
    QMutex m_logMutex;
};
