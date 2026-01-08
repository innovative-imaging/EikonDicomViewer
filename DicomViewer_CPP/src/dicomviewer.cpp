#include "dicomviewer.h"
#include "DicomFrameProcessor.h"
#include "saveimagedialog.h"
#include "saverundialog.h"
#include "dvdcopyworker.h"
#include "thumbnailTask.h"

#include <chrono>
#include <cstdlib> // For std::exit
#include <QtWidgets/QApplication>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtGui/QPainter>
#include <QtCore/QDir>
#include <QtCore/QStandardPaths>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QTextStream>
#include <QtCore/QBuffer>
#include <QtCore/QDataStream>
#include <QtCore/QTimer>
#include <QtCore/QThread>
#include <QtCore/QThreadPool>
#include <QtCore/QRunnable>
#include <QtCore/QMutex>
#include <QtCore/QMetaObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QGridLayout>

#ifdef HAVE_DCMTK
#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmimgle/dcmimage.h"
#include "dcmtk/dcmdata/dcfilefo.h"
#include "dcmtk/dcmdata/dcdeftag.h"
#include "dcmtk/ofstd/ofstd.h"
// Include JPEG decompression support
#include "dcmtk/dcmjpeg/djdecode.h"
#include "dcmtk/dcmjpeg/djencode.h"
#include "dcmtk/dcmjpeg/djrplol.h"
#include "dcmtk/dcmdata/dccodec.h"
#endif

// Thread-aware logging helpers
#define LOG_THREAD_ID() QString("[Thread:0x%1]").arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16)

// ========== PATH NORMALIZATION UTILITIES ==========
// Defensive path comparison to handle Windows short vs long path formats
static bool pathsAreEquivalent(const QString& path1, const QString& path2) {
    if (path1 == path2) {
        return true; // Exact match - fastest case
    }
    
    // Normalize both paths to absolute format for comparison
    QFileInfo info1(path1);
    QFileInfo info2(path2);
    
    // Compare normalized absolute file paths
    return info1.absoluteFilePath() == info2.absoluteFilePath();
}
#define qDebugT() qDebug() << LOG_THREAD_ID()

// Global logging function implementation for use by other files
static DicomViewer* g_dicomViewer = nullptr;

// ========== IMAGE PROCESSING PIPELINE IMPLEMENTATION ==========

ImageProcessingPipeline::ImageProcessingPipeline()
    : m_hFlipEnabled(false)
    , m_vFlipEnabled(false)
    , m_invertEnabled(false)
    , m_windowLevelEnabled(true)
    , m_windowCenter(0.0)    // Default center for medical imaging
    , m_windowWidth(2000.0)  // Default width for medical imaging
    , m_bitsStored(8)        // Default to 8-bit
{
}

void ImageProcessingPipeline::setHorizontalFlipEnabled(bool enabled)
{
    m_hFlipEnabled = enabled;
}

void ImageProcessingPipeline::setVerticalFlipEnabled(bool enabled)
{
    m_vFlipEnabled = enabled;
}

void ImageProcessingPipeline::setInvertEnabled(bool enabled)
{
    m_invertEnabled = enabled;
}

void ImageProcessingPipeline::setWindowLevel(double windowCenter, double windowWidth)
{
    m_windowCenter = windowCenter;
    m_windowWidth = windowWidth;
}

void ImageProcessingPipeline::setWindowLevelEnabled(bool enabled)
{
    m_windowLevelEnabled = enabled;
}

void ImageProcessingPipeline::setBitsStored(int bitsStored)
{
    m_bitsStored = bitsStored;
}

void ImageProcessingPipeline::resetAllTransformations()
{
    m_hFlipEnabled = false;
    m_vFlipEnabled = false;
    m_invertEnabled = false;
    m_windowLevelEnabled = true;
    m_windowCenter = 0.0;    // Default center for medical imaging
    m_windowWidth = 2000.0;  // Default width for medical imaging
    // Note: Don't reset m_bitsStored - it should persist for the image
}

bool ImageProcessingPipeline::hasAnyTransformations() const
{
    return m_hFlipEnabled || m_vFlipEnabled || m_invertEnabled || m_windowLevelEnabled;
}

QImage ImageProcessingPipeline::processImage(const QImage& sourceImage) const
{
    if (sourceImage.isNull()) {
        return QImage(); // Pass through null images
    }

    
    // Pipeline: Decompressed Image ? Window/Level ? H-Flip ? V-Flip ? Invert ? Display
    // Note: Decompression already handled by DCMTK/GDCM/libjpeg libraries
    QImage result = sourceImage;
    result = windowLevelStage(result);
    result = horizontalFlipStage(result);
    result = verticalFlipStage(result);
    result = invertStage(result);
    
    return result;
}

QImage ImageProcessingPipeline::horizontalFlipStage(const QImage& input) const
{
    if (!m_hFlipEnabled) {
        return input; // Pass through unchanged if disabled
    }
    
    // Note: Following Python behavior where horizontal_flip uses vertical flip (np.flipud)
    return input.mirrored(false, true); // Flip vertically
}

QImage ImageProcessingPipeline::verticalFlipStage(const QImage& input) const
{
    if (!m_vFlipEnabled) {
        return input; // Pass through unchanged if disabled
    }
    
    // Note: Following Python behavior where vertical_flip uses horizontal flip (np.fliplr)
    return input.mirrored(true, false); // Flip horizontally
}

QImage ImageProcessingPipeline::invertStage(const QImage& input) const
{
    if (!m_invertEnabled) {
        return input; // Pass through unchanged if disabled
    }
    
    QImage result = input;
    result.invertPixels();
    return result;
}

QImage ImageProcessingPipeline::windowLevelStage(const QImage& input) const
{
    if (!m_windowLevelEnabled) {
        return input; // Pass through unchanged if disabled
    }
    
    
    // Apply window/level transformation
    double minValue = m_windowCenter - (m_windowWidth / 2.0);
    double maxValue = m_windowCenter + (m_windowWidth / 2.0);
    
    
    QImage result = input.convertToFormat(QImage::Format_RGB32);
    
    // Sample some pixels to debug pixel value range
    static bool debugSampled = false;
    if (!debugSampled) {
        debugSampled = true;
        for (int y = 0; y < qMin(result.height(), 10); y += 2) {
            const QRgb* scanLine = reinterpret_cast<const QRgb*>(result.constScanLine(y));
            for (int x = 0; x < qMin(result.width(), 10); x += 2) {
                QRgb pixel = scanLine[x];
                int gray = qGray(pixel);
                double maxPixelValue = (1 << m_bitsStored) - 1;
                double originalPixelValue = (gray / 255.0) * maxPixelValue;
            }
        }
    }
    
    for (int y = 0; y < result.height(); ++y) {
        QRgb* scanLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            QRgb pixel = scanLine[x];
            
            // Get the grayscale value from the 8-bit image (0-255)
            // DCMTK has already converted from original bit depth to 8-bit
            // We should apply windowing directly to this 8-bit data
            int pixelValue = qGray(pixel);
            
            // Apply window/level algorithm directly to 8-bit data
            double windowedValue;
            if (m_windowWidth > 1) {  // Minimum meaningful width
                if (pixelValue <= minValue) {
                    windowedValue = 0.0;
                } else if (pixelValue >= maxValue) {
                    windowedValue = 255.0;
                } else {
                    // Scale within window range to 0-255
                    windowedValue = ((pixelValue - minValue) / m_windowWidth) * 255.0;
                }
            } else {
                // If width too small, use mid-gray
                windowedValue = 128.0;
            }
            
            int newGray = qBound(0, static_cast<int>(windowedValue), 255);
            scanLine[x] = qRgb(newGray, newGray, newGray);
        }
    }
    
    return result;
}



// ========== DICOM VIEWER IMPLEMENTATION ==========

DicomViewer::DicomViewer(QWidget *parent, const QString& sourceDrive)
    : QMainWindow(parent)
    , m_providedSourceDrive(sourceDrive)
    , m_playbackController(nullptr)
    , m_inputHandler(nullptr)
    , m_centralWidget(nullptr)
    , m_topToolbar(nullptr)
    , m_closeButton(nullptr)
    , m_playAction(nullptr)
    , m_windowLevelToggleAction(nullptr)
    , m_saveImageAction(nullptr)
    , m_saveRunAction(nullptr)
    , m_leftSidebar(nullptr)
    , m_dicomTree(nullptr)
    , m_thumbnailPanel(nullptr)
    , m_thumbnailList(nullptr)
    , m_thumbnailToggleButton(nullptr)
    , m_thumbnailPanelCollapsed(false)  // Default to expanded
    , m_completedThumbnails(0)
    , m_totalThumbnails(0)
    , m_activeThumbnailTasks(0)
    , m_mainStack(nullptr)
    , m_imageWidget(nullptr)
    , m_reportArea(nullptr)
    , m_imageLabel(nullptr)
    , m_graphicsView(nullptr)
    , m_graphicsScene(nullptr)
    , m_pixmapItem(nullptr)
    , m_overlayTopLeft(nullptr)
    , m_overlayTopRight(nullptr)
    , m_overlayBottomLeft(nullptr)
    , m_overlayBottomRight(nullptr)
    , m_currentFrame(0)
    , m_totalFrames(1)
    , m_currentDisplayedFrame(-1)
    , m_isPlaying(false)
    , m_playbackPausedForFrame(false)
    , m_playbackTimer(nullptr)
    , m_progressiveLoader(nullptr)
    , m_frameProcessor(nullptr)
    , m_isLoadingProgressively(false)
    , m_allFramesCached(false)
    , m_progressiveTimer(nullptr)
    , m_lastProgressiveDisplayTime(0)
    , m_targetProgressiveFPS(15) // Increased from 7 to match GDCM performance capabilities
    , m_imagePipeline(new ImageProcessingPipeline())
    , m_transformationsEnabled(true)
    , m_zoomFactor(1.0)
    , m_minZoomFactor(0.1)
    , m_maxZoomFactor(4.0)
    , m_zoomIncrement(1.05)
    , m_windowingActive(false)
    , m_windowLevelModeEnabled(false)  // Window/level mode disabled by default
    , m_originalWindowCenter(0)
    , m_originalWindowWidth(0)
    , m_currentWindowCenter(0)
    , m_currentWindowWidth(0)
    , m_windowingSensitivity(1.0)
    , m_currentPositionerPrimaryAngle(0.0)
    , m_currentPositionerSecondaryAngle(0.0)
    , m_currentXRayTubeCurrent(0.0)
    , m_currentKVP(0.0)
    , m_hasPositionerAngles(false)
    , m_hasTechnicalParams(false)
    , m_dicomReader(nullptr)
    , m_dicomInfoVisible(false)
    , m_dicomInfoWidget(nullptr)
    , m_dicomInfoTextEdit(nullptr)
    , m_cachedDicomInfoFilePath()
    , m_cachedDicomInfoHtml()
    , m_copyProgressTimer(nullptr)
    , m_copyInProgress(false)
    , m_currentCopyProgress(0)
    , m_dvdDetectionInProgress(false)
    , m_ffmpegCopyCompleted(false)
    , m_allThumbnailsComplete(false)
    , m_completedFiles()
    , m_fullyCompletedFiles()
    , m_workerReady(false)
    , m_thumbnailGenerationActive(0)    // NEW: Initialize atomic counter
    , m_firstImageAutoSelected(false)
    , m_progressWidget(nullptr)
    , m_progressLabel(nullptr)
    , m_progressBar(nullptr)
    , m_statusBar(nullptr)
    , m_statusLabel(nullptr)
    , m_statusProgressBar(nullptr)
    // State-based architecture initialization
    , m_selectionInProgress(false)
    , m_thumbnailPanelProcessingActive(false)
    , m_displayMonitor(nullptr)
    , m_displayMonitorActive(false)
    , m_firstImageMonitor(nullptr)
    , m_firstImageFound(false)
{
    // Initialize logging first
    initializeLogging();
    
    // Remove title bar and make frameless
    setWindowFlags(Qt::FramelessWindowHint);
    
    // Get screen geometry for full screen like Python version
    QScreen* screen = QGuiApplication::primaryScreen();
    QRect screenGeometry = screen->geometry();
    setGeometry(screenGeometry);
    
    // Icons are now embedded in resources, no need for external path
    m_iconPath = ":/icons";
    
    // Initialize DICOM reader
    m_dicomReader = new DicomReader();
    
#ifdef HAVE_DCMTK
    // Register JPEG decompression codecs for compressed DICOM images
    DJDecoderRegistration::registerCodecs();
#endif
    
    // Create timer
    m_playbackTimer = new QTimer(this);
    connect(m_playbackTimer, &QTimer::timeout, this, &DicomViewer::nextFrame);
    
    // Create progressive display timer for FPS-controlled progressive loading
    m_progressiveTimer = new QTimer(this);
    m_progressiveTimer->setSingleShot(true);
    connect(m_progressiveTimer, &QTimer::timeout, this, &DicomViewer::onProgressiveTimerTimeout);
    
    // Create copy progress timer
    m_copyProgressTimer = new QTimer(this);
    connect(m_copyProgressTimer, &QTimer::timeout, this, &DicomViewer::onCopyProgressTimeout);
    
    // Setup paths for DVD copying with normalization to prevent short/long path format inconsistencies
    m_localDestPath = PathNormalizer::getCanonicalDestPath();
    logMessage(LOG_INFO, QString("PathNormalizer: Canonical destination path initialized: %1").arg(m_localDestPath));
    
    // Initialize frame processor
    m_frameProcessor = new DicomFrameProcessor();
    
    // Create proper central widget with layout
    m_centralWidget = new QWidget;
    setCentralWidget(m_centralWidget);
    
    QHBoxLayout* mainLayout = new QHBoxLayout(m_centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    
    // Step 2: Add left sidebar
    m_leftSidebar = new QFrame;
    m_leftSidebar->setObjectName("left_sidebar");
    m_leftSidebar->setFixedWidth(250);
    m_leftSidebar->setStyleSheet(R"(
        QFrame#left_sidebar { 
            background-color: #2a2a2a; 
            border-right: 1px solid #444444; 
        }
    )");
    
    QVBoxLayout* sidebarLayout = new QVBoxLayout(m_leftSidebar);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    sidebarLayout->setSpacing(0);
    m_dicomTree = new QTreeWidget;
    
    // Ensure tree widget shows branches and indentation properly
    m_dicomTree->setRootIsDecorated(true);  // Show expand/collapse triangles
    m_dicomTree->setIndentation(25);        // Indentation for each level
    m_dicomTree->setUniformRowHeights(false);
    m_dicomTree->setItemsExpandable(true);  // Allow expanding/collapsing
    m_dicomTree->setExpandsOnDoubleClick(true);  // Double-click to expand
    m_dicomTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_dicomTree->setHeaderHidden(false);    // Show header
    m_dicomTree->header()->setStretchLastSection(true);  // Stretch header to fit
    
    m_dicomTree->setHeaderLabel("All patients (Patients: 0, Images: 0)");
    // Improved styling to ensure tree structure is clearly visible
    m_dicomTree->setStyleSheet(R"(
        QTreeWidget {
            background-color: #2a2a2a;
            color: white;
            border: none;
            font-size: 12px;
            outline: none;
        }
        QTreeWidget::item {
            padding: 4px;
            border: none;
            min-height: 20px;
        }
        QTreeWidget::item:selected {
            background-color: #0078d7;
            color: white;
        }
        QTreeWidget::item:hover {
            background-color: #404040;
        }
        QTreeWidget::branch {
            background: transparent;
        }
        QTreeWidget::branch:has-siblings:!adjoins-item {
            border-image: none;
            border-left: 1px solid #666666;
        }
        QTreeWidget::branch:has-siblings:adjoins-item {
            border-image: none;
            border-left: 1px solid #666666;
            border-top: 1px solid #666666;
        }
        QTreeWidget::branch:!has-children:!has-siblings:adjoins-item {
            border-image: none;
            border-left: 1px solid #666666;
            border-top: 1px solid #666666;
        }
        QTreeWidget::branch:closed:has-children:has-siblings {
            border-image: none;
            border-left: 1px solid #666666;
            image: url(none);
        }
        QTreeWidget::branch:open:has-children:has-siblings {
            border-image: none;
            border-left: 1px solid #666666;
            image: url(none);
        }
        QTreeWidget::branch:closed:has-children:!has-siblings {
            border-image: none;
            image: url(none);
        }
        QTreeWidget::branch:open:has-children:!has-siblings {
            border-image: none;
            image: url(none);
        }
        QHeaderView::section {
            background-color: #3a3a3a;
            color: white;
            padding: 5px;
            border: none;
            font-weight: bold;
        }
    )");
    
    connect(m_dicomTree, &QTreeWidget::currentItemChanged, 
            this, &DicomViewer::onTreeItemSelected);
    
    // Event filter will be installed after UI is fully initialized
    // m_dicomTree->installEventFilter(this);
    
    if (sidebarLayout && m_dicomTree) {
        sidebarLayout->addWidget(m_dicomTree);
    }
    
    if (mainLayout && m_leftSidebar) {
        mainLayout->addWidget(m_leftSidebar);
    }
    
    // Step 2.25: Create Thumbnail Panel
    createThumbnailPanel();
    if (mainLayout && m_thumbnailPanel) {
        mainLayout->addWidget(m_thumbnailPanel);
    }

    // Step 2.5: Create DICOM Info Panel (between tree and main content like Python version)
    m_dicomInfoWidget = new QFrame;
    m_dicomInfoWidget->setObjectName("dicom_info_panel"); 
    m_dicomInfoWidget->setFixedWidth(400);  // Same as Python version
    m_dicomInfoWidget->setStyleSheet("QFrame#dicom_info_panel { background-color: #2a2a2a; border-right: 1px solid #666; }");
    m_dicomInfoWidget->hide();  // Initially hidden like Python
    
    QVBoxLayout* dicomInfoLayout = new QVBoxLayout(m_dicomInfoWidget);
    dicomInfoLayout->setContentsMargins(0, 0, 0, 0);
    dicomInfoLayout->setSpacing(0);
    
    // Header with title (like Python)
    QVBoxLayout* headerLayout = new QVBoxLayout;
    headerLayout->setContentsMargins(5, 5, 5, 5);
    
    QLabel* titleLabel = new QLabel("DICOM Tags");
    titleLabel->setStyleSheet("color: white; font-weight: bold; font-size: 14px; padding: 5px;");
    headerLayout->addWidget(titleLabel);
    
    dicomInfoLayout->addLayout(headerLayout);
    
    // DICOM info text area
    m_dicomInfoTextEdit = new QTextEdit;
    m_dicomInfoTextEdit->setObjectName("dicom_info_text");
    m_dicomInfoTextEdit->setReadOnly(true);
    m_dicomInfoTextEdit->setStyleSheet(R"(
        QTextEdit {
            background-color: #2b2b2b;
            color: white;
            border: 1px solid #666666;
            padding: 2px;
            selection-background-color: #0078d4;
        }
    )");
    dicomInfoLayout->addWidget(m_dicomInfoTextEdit);
    
    // Add DICOM info widget to main layout
    mainLayout->addWidget(m_dicomInfoWidget);
    
    // Debug: Confirm widget creation
    logMessage("DEBUG", QString("[DICOM INFO] DICOM info widget created successfully in constructor. Widget pointer: %1").arg(reinterpret_cast<quintptr>(m_dicomInfoWidget)));
    
    // Step 3: Main content area with stacked widget
    m_mainStack = new QStackedWidget;
    
    // Create image display widget
    m_imageWidget = new QWidget;
    m_imageWidget->setStyleSheet("background-color: black;");
    
    QVBoxLayout* imageLayout = new QVBoxLayout(m_imageWidget);
    imageLayout->setContentsMargins(0, 0, 0, 0);
    
    // Create placeholder label
    m_imageLabel = new QLabel("Select a DICOMDIR file to begin.");
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setStyleSheet("color: white; font-size: 16px;");
    
    // Create graphics view (hidden initially)
    m_graphicsView = new QGraphicsView;
    m_graphicsScene = new QGraphicsScene;
    m_graphicsView->setScene(m_graphicsScene);
    
    // Configure graphics view
    m_graphicsView->setDragMode(QGraphicsView::NoDrag);
    m_graphicsView->setRenderHint(QPainter::Antialiasing);
    m_graphicsView->setRenderHint(QPainter::SmoothPixmapTransform);
    m_graphicsView->setBackgroundBrush(QBrush(QColor(0, 0, 0)));
    m_graphicsView->setMouseTracking(true);
    m_graphicsView->viewport()->setMouseTracking(true);
    
    // *** INSTALL EVENT FILTERS IMMEDIATELY AFTER GRAPHICS VIEW CREATION ***
    m_graphicsView->installEventFilter(this);
    m_graphicsView->viewport()->installEventFilter(this);
    
    imageLayout->addWidget(m_imageLabel);
    imageLayout->addWidget(m_graphicsView);
    
    // Initially hide graphics view
    m_graphicsView->hide();
    
    // Create report area
    m_reportArea = new QTextEdit;
    m_reportArea->setObjectName("report_area");
    m_reportArea->setReadOnly(true);
    m_reportArea->setStyleSheet(R"(
        QTextEdit#report_area { 
            background-color: #ffffff; 
            color: #000000; 
            border: none; 
            font-family: "Courier New", Courier, monospace; 
            font-size: 12px; 
        }
    )");
    
    m_mainStack->addWidget(m_imageWidget);
    m_mainStack->addWidget(m_reportArea);
    
    mainLayout->addWidget(m_mainStack);
    
    // Initialize professional framework BEFORE creating toolbar
    initializeFramework();
    
    // Step 1: Add toolbar
    createToolbars();
    
    // Check FFmpeg availability immediately after toolbar creation
    checkInitialFfmpegAvailability();
    
    // Step 4: Add overlay labels
    createOverlayLabels(m_imageWidget);
    
    // Step 5: Create status bar
    createStatusBar();
    
    // Step 7: Initialize DVD worker thread first
    initializeDvdWorker();
    
    // Initialize Display Monitor System
    initializeDisplayMonitor();
    
    // Initialize file availability monitoring state
    m_fileAvailabilityMonitoringActive = false;
    
    // Set global DicomViewer instance for logging functions
    g_dicomViewer = this;
    
    // Step 8: Auto-load DICOMDIR if present in executable directory
    // Use QTimer::singleShot to defer DVD operations until after constructor completes
    QTimer::singleShot(0, this, [this]() {
        autoLoadDicomdir();
    });
    
    // Step 8: Install event filters after full initialization
    installEventFilters();
}

DicomViewer::~DicomViewer()
{
    // Clear global pointer
    g_dicomViewer = nullptr;
    
    // Stop monitoring systems
    stopFileAvailabilityMonitoring();
    stopFirstImageMonitor();
    
    if (m_playbackTimer && m_playbackTimer->isActive()) {
        m_playbackTimer->stop();
    }
    
    // Stop and clean up progressive loader
    if (m_progressiveLoader) {
        m_progressiveLoader->stop();
        m_progressiveLoader->wait();
        delete m_progressiveLoader;
    }
    
    // Clean up DVD worker thread
    if (m_dvdWorkerThread) {
        m_dvdWorkerThread->quit();
        m_dvdWorkerThread->wait();
    }
    
    delete m_dicomReader;
    delete m_frameProcessor;
    delete m_imagePipeline;
    
#ifdef HAVE_DCMTK
    // Clean up JPEG decompression codecs
    DJDecoderRegistration::cleanup();
#endif
}

void DicomViewer::installEventFilters()
{
    // Install event filters safely after full UI initialization
    if (m_dicomTree) {
        m_dicomTree->installEventFilter(this);
    }
    
    if (m_graphicsView) {
        m_graphicsView->installEventFilter(this);
        if (m_graphicsView->viewport()) {
            m_graphicsView->viewport()->installEventFilter(this);
        }
    }
}

void DicomViewer::initializeDvdWorker()
{
    // Initialize DVD worker thread and worker object
    m_dvdWorkerThread = new QThread(this);
    m_dvdWorker = new DvdCopyWorker(m_localDestPath);
    m_workerReady = false;  // Reset worker ready state
    
    // Set preferred source drive if provided via command line
    if (!m_providedSourceDrive.isEmpty()) {
        qDebugT() << "Setting preferred source drive for DVD worker:" << m_providedSourceDrive;
        m_dvdWorker->setPreferredSourceDrive(m_providedSourceDrive);
    }
    
    // Move worker to thread
    m_dvdWorker->moveToThread(m_dvdWorkerThread);
    
    // Connect thread signals
    connect(m_dvdWorkerThread, &QThread::started, 
            m_dvdWorker, &DvdCopyWorker::emitWorkerReady);
    connect(m_dvdWorkerThread, &QThread::started, 
            m_dvdWorker, &DvdCopyWorker::startDvdDetectionAndCopy);
    connect(m_dvdWorkerThread, &QThread::finished, 
            m_dvdWorker, &QObject::deleteLater);
    
    // Connect worker signals to main thread slots
    connect(m_dvdWorker, &DvdCopyWorker::workerReady,
            this, &DicomViewer::onWorkerReady);
    connect(m_dvdWorker, &DvdCopyWorker::dvdDetected, 
            this, &DicomViewer::onDvdDetected);
    connect(m_dvdWorker, &DvdCopyWorker::copyStarted, 
            this, &DicomViewer::onCopyStarted);
    connect(m_dvdWorker, &DvdCopyWorker::fileProgress, 
            this, &DicomViewer::onFileProgress);
    connect(m_dvdWorker, &DvdCopyWorker::overallProgress, 
            this, &DicomViewer::onOverallProgress);
    connect(m_dvdWorker, &DvdCopyWorker::copyCompleted, 
            this, &DicomViewer::onCopyCompleted);
    connect(m_dvdWorker, &DvdCopyWorker::copyCompleted, 
            this, [this](bool success) {
                logMessage("DEBUG", QString("*** LAMBDA: copyCompleted signal received with success: %1 ***").arg(success ? "true" : "false"));
            });
    connect(m_dvdWorker, &DvdCopyWorker::workerError, 
            this, &DicomViewer::onWorkerError);
    connect(m_dvdWorker, &DvdCopyWorker::statusChanged, 
            this, [this](const QString& status) {
                logMessage("DEBUG", QString("DVD Worker Status: %1").arg(status));
            });
    connect(m_dvdWorker, &DvdCopyWorker::fileCompleted,
            this, &DicomViewer::onFileReadyForThumbnail);
    
    // Connect signal for sequential robocopy (only method used)
    bool seqConnected = connect(this, &DicomViewer::requestSequentialRobocopyStart,
                               m_dvdWorker, &DvdCopyWorker::startSequentialRobocopy,
                               Qt::QueuedConnection);
    logMessage("DEBUG", QString("[DVD WORKER] Sequential robocopy signal connection established: %1").arg(seqConnected ? "SUCCESS" : "FAILED"));
    
    // Connect ffmpeg copy completion signal to slot
    connect(this, &DicomViewer::ffmpegCopyCompleted,
            this, &DicomViewer::onFfmpegCopyCompleted,
            Qt::QueuedConnection);
}

void DicomViewer::createToolbars()
{
    m_topToolbar = addToolBar(tr("Main Toolbar"));
    m_topToolbar->setIconSize(QSize(48, 48));
    
    // Remove toolbar handles and grips
    m_topToolbar->setMovable(false);
    m_topToolbar->setFloatable(false);
    m_topToolbar->setContentsMargins(0, 0, 0, 0);
    
    // Apply dark theme to toolbar specifically
    m_topToolbar->setStyleSheet(R"(
        QToolBar { 
            background-color: #333333; 
            border: none; 
            padding: 5px; 
            margin: 0px; 
            spacing: 0px;
        }
        QToolBar::separator { width: 0px; height: 0px; }
        QToolBar QToolButton { 
            background-color: #333333; 
            border: 1px solid #555555; 
            padding: 5px; 
            margin: 2px; 
        }
        QToolBar QToolButton:hover { background-color: #555555; }
    )");
    
    // Define toolbar actions
    struct ToolbarAction {
        QString iconName;
        QString text;
        QString tooltip;
        void (DicomViewer::*slot)();
    };
    
    QList<ToolbarAction> actions = {
        {"OpenFolder_96.png", "Open", "Open DICOMDIR", &DicomViewer::openDicomDir},
        {"ZoomIn_96.png", "Zoom In", "Zoom In", &DicomViewer::zoomIn},
        {"ZoomOut_96.png", "Zoom Out", "Zoom Out", &DicomViewer::zoomOut},
        {"ZoomFit_96.png", "Fit to Window", "Fit to Window", &DicomViewer::fitToWindow},
        {"", "", "", nullptr}, // Separator
        {"previous-frame_96.png", "Prev Image", "Previous Image", &DicomViewer::previousImage},
        {"Play_96.png", "Play", "Play/Pause", &DicomViewer::togglePlayback},
        {"next-frame_96.png", "Next Image", "Next Image", &DicomViewer::nextImage},
        {"", "", "", nullptr}, // Separator
        {"HorizontalFlip_96.png", "H-Flip", "Horizontal Flip (Ctrl+H)", &DicomViewer::horizontalFlip},
        {"VerticalFlip_96.png", "V-Flip", "Vertical Flip (Ctrl+V)", &DicomViewer::verticalFlip},
        {"InvertNew.png", "Invert", "Invert (Ctrl+I)", &DicomViewer::invertImage},
        {"WWL_96.png", "W/L Mode", "Toggle Window/Level Mode", &DicomViewer::toggleWindowLevelMode},
        {"ResetSettings_96.png", "Reset All", "Reset All (Ctrl+R / Esc)", &DicomViewer::resetTransformations},
        {"", "", "", nullptr}, // Separator
        {"ImageSave_96.png", "Save Image", "Save Image", &DicomViewer::saveImage},
        {"RunSave_96.png", "Save Run", "Save Run", &DicomViewer::saveRun},
        {"", "", "", nullptr}, // Separator
        {"Info_96.png", "Info", "Toggle DICOM Info", &DicomViewer::toggleDicomInfo},
    };
    
    for (const auto& action : actions) {
        if (action.iconName.isEmpty() && action.text.isEmpty()) {
            // Add separator (both icon and text are empty)
            m_topToolbar->addSeparator();
            continue;
        }
        
        QIcon icon;
        
        if (!action.iconName.isEmpty()) {
            QString iconPath = m_iconPath + "/" + action.iconName;
            icon = QIcon(iconPath);
            
            // Check if the icon is valid (resource-based icons should always be valid)
            if (icon.isNull()) {
                // Create a simple colored rectangle as fallback
                QPixmap pixmap(48, 48);
                pixmap.fill(QColor(100, 100, 100));
                icon = QIcon(pixmap);
            }
        } else {
            // No icon specified - create a text-based button appearance
            QPixmap pixmap(48, 48);
            pixmap.fill(QColor(64, 64, 64));  // Slightly darker than fallback
            QPainter painter(&pixmap);
            painter.setPen(QColor(255, 255, 255));
            painter.setFont(QFont("Arial", 10, QFont::Bold));
            painter.drawText(pixmap.rect(), Qt::AlignCenter, "W/L");
            icon = QIcon(pixmap);
        }
        
        QAction* toolAction = m_topToolbar->addAction(icon, action.text);
        toolAction->setToolTip(action.tooltip);
        
        // Store reference to play button for icon updates
        if (action.iconName == "Play_96.png") {
            m_playAction = toolAction;
        }
        
        // Store reference to window/level toggle button
        if (action.slot == &DicomViewer::toggleWindowLevelMode) {
            m_windowLevelToggleAction = toolAction;
        }
        
        // Store references to export actions for enable/disable control
        if (action.slot == &DicomViewer::saveImage) {
            m_saveImageAction = toolAction;
        } else if (action.slot == &DicomViewer::saveRun) {
            m_saveRunAction = toolAction;
            toolAction->setEnabled(false); // Disable video export until ffmpeg copy is completed
            qDebugT() << "Save run button created and disabled - awaiting ffmpeg copy completion";
        }
        
        // Store transformation actions for enable/disable functionality
        if (action.slot == &DicomViewer::horizontalFlip) {
            m_transformationActions["horizontal_flip"] = toolAction;
        } else if (action.slot == &DicomViewer::verticalFlip) {
            m_transformationActions["vertical_flip"] = toolAction;
        } else if (action.slot == &DicomViewer::invertImage) {
            m_transformationActions["invert_image"] = toolAction;
        } else if (action.slot == &DicomViewer::resetTransformations) {
            m_transformationActions["reset_transformations"] = toolAction;
        }
        
        if (action.slot) {
            connect(toolAction, &QAction::triggered, this, action.slot);
        }
    }
    
    // Add spacer to push close button to the right
    QWidget* spacer = new QWidget();
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_topToolbar->addWidget(spacer);
    
    // Add close button at the right end of toolbar
    QString closeIconPath = m_iconPath + "/Close_96.png";
    QIcon closeIcon(closeIconPath);
    
    if (closeIcon.isNull()) {
        // Create a simple colored rectangle as fallback
        QPixmap pixmap(48, 48);
        pixmap.fill(QColor(200, 100, 100));
        QPainter painter(&pixmap);
        painter.setPen(QColor(255, 255, 255));
        painter.setFont(QFont("Arial", 24, QFont::Bold));
        painter.drawText(pixmap.rect(), Qt::AlignCenter, "X");
        closeIcon = QIcon(pixmap);
    }
    
    QAction* closeAction = m_topToolbar->addAction(closeIcon, "Close");
    closeAction->setToolTip("Close Application");
    connect(closeAction, &QAction::triggered, this, &DicomViewer::close);
}

void DicomViewer::createCentralWidget()
{
    m_centralWidget = new QWidget;
    setCentralWidget(m_centralWidget);
    
    QHBoxLayout* mainLayout = new QHBoxLayout(m_centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    
    // Left sidebar
    m_leftSidebar = new QFrame;
    m_leftSidebar->setObjectName("left_sidebar");
    m_leftSidebar->setFixedWidth(250);
    
    QVBoxLayout* sidebarLayout = new QVBoxLayout(m_leftSidebar);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    sidebarLayout->setSpacing(0);
    
    // Create tree widget
    m_dicomTree = new QTreeWidget;
    m_dicomTree->setHeaderLabel("All patients (Patients: 0, Images: 0)");
    connect(m_dicomTree, &QTreeWidget::currentItemChanged, 
            this, &DicomViewer::onTreeItemSelected);
    
    // Install event filter for tree widget
    m_dicomTree->installEventFilter(this);
    
    sidebarLayout->addWidget(m_dicomTree);
    mainLayout->addWidget(m_leftSidebar);
    
    // Create DICOM Info Panel (like Python version - between tree and main content)
    m_dicomInfoWidget = new QFrame;
    m_dicomInfoWidget->setObjectName("dicom_info_panel"); 
    m_dicomInfoWidget->setFixedWidth(400);  // Same as Python version
    m_dicomInfoWidget->setStyleSheet("QFrame#dicom_info_panel { background-color: #2a2a2a; border-right: 1px solid #666; }");
    m_dicomInfoWidget->hide();  // Initially hidden like Python
    
    QVBoxLayout* dicomInfoLayout = new QVBoxLayout(m_dicomInfoWidget);
    dicomInfoLayout->setContentsMargins(0, 0, 0, 0);
    dicomInfoLayout->setSpacing(0);
    
    // Header with title (like Python)
    QVBoxLayout* headerLayout = new QVBoxLayout;
    headerLayout->setContentsMargins(5, 5, 5, 5);
    
    QLabel* titleLabel = new QLabel("DICOM Tags");
    titleLabel->setStyleSheet("color: white; font-weight: bold; font-size: 14px; padding: 5px;");
    headerLayout->addWidget(titleLabel);
    
    dicomInfoLayout->addLayout(headerLayout);
    
    // DICOM info text area
    m_dicomInfoTextEdit = new QTextEdit;
    m_dicomInfoTextEdit->setObjectName("dicom_info_text");
    m_dicomInfoTextEdit->setReadOnly(true);
    m_dicomInfoTextEdit->setStyleSheet(R"(
        QTextEdit {
            background-color: #2b2b2b;
            color: white;
            border: 1px solid #666666;
            padding: 2px;
            selection-background-color: #0078d4;
        }
    )");
    dicomInfoLayout->addWidget(m_dicomInfoTextEdit);
    
    mainLayout->addWidget(m_dicomInfoWidget);
    
    // Main content area (stacked widget)
    m_mainStack = new QStackedWidget;
    
    // Create image display widget
    m_imageWidget = createImageWidget();
    
    // Create report area
    m_reportArea = new QTextEdit;
    m_reportArea->setObjectName("report_area");
    m_reportArea->setReadOnly(true);
    
    m_mainStack->addWidget(m_imageWidget);
    m_mainStack->addWidget(m_reportArea);
    
    mainLayout->addWidget(m_mainStack);
}

QWidget* DicomViewer::createImageWidget()
{
    QWidget* widget = new QWidget;
    widget->setStyleSheet("background-color: black;");
    
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    
    // Create graphics view for GPU-accelerated zoom
    m_graphicsView = new QGraphicsView;
    m_graphicsScene = new QGraphicsScene;
    m_graphicsView->setScene(m_graphicsScene);
    
    // Configure graphics view
    m_graphicsView->setDragMode(QGraphicsView::NoDrag);
    m_graphicsView->setRenderHint(QPainter::Antialiasing);
    m_graphicsView->setRenderHint(QPainter::SmoothPixmapTransform);
    m_graphicsView->setBackgroundBrush(QBrush(QColor(0, 0, 0)));
    
    // Enable mouse tracking for windowing
    m_graphicsView->setMouseTracking(true);
    m_graphicsView->viewport()->setMouseTracking(true);
    
    // Install event filter - NOTE: This function is not used in main constructor path
    // Event filters are installed in constructor instead
    // m_graphicsView->installEventFilter(this);
    // m_graphicsView->viewport()->installEventFilter(this);
    
    // Create placeholder label
    m_imageLabel = new QLabel("Select a DICOMDIR file to begin.");
    m_imageLabel->setObjectName("image_display");
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setStyleSheet("color: white; font-size: 16px;");
    
    layout->addWidget(m_imageLabel);
    layout->addWidget(m_graphicsView);
    
    // Initially hide graphics view
    m_graphicsView->hide();
    
    // Create overlay labels
    createOverlayLabels(widget);
    
    return widget;
}

void DicomViewer::createThumbnailPanel()
{
    m_thumbnailPanel = new QFrame;
    m_thumbnailPanel->setObjectName("thumbnail_panel");
    m_thumbnailPanel->setFixedWidth(220);
    m_thumbnailPanel->setStyleSheet(R"(
        QFrame#thumbnail_panel { 
            background-color: #2a2a2a; 
            border-left: 1px solid #666666;
            border-right: 1px solid #444444; 
        }
    )");
    
    QVBoxLayout* thumbnailLayout = new QVBoxLayout(m_thumbnailPanel);
    thumbnailLayout->setContentsMargins(5, 5, 10, 5);  // Reduced right margin
    thumbnailLayout->setSpacing(0);
    
    // Header with toggle button
    QWidget* headerWidget = new QWidget;
    QHBoxLayout* headerLayout = new QHBoxLayout(headerWidget);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(5);
    
    // Header label
    QLabel* headerLabel = new QLabel("Thumbnails");
    headerLabel->setStyleSheet("color: white; font-weight: bold; font-size: 12px; padding: 3px;");
    headerLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    
    // Toggle button (collapse/expand)
    m_thumbnailToggleButton = new QPushButton("?");
    m_thumbnailToggleButton->setFixedSize(32, 32);  // Larger size for better visibility
    m_thumbnailToggleButton->setStyleSheet(R"(
        QPushButton {
            background-color: #3a3a3a;
            color: #ffffff;
            border: 2px solid #666666;
            border-radius: 6px;
            font-size: 18px;
            font-weight: bold;
            text-align: center;
            margin: 2px;
        }
        QPushButton:hover {
            background-color: #5a5a5a;
            border-color: #888888;
            color: #ffff00;
        }
        QPushButton:pressed {
            background-color: #2a2a2a;
            border-color: #aaaaaa;
        }
        QPushButton:focus {
            border-color: #0078d4;
            outline: none;
        }
    )");
    m_thumbnailToggleButton->setToolTip("Click to collapse/expand thumbnail panel");
    connect(m_thumbnailToggleButton, &QPushButton::clicked, this, &DicomViewer::toggleThumbnailPanel);
    
    // Move button to left side for better visibility when collapsed
    headerLayout->addWidget(m_thumbnailToggleButton);
    headerLayout->addWidget(headerLabel);
    headerLayout->addStretch();
    thumbnailLayout->addWidget(headerWidget);
    
    // Thumbnail list - use IconMode with precise width control
    m_thumbnailList = new QListWidget;
    m_thumbnailList->setObjectName("thumbnail_list");
    m_thumbnailList->setViewMode(QListWidget::IconMode);  // Back to IconMode
    m_thumbnailList->setResizeMode(QListWidget::Fixed);
    m_thumbnailList->setMovement(QListWidget::Static);
    m_thumbnailList->setGridSize(QSize(200, 170));  // Further reduced grid height
    m_thumbnailList->setIconSize(QSize(190, 150));  // Slightly smaller thumbnails
    m_thumbnailList->setSpacing(2);  // Minimal spacing between items
    m_thumbnailList->setUniformItemSizes(true);  // Force uniform sizing
    
    // Set fixed width to prevent stretching
    m_thumbnailList->setFixedWidth(210);  // Adjusted for narrower panel
    m_thumbnailList->setStyleSheet(R"(
        QListWidget {
            background-color: #2a2a2a;
            color: white;
            border: 1px solid #444444;
            outline: none;
        }
        QListWidget::item {
            background-color: transparent;
            border: 2px solid transparent;
            border-radius: 4px;
            margin: 4px;
        }
        QListWidget::item:selected {
            background-color: transparent;
            border: 3px solid #FFD700;
        }
        QListWidget::item:hover {
            background-color: #404040;
            border: 2px solid #666666;
        }
    )");
    
    connect(m_thumbnailList, &QListWidget::currentItemChanged,
            this, &DicomViewer::onThumbnailItemSelected);
    
    thumbnailLayout->addWidget(m_thumbnailList);
    
    // Initially hide the thumbnail panel until thumbnails are loaded
    m_thumbnailPanel->setVisible(false);
}

void DicomViewer::createOverlayLabels(QWidget* parent)
{
    if (!parent) {
        return;
    }
    
    QString overlayStyle = R"(
        QLabel {
            color: #FFFF64;
            font-family: 'Consolas', 'Monaco', monospace;
            font-size: 12px;
            font-weight: bold;
            background-color: transparent;
            padding: 5px;
        }
    )";
    
    // Top-left overlay
    m_overlayTopLeft = new QLabel(parent);
    m_overlayTopLeft->setStyleSheet(overlayStyle);
    m_overlayTopLeft->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_overlayTopLeft->setText("");
    
    // Top-right overlay
    m_overlayTopRight = new QLabel(parent);
    m_overlayTopRight->setStyleSheet(overlayStyle);
    m_overlayTopRight->setAlignment(Qt::AlignTop | Qt::AlignRight);
    m_overlayTopRight->setText("");
    
    // Bottom-left overlay
    m_overlayBottomLeft = new QLabel(parent);
    m_overlayBottomLeft->setStyleSheet(overlayStyle);
    m_overlayBottomLeft->setAlignment(Qt::AlignBottom | Qt::AlignLeft);
    m_overlayBottomLeft->setText("");
    
    // Bottom-right overlay
    m_overlayBottomRight = new QLabel(parent);
    m_overlayBottomRight->setStyleSheet(overlayStyle);
    m_overlayBottomRight->setAlignment(Qt::AlignBottom | Qt::AlignRight);
    m_overlayBottomRight->setText("");
    
    // Make overlays visible and raise them to front
    m_overlayTopLeft->show();
    m_overlayTopRight->show();
    m_overlayBottomLeft->show();
    m_overlayBottomRight->show();
    
    m_overlayTopLeft->raise();
    m_overlayTopRight->raise();
    m_overlayBottomLeft->raise();
    m_overlayBottomRight->raise();
}

void DicomViewer::createCloseButton()
{
    m_closeButton = new QPushButton(this);
    
    // Load close icon from resources
    QString closeIconPath = m_iconPath + "/Close_96.png";
    QIcon closeIcon(closeIconPath);
    if (!closeIcon.isNull()) {
        m_closeButton->setIcon(closeIcon);
        m_closeButton->setIconSize(QSize(48, 48));
    } else {
        m_closeButton->setText("X");
        m_closeButton->setStyleSheet("color: white; font-weight: bold; font-size: 16px;");
    }
    
    // Style to match toolbar buttons
    m_closeButton->setFixedSize(64, 64);
    m_closeButton->setStyleSheet(R"(
        QPushButton {
            background-color: #333333;
            border: 1px solid #555555;
            padding: 5px;
            margin: 2px;
            color: white;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: #555555;
        }
        QPushButton:pressed {
            background-color: #333333;
        }
    )");
    
    // Connect to close
    connect(m_closeButton, &QPushButton::clicked, this, &DicomViewer::close);
    
    // Position and show
    m_closeButton->move(width() - 74, 10);
    m_closeButton->raise();
    m_closeButton->show();
}

void DicomViewer::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateOverlayPositions();
    
    // Update close button position
    if (m_closeButton) {
        m_closeButton->move(width() - 74, 10);
        m_closeButton->raise();
    }
    
    // Update DICOM info panel position if visible
    if (m_dicomInfoWidget && m_dicomInfoVisible) {
        const int panelWidth = 400;
        const int panelHeight = height() - 100;
        m_dicomInfoWidget->setFixedSize(panelWidth, panelHeight);
        m_dicomInfoWidget->move(width() - panelWidth - 20, 60);
        m_dicomInfoWidget->raise();
    }
}

void DicomViewer::updateOverlayPositions()
{
    if (!m_imageWidget || !m_overlayTopLeft || !m_overlayTopRight || 
        !m_overlayBottomLeft || !m_overlayBottomRight) {
        return;
    }
    
    QRect rect = m_imageWidget->rect();
    int margin = 10;
    int overlayWidth = 300;
    int overlayHeight = 100;
    
    // Position overlays at corners
    m_overlayTopLeft->setGeometry(margin, margin, overlayWidth, overlayHeight);
    m_overlayTopRight->setGeometry(rect.width() - overlayWidth - margin, margin, overlayWidth, overlayHeight);
    m_overlayBottomLeft->setGeometry(margin, rect.height() - overlayHeight - margin, overlayWidth, overlayHeight);
    m_overlayBottomRight->setGeometry(rect.width() - overlayWidth - margin, rect.height() - overlayHeight - margin, overlayWidth, overlayHeight);
}

void DicomViewer::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    updateOverlayPositions();
}

// Placeholder implementations for all the slots
void DicomViewer::nextFrame()
{
    // Allow navigation even during loading if we have at least the first frame
    if (m_totalFrames <= 1 && !m_isLoadingProgressively) {
        return;
    }
    
    // Don't stop playback when advancing frames during playback (Python behavior)
    bool wasPlaying = m_isPlaying;
    if (m_isPlaying && !m_allFramesCached) {
        if (m_playbackTimer) {
            m_playbackTimer->stop();
        }
        m_isPlaying = false;
        updatePlayButtonIcon("Play_96.png");
    }
    
    m_currentFrame = (m_currentFrame + 1) % m_totalFrames;
    
    // Use cached frame if available (Python behavior)
    if (m_frameCache.contains(m_currentFrame)) {
        displayCachedFrame(m_currentFrame);
    } else {
        // Go back to previous frame if next isn't ready (Python behavior)
        m_currentFrame = (m_currentFrame - 1 + m_totalFrames) % m_totalFrames;
        if (m_frameCache.contains(m_currentFrame)) {
            displayCachedFrame(m_currentFrame);
        }
    }
    
    // Resume playback if it was active before (Python behavior)
    if (wasPlaying && m_allFramesCached) {
        if (m_playbackTimer) {
            m_playbackTimer->start();
    }
        m_isPlaying = true;
        updatePlayButtonIcon("Pause_96.png");
}
}

void DicomViewer::automaticNextFrame()
{
    // This method is only called by the timer for automatic playback
    // Allow navigation even during loading if we have at least the first frame
    if (m_totalFrames <= 1) {
        return;
    }
    
    int nextFrame = (m_currentFrame + 1) % m_totalFrames;
    
    // Use cached frame if available
    if (m_frameCache.contains(nextFrame)) {
        m_currentFrame = nextFrame;
        displayCachedFrame(m_currentFrame);
        m_playbackPausedForFrame = false;  // Clear pause flag if we can continue
        
        // Log looping behavior for debugging
        if (nextFrame == 0) {
        }
        
    } else {
        // Frame not ready yet - remember we're waiting but keep timer running
        // This allows continuous looping once frames are loaded
        m_playbackPausedForFrame = true;
    }
    
    // Ensure continuous playback - the modulo operation above already handles looping
    // When we reach the last frame, next frame becomes 0 automatically
    // Timer continues running, creating continuous replay until user intervention
}

void DicomViewer::previousFrame()
{
    // Allow navigation even during loading if we have at least the first frame
    if (m_totalFrames <= 1 && !m_isLoadingProgressively) {
        return;
    }
    
    // If playback is active, pause it when user manually navigates (Python behavior)
    if (m_isPlaying) {
        if (m_playbackTimer) {
            m_playbackTimer->stop();
        }
        m_isPlaying = false;
        updatePlayButtonIcon("Play_96.png");
    }
    
    int prevFrame = (m_currentFrame - 1 + m_totalFrames) % m_totalFrames;
    
    // Try DicomFrameProcessor first for direct frame loading
    if (m_frameProcessor && !m_currentImagePath.isEmpty()) {
        QImage frameImage = m_frameProcessor->getFrameAsQImage(prevFrame);
        if (!frameImage.isNull()) {
            m_currentFrame = prevFrame;
            m_currentPixmap = QPixmap::fromImage(frameImage);
            m_originalPixmap = m_currentPixmap;  // Store the original unmodified pixmap
            updateImageDisplay();
            updateOverlayInfo();
            return;
        }
    }
    
    // Fallback to cached frame system
    if (m_allFramesCached || m_frameCache.contains(prevFrame)) {
        displayCachedFrame(prevFrame);
    } else {
        // If frames are still loading, only go back if the frame is cached
    if (m_frameCache.contains(prevFrame)) {
        displayCachedFrame(prevFrame);
        } else {
            // Go forward to next frame if previous isn't ready
            int nextFrame = (m_currentFrame + 1) % m_totalFrames;
            if (m_frameCache.contains(nextFrame)) {
                displayCachedFrame(nextFrame);
    }
}
    }

}

void DicomViewer::togglePlayback()
{
    logMessage("[USER ACTION] Toggle playback requested", LOG_DEBUG);
    // Use professional framework for playback control
    if (m_playbackController) {
        m_playbackController->togglePlayback();
    } else {
        // Fallback to legacy implementation if framework not initialized
        
        // Allow pause even during progressive loading, but only start play if ready or loading
        if (m_totalFrames <= 1 && !m_isLoadingProgressively) {
            return;
        }
        
        if (m_isPlaying) {
            // Stop playback (works during progressive loading and normal playback)
            if (m_playbackTimer) {
                m_playbackTimer->stop();
            }
            m_isPlaying = false;
            m_playbackPausedForFrame = false;  // Clear pause flag when stopping
            updatePlayButtonIcon("Play_96.png");
        } else {
            // Start playback: either for cached frames or during progressive loading
            bool allFramesReady = !m_isLoadingProgressively && m_allFramesCached;
            bool canStartDuringLoading = m_isLoadingProgressively && m_totalFrames > 1;
            
            if (allFramesReady || canStartDuringLoading) {
                
                // Use framework playback controller if available
                if (m_playbackController) {
                    // Framework handles timing - just start playback
                    m_playbackController->play();
                    m_isPlaying = true;
                    m_playbackPausedForFrame = false;  // Clear pause flag when starting
                    updatePlayButtonIcon("Pause_96.png");
                } else {
                    // Legacy fallback timer system
                    if (!m_playbackTimer) {
                        m_playbackTimer = new QTimer(this);
                        connect(m_playbackTimer, &QTimer::timeout, this, &DicomViewer::automaticNextFrame);
                    }
                    
                    // Use appropriate default frame rate if not set
                    int interval = m_playbackTimer->interval();
                    if (interval <= 0) {
                        if (m_totalFrames > 100) {
                            interval = 33; // ~30 fps
                        } else if (m_totalFrames > 50) {
                            interval = 40; // 25 fps
                        } else {
                            interval = 67; // ~15 fps
                        }
                        m_playbackTimer->setInterval(interval);
                    }
                    
                    // Start at beginning if at end
                    if (m_currentFrame >= m_totalFrames - 1) {
                        m_currentFrame = 0;
                        displayCachedFrame(0);
                    }
                    
                    m_playbackTimer->start();
                    m_isPlaying = true;
                    m_playbackPausedForFrame = false;  // Clear pause flag when starting
                    updatePlayButtonIcon("Pause_96.png");
                    
                    QString context = m_isLoadingProgressively ? " during progressive loading" : " with cached frames";
                }
            } else {
                // Logic for other conditions
            }
        }
    }
}

void DicomViewer::nextImage()
{
    if (!m_dicomTree) return;
    
    QTreeWidgetItem* currentItem = m_dicomTree->currentItem();
    if (!currentItem) {
        // No current selection, select first image item
        selectFirstImageItem();
        return;
    }
    
    // Find the next selectable item (image or series)
    QTreeWidgetItem* nextItem = findNextSelectableItem(currentItem);
    if (nextItem) {
        m_dicomTree->setCurrentItem(nextItem);
        m_dicomTree->scrollToItem(nextItem);
    }
}

void DicomViewer::previousImage()
{
    if (!m_dicomTree) return;
    
    QTreeWidgetItem* currentItem = m_dicomTree->currentItem();
    if (!currentItem) {
        // No current selection, select last image item
        selectLastImageItem();
        return;
    }
    
    // Find the previous selectable item (image or series)
    QTreeWidgetItem* prevItem = findPreviousSelectableItem(currentItem);
    if (prevItem) {
        m_dicomTree->setCurrentItem(prevItem);
        m_dicomTree->scrollToItem(prevItem);
    }
}

void DicomViewer::horizontalFlip()
{
    m_imagePipeline->setHorizontalFlipEnabled(!m_imagePipeline->isHorizontalFlipEnabled());
    processThroughPipeline();
}

void DicomViewer::verticalFlip()
{
    m_imagePipeline->setVerticalFlipEnabled(!m_imagePipeline->isVerticalFlipEnabled());
    processThroughPipeline();
}

void DicomViewer::invertImage()
{
    m_imagePipeline->setInvertEnabled(!m_imagePipeline->isInvertEnabled());
    processThroughPipeline();
}

void DicomViewer::resetTransformations()
{
    // Reset all transformations but preserve original window/level values
    m_imagePipeline->resetAllTransformations();
    
    // Restore original window/level values instead of using hardcoded defaults
    if (m_originalWindowWidth > 0) {
        m_imagePipeline->setWindowLevel(m_originalWindowCenter, m_originalWindowWidth);
        m_imagePipeline->setWindowLevelEnabled(true);
        
        // Update current values to reflect the reset
        m_currentWindowCenter = m_originalWindowCenter;
        m_currentWindowWidth = m_originalWindowWidth;
    }
    
    m_zoomFactor = 1.0;
    processThroughPipeline();
    fitToWindow();
    
    // Update overlay text to show new window/level values
    updateOverlayInfo();
}

void DicomViewer::setWindowLevelPreset(const QString& presetName)
{
    
    if (presetName == "lung") {
        m_imagePipeline->setWindowLevel(-600, 1200);
    } else if (presetName == "bone") {
        m_imagePipeline->setWindowLevel(300, 1500);
    } else if (presetName == "soft_tissue") {
        m_imagePipeline->setWindowLevel(50, 350);
    } else if (presetName == "brain") {
        m_imagePipeline->setWindowLevel(40, 80);
    } else if (presetName == "abdomen") {
        m_imagePipeline->setWindowLevel(60, 400);
    }
    
    // Only enable if toggle button is ON
    if (m_windowLevelModeEnabled) {
        m_imagePipeline->setWindowLevelEnabled(true);
    } else {
    }
    processThroughPipeline();
}

void DicomViewer::showWindowLevelDialog()
{
    
    // For now, show a simple message box with current values
    // In a full implementation, this would show a proper dialog
    // Show the original DICOM values that are displayed in UI, not the scaled pipeline values
    double currentCenter = m_currentWindowCenter;
    double currentWidth = m_currentWindowWidth;
    
    QString message = QString("Current Window/Level:\nCenter: %1\nWidth: %2\n\n(Custom dialog not yet implemented)")
                     .arg(currentCenter).arg(currentWidth);
    
    QMessageBox::information(this, "Window/Level", message);
}



void DicomViewer::zoomIn()
{
    logMessage("[USER ACTION] Zoom in requested", LOG_DEBUG);
    if (m_graphicsView && m_zoomFactor < m_maxZoomFactor) {
        m_zoomFactor *= m_zoomIncrement;
        m_graphicsView->scale(m_zoomIncrement, m_zoomIncrement);
        updateZoomOverlay();
    }
}

void DicomViewer::zoomOut()
{
    logMessage("[USER ACTION] Zoom out requested", LOG_DEBUG);
    if (m_graphicsView && m_zoomFactor > m_minZoomFactor) {
        m_zoomFactor /= m_zoomIncrement;
        m_graphicsView->scale(1.0 / m_zoomIncrement, 1.0 / m_zoomIncrement);
        updateZoomOverlay();
    }
}

void DicomViewer::fitToWindow()
{
    if (m_graphicsView && m_pixmapItem) {
        m_graphicsView->fitInView(m_pixmapItem, Qt::KeepAspectRatio);
        m_zoomFactor = calculateFitToWindowZoom();
        updateZoomOverlay();
    }
}

void DicomViewer::openDicomDir()
{
    QString fileName = QFileDialog::getOpenFileName(this,
        tr("Select DICOMDIR File"), "", 
        tr("DICOMDIR Files (*.dcm *.DCM DICOMDIR);;All Files (*)"));
    
    if (!fileName.isEmpty()) {
        // Enable Save Run button when opening files directly
        if (m_saveRunAction) {
            m_saveRunAction->setEnabled(true);
        }
        loadDicomDir(fileName);
    }
}

void DicomViewer::saveImage()
{
    if (m_currentPixmap.isNull()) {
        return;
    }
    
    // Stop playback if running
    if (m_isPlaying) {
        togglePlayback();
    }
    
    try {
        QSize currentSize(m_currentPixmap.width(), m_currentPixmap.height());
        SaveImageDialog dialog(this, currentSize);
        
        if (dialog.exec() == QDialog::Accepted) {
            SaveImageDialog::ExportSettings settings = dialog.getExportSettings();
            performImageExport(settings);
        }
    } catch (const std::exception& e) {
    }
}

void DicomViewer::saveRun()
{
    // Safety check: Don't allow video export if ffmpeg copy hasn't completed
    if (!m_ffmpegCopyCompleted) {
        QMessageBox::warning(this, "Video Export Not Available", 
            "Video export is not yet available. FFmpeg is still being copied.\n"
            "Please wait for the copy process to complete.");
        qDebugT() << "Save run blocked - ffmpeg copy not yet completed";
        return;
    }
    
    if (m_currentPixmap.isNull()) {
        return;
    }
    
    // Stop playback if running
    if (m_isPlaying) {
        togglePlayback();
    }
    
    try {
        QSize currentSize(m_currentPixmap.width(), m_currentPixmap.height());
        SaveRunDialog dialog(this, currentSize);
        
        if (dialog.exec() == QDialog::Accepted) {
            SaveRunDialog::ExportSettings settings = dialog.getExportSettings();
            performVideoExport(settings);
        }
    } catch (const std::exception& e) {
        qDebugT() << "Exception in saveRun:" << e.what();
    }
}

void DicomViewer::onThumbnailItemSelected(QListWidgetItem* current, QListWidgetItem* previous)
{
    Q_UNUSED(previous)
    if (!current) return;
    
    // Get the file path from the item data
    QString filePath = current->data(Qt::UserRole).toString();
    
    // Skip patient separator items
    if (filePath == "PATIENT_SEPARATOR") {
        return;
    }
    
    if (!filePath.isEmpty()) {
        // NEW: Check if thumbnail generation is active
        if (m_thumbnailGenerationActive.loadRelaxed() == 1) {
            // Queue the selection instead of processing immediately
            QMutexLocker pendingLocker(&m_pendingSelectionsMutex);
            m_pendingSelections.enqueue(filePath);
            logMessage("DEBUG", QString("Queued thumbnail selection during generation: %1").arg(filePath));
            return;
        }
        
        // NEW: Verify file readiness
        {
            QMutexLocker fileLocker(&m_fileStatesMutex);
            QString fileName = QFileInfo(filePath).fileName();
            if (m_copyInProgress && !m_fileReadyStates.value(fileName, false)) {
                logMessage("DEBUG", QString("File not ready for selection: %1").arg(fileName));
                return;
            }
        }
        
        logMessage("DEBUG", QString("[THUMBNAIL] Selected thumbnail with path: %1").arg(filePath));
        logMessage("DEBUG", QString("[THUMBNAIL] Thumbnail selection - File path: %1").arg(filePath));
        logMessage("DEBUG", QString("[THUMBNAIL] Copy in progress: %1").arg(m_copyInProgress ? "true" : "false"));
        
        // Use display monitor instead of direct display
        requestDisplay(filePath);
        
        // Find and select the corresponding tree item
        if (m_dicomTree) {
            bool foundMatch = false;
            QTreeWidgetItemIterator it(m_dicomTree);
            while (*it) {
                QVariantList userData = (*it)->data(0, Qt::UserRole).toList();
                if (userData.size() >= 2) {
                    QString itemType = userData[0].toString();
                    QString itemPath = userData[1].toString();
                    // CRITICAL FIX: Use defensive path comparison to handle short vs long path formats
                    if ((itemType == "image" || itemType == "report") && pathsAreEquivalent(itemPath, filePath)) {
                        logMessage("DEBUG", QString("[THUMBNAIL] Found matching tree item for path: %1").arg(filePath));
                        logMessage("DEBUG", QString("[THUMBNAIL] Path match found - Tree path: %1").arg(itemPath));
                        m_dicomTree->setCurrentItem(*it);
                        foundMatch = true;
                        break;
                    }
                }
                ++it;
            }
            
            if (!foundMatch) {
                logMessage("WARNING", QString("[THUMBNAIL] WARNING: No matching tree item found for thumbnail path: %1").arg(filePath));
                logMessage("WARNING", "[THUMBNAIL] This indicates a path mismatch between thumbnails and tree items");
                
                // Try to find a tree item with the same filename but different path
                QString fileName = QFileInfo(filePath).fileName();
                QTreeWidgetItemIterator it2(m_dicomTree);
                while (*it2) {
                    QVariantList userData = (*it2)->data(0, Qt::UserRole).toList();
                    if (userData.size() >= 2) {
                        QString itemType = userData[0].toString();
                        QString itemPath = userData[1].toString();
                        QString treeFileName = QFileInfo(itemPath).fileName();
                        if ((itemType == "image" || itemType == "report") && treeFileName == fileName) {
                            logMessage("DEBUG", "[THUMBNAIL] Found tree item with same filename but different path:");
                            logMessage("DEBUG", QString("[THUMBNAIL]   Thumbnail path: %1").arg(filePath));
                            logMessage("DEBUG", QString("[THUMBNAIL]   Tree item path: %1").arg(itemPath));
                            logMessage("DEBUG", "[THUMBNAIL] Selecting tree item with local path");
                            logMessage("DEBUG", QString("[THUMBNAIL] Fallback match - Thumbnail: %1 Tree: %2").arg(filePath, itemPath));
                            m_dicomTree->setCurrentItem(*it2);
                            break;
                        }
                    }
                    ++it2;
                }
            }
        }
        
        // Load the DICOM image or report based on type
        // The tree selection will trigger the appropriate display via onTreeItemSelected
        // No need to call loadDicomImage directly here
    }
}

void DicomViewer::updateThumbnailPanel()
{
    // Prevent recursive thumbnail panel updates
    if (m_thumbnailPanelProcessingActive) {
        logMessage("DEBUG", "[THUMBNAIL PANEL] Already processing - ignoring update request");
        return;
    }
    
    if (!m_thumbnailList || !m_dicomTree) {
        logMessage("WARN", "Thumbnail panel or tree not available");
        return;
    }
    
    // Prevent updating thumbnails if already in progress
    if (m_thumbnailGenerationActive) {
        logMessage("DEBUG", "Thumbnail generation already in progress - skipping update");
        return;
    }
    
    // NEW: Only create thumbnails if ALL files are complete (have cine/image icons)
    if (!areAllFilesComplete()) {
        logMessage("DEBUG", "[THUMBNAIL PANEL] Delaying thumbnail creation - not all files are complete yet");
        logMessage("DEBUG", QString("[THUMBNAIL PANEL] Completed: %1, Total in tree: %2").arg(m_fullyCompletedFiles.size()).arg(getTotalFileCount()));
        return;
    }
    
    logMessage("INFO", "[THUMBNAIL PANEL] All files complete - starting thumbnail creation");
    
    // Set processing guard
    m_thumbnailPanelProcessingActive = true;
    QScopeGuard panelGuard([this]() {
        m_thumbnailPanelProcessingActive = false;
    });
    
    logMessage("DEBUG", "[THUMBNAIL PANEL] Updating thumbnail panel...");
    
    // Clear existing thumbnails and reset counters
    m_thumbnailList->clear();
    m_pendingThumbnailPaths.clear();
    m_completedThumbnails = 0;
    m_totalThumbnails = 0;
    
    // Collect patients in tree order first to preserve sequence
    QStringList patientOrder;
    QTreeWidgetItemIterator patientIt(m_dicomTree);
    while (*patientIt) {
        QVariantList userData = (*patientIt)->data(0, Qt::UserRole).toList();
        if (userData.size() >= 1 && userData[0].toString() == "patient") {
            QString patientName = (*patientIt)->text(0);
            if (!patientOrder.contains(patientName)) {
                patientOrder.append(patientName);
            }
        }
        ++patientIt;
    }
    
    // Now collect files for each patient in order
    QMap<QString, QList<QPair<QString, QString>>> patientGroups; 
    QTreeWidgetItemIterator it(m_dicomTree);
    
    while (*it) {
        QVariantList userData = (*it)->data(0, Qt::UserRole).toList();
        if (userData.size() >= 2) {
            QString itemType = userData[0].toString();
            QString filePath = userData[1].toString();
            
            if (itemType == "image" || itemType == "report") {
                // Find patient name from tree hierarchy
                QString patientName = "Unknown Patient";
                QTreeWidgetItem* current = *it;
                while (current) {
                    QVariantList currentData = current->data(0, Qt::UserRole).toList();
                    if (currentData.size() >= 1 && currentData[0].toString() == "patient") {
                        patientName = current->text(0);
                        break;
                    }
                    current = current->parent();
                }
                
                patientGroups[patientName].append(qMakePair(filePath, itemType));
            }
        }
        ++it;
    }
    
    // Add items grouped by patient in tree order
    for (const QString& patientName : patientOrder) {
        if (!patientGroups.contains(patientName)) continue;
        
        QList<QPair<QString, QString>> patientFiles = patientGroups[patientName];
        
        logMessage("DEBUG", QString("Creating thumbnails for patient: %1 with %2 files").arg(patientName).arg(patientFiles.size()));
        
        // Add thumbnails for this patient with embedded patient name
        for (const auto& filePair : patientFiles) {
            QString filePath = filePair.first;
            QString itemType = filePair.second;
            
            m_pendingThumbnailPaths.append(filePath);
            m_totalThumbnails++;
            
            // Create placeholder thumbnail item
            QListWidgetItem* thumbnailItem = new QListWidgetItem;
            thumbnailItem->setData(Qt::UserRole, filePath);
            thumbnailItem->setData(Qt::UserRole + 1, itemType);
            thumbnailItem->setData(Qt::UserRole + 2, patientName); // Store patient name
            thumbnailItem->setIcon(QIcon(createLoadingThumbnail()));
            
            // Compact item size for minimal spacing
            thumbnailItem->setSizeHint(QSize(200, 170));  // Reduced height
            
            // Don't set text until thumbnail is generated
            thumbnailItem->setText("");
            m_thumbnailList->addItem(thumbnailItem);
        }
    }
    
    logMessage("DEBUG", QString("Found %1 images for thumbnail generation").arg(m_totalThumbnails));
    
    // Start background thumbnail generation if we have images
    if (m_totalThumbnails > 0) {
        m_allThumbnailsComplete = false;  // Reset completion flag when starting generation
        updateStatusBar(QString("Generating thumbnails... (0/%1)").arg(m_totalThumbnails), 0);
        generateThumbnailsInBackground();
    } else {
        // If no thumbnails to generate, still trigger onAllThumbnailsGenerated 
        // to handle the panel visibility and any pending selections
        QTimer::singleShot(0, this, &DicomViewer::onAllThumbnailsGenerated);
    }
}

void DicomViewer::generateThumbnailsInBackground()
{
    // Check if generation already active
    if (!m_thumbnailGenerationActive.testAndSetAcquire(0, 1)) {
        logMessage("DEBUG", "Thumbnail generation already active - skipping");
        return;
    }
    
    // Initialize progress tracking and update status bar
    m_completedThumbnails = 0;
    m_totalThumbnails = m_pendingThumbnailPaths.size();
    m_activeThumbnailTasks = m_pendingThumbnailPaths.size();
    updateStatusBar(QString("Generating thumbnails... (0/%1)").arg(int(m_totalThumbnails)), 0);
    
    logMessage("DEBUG", QString("Starting parallel thumbnail generation for %1 files using QThreadPool").arg(int(m_totalThumbnails)));
    
    // Submit each thumbnail as a separate task to the thread pool
    for (const QString& filePath : m_pendingThumbnailPaths) {
        ThumbnailTask* task = new ThumbnailTask(filePath, this);
        
        // Connect task completion signal to our slot with queued connection for thread safety
        connect(task, &ThumbnailTask::taskCompleted, 
                this, &DicomViewer::onThumbnailTaskCompleted, 
                Qt::QueuedConnection);
        
        // Submit task to global thread pool for parallel execution
        QThreadPool::globalInstance()->start(task);
    }
    
    logMessage("DEBUG", QString("Submitted %1 thumbnail tasks to thread pool (max threads: %2)")
               .arg(m_pendingThumbnailPaths.size())
               .arg(QThreadPool::globalInstance()->maxThreadCount()));
}

void DicomViewer::onThumbnailTaskCompleted(const QString& filePath, const QPixmap& thumbnail, const QString& instanceNumber)
{
    // Call the existing thumbnail completion handler
    onThumbnailGeneratedWithMetadata(filePath, thumbnail, instanceNumber);
    
    // Thread-safe decrement of active tasks counter
    int remainingTasks = --m_activeThumbnailTasks;
    
    logMessage("DEBUG", QString("Thumbnail task completed for: %1, remaining tasks: %2")
               .arg(QFileInfo(filePath).baseName()).arg(remainingTasks));
    
    // Check if all tasks are complete
    if (remainingTasks == 0) {
        logMessage("DEBUG", "All thumbnail tasks completed - triggering completion handler");
        
        // Reset active flag  
        m_thumbnailGenerationActive = 0;
        
        // Trigger completion
        onAllThumbnailsGenerated();
    }
}

void DicomViewer::checkAndShowThumbnailPanel()
{
    // Only show the thumbnail panel if all conditions are met:
    // 1. All thumbnails are generated
    // 2. No copy operation is in progress
    // 3. No DVD detection is in progress
    if (m_allThumbnailsComplete && !m_copyInProgress && !m_dvdDetectionInProgress) {
        m_thumbnailPanel->setVisible(true);
        logMessage("DEBUG", "Thumbnail panel shown - all operations complete");
    } else {
        logMessage("DEBUG", QString("Thumbnail panel not shown - operations still in progress: thumbnailsComplete: %1, copyInProgress: %2, dvdDetectionInProgress: %3")
                   .arg(m_allThumbnailsComplete).arg(m_copyInProgress).arg(m_dvdDetectionInProgress));
    }
}

QPixmap DicomViewer::createLoadingThumbnail()
{
    QPixmap loadingPixmap(190, 150);
    loadingPixmap.fill(QColor(60, 60, 60));
    
    QPainter painter(&loadingPixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    
    // Draw loading indicator
    painter.setPen(QColor(200, 200, 200));
    painter.setFont(QFont("Arial", 11));
    
    QRect textRect = loadingPixmap.rect();
    painter.drawText(textRect, Qt::AlignCenter, "Loading...");
    
    // Draw simple loading animation rectangle
    painter.setPen(QPen(QColor(100, 149, 237), 2));
    painter.drawRect(5, 15, 180, 120);
    
    return loadingPixmap;
}

QPixmap DicomViewer::createFrameTypeIcon(int frameCount)
{
    QPixmap icon(20, 16);
    icon.fill(Qt::transparent);
    
    QPainter painter(&icon);
    painter.setRenderHint(QPainter::Antialiasing);
    
    if (frameCount > 1) {
        // Multi-frame icon (film strip style)
        painter.setPen(QPen(QColor(100, 220, 100), 2));
        painter.fillRect(2, 2, 12, 8, QColor(100, 220, 100, 100));
        painter.drawRect(2, 2, 12, 8);
        painter.fillRect(5, 5, 12, 8, QColor(100, 220, 100, 150));
        painter.drawRect(5, 5, 12, 8);
    } else {
        // Single frame icon (camera style)
        painter.setPen(QPen(QColor(220, 220, 100), 2));
        painter.fillRect(3, 3, 14, 10, QColor(220, 220, 100, 150));
        painter.drawRect(3, 3, 14, 10);
        painter.drawEllipse(6, 6, 4, 4);
    }
    
    return icon;
}

QPixmap DicomViewer::createReportThumbnail(const QString& filePath)
{
    QPixmap reportThumbnail(190, 150);
    reportThumbnail.fill(QColor(42, 42, 42));
    
    QPainter painter(&reportThumbnail);
    painter.setRenderHint(QPainter::Antialiasing);
    
    // Create a document-style background
    painter.fillRect(15, 25, 160, 110, QColor(240, 240, 240));
    painter.setPen(QPen(QColor(200, 200, 200), 1));
    painter.drawRect(15, 25, 160, 110);
    
    // Draw document lines to simulate text
    painter.setPen(QPen(QColor(180, 180, 180), 1));
    for (int i = 0; i < 6; ++i) {
        int y = 35 + i * 12;
        painter.drawLine(20, y, 170, y);
    }
    
    // Draw report icon/symbol in the center
    painter.setPen(QPen(QColor(100, 149, 237), 3));
    painter.setFont(QFont("Arial", 20, QFont::Bold));
    painter.drawText(QRect(15, 25, 160, 110), Qt::AlignCenter, "SR");
    
    // Get report metadata
    QString reportType = "Structure Report";
    QString instanceNumber = "RPT";
    
    // Try to extract more specific information from the file
    try {
        DicomFrameProcessor tempProcessor;
        if (tempProcessor.loadDicomFile(filePath)) {
            // Get Instance Number from DICOM tag (0020,0013)
            QString dicomInstanceNumber = tempProcessor.getDicomTagValue("0020,0013");
            if (!dicomInstanceNumber.isEmpty()) {
                instanceNumber = dicomInstanceNumber;
            }
            
            // Try to get more specific report type from other tags if available
            QString seriesDescription = tempProcessor.getDicomTagValue("0008,103E");
            if (!seriesDescription.isEmpty()) {
                reportType = seriesDescription;
                // Truncate if too long
                if (reportType.length() > 15) {
                    reportType = reportType.left(12) + "...";
                }
            }
        }
    } catch (...) {
        // Use default values if DICOM reading fails
    }
    
    // Create top overlay bar with background
    painter.fillRect(0, 0, reportThumbnail.width(), 20, QColor(0, 0, 0, 180));
    
    // Draw "DOC" at top-left to indicate document type
    painter.setPen(QColor(255, 255, 255));
    painter.setFont(QFont("Arial", 11, QFont::Bold));
    painter.drawText(5, 14, "DOC");
    
    // Draw instance number in center
    QFontMetrics fm(painter.font());
    int textWidth = fm.horizontalAdvance(instanceNumber);
    painter.drawText((reportThumbnail.width() - textWidth) / 2, 14, instanceNumber);
    
    // Load and draw report/list icon at top-right
    QString iconPath = "DicomViewer_CPP/resources/icons/List.png";
    QPixmap iconPixmap(iconPath);
    if (iconPixmap.isNull()) {
        // Try absolute path if relative doesn't work
        QString absoluteIconPath = "d:/Repos/EikonDicomViewer/DicomViewer_CPP/resources/icons/List.png";
        iconPixmap.load(absoluteIconPath);
    }
    
    if (!iconPixmap.isNull()) {
        // Scale icon to proper size (16x16 for top overlay)
        QPixmap scaledIcon = iconPixmap.scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        painter.drawPixmap(reportThumbnail.width() - 20, 2, scaledIcon);
    } else {
        // Fallback: draw a simple text icon
        painter.setPen(QColor(100, 149, 237));
        painter.setFont(QFont("Arial", 10, QFont::Bold));
        painter.drawText(reportThumbnail.width() - 18, 14, "R");
    }
    
    // Draw report type at the bottom
    painter.fillRect(0, reportThumbnail.height() - 20, reportThumbnail.width(), 20, QColor(0, 0, 0, 180));
    painter.setPen(QColor(255, 255, 255));
    painter.setFont(QFont("Arial", 9));
    int typeTextWidth = fm.horizontalAdvance(reportType);
    painter.drawText((reportThumbnail.width() - typeTextWidth) / 2, reportThumbnail.height() - 6, reportType);
    
    return reportThumbnail;
}

QListWidgetItem* DicomViewer::createPatientSeparator(const QString& patientName)
{
    QListWidgetItem* separatorItem = new QListWidgetItem;
    
    // Create separator widget with minimal height
    QWidget* separatorWidget = new QWidget;
    separatorWidget->setFixedHeight(22); // Reduced height to minimize gap
    
    QHBoxLayout* layout = new QHBoxLayout(separatorWidget);
    layout->setContentsMargins(6, 2, 6, 2); // Reduced margins
    layout->setSpacing(4);
    
    // Patient icon
    QLabel* iconLabel = new QLabel;
    QPixmap patientIcon("d:/Repos/EikonDicomViewer/DicomViewer_CPP/resources/icons/Person.png");
    if (!patientIcon.isNull()) {
        iconLabel->setPixmap(patientIcon.scaled(14, 14, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        iconLabel->setText("P");
        iconLabel->setStyleSheet("color: #4CAF50; font-weight: bold; font-size: 10px;");
    }
    iconLabel->setFixedSize(14, 14);
    
    // Patient name label
    QLabel* nameLabel = new QLabel(patientName.isEmpty() ? "Unknown Patient" : patientName);
    nameLabel->setStyleSheet(
        "color: #FFFFFF; "
        "font-weight: bold; "
        "font-size: 10px; "
        "background: transparent;"
    );
    nameLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    
    layout->addWidget(iconLabel);
    layout->addWidget(nameLabel);
    layout->addStretch();
    
    // Set a clearly visible but compact background
    separatorWidget->setStyleSheet(
        "QWidget { "
        "background-color: #505050; "
        "border: 1px solid #707070; "
        "border-radius: 2px; "
        "}"
    );
    
    // Set item properties with minimal sizing
    separatorItem->setSizeHint(QSize(200, 22)); // Reduced height
    separatorItem->setData(Qt::UserRole, "PATIENT_SEPARATOR");
    separatorItem->setData(Qt::UserRole + 1, patientName);
    separatorItem->setFlags(Qt::ItemIsEnabled); // Not selectable
    
    // First add the item, then set the widget
    m_thumbnailList->addItem(separatorItem);
    m_thumbnailList->setItemWidget(separatorItem, separatorWidget);
    
    logMessage("DEBUG", QString("Created compact patient separator for: %1").arg(patientName));
    
    return separatorItem;
    separatorItem->setFlags(Qt::ItemIsEnabled); // Not selectable
    
    return separatorItem;
}

void DicomViewer::toggleThumbnailPanel()
{
    logMessage("[USER ACTION] Toggle thumbnail panel requested", LOG_DEBUG);
    m_thumbnailPanelCollapsed = !m_thumbnailPanelCollapsed;
    
    if (m_thumbnailPanelCollapsed) {
        // True collapse: hide thumbnail list and shrink panel width
        m_thumbnailList->hide();
        m_thumbnailToggleButton->setText("?");
        m_thumbnailPanel->setFixedWidth(55);  // Optimized width to ensure button is fully visible with padding
    } else {
        // Expand: show thumbnail list and restore full width
        m_thumbnailList->show();
        m_thumbnailToggleButton->setText("?");
        m_thumbnailPanel->setFixedWidth(220);  // Full width
    }
    
    logMessage("DEBUG", QString("Thumbnail panel %1").arg(m_thumbnailPanelCollapsed ? "collapsed" : "expanded"));
}

void DicomViewer::onTreeItemSelected(QTreeWidgetItem *current, QTreeWidgetItem *previous)
{
    Q_UNUSED(previous)
    if (!current) {
        return;
    }
    
    logMessage("DEBUG", QString("[USER ACTION] Tree item selected: %1").arg(current->text(0)));
    
    // Extract file path and type
    QVariantList userData = current->data(0, Qt::UserRole).toList();
    if (userData.size() < 2) {
        // Handle non-image selections
        m_imageLabel->setText("Selected: " + current->text(0));
        m_mainStack->setCurrentWidget(m_imageWidget);
        return;
    }
    
    QString itemType = userData[0].toString();
    QString filePath = userData[1].toString();
    
    if (itemType == "image") {
        // CRITICAL FIX: Check file state with original tree path first (for non-DVD mode)
        // In non-DVD mode, files are NOT copied, so tree path should be used directly
        QString originalPath = filePath;
        QString normalizedPath = PathNormalizer::normalize(filePath);
        
        // Check file state with original path first (non-DVD mode)
        FileState originalState = FileState::NotReady;
        {
            QMutexLocker locker(&m_fileStatesMutex);
            originalState = m_fileStates.value(originalPath, FileState::NotReady);
        }
        
        // Check file state with normalized path (DVD mode)
        FileState normalizedState = FileState::NotReady;
        {
            QMutexLocker locker(&m_fileStatesMutex);
            normalizedState = m_fileStates.value(normalizedPath, FileState::NotReady);
        }
        
        // Use the path that has a valid file state (Available or DisplayReady)
        QString effectivePath = originalPath;
        FileState effectiveState = originalState;
        
        // CRITICAL: In non-DVD mode, files exist at original path, not normalized path
        // Check file existence to determine which path to use
        bool originalExists = QFile::exists(originalPath);
        bool normalizedExists = QFile::exists(normalizedPath);
        
        if (originalExists && (originalState == FileState::Available || originalState == FileState::DisplayReady)) {
            // Best case: Original path exists and has valid state
            effectivePath = originalPath;
            effectiveState = originalState;
        } else if (originalExists && originalState == FileState::NotReady && 
                   (normalizedState == FileState::Available || normalizedState == FileState::DisplayReady)) {
            // Non-DVD mode: File exists at original path but state stored under normalized path
            effectivePath = originalPath;  // Use original path where file exists
            effectiveState = normalizedState;  // But use normalized state for validation
        } else if (normalizedExists && (normalizedState == FileState::Available || normalizedState == FileState::DisplayReady)) {
            // DVD mode: File exists at normalized path
            effectivePath = normalizedPath;
            effectiveState = normalizedState;
        } else if (originalExists) {
            // Fallback: Use original path if it exists, regardless of state
            effectivePath = originalPath;
            effectiveState = originalState;
        } else {
            // Final fallback: Use normalized path
            effectivePath = normalizedPath;
            effectiveState = normalizedState;
        }
        
        logMessage("DEBUG", QString("[PATH SELECTION] Tree path: %1").arg(originalPath));
        logMessage("DEBUG", QString("[PATH SELECTION] Normalized path: %1").arg(normalizedPath));
        logMessage("DEBUG", QString("[PATH SELECTION] Original state: %1, Normalized state: %2").arg(static_cast<int>(originalState)).arg(static_cast<int>(normalizedState)));
        logMessage("DEBUG", QString("[PATH SELECTION] Using effective path: %1 with state: %2").arg(effectivePath).arg(static_cast<int>(effectiveState)));
        
        logMessage("INFO", QString("[USER CLICK] File: %1").arg(QFileInfo(originalPath).fileName()));
        logMessage("INFO", QString("[USER CLICK] Tree Path: %1").arg(originalPath));
        logMessage("INFO", QString("[USER CLICK] Effective Path: %1").arg(effectivePath));
        logMessage("INFO", QString("[USER CLICK] Current File State: %1").arg(static_cast<int>(effectiveState)));
        logMessage("INFO", QString("[USER CLICK] File Exists: %1").arg(QFile::exists(effectivePath) ? "YES" : "NO"));
        
        // Apply selection guard - this prevents recursion AND handles DisplayReady optimization
        if (!beginSelection(effectivePath)) {
            return; // Selection blocked by guard (recursion, DisplayReady, or duplicate)
        }
        
        // Ensure proper cleanup using RAII
        QScopeGuard selectionGuard([this]() {
            endSelection();
        });
        
        // Validate file state before proceeding
        FileState fileState = effectiveState;
        if (fileState == FileState::NotReady || fileState == FileState::Copying) {
            QString stateNames[] = {"NotReady", "Copying", "Available", "DisplayReady"};
            QString stateName = (static_cast<int>(fileState) < 4) ? stateNames[static_cast<int>(fileState)] : "Unknown";
            
            logMessage("WARN", QString("[SELECTION BLOCKED] File: %1").arg(QFileInfo(effectivePath).fileName()));
            logMessage("WARN", QString("[SELECTION BLOCKED] State: %1 (%2) - Expected Available(2) or DisplayReady(3)")
                     .arg(stateName).arg(static_cast<int>(fileState)));
            logMessage("WARN", QString("[SELECTION BLOCKED] User cannot select files in NotReady or Copying state"));
            logMessage("DEBUG", QString("[SELECTION] File not ready for selection: %1 State: %2")
                     .arg(effectivePath).arg(static_cast<int>(fileState)));
            return;
        }
        
        logMessage("INFO", QString("[SELECTION SUCCESS] File ready for display: %1").arg(QFileInfo(effectivePath).fileName()));
        
        // Request display through monitor instead of direct loading
        requestDisplay(effectivePath);
        
        // Handle thumbnail synchronization
        synchronizeThumbnailSelection(effectivePath);
        
    } else if (itemType == "report") {
        // This is a Structured Report document
        m_imageLabel->setText("Selected: " + current->text(0));
        m_mainStack->setCurrentWidget(m_reportArea);
        
        // Check if the file path is valid before trying to display report
        QFileInfo fileInfo(filePath);
        if (!fileInfo.exists() || fileInfo.isDir()) {
            QString errorMsg = QString("SR Document Error\n\n");
            errorMsg += QString("Item: %1\n").arg(current->text(0));
            errorMsg += QString("Path: %1\n\n").arg(filePath);
            if (fileInfo.isDir()) {
                errorMsg += "Error: Path points to a directory instead of a DICOM file.\n";
                errorMsg += "This SR document entry in the DICOMDIR is malformed.\n\n";
            } else {
                errorMsg += "Error: File does not exist.\n\n";
            }
            errorMsg += "Possible solutions:\n";
            errorMsg += "- Check if the DICOM files are in the correct location\n";
            errorMsg += "- Verify the DICOMDIR file is not corrupted\n";
            errorMsg += "- Ensure all referenced files exist in the dataset";
            
            m_reportArea->setPlainText(errorMsg);
        } else {
            displayReport(filePath);
        }
    } else {
        // Other types (series, study, patient) - just show selection
        m_imageLabel->setText("Selected: " + current->text(0));
        m_mainStack->setCurrentWidget(m_imageWidget);
    }
}

bool DicomViewer::eventFilter(QObject *obj, QEvent *event)
{
    // Safety check: ensure event and obj are valid
    if (!obj || !event) {
        return QMainWindow::eventFilter(obj, event);
    }

    // Handle mouse events for zoom and windowing
    if (obj == m_graphicsView || obj == m_graphicsView->viewport()) {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease) {
        }
        if (event->type() == QEvent::Wheel) {
            QWheelEvent* wheelEvent = static_cast<QWheelEvent*>(event);
            if (wheelEvent->angleDelta().y() > 0) {
                zoomIn();
            } else {
                zoomOut();
            }
            return true;
        }
        
        // Handle left mouse button for window/level adjustment (when enabled)
        if (event->type() == QEvent::MouseButtonPress) {
            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton && m_windowLevelModeEnabled) {
                startWindowing(mouseEvent->pos());
                return true;
            } else if (mouseEvent->button() == Qt::LeftButton) {
            }

        }
        else if (event->type() == QEvent::MouseMove) {
            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
            if (m_windowingActive && m_windowLevelModeEnabled && (mouseEvent->buttons() & Qt::LeftButton)) {
                updateWindowing(mouseEvent->pos());
                return true;
            } else if (mouseEvent->buttons() & Qt::LeftButton) {
            }

        }
        else if (event->type() == QEvent::MouseButtonRelease) {
            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton && m_windowingActive && m_windowLevelModeEnabled) {
                endWindowing();
                return true;
            } else if (mouseEvent->button() == Qt::LeftButton) {
            }
        }
    }
    
    // Handle Left/Right arrow keys for frame navigation when tree has focus
    // These should only be used for frame navigation, not tree navigation
    if (obj == m_dicomTree && event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent && (keyEvent->key() == Qt::Key_Left || keyEvent->key() == Qt::Key_Right)) {
            // SAFETY: Only process if viewer is fully initialized
            if (m_frameProcessor && m_totalFrames > 1 && !m_currentImagePath.isEmpty()) {
                try {
                    if (keyEvent->key() == Qt::Key_Left) {
                        onPreviousFrameRequested();
                    } else {
                        onNextFrameRequested();
                    }
                } catch (...) {
                    // Catch any exceptions during frame navigation
                }
            }
            // ALWAYS block the event from affecting the tree widget
            return true;
        }
    }
    
    return QMainWindow::eventFilter(obj, event);
}

void DicomViewer::keyPressEvent(QKeyEvent *event)
{
    logMessage(QString("[USER ACTION] Key pressed: %1").arg(event->key()), LOG_DEBUG);
    // Handle Left/Right arrows for frame navigation directly
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
        if (m_totalFrames > 1) {
            if (event->key() == Qt::Key_Left) {
                onPreviousFrameRequested();
            } else {
                onNextFrameRequested();
            }
            return; // Event handled
        } else {
            return; // Consume the event but don't act
        }
    }
    
    // Use professional framework for input handling (for other keys)
    if (m_inputHandler) {
        if (m_inputHandler->processKeyEvent(event)) {
            return; // Event was handled by framework
        }
    }
    
    // Allow up/down arrow keys for tree navigation (not handled by framework)
    if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
        // Check if tree widget has focus, if not, give it focus
        if (m_dicomTree && !m_dicomTree->hasFocus()) {
            m_dicomTree->setFocus();
        }
    }
    
    QMainWindow::keyPressEvent(event);
}

void DicomViewer::closeEvent(QCloseEvent *event)
{
    logMessage("DEBUG", "CloseEvent: Starting application shutdown...");
    
    // Stop all timers first
    if (m_playbackTimer && m_playbackTimer->isActive()) {
        m_playbackTimer->stop();
        logMessage("DEBUG", "CloseEvent: Playback timer stopped");
    }
    
    if (m_progressiveTimer && m_progressiveTimer->isActive()) {
        m_progressiveTimer->stop();
        logMessage("DEBUG", "CloseEvent: Progressive timer stopped");
    }
    
    if (m_copyProgressTimer && m_copyProgressTimer->isActive()) {
        m_copyProgressTimer->stop();
        logMessage("DEBUG", "CloseEvent: Copy progress timer stopped");
    }
    
    // Stop and clean up progressive loader thread
    if (m_progressiveLoader) {
        logMessage("DEBUG", "CloseEvent: Stopping progressive loader...");
        m_progressiveLoader->stop();
        m_progressiveLoader->wait(3000); // 3 second timeout
        delete m_progressiveLoader;
        m_progressiveLoader = nullptr;
        logMessage("DEBUG", "CloseEvent: Progressive loader cleaned up");
    }
    
    // Robocopy process is now handled by DvdCopyWorker
    
    // Clean up DVD worker thread
    if (m_dvdWorkerThread) {
        logMessage("DEBUG", "CloseEvent: Stopping DVD worker thread...");
        m_dvdWorkerThread->quit();
        if (!m_dvdWorkerThread->wait(3000)) { // 3 second timeout
            logMessage("WARN", "CloseEvent: Force terminating DVD worker thread...");
            m_dvdWorkerThread->terminate();
            m_dvdWorkerThread->wait(1000);
        }
        logMessage("DEBUG", "CloseEvent: DVD worker thread stopped");
    }
    
    // Force cleanup of any remaining resources
    logMessage("DEBUG", "CloseEvent: Final cleanup and quit...");
    
    // Accept the close event and force quit
    event->accept();
    
    // Use exit() instead of QApplication::quit() for more forceful termination
    QApplication::processEvents(); // Process any remaining events
    std::exit(0); // Force immediate termination
}

// Placeholder implementations for private methods
void DicomViewer::updateOverlayInfo()
{
    if (!m_overlayTopLeft || !m_overlayTopRight || !m_overlayBottomLeft || !m_overlayBottomRight) {
        return;
    }
    
    // Top Left Corner: Patient Info
    QString topLeftText;
    if (!m_currentPatientId.isEmpty()) {
        topLeftText += m_currentPatientId + "\n";
    }
    
    if (!m_currentPatientName.isEmpty()) {
        topLeftText += m_currentPatientName;
        // Add sex and age on same line if available
        if (!m_currentPatientSex.isEmpty() || !m_currentPatientAge.isEmpty()) {
            topLeftText += ", ";
            if (!m_currentPatientSex.isEmpty()) {
                topLeftText += m_currentPatientSex;
            }
            if (!m_currentPatientAge.isEmpty()) {
                if (!m_currentPatientSex.isEmpty()) {
                    topLeftText += ", ";
                }
                topLeftText += m_currentPatientAge;
            }
        }
        topLeftText += "\n";
    }
    
    if (!m_currentStudyDescription.isEmpty()) {
        topLeftText += m_currentStudyDescription + "\n";
    }
    
    if (!m_currentSeriesDescription.isEmpty()) {
        topLeftText += m_currentSeriesDescription;
    }
    
    // Top Right Corner: Performing Physician and Institution
    QString topRightText;
    if (!m_currentPerformingPhysician.isEmpty()) {
        topRightText += m_currentPerformingPhysician + "\n";
    }
    
    if (!m_currentInstitutionName.isEmpty()) {
        topRightText += m_currentInstitutionName + "\n";
    }
    
    // Format acquisition date as DD-Mon-YYYY and time as HH:MM:SS
    if (!m_currentAcquisitionDate.isEmpty() && m_currentAcquisitionDate.length() >= 8) {
        QStringList monthNames = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        
        QString year = m_currentAcquisitionDate.mid(0, 4);
        QString month = m_currentAcquisitionDate.mid(4, 2);
        QString day = m_currentAcquisitionDate.mid(6, 2);
        
        bool ok;
        int monthNum = month.toInt(&ok);
        if (ok && monthNum >= 1 && monthNum <= 12) {
            QString formattedDate = QString("%1-%2-%3").arg(day).arg(monthNames[monthNum-1]).arg(year);
            topRightText += formattedDate;
            
            // Add time if available
            if (!m_currentAcquisitionTime.isEmpty() && m_currentAcquisitionTime.length() >= 6) {
                QString hour = m_currentAcquisitionTime.mid(0, 2);
                QString minute = m_currentAcquisitionTime.mid(2, 2);
                QString second = m_currentAcquisitionTime.mid(4, 2);
                QString formattedTime = QString(" %1:%2:%3").arg(hour).arg(minute).arg(second);
                topRightText += formattedTime;
            }
        }
    }
    
    // Bottom Left Corner: Positioning angles and frame info
    QString bottomLeftText;
    
    // LAO/RAO (Primary Angle)
    if (m_hasPositionerAngles) {
        if (m_currentPositionerPrimaryAngle > 0) {
            bottomLeftText += QString("LAO: %1°\n").arg(m_currentPositionerPrimaryAngle, 0, 'f', 1);
        } else if (m_currentPositionerPrimaryAngle < 0) {
            bottomLeftText += QString("RAO: %1°\n").arg(qAbs(m_currentPositionerPrimaryAngle), 0, 'f', 1);
        } else {
            bottomLeftText += "LAO: 0°\n";
        }
        
        // CRAN/CAUD (Secondary Angle)
        if (m_currentPositionerSecondaryAngle > 0) {
            bottomLeftText += QString("CAUD: %1°\n").arg(m_currentPositionerSecondaryAngle, 0, 'f', 1);
        } else if (m_currentPositionerSecondaryAngle < 0) {
            bottomLeftText += QString("CRAN: %1°\n").arg(qAbs(m_currentPositionerSecondaryAngle), 0, 'f', 1);
        } else {
            bottomLeftText += "CRAN: 0°\n";
        }
    } else {
        bottomLeftText += "LAO/RAO: --\n";
        bottomLeftText += "CRAN/CAUD: --\n";
    }
    
    // Frame information - show actually displayed frame, not requested frame
    bottomLeftText += QString("Frame %1/%2").arg(m_currentDisplayedFrame + 1).arg(m_totalFrames);
    
    // Bottom Right Corner: Technical parameters, zoom, and window/level
    QString bottomRightText;
    
    // X-Ray technical parameters
    if (m_hasTechnicalParams) {
        QString techLine;
        if (m_currentXRayTubeCurrent > 0) {
            techLine += QString("%1 mA").arg(m_currentXRayTubeCurrent, 0, 'f', 0);
        }
        
        if (m_currentKVP > 0) {
            if (!techLine.isEmpty()) {
                techLine += "  ";
            }
            techLine += QString("%1 kV").arg(m_currentKVP, 0, 'f', 2);
        }
        
        if (!techLine.isEmpty()) {
            bottomRightText += techLine + "\n";
        }
    }
    
    // Zoom percentage
    int zoomPercentage = static_cast<int>(m_zoomFactor * 100);
    bottomRightText += QString("Zoom: %1%\n").arg(zoomPercentage);
    
    // Window/Level values - show if window width is positive (center can be negative)
    if (m_currentWindowWidth > 0) {
        bottomRightText += QString("WL: %1 WW: %2").arg(m_currentWindowCenter, 0, 'f', 0).arg(m_currentWindowWidth, 0, 'f', 0);
        logMessage("DEBUG", QString("UI OVERLAY DISPLAY: WL=%1 WW=%2 (from m_currentWindow variables)")
            .arg(m_currentWindowCenter, 0, 'f', 0).arg(m_currentWindowWidth, 0, 'f', 0));
    } else {
        // Show current pipeline values as fallback
        double pipelineCenter = m_imagePipeline->getWindowCenter();
        double pipelineWidth = m_imagePipeline->getWindowWidth();
        if (pipelineWidth > 0) {
            bottomRightText += QString("WL: %1 WW: %2").arg(pipelineCenter, 0, 'f', 0).arg(pipelineWidth, 0, 'f', 0);
            logMessage("DEBUG", QString("UI OVERLAY FALLBACK: WL=%1 WW=%2 (from pipeline - should NOT happen)")
                .arg(pipelineCenter, 0, 'f', 0).arg(pipelineWidth, 0, 'f', 0));
        }
    }
    
    // Update the overlay labels
    m_overlayTopLeft->setText(topLeftText);
    m_overlayTopRight->setText(topRightText);
    m_overlayBottomLeft->setText(bottomLeftText);
    m_overlayBottomRight->setText(bottomRightText);
    
    // Note: DICOM info panel is updated only when image changes or info panel is toggled,
    // not on every frame change. This avoids expensive file I/O and HTML generation.
}

void DicomViewer::positionOverlays()
{
    updateOverlayPositions();
}

void DicomViewer::updateImageDisplay()
{
    if (!m_currentPixmap.isNull() && m_graphicsScene && m_graphicsView) {
        // Clear previous image
        if (m_pixmapItem) {
            m_graphicsScene->removeItem(m_pixmapItem);
            delete m_pixmapItem;
        }
        
        // Add new pixmap to scene
        m_pixmapItem = m_graphicsScene->addPixmap(m_currentPixmap);
        
        // Center the image in the scene
        QRectF pixmapRect = m_pixmapItem->boundingRect();
        m_pixmapItem->setPos(-pixmapRect.width()/2, -pixmapRect.height()/2);
        
        // Set scene rect to center around the image
        m_graphicsScene->setSceneRect(pixmapRect.translated(-pixmapRect.width()/2, -pixmapRect.height()/2));
        
        // Center the view on the image
        m_graphicsView->centerOn(0, 0);
        
        // Show graphics view and hide label
        m_imageLabel->hide();
        m_graphicsView->show();
        
        // Update zoom display
        updateZoomOverlay();
    } else {
    }
}

void DicomViewer::updateZoomOverlay()
{
    // Update overlay info which includes zoom information
    updateOverlayInfo();
}

void DicomViewer::updateCursorMode()
{
    // TODO: Update cursor based on zoom level
}

void DicomViewer::updatePlayButtonIcon(const QString& iconFilename)
{
    if (!m_playAction) {
        return;
    }
    
    QString iconPath = m_iconPath + "/" + iconFilename;
    QIcon newIcon(iconPath);
    
    // Check if the icon is valid
    if (newIcon.isNull()) {
        // Create a simple colored rectangle as fallback
        QPixmap pixmap(48, 48);
        if (iconFilename.contains("Pause")) {
            pixmap.fill(QColor(200, 100, 100)); // Red-ish for pause
        } else {
            pixmap.fill(QColor(100, 200, 100)); // Green-ish for play
        }
        newIcon = QIcon(pixmap);
    }
    
    m_playAction->setIcon(newIcon);
}

void DicomViewer::processThroughPipeline()
{
    if (!m_transformationsEnabled) {
        return;
    }
    
    // Use original pixmap if available, otherwise use current pixmap
    QPixmap sourcePixmap = m_originalPixmap.isNull() ? m_currentPixmap : m_originalPixmap;
    if (sourcePixmap.isNull()) {
        return;
    }
    
    
    // Convert to image and process through pipeline
    QImage sourceImage = sourcePixmap.toImage();
    QImage processedImage = m_imagePipeline->processImage(sourceImage);
    
    if (processedImage.isNull()) {
        return;
    }
    
    // Convert back to pixmap and update display
    m_currentPixmap = QPixmap::fromImage(processedImage);
    
    // Do NOT sync W/L values from pipeline back to UI variables!
    // m_currentWindowCenter and m_currentWindowWidth should always contain
    // the original DICOM values for UI display, not the scaled pipeline values.
    
    updateImageDisplay();
}

void DicomViewer::resetZoomToFit()
{
    logMessage("[USER ACTION] Reset zoom to fit requested", LOG_DEBUG);
    fitToWindow();
}

double DicomViewer::calculateFitToWindowZoom()
{
    // TODO: Calculate proper zoom factor for fit-to-window
    return 1.0;
}

void DicomViewer::startWindowing(const QPoint& pos)
{
    m_windowingActive = true;
    m_windowingStartPos = pos;
    
    // Do NOT overwrite the original DICOM values!
    // m_currentWindowCenter and m_currentWindowWidth should already contain 
    // the original DICOM values from when the image was loaded.
    // m_originalWindowCenter and m_originalWindowWidth are for reset functionality.
    // The pipeline contains scaled values which should never be copied back to UI variables.
    
    // Enable window/level processing if not already enabled
    if (!m_imagePipeline->isWindowLevelEnabled()) {
        m_imagePipeline->setWindowLevelEnabled(true);
    }
    
    // Set cursor to indicate windowing mode
    if (m_graphicsView) {
        m_graphicsView->setCursor(Qt::SizeAllCursor);
    }
}

void DicomViewer::updateWindowing(const QPoint& pos)
{
    if (!m_windowingActive) {
        return;
    }
    
    // Calculate movement delta
    QPoint delta = pos - m_windowingStartPos;
    
    // Mouse windowing like Python implementation
    // X-axis (horizontal) controls window width (contrast)
    // Y-axis (vertical) controls window level/center (brightness) - inverted for intuitive behavior
    
    // Use adaptive sensitivity based on original window width (like Python version)
    // Make sensitivity more reasonable - use smaller base multiplier
    double adaptiveSensitivity = qMax(m_windowingSensitivity, m_originalWindowWidth / 200.0);
    
    double widthDelta = delta.x() * adaptiveSensitivity;  // Horizontal movement for width
    double centerDelta = -delta.y() * adaptiveSensitivity;  // Vertical movement for center (negative for intuitive up=brighter)
    
    // Calculate new values
    double newWidth = m_originalWindowWidth + widthDelta;
    double newCenter = m_originalWindowCenter + centerDelta;
    
    // Clamp to medical imaging ranges (match Python limits)
    newWidth = qBound(1.0, newWidth, 655536.0);  // Width: 1 to 655536
    newCenter = qBound(-32000.0, newCenter, 655536.0);  // Center: -32000 to 655536
    
    // Update values (these are kept as original DICOM values for UI display)
    m_currentWindowCenter = newCenter;
    m_currentWindowWidth = newWidth;
    
    // Scale window values for pipeline processing based on bit depth
    double pipelineCenter, pipelineWidth;
    int bitsStored = m_imagePipeline->getBitsStored();
    
    if (bitsStored > 8) {
        // For >8-bit images, scale window values to 8-bit range for pipeline processing
        double maxOriginalValue = (1 << bitsStored) - 1;  // e.g., 16383 for 14-bit
        double scaleFactor = 255.0 / maxOriginalValue;    // e.g., 255/16383 ≈ 0.0156
        
        pipelineCenter = newCenter * scaleFactor;
        pipelineWidth = newWidth * scaleFactor;
    } else {
        // For 8-bit images, use values directly
        pipelineCenter = newCenter;
        pipelineWidth = newWidth;
    }
    
    // Update the pipeline with scaled values for internal processing
    m_imagePipeline->setWindowLevel(pipelineCenter, pipelineWidth);
    
    // Apply window/level to current image through pipeline
    processThroughPipeline();
    updateOverlayInfo(); // Update overlay to show new values
}

void DicomViewer::endWindowing()
{
    m_windowingActive = false;
    
    // Restore cursor based on current W/L mode state
    if (m_graphicsView) {
        if (m_windowLevelModeEnabled) {
            m_graphicsView->setCursor(Qt::CrossCursor);  // Stay in W/L mode
        } else {
            m_graphicsView->setCursor(Qt::ArrowCursor);   // Normal mode
        }
    }
}

void DicomViewer::resetWindowLevel()
{
    // Reset window/level to original DICOM values instead of disabling
    if (m_originalWindowWidth > 0) {
        m_imagePipeline->setWindowLevel(m_originalWindowCenter, m_originalWindowWidth);
        m_imagePipeline->setWindowLevelEnabled(true);
        
        // Update current values to reflect the reset
        m_currentWindowCenter = m_originalWindowCenter;
        m_currentWindowWidth = m_originalWindowWidth;
    } else {
        // Fallback: disable window/level if no original values available
        m_imagePipeline->setWindowLevelEnabled(false);
    }
    processThroughPipeline();
    
    // Update overlay text to show new window/level values
    updateOverlayInfo();
}

void DicomViewer::toggleWindowLevelMode()
{
    logMessage("[USER ACTION] Toggle window/level mode requested", LOG_DEBUG);
    m_windowLevelModeEnabled = !m_windowLevelModeEnabled;
    

    
    // Enable/disable the pipeline's window/level processing
    if (m_windowLevelModeEnabled) {
        // Enable window/level interaction mode, but don't change current settings
        // The user needs to drag the mouse to start adjusting window/level
        // Keep current window/level values active
        m_imagePipeline->setWindowLevelEnabled(true);
    } else {
        // Disable interactive window/level mode, but restore original DICOM values
        // Don't disable windowing completely - restore to original settings
        if (m_originalWindowWidth > 0) {
            // Restore original DICOM window/level values
            m_imagePipeline->setWindowLevel(m_originalWindowCenter, m_originalWindowWidth);
            m_imagePipeline->setWindowLevelEnabled(true);
            
            // Update current values to reflect the restored state
            m_currentWindowCenter = m_originalWindowCenter;
            m_currentWindowWidth = m_originalWindowWidth;
            
            logMessage("DEBUG", QString("Restored original W/L: Center=%1 Width=%2")
                     .arg(m_originalWindowCenter).arg(m_originalWindowWidth));
        } else {
            // Fallback: if no original values, use reasonable defaults instead of disabling
            m_imagePipeline->setWindowLevel(127.5, 255.0);  // 8-bit defaults
            m_imagePipeline->setWindowLevelEnabled(true);
            
            m_currentWindowCenter = 127.5;
            m_currentWindowWidth = 255.0;
            
            logMessage("DEBUG", "No original W/L values - using 8-bit defaults");
        }
        
        // Process through pipeline to apply restored values
        processThroughPipeline();
    }
    
    // Update the button icon based on state
    if (m_windowLevelToggleAction) {
        QString iconName = m_windowLevelModeEnabled ? "WWL_Enabled_96.png" : "WWL_96.png";
        QIcon icon(QString(":/icons/") + iconName);
        m_windowLevelToggleAction->setIcon(icon);
        
    }
    
    // Update cursor to indicate mode change
    if (m_graphicsView) {
        if (m_windowLevelModeEnabled) {
            m_graphicsView->setCursor(Qt::CrossCursor);
        } else {
            m_graphicsView->setCursor(Qt::ArrowCursor);
        }
    }
}

void DicomViewer::applyWindowLevel(double center, double width)
{
    // Scale window values for pipeline processing based on bit depth
    double pipelineCenter, pipelineWidth;
    int bitsStored = m_imagePipeline->getBitsStored();
    
    if (bitsStored > 8) {
        // For >8-bit images, scale window values to 8-bit range for pipeline processing
        double maxOriginalValue = (1 << bitsStored) - 1;  // e.g., 16383 for 14-bit
        double scaleFactor = 255.0 / maxOriginalValue;    // e.g., 255/16383 ≈ 0.0156
        
        pipelineCenter = center * scaleFactor;
        pipelineWidth = width * scaleFactor;
    } else {
        // For 8-bit images, use values directly
        pipelineCenter = center;
        pipelineWidth = width;
    }
    
    m_imagePipeline->setWindowLevel(pipelineCenter, pipelineWidth);
    // Only enable if toggle button is ON
    if (m_windowLevelModeEnabled) {
        m_imagePipeline->setWindowLevelEnabled(true);
    } else {
    }
    processThroughPipeline();
}

void DicomViewer::loadDicomDir(const QString& dicomdirPath)
{
    logMessage("DEBUG", QString("loadDicomDir called with path: %1").arg(dicomdirPath));
    
    // Stop any existing first image monitor
    stopFirstImageMonitor();
    
    try {
        // Clear existing data
        m_dicomTree->clear();
        
        if (!m_dicomReader) {
            m_imageLabel->setText("Error: DICOM reader not initialized");
            return;
        }
        
        // Load DICOMDIR using DicomReader
        if (!m_dicomReader->loadDicomDir(dicomdirPath)) {
            m_imageLabel->setText(QString("Error loading DICOMDIR: %1").arg(m_dicomReader->getLastError()));
            return;
        }
        
        // Populate tree widget using DicomReader
        m_dicomReader->populateTreeWidget(m_dicomTree);
        
        logMessage("DEBUG", "[LOAD DICOMDIR] Tree populated, about to call detectAndStartDvdCopy()");
        
        // Start DVD detection and copy if needed BEFORE thumbnail generation
        detectAndStartDvdCopy();
        
        logMessage("DEBUG", "[LOAD DICOMDIR] detectAndStartDvdCopy() completed");
        
       
        // Expand first level items and select first image if available
        expandFirstItems();
        
        // Initialize file states for all DICOM files
        initializeFileStatesFromTree();
        
        // NOTE: File availability monitoring will be started after first image is displayed
        // to prevent thumbnails from blocking initial image display
        
        // Start monitoring for first available image (will check if already displaying)
        logMessage("DEBUG", "[LOAD DICOMDIR] Starting first image monitor");
        startFirstImageMonitor();
        
        // For local files (no DVD copy needed), auto-select first image after tree population
        QTimer::singleShot(100, [this]() {
            logMessage("DEBUG", QString("[AUTO-SELECT DEBUG] Timer triggered - copyInProgress: %1, dvdDetectionInProgress: %2, firstImageAutoSelected: %3")
                     .arg(m_copyInProgress).arg(m_dvdDetectionInProgress).arg(m_firstImageAutoSelected));
            
            if (!m_copyInProgress && !m_dvdDetectionInProgress && !m_firstImageAutoSelected) {
                logMessage("DEBUG", "[LOCAL FILES] Auto-selecting first image for local DICOMDIR");
                autoSelectFirstAvailableImage();  // Use our new state-based method
            } else {
                logMessage("DEBUG", "[LOCAL FILES] Auto-selection blocked by flags");
            }
        });
        
        // Backup timer for auto-selection in case the first one doesn't work
        QTimer::singleShot(500, [this]() {
            if (!m_dicomTree->currentItem() && m_dicomTree->topLevelItemCount() > 0) {
                logMessage("DEBUG", "[AUTO-SELECT BACKUP] No tree item selected, forcing selection with state-based method");
                autoSelectFirstAvailableImage();  // Use our new state-based method
            }
        });
        
        // Update display message
        if (m_dicomReader->getTotalImages() > 0) {
            m_imageLabel->setText("DICOMDIR loaded successfully. Select an image to view.");
            // Clear status bar when DICOMDIR is loaded successfully
            updateStatusBar("Ready", -1);
            
            // Start display monitor now that content is loaded
            startDisplayMonitor();
        } else {
            m_imageLabel->setText("DICOMDIR loaded but no images found.");
        }
        
        
    } catch (const std::exception& e) {
        m_imageLabel->setText("Error loading DICOMDIR file.");
    } catch (...) {
        m_imageLabel->setText("Unknown error loading DICOMDIR file.");
    }
}

void DicomViewer::autoLoadDicomdir()
{
    
    // Get the directory where the executable is located
    QString executablePath = QApplication::applicationDirPath();
    
    QDir executableDir(executablePath);
    
    // Define possible DICOMDIR filenames to look for
    QStringList dicomdirFilenames = {
        "DICOMDIR",
        "dicomdir", 
        "DICOMDIR.dcm",
        "dicomdir.dcm",
        "DICOMDIR.DCM"
    };
    
    // Search for DICOMDIR file in the executable directory
    QString foundDicomdirPath;
    for (const QString& filename : dicomdirFilenames) {
        QString fullPath = executableDir.absoluteFilePath(filename);
        
        if (QFile::exists(fullPath)) {
            foundDicomdirPath = fullPath;
            break;
        }
    }
    
    // If DICOMDIR found, load it automatically
    if (!foundDicomdirPath.isEmpty()) {
        m_imageLabel->setText("Auto-loading DICOMDIR...");
        loadDicomDir(foundDicomdirPath);
    } else {
        // Keep the default message
        m_imageLabel->setText("Select a DICOMDIR file to begin.");
    }
}

void DicomViewer::checkInitialFfmpegAvailability()
{
    qDebugT() << "Initializing Save Run button as disabled until ffmpeg copy completes";
    
      // First check if FFmpeg already exists locally
    QString executablePath = QApplication::applicationDirPath();
    QString localFfmpegPath = QDir(executablePath).absoluteFilePath("ffmpeg.exe");
    
    if (QFile::exists(localFfmpegPath)) {
        // FFmpeg already available - enable Save Run button immediately
        if (m_saveRunAction) {
            m_saveRunAction->setEnabled(true);
            m_ffmpegCopyCompleted = true; // Mark as completed
        }
        logMessage("DEBUG", "FFmpeg found locally - Video export ready");
        return; // Exit early - no need to wait for copy
    }
    
    if (m_saveRunAction) {
        // ALWAYS start with button disabled - keep original sequence as is
        m_saveRunAction->setEnabled(false);
        qDebugT() << "Save run button disabled at startup - will be enabled only after ffmpeg copy thread completion";
        logMessage("INFO", "Save Run button disabled - awaiting ffmpeg copy completion");
    }
}

QString DicomViewer::findFfmpegExecutable()
{
    // First, check if ffmpeg.exe is in the same directory as DicomViewer.exe
    QString executablePath = QApplication::applicationDirPath();
    QString localFfmpegPath = QDir(executablePath).absoluteFilePath("ffmpeg.exe");
    
    logMessage("DEBUG", QString("Checking for ffmpeg.exe in executable directory: %1").arg(localFfmpegPath));
    
    if (QFile::exists(localFfmpegPath)) {
        logMessage("DEBUG", "Found ffmpeg.exe in local directory");
        return localFfmpegPath;
    }
    
    // Check temp folder where DVD copy might have placed it
    QString tempFfmpegPath = QDir::tempPath() + "/Ekn_TempData/ffmpeg.exe";
    logMessage("DEBUG", QString("Checking for ffmpeg.exe in temp folder: %1").arg(tempFfmpegPath));
    
    if (QFile::exists(tempFfmpegPath)) {
        logMessage("DEBUG", "Found ffmpeg.exe in temp folder");
        return tempFfmpegPath;
    }
    
    // If not found locally, check the DVD drive (if available)
    if (!m_dvdSourcePath.isEmpty()) {
        QString dvdFfmpegPath = m_dvdSourcePath + "/ffmpeg.exe";
        
        logMessage("DEBUG", QString("Checking for ffmpeg.exe on DVD drive: %1").arg(dvdFfmpegPath));
        
        if (QFile::exists(dvdFfmpegPath)) {
            logMessage("DEBUG", "Found ffmpeg.exe on DVD drive");
            return dvdFfmpegPath;
        }
    }
    
    // Fallback: try to detect DVD drives and check for ffmpeg
    QStringList drivesToCheck = {"D:", "E:", "F:", "G:", "H:"};
    
    for (const QString& drive : drivesToCheck) {
        QString driveFfmpegPath = drive + "/ffmpeg.exe";
        
        logMessage("DEBUG", QString("Checking for ffmpeg.exe on drive: %1").arg(driveFfmpegPath));
        
        if (QFile::exists(driveFfmpegPath)) {
            // Verify this is likely the DVD drive by checking for DicomFiles directory
            QString dicomPath = drive + "/DicomFiles";
            if (QDir(dicomPath).exists()) {
                logMessage("DEBUG", QString("Found ffmpeg.exe on DVD drive: %1").arg(driveFfmpegPath));
                return driveFfmpegPath;
            }
        }
    }
    
    logMessage("WARN", "ffmpeg.exe not found in any location");
    return QString(); // Return empty string if not found
}

void DicomViewer::initializeLogging()
{
    // Initialize minimum log level based on build configuration
    m_minLogLevel = static_cast<LogLevel>(DEFAULT_LOG_LEVEL);
    
    // Set up log file in the same directory as the executable
    QString executablePath = QApplication::applicationDirPath();
    m_logFilePath = QDir(executablePath).absoluteFilePath("DicomViewer.log");
    
    // Write initial log entry
    logMessage("INFO", "DicomViewer application started");
    
    // Build information for version tracking
#ifdef _DEBUG
    logMessage("INFO", "BUILD INFO: Debug build with delayed thumbnail fixes (v2.1-DelayedThumbnails-DEBUG)");
#else
    logMessage("INFO", "BUILD INFO: Release build with delayed thumbnail fixes (v2.1-DelayedThumbnails-RELEASE)");
#endif
    logMessage("INFO", "BUILD DATE: " + QString(__DATE__) + " " + QString(__TIME__));
    logMessage("INFO", "THUMBNAIL SYSTEM: Delayed creation until all files complete (Race condition fix)");
    
    logMessage("INFO", "Log file: " + m_logFilePath);
    logMessage("INFO", "Executable directory: " + executablePath);
}

void DicomViewer::logMessage(const QString& level, const QString& message) const
{
    // Convert string level to enum for filtering
    LogLevel enumLevel;
    if (level == "DEBUG") enumLevel = LOG_DEBUG;
    else if (level == "INFO") enumLevel = LOG_INFO;
    else if (level == "WARNING" || level == "WARN") enumLevel = LOG_WARN;
    else if (level == "ERROR") enumLevel = LOG_ERROR;
    else enumLevel = LOG_INFO; // Default fallback
    
    // Filter based on minimum log level
    if (enumLevel < m_minLogLevel) {
        return; // Skip this log message
    }
    
    QMutexLocker locker(&m_logMutex);
    
    QFile logFile(m_logFilePath);
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Append)) {
        QTextStream stream(&logFile);
        
        QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
        Qt::HANDLE threadId = QThread::currentThreadId();
        QString threadIdStr = QString("0x%1").arg(reinterpret_cast<quintptr>(threadId), 0, 16);
        QString logEntry = QString("[%1] [Thread:%2] %3: %4").arg(timestamp, threadIdStr, level, message);
        
        stream << logEntry << Qt::endl;
        logFile.close();
        
        // Output to console for debugging (only in debug builds)
        #ifdef _DEBUG
        std::cout << logEntry.toStdString() << std::endl;
        #endif
    }
}

// Enum-based logMessage method
void DicomViewer::logMessage(LogLevel level, const QString& message) const
{
    // Filter based on minimum log level
    if (level < m_minLogLevel) {
        return; // Skip this log message
    }
    
    // Convert enum to string for the legacy method
    QString levelStr;
    switch (level) {
        case LOG_DEBUG: levelStr = "DEBUG"; break;
        case LOG_INFO:  levelStr = "INFO"; break;
        case LOG_WARN:  levelStr = "WARN"; break;
        case LOG_ERROR: levelStr = "ERROR"; break;
        default:    levelStr = "INFO"; break;
    }
    
    // Delegate to the string-based method
    logMessage(levelStr, message);
}

// Message-first enum-based logMessage method (for convenience)
void DicomViewer::logMessage(const QString& message, LogLevel level) const
{
    // Just call the level-first version
    logMessage(level, message);
}

bool DicomViewer::copyFfmpegExe()
{
    qDebugT() << "copyFfmpegExe() function entered";
    logMessage("INFO", "Starting ffmpeg.exe copy");
    
    if (m_dvdSourcePath.isEmpty()) {
        // First, try to use provided source drive from command line
        if (!m_providedSourceDrive.isEmpty()) {
            QString testDrive = m_providedSourceDrive;
            if (!testDrive.endsWith(":")) {
                testDrive += ":";
            }
            
            logMessage("INFO", "Using provided source drive for ffmpeg copy: " + testDrive);
            m_dvdSourcePath = testDrive;
        } else {
            // Fallback to auto-detection
            logMessage("DEBUG", "DVD source path empty - attempting detection");
            QStringList drives = {"D:", "E:", "F:", "G:", "H:"};
            
            for (const QString& drive : drives) {
                if (QDir(drive + "/DicomFiles").exists()) {
                    m_dvdSourcePath = drive;
                    logMessage("INFO", "Detected DVD: " + drive);
                    break;
                }
            }
            
            if (m_dvdSourcePath.isEmpty()) {
                logMessage("DEBUG", "No DVD detected - skipping copy");
                emit ffmpegCopyCompleted(true); // Still enable exports if local ffmpeg exists
                return true;
            }
        }
    }
    
    QString source = m_dvdSourcePath + "/ffmpeg.exe";
    logMessage("INFO", "Source Path for ffmpeg copy: " + source);

    if (!QFile::exists(source)) {
        logMessage("WARNING", "ffmpeg.exe not found - skipping copy");
        emit ffmpegCopyCompleted(true); // Still enable exports if local ffmpeg exists
        return true;
    }
    
    // Calculate destination directory
    QString tempDir;
    if (!m_currentImagePath.isEmpty()) {
        QString currentDir = QFileInfo(m_currentImagePath).absolutePath();
        logMessage("INFO", "Current path: " + currentDir);
        
        if (currentDir.contains("DicomFiles", Qt::CaseInsensitive)) {
            QDir dir(currentDir);
            if (dir.cdUp()) {
                tempDir = dir.absolutePath();
                logMessage("INFO", "Using parent dir: " + tempDir);
            } else {
                tempDir = currentDir;
            }
        } else {
            tempDir = currentDir;
        }
    } else {
        tempDir = QDir::temp().absoluteFilePath("Ekn_TempData");
        logMessage("INFO", "Using fallback: " + tempDir);
    }
    
    QString dest = QDir(tempDir).absoluteFilePath("ffmpeg.exe");
    logMessage("INFO", "Copy: " + source + " -> " + dest);
    
    if (QFile::exists(dest)) {
        logMessage("INFO", "ffmpeg.exe already exists - skipping");
        emit ffmpegCopyCompleted(true);
        return true;
    }
    
    // Ensure directory exists
    QDir destDir = QFileInfo(dest).absoluteDir();
    if (!destDir.exists()) {
        if (!destDir.mkpath(".")) {
            logMessage("ERROR", "Cannot create directory");
            return false;
        }
    }
    
    // Synchronous copy operation
    logMessage("INFO", "Starting synchronous ffmpeg copy");
    
    if (QFile::copy(source, dest)) {
        logMessage("INFO", "ffmpeg.exe copied successfully to: " + dest);
        emit ffmpegCopyCompleted(true);
        return true;
    } else {
        logMessage("ERROR", "Failed to copy ffmpeg.exe from: " + source + " to: " + dest);
        emit ffmpegCopyCompleted(false);
        return false;
    }
}

void DicomViewer::expandFirstItems()
{
    logMessage("DEBUG", "[EXPAND FIRST] expandFirstItems() called");
    
    if (m_dicomTree->topLevelItemCount() > 0) {
        logMessage("DEBUG", QString("[EXPAND FIRST] Found %1 top level items").arg(m_dicomTree->topLevelItemCount()));
        
        // Expand all patients to show their studies
        for (int i = 0; i < m_dicomTree->topLevelItemCount(); i++) {
            QTreeWidgetItem* patient = m_dicomTree->topLevelItem(i);
            patient->setExpanded(true);
            
            // Expand first study of each patient
            if (patient->childCount() > 0) {
                QTreeWidgetItem* firstStudy = patient->child(0);
                firstStudy->setExpanded(true);
                
                // Expand first series of first study
                if (firstStudy->childCount() > 0) {
                    QTreeWidgetItem* firstSeries = firstStudy->child(0);
                    firstSeries->setExpanded(true);
                    
                    // CRITICAL: Remove auto-selection to break recursion
                    // OLD CODE (CAUSES RECURSION): 
                    // onTreeItemSelected(firstImage, nullptr);
                    
                    logMessage("DEBUG", "[EXPAND FIRST] Tree expanded without auto-selection to prevent recursion");
                }
            }
        }
        
        // Use safe auto-selection method instead
        QTimer::singleShot(100, this, &DicomViewer::autoSelectFirstAvailableImage);
        
    } else {
        logMessage("DEBUG", "[EXPAND FIRST] No top level items found in tree");
    }
}

void DicomViewer::loadDicomImage(const QString& filePath)
{
    if (filePath.isEmpty()) return;
    
    // NEW: Protect DCMTK operations
    QMutexLocker dcmtkLocker(&m_dcmtkAccessMutex);
    
    // NEW: Verify file readiness
    {
        QMutexLocker fileLocker(&m_fileStatesMutex);
        QString fileName = QFileInfo(filePath).fileName();
        if (m_copyInProgress && !m_fileReadyStates.value(fileName, false)) {
            logMessage("WARN", QString("Cannot load image - file not ready: %1").arg(fileName));
            return;
        }
    }
    
#ifdef HAVE_DCMTK
    
    // Clear DICOM info cache when loading a new image
    if (m_currentImagePath != filePath) {
        m_cachedDicomInfoFilePath.clear();
        m_cachedDicomInfoHtml.clear();
    }
    
    // Switch to image widget
    m_mainStack->setCurrentWidget(m_imageWidget);
    
    // Disable transformation actions during loading
    setTransformationActionsEnabled(false);
    
    // Reset transformations for new image
    m_imagePipeline->resetAllTransformations();
    
    // Stop any ongoing playback and previous loading
    if (m_isPlaying) {
        togglePlayback();
    }
    
    // Stop any previous progressive loading
    if (m_progressiveLoader) {
        m_progressiveLoader->stop();
        m_progressiveLoader->wait();
        delete m_progressiveLoader;
        m_progressiveLoader = nullptr;
    }
    
    // Only clear frame cache if loading a different image
    if (filePath != m_currentImagePath) {
        clearFrameCache();
        m_isLoadingProgressively = false;
        m_allFramesCached = false;
        m_zoomFactor = 1.0; // Reset zoom for new image
    } else {
        // Keep existing cache and state for same image
        m_isLoadingProgressively = false; // Ensure we're not in loading mode
    }
    
    QString actualFilePath = filePath;
    
    // Check if the path is a directory (common with DICOMDIR references)
    QFileInfo fileInfo(filePath);
    if (fileInfo.isDir()) {
        QDir dir(filePath);
        
        // Look for DICOM files (they often have no extension or various extensions)
        QStringList nameFilters;
        nameFilters << "*" << "*.dcm" << "*.DCM" << "*.dicom" << "*.DICOM";
        
        QStringList files = dir.entryList(nameFilters, QDir::Files | QDir::Readable);
        
        // Filter out obvious non-DICOM files
        for (auto it = files.begin(); it != files.end(); ) {
            QString fileName = *it;
            if (fileName.endsWith(".txt", Qt::CaseInsensitive) ||
                fileName.endsWith(".inf", Qt::CaseInsensitive) ||
                fileName.endsWith(".log", Qt::CaseInsensitive) ||
                fileName == "." || fileName == "..") {
                it = files.erase(it);
            } else {
                ++it;
            }
        }
        
        if (!files.isEmpty()) {
            // Try to find a file that actually contains image data (not SR documents)
            bool foundImageFile = false;
            QStringList imageFiles;
            QStringList nonImageFiles;
            
            // First pass: categorize files
            for (const QString& file : files) {
                QString testPath = dir.absoluteFilePath(file);
                
                try {
                    DcmFileFormat dcmFile;
                    OFCondition status = dcmFile.loadFile(testPath.toLocal8Bit().constData());
                    if (status.good()) {
                        DcmDataset* dataset = dcmFile.getDataset();
                        if (dataset) {
                            // Check if it has pixel data (actual medical image)
                            DcmElement* pixelDataElement = nullptr;
                            if (dataset->findAndGetElement(DCM_PixelData, pixelDataElement).good() && pixelDataElement) {
                                // Check if it's a real image (not just a dose report)
                                OFString seriesDesc;
                                dataset->findAndGetOFString(DCM_SeriesDescription, seriesDesc);
                                QString series = QString::fromStdString(seriesDesc.c_str()).toLower();
                                
                                // Skip dose reports and structured reports
                                if (!series.contains("dose") && !series.contains("report") && !series.contains("sr")) {
                                    imageFiles.append(testPath);
                                } else {
                                    nonImageFiles.append(testPath);
                                }
                            } else {
                                nonImageFiles.append(testPath);
                            }
                        }
                    }
                } catch (...) {
                    // Ignore errors and try next file
                }
            }
            
            // Prefer actual images over reports
            if (!imageFiles.isEmpty()) {
                actualFilePath = imageFiles.first();
                foundImageFile = true;
            } else if (!nonImageFiles.isEmpty()) {
                actualFilePath = nonImageFiles.first();
                foundImageFile = true;
            }
            
            // If no file with image data found, use the first file anyway
            if (!foundImageFile) {
                actualFilePath = dir.absoluteFilePath(files.first());
            }
        } else {
            m_imageLabel->setText("No DICOM files found in directory");
            return;
        }
    }
    
    if (!QFile::exists(actualFilePath)) {
        // Handle missing file using copy monitoring system
        handleMissingFile(actualFilePath);
        return;
    }
    
    // Check if this specific file is still being copied
    QString filename = QFileInfo(actualFilePath).fileName();
    bool fileIsCompleted = m_fullyCompletedFiles.contains(filename);
    
    if (m_copyInProgress && !fileIsCompleted) {
        logMessage("DEBUG", QString("[FILE ACCESS] File not yet completed: %1 - copy still in progress").arg(filename));
        
        // Show progress in status bar, not on image display
        if (m_copyInProgress && m_currentCopyProgress > 0) {
            updateStatusBar(QString("Loading from media... %1%").arg(m_currentCopyProgress), m_currentCopyProgress);
        } else {
            updateStatusBar("Loading from media...", -1);
        }
        
        // Show appropriate message in image area for file not ready
        m_imageLabel->setText(QString("File is being copied from media...\n\n%1").arg(filename));
        return;
    }
    
    logMessage("DEBUG", QString("[FILE ACCESS] File is ready for access: %1 completed: %2").arg(filename).arg(fileIsCompleted));
    
    // Store current image path
    m_currentImagePath = actualFilePath;
    
    try {
        // Quick metadata check to determine if this is a valid DICOM image
        DcmFileFormat dcmFile;
        OFCondition status = dcmFile.loadFile(actualFilePath.toLocal8Bit().constData());
        
        if (status.bad()) {
            m_imageLabel->setText("Error loading DICOM file");
            setTransformationActionsEnabled(true);
            return;
        }
        
        DcmDataset* dataset = dcmFile.getDataset();
        if (!dataset) {
            m_imageLabel->setText("Invalid DICOM file");
            setTransformationActionsEnabled(true);
            return;
        }
        
        // Check if it's an image by looking for image-related tags
        Uint16 rows, columns;
        if (!dataset->findAndGetUint16(DCM_Rows, rows).good() || 
            !dataset->findAndGetUint16(DCM_Columns, columns).good()) {
            
            // Check what type of DICOM object this is
            OFString sopClassUID;
            if (dataset->findAndGetOFString(DCM_SOPClassUID, sopClassUID).good()) {
                logMessage("DEBUG", QString("DICOM file SOP Class UID: %1").arg(QString::fromLatin1(sopClassUID.c_str())));
                
                // Check if it's a Structured Report
                if (sopClassUID.find("1.2.840.10008.5.1.4.1.1.88") != OFString_npos) {
                    // This is a Structured Report - display it
                    displayReport(actualFilePath);
                    return;
                } else {
                    m_imageLabel->setText(QString("Selected DICOM file is not an image.\nSOP Class: %1").arg(sopClassUID.c_str()));
                }
            } else {
                m_imageLabel->setText("Selected file is not a DICOM image.\nMissing image dimensions (Rows/Columns tags).");
            }
            
            setTransformationActionsEnabled(true);
            return;
        }
        
        // Determine frame count
        OFString numberOfFramesStr;
        dataset->findAndGetOFString(DCM_NumberOfFrames, numberOfFramesStr);
        
        int totalFrames = 1;
        if (!numberOfFramesStr.empty()) {
            totalFrames = atoi(numberOfFramesStr.c_str());
        }
        
        
        // Extract DICOM metadata for overlays
        extractDicomMetadata(actualFilePath);
        
        // Show loading message
        m_imageLabel->setText(QString("Loading... (0/%1 frames)").arg(totalFrames));
        
        // Load file in DicomFrameProcessor for direct access
        if (m_frameProcessor && m_frameProcessor->loadDicomFile(actualFilePath)) {
        }
        
        // Start progressive loading
        m_progressiveLoader = new ProgressiveFrameLoader(actualFilePath);
        
        // Connect signals with Qt::QueuedConnection for responsive cross-thread communication
        connect(m_progressiveLoader, &ProgressiveFrameLoader::frameReady,
                this, &DicomViewer::onFrameReady, Qt::QueuedConnection);
        connect(m_progressiveLoader, &ProgressiveFrameLoader::allFramesLoaded,
                this, &DicomViewer::onAllFramesLoaded, Qt::QueuedConnection);
        connect(m_progressiveLoader, &ProgressiveFrameLoader::firstFrameInfo,
                this, &DicomViewer::onFirstFrameInfo, Qt::QueuedConnection);
        connect(m_progressiveLoader, &ProgressiveFrameLoader::errorOccurred,
                this, &DicomViewer::onLoadingError, Qt::QueuedConnection);
        connect(m_progressiveLoader, &ProgressiveFrameLoader::loadingProgress,
                this, &DicomViewer::onLoadingProgress, Qt::QueuedConnection);
        
        m_isLoadingProgressively = true;
        m_currentFrame = 0;
        m_totalFrames = totalFrames;
        
        // Reset progressive display timing for new image
        m_lastProgressiveDisplayTime = 0;
        
        // Set target FPS from DICOM frame timing if available, otherwise default to 7 FPS
        m_targetProgressiveFPS = 15; // Increased to match GDCM performance - will be updated from DICOM data if available
        
        // Start the progressive loading thread
        m_progressiveLoader->start();
        
    } catch (const std::exception& e) {
        m_imageLabel->setText("Error: " + QString(e.what()));
        setTransformationActionsEnabled(true);
    } catch (...) {
        m_imageLabel->setText("Unknown error loading DICOM image");
        setTransformationActionsEnabled(true);
    }
#else
    Q_UNUSED(filePath)
    m_imageLabel->setText("DCMTK support not available");
#endif
}

QPixmap DicomViewer::convertDicomFrameToPixmap(const QString& filePath, int frameIndex)
{
#ifdef HAVE_DCMTK
    try {
        // Create DicomImage object with automatic decompression
        DicomImage* dicomImage = new DicomImage(filePath.toLocal8Bit().constData(), CIF_AcrNemaCompatibility);
        
        if (dicomImage == nullptr || dicomImage->getStatus() != EIS_Normal) {
            // Get detailed error information
            EI_Status status = dicomImage ? dicomImage->getStatus() : EIS_InvalidDocument;
            
            // Check if this is a compression-related issue
            if (status == EIS_NotSupportedValue) {
            }
            
            delete dicomImage;
            return QPixmap();
        }
        
        // Check if frame index is valid
        if (frameIndex >= (int)dicomImage->getFrameCount()) {
            delete dicomImage;
            return QPixmap();
        }
        
        // Get image dimensions
        unsigned long width = dicomImage->getWidth();
        unsigned long height = dicomImage->getHeight();
        unsigned long depth = dicomImage->getDepth();
        
        
        // Select specific frame for multiframe images
        if (dicomImage->getFrameCount() > 1) {
            dicomImage = dicomImage->createScaledImage(width, height, 1, frameIndex);
            if (!dicomImage || dicomImage->getStatus() != EIS_Normal) {
                delete dicomImage;
                return QPixmap();
            }
        }
        

        
        // Convert to 8-bit for display (with window/level applied by DCMTK)
        const void* pixelData = dicomImage->getOutputData(8 /* bits per sample */);
        if (!pixelData) {
            delete dicomImage;
            return QPixmap();
        }
        
        // Create QImage from pixel data
        QImage qImage((const uchar*)pixelData, width, height, QImage::Format_Grayscale8);
        
        // Convert to RGB format for better compatibility
        QImage rgbImage = qImage.convertToFormat(QImage::Format_RGB888);
        
        // Create pixmap
        QPixmap pixmap = QPixmap::fromImage(rgbImage);
        
        delete dicomImage;
        return pixmap;
        
    } catch (const std::exception& e) {
        return QPixmap();
    } catch (...) {
        return QPixmap();
    }
#else
    Q_UNUSED(filePath)
    Q_UNUSED(frameIndex)
    return QPixmap();
#endif
}

void DicomViewer::setupMultiframePlayback(const QString& filePath)
{
#ifdef HAVE_DCMTK
    // Initialize legacy timer if framework is not available (backwards compatibility)
    if (!m_playbackController && !m_playbackTimer) {
        m_playbackTimer = new QTimer(this);
        connect(m_playbackTimer, &QTimer::timeout, this, &DicomViewer::nextFrame);
    }
    
    try {
        // Load DICOM file to get frame timing information
        DcmFileFormat dcmFile;
        OFCondition status = dcmFile.loadFile(filePath.toLocal8Bit().constData());
        
        if (status.bad()) {
            return;
        }
        
        DcmDataset* dataset = dcmFile.getDataset();
        if (!dataset) {
            return;
        }
        
        // Extract frame timing information
        OFString frameTimeStr;
        int frameTimeMs = 100; // Default 100ms (10 fps)
        bool foundTiming = false;
        
        // Priority 1: Frame Time (0018,1063) - most precise
        if (dataset->findAndGetOFString(DCM_FrameTime, frameTimeStr).good()) {
            double frameTime = atof(frameTimeStr.c_str());
            if (frameTime > 0) {
                frameTimeMs = (int)frameTime;
                foundTiming = true;
            }
        }
        
        // Priority 2: Recommended Display Frame Rate (0008,2144)
        if (!foundTiming) {
            OFString frameRateStr;
            if (dataset->findAndGetOFString(DCM_RecommendedDisplayFrameRate, frameRateStr).good()) {
                double frameRate = atof(frameRateStr.c_str());
                if (frameRate > 0) {
                    frameTimeMs = (int)(1000.0 / frameRate);
                    foundTiming = true;
                }
            }
        }
        
        // Priority 3: Cine Rate (0018,0040) - for ultrasound/cine loops
        if (!foundTiming) {
            OFString cineRateStr;
            if (dataset->findAndGetOFString(DCM_CineRate, cineRateStr).good()) {
                double cineRate = atof(cineRateStr.c_str());
                if (cineRate > 0) {
                    frameTimeMs = (int)(1000.0 / cineRate);
                    foundTiming = true;
                }
            }
        }
        
        // If no timing found, use intelligent defaults based on modality
        if (!foundTiming) {
            OFString modality;
            if (dataset->findAndGetOFString(DCM_Modality, modality).good()) {
                QString modalityStr = QString::fromStdString(modality.c_str()).toUpper();
                if (modalityStr == "US") { // Ultrasound - typically faster
                    frameTimeMs = 40; // 25 fps
                } else if (modalityStr == "XA" || modalityStr == "RF") { // Angiography - medium speed
                    frameTimeMs = 67; // ~15 fps
                } else { // Default for other modalities
                    frameTimeMs = 100; // 10 fps
                }
            } else {
            }
        }
        
        // Apply frame rate limits to prevent display issues
        const int MIN_FRAME_TIME_MS = 16;  // ~60 FPS max
        const int MAX_FRAME_TIME_MS = 2000; // 0.5 FPS min
        
        if (frameTimeMs < MIN_FRAME_TIME_MS) {
            frameTimeMs = MIN_FRAME_TIME_MS;
        } else if (frameTimeMs > MAX_FRAME_TIME_MS) {
            frameTimeMs = MAX_FRAME_TIME_MS;
        }
        
        // Update progressive loading FPS to match DICOM timing
        double fps = 1000.0 / frameTimeMs;
        m_targetProgressiveFPS = (int)fps;
        
        // Setup playback using simplified framework if available
        if (m_playbackController) {
            // Use simplified framework for playback control
            m_playbackController->setFrameRate(fps);
            m_playbackController->setTotalFrames(m_totalFrames);
            m_playbackController->setCurrentFrame(0);
            
            
            // Auto-start playback when first frame loads if configured
            // BUT ONLY if not in progressive loading mode - we want progressive display first
            if (m_playbackController->autoPlayPolicy() == DicomPlaybackController::OnFirstFrame && !m_isLoadingProgressively) {
                QTimer::singleShot(100, m_playbackController, &DicomPlaybackController::play);
            }
        } else {
            // Legacy timer-based playback
            if (m_playbackTimer) {
                m_playbackTimer->setInterval(frameTimeMs);
            }
            
        }
        
    } catch (const std::exception& e) {
    } catch (...) {
    }
#else
    Q_UNUSED(filePath)
#endif
}

// Progressive loading slot implementations
void DicomViewer::onFrameReady(int frameNumber)
{
    auto uiStart = std::chrono::high_resolution_clock::now();
    auto uiStartTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(uiStart.time_since_epoch()).count();
    
    
    // Fetch frame data from thread-safe cache instead of receiving through signal
    if (!m_progressiveLoader || !m_progressiveLoader->isFrameReady(frameNumber)) {
        return;
    }
    
    auto fetchStart = std::chrono::high_resolution_clock::now();
    QPixmap pixmap = m_progressiveLoader->getFramePixmap(frameNumber);
    QByteArray originalPixelData = m_progressiveLoader->getFrameOriginalData(frameNumber);
    auto fetchEnd = std::chrono::high_resolution_clock::now();
    auto fetchDuration = std::chrono::duration_cast<std::chrono::milliseconds>(fetchEnd - fetchStart).count();
    
    
    // CRITICAL: Check if this frame belongs to the currently loading image
    // Prevent contamination from previous image loading processes
    if (sender() != m_progressiveLoader) {
        return;
    }
    
    if (!m_isLoadingProgressively) {
        return;
    }
    
    // Cache the frame and original pixel data
    m_frameCache[frameNumber] = pixmap;
    m_originalPixelCache[frameNumber] = originalPixelData;
    
    // For the first frame (frame 0), display it immediately and set as current
    if (frameNumber == 0) {
        m_currentFrame = 0;
        m_currentPixmap = pixmap;
        m_originalPixmap = pixmap;  // Store the original unmodified pixmap
        m_currentDisplayedFrame = 0;
        auto displayStart = std::chrono::high_resolution_clock::now();
        updateImageDisplay();
        auto displayEnd = std::chrono::high_resolution_clock::now();
        auto displayDuration = std::chrono::duration_cast<std::chrono::milliseconds>(displayEnd - displayStart).count();
        
        updateOverlayInfo();
        
        // Fit to window initially for new images
        if (m_zoomFactor == 1.0) {
            fitToWindow();
        }
        
        auto uiEnd = std::chrono::high_resolution_clock::now();
        auto uiDuration = std::chrono::duration_cast<std::chrono::milliseconds>(uiEnd - uiStart).count();
        auto uiEndTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(uiEnd.time_since_epoch()).count();
        
        // Enable transformations after first frame loads
        setTransformationActionsEnabled(true);
        
        // If DICOM info panel is visible, refresh it with the new image data
        if (m_dicomInfoVisible && !m_currentImagePath.isEmpty()) {
            populateDicomInfo(m_currentImagePath);
        }
        
        // For multi-frame images, setup playback controller but don't start timer yet
        // We want progressive display during first load, timer-based replay afterward
        if (m_totalFrames > 1) {
            setupMultiframePlayback(m_currentImagePath);
        }
    } else {
        // Implement smart progressive display strategy:
        // 1) First loading: Display frames as they become ready, but respect FPS timing
        // 2) Subsequent replays: Timer handles all display using cached frames
        
        if (!m_isPlaying) {
            // Progressive display strategy: Show ALL frames in sequence at target FPS
            // - If frame time has elapsed: Display immediately when frame is ready
            // - If frame arrives early: Wait until proper time, then display
            // - NEVER skip frames - all frames shown in sequential order
            qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
            int frameInterval = 1000 / m_targetProgressiveFPS; // milliseconds per frame
            
            if (m_lastProgressiveDisplayTime == 0 || 
                (currentTime - m_lastProgressiveDisplayTime) >= frameInterval) {
                // Frame time has elapsed OR this is first frame - display immediately
                auto displayStart = std::chrono::high_resolution_clock::now();
                qint64 timeSinceLastFrame = (m_lastProgressiveDisplayTime == 0) ? 0 : (currentTime - m_lastProgressiveDisplayTime);
                displayCachedFrame(frameNumber);
                auto displayEnd = std::chrono::high_resolution_clock::now();
                auto displayDuration = std::chrono::duration_cast<std::chrono::milliseconds>(displayEnd - displayStart).count();
                
                m_currentDisplayedFrame = frameNumber;
                m_currentFrame = frameNumber;
                
                auto uiEnd = std::chrono::high_resolution_clock::now();
                auto uiDuration = std::chrono::duration_cast<std::chrono::milliseconds>(uiEnd - uiStart).count();
                auto uiEndTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(uiEnd.time_since_epoch()).count();
                updateOverlayInfo();
                
                m_lastProgressiveDisplayTime = currentTime;
            } else {
                // Frame came too quickly - schedule display at the right time
                int delayMs = frameInterval - (currentTime - m_lastProgressiveDisplayTime);
                qint64 timeSinceLastFrame = currentTime - m_lastProgressiveDisplayTime;
                
                auto uiEnd = std::chrono::high_resolution_clock::now();
                auto uiDuration = std::chrono::duration_cast<std::chrono::milliseconds>(uiEnd - uiStart).count();
                auto uiEndTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(uiEnd.time_since_epoch()).count();
                
                // Schedule display at the precise FPS timing - NO FRAME SKIPPING
                QTimer::singleShot(delayMs, this, [this, frameNumber]() {
                    if (!m_isPlaying && m_frameCache.contains(frameNumber)) {
                        auto scheduledStart = std::chrono::high_resolution_clock::now();
                        auto scheduledStartTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(scheduledStart.time_since_epoch()).count();
                        qint64 actualTime = QDateTime::currentMSecsSinceEpoch();
                        
                        
                        auto displayStart = std::chrono::high_resolution_clock::now();
                        displayCachedFrame(frameNumber);
                        auto displayEnd = std::chrono::high_resolution_clock::now();
                        auto displayDuration = std::chrono::duration_cast<std::chrono::milliseconds>(displayEnd - displayStart).count();
                        
                        m_currentDisplayedFrame = frameNumber;
                        m_currentFrame = frameNumber;
                        updateOverlayInfo();
                        
                        auto scheduledEnd = std::chrono::high_resolution_clock::now();
                        auto scheduledDuration = std::chrono::duration_cast<std::chrono::milliseconds>(scheduledEnd - scheduledStart).count();
                        auto scheduledEndTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(scheduledEnd.time_since_epoch()).count();
                        
                        m_lastProgressiveDisplayTime = actualTime;
                    }
                });
            }
        } else {
        }
    }
}

void DicomViewer::onAllFramesLoaded(int totalFrames)
{
    
    m_allFramesCached = true;
    m_isLoadingProgressively = false;
    
    // Re-enable transformation actions now that loading is complete
    setTransformationActionsEnabled(true);
    
    // Reset window title (remove loading indicator)
    setWindowTitle("DICOM Viewer C++");
    
    // Start automatic playback for multi-frame images (like Python implementation)
    if (m_totalFrames > 1 && !m_isPlaying) {
        
        // Ensure playback controller is properly configured
        if (m_playbackController) {
            m_playbackController->setTotalFrames(m_totalFrames);
            
            // Start playback automatically
            if (!m_isPlaying) {
                togglePlayback();
                // Icon will be updated by togglePlayback() to show Pause
            }
        }
    } else if (m_totalFrames == 1) {
        // For single-frame images, ensure the play icon is shown (not pause)
        updatePlayButtonIcon("Play_96.png");
    }
    
}

void DicomViewer::onProgressiveTimerTimeout()
{
    // Timer has fired, indicating it's time for the next progressive frame display
    // This ensures we don't display frames faster than the target FPS during progressive loading
}

void DicomViewer::onFirstFrameInfo(const QString& patientName, const QString& patientId, int totalFrames)
{
    
    // Store the total frames count immediately
    m_totalFrames = totalFrames;
    
    // TODO: Update overlay with patient information
    // This will be implemented when we add the overlay system
    
    // Ensure overlays are properly positioned
    updateOverlayPositions();
}

void DicomViewer::onLoadingError(const QString& errorMessage)
{
    
    m_imageLabel->setText(QString("Error loading image:\n%1").arg(errorMessage));
    m_isLoadingProgressively = false;
    
    // Reset window title
    setWindowTitle("DICOM Viewer C++");
    
    // Re-enable transformation actions even on error
    setTransformationActionsEnabled(true);
}

void DicomViewer::onLoadingProgress(int currentFrame, int totalFrames)
{
    // Update loading message
    m_imageLabel->setText(QString("Loading... (%1/%2 frames)").arg(currentFrame).arg(totalFrames));
    
    // Update window title with progress
    setWindowTitle(QString("DICOM Viewer C++ - Loading (%1/%2 frames)").arg(currentFrame).arg(totalFrames));
}

void DicomViewer::displayCachedFrame(int frameIndex)
{
    if (m_frameCache.contains(frameIndex)) {
        
        // Update current frame number
        m_currentFrame = frameIndex;
        
        // Use the cached frame as original source
        QPixmap cachedPixmap = m_frameCache[frameIndex];
        m_originalPixmap = cachedPixmap;  // Store the original unmodified pixmap
        m_currentDisplayedFrame = frameIndex;
        
        // Process through pipeline to apply any active transformations
        processThroughPipeline();
        
        // Update overlays
        updateOverlayInfo();
        
    } else {
    }
}

void DicomViewer::clearFrameCache()
{
    
    // Stop and reset playback controller first
    if (m_playbackController) {
        m_playbackController->stop();
        m_playbackController->setTotalFrames(1);
        m_playbackController->setCurrentFrame(0);
    }
    
    // Stop legacy playback timer
    if (m_playbackTimer && m_playbackTimer->isActive()) {
        m_playbackTimer->stop();
        m_isPlaying = false;
        m_playbackPausedForFrame = false;  // Clear pause flag when resetting
    }
    
    // Clear all cached data
    m_frameCache.clear();
    m_originalPixelCache.clear();
    m_currentFrame = 0;
    m_currentDisplayedFrame = -1;
    m_totalFrames = 1;
    m_allFramesCached = false;
    
}

void DicomViewer::setTransformationActionsEnabled(bool enabled)
{
    m_transformationsEnabled = enabled;
    
    // Enable/disable transformation toolbar buttons
    for (auto it = m_transformationActions.begin(); it != m_transformationActions.end(); ++it) {
        if (it.value()) {
            it.value()->setEnabled(enabled);
        }
    }
}

QString DicomViewer::cleanDicomText(const QString& text)
{
    if (text.isEmpty()) {
        return "N/A";
    }
    // Replace DICOM component separators (^) with spaces and trim
    QString result = text;
    return result.replace('^', ' ').trimmed();
}

void DicomViewer::extractDicomMetadata(const QString& filePath)
{
#ifdef HAVE_DCMTK
    try {
        DcmFileFormat dcmFile;
        OFCondition status = dcmFile.loadFile(filePath.toLocal8Bit().constData());
        if (status.bad()) {
            return;
        }
        
        DcmDataset* dataset = dcmFile.getDataset();
        if (!dataset) {
            return;
        }
        
        OFString value;
        
        // Patient Information
        if (dataset->findAndGetOFString(DCM_PatientID, value).good()) {
            m_currentPatientId = cleanDicomText(QString::fromStdString(value.c_str()));
        }
        
        if (dataset->findAndGetOFString(DCM_PatientName, value).good()) {
            m_currentPatientName = cleanDicomText(QString::fromStdString(value.c_str()));
        }
        
        if (dataset->findAndGetOFString(DCM_PatientSex, value).good()) {
            m_currentPatientSex = QString::fromStdString(value.c_str()).trimmed();
        }
        
        if (dataset->findAndGetOFString(DCM_PatientAge, value).good()) {
            m_currentPatientAge = QString::fromStdString(value.c_str()).trimmed();
        }
        
        // Study and Series Information
        if (dataset->findAndGetOFString(DCM_StudyDescription, value).good()) {
            m_currentStudyDescription = cleanDicomText(QString::fromStdString(value.c_str()));
        }
        
        if (dataset->findAndGetOFString(DCM_SeriesDescription, value).good()) {
            m_currentSeriesDescription = cleanDicomText(QString::fromStdString(value.c_str()));
        }
        
        // Performing Physician
        if (dataset->findAndGetOFString(DCM_PerformingPhysicianName, value).good()) {
            m_currentPerformingPhysician = cleanDicomText(QString::fromStdString(value.c_str()));
        }
        
        // Institution
        if (dataset->findAndGetOFString(DCM_InstitutionName, value).good()) {
            m_currentInstitutionName = cleanDicomText(QString::fromStdString(value.c_str()));
        }
        
        // Acquisition Date and Time
        if (dataset->findAndGetOFString(DCM_AcquisitionDate, value).good()) {
            m_currentAcquisitionDate = QString::fromStdString(value.c_str()).trimmed();
        }
        
        if (dataset->findAndGetOFString(DCM_AcquisitionTime, value).good()) {
            m_currentAcquisitionTime = QString::fromStdString(value.c_str()).trimmed();
        }
        
        // Positioner Angles
        Float64 floatValue;
        m_hasPositionerAngles = false;
        if (dataset->findAndGetFloat64(DCM_PositionerPrimaryAngle, floatValue).good()) {
            m_currentPositionerPrimaryAngle = floatValue;
            m_hasPositionerAngles = true;
        }
        
        if (dataset->findAndGetFloat64(DCM_PositionerSecondaryAngle, floatValue).good()) {
            m_currentPositionerSecondaryAngle = floatValue;
            m_hasPositionerAngles = true;
        }
        
        // Technical Parameters
        m_hasTechnicalParams = false;
        
        // Try to get X-Ray Tube Current (mA) - Tag (0018,1151)
        bool foundTubeCurrent = false;
        
        // Method 1: Try as Float64
        if (dataset->findAndGetFloat64(DCM_XRayTubeCurrent, floatValue).good()) {
            m_currentXRayTubeCurrent = floatValue;
            m_hasTechnicalParams = true;
            foundTubeCurrent = true;
        }
        
        // Method 2: Try as Uint16
        if (!foundTubeCurrent) {
            Uint16 uint16Value;
            if (dataset->findAndGetUint16(DCM_XRayTubeCurrent, uint16Value).good()) {
                m_currentXRayTubeCurrent = static_cast<double>(uint16Value);
                m_hasTechnicalParams = true;
                foundTubeCurrent = true;
            }
        }
        
        // Method 3: Try as string and convert
        if (!foundTubeCurrent) {
            OFString stringValue;
            if (dataset->findAndGetOFString(DCM_XRayTubeCurrent, stringValue).good()) {
                bool ok;
                double val = QString::fromStdString(stringValue.c_str()).toDouble(&ok);
                if (ok) {
                    m_currentXRayTubeCurrent = val;
                    m_hasTechnicalParams = true;
                    foundTubeCurrent = true;
                }
            }
        }
        
        // Method 4: Try accessing by hex tag directly (0018,1151)
        if (!foundTubeCurrent) {
            DcmElement* element = nullptr;
            if (dataset->findAndGetElement(DcmTagKey(0x0018, 0x1151), element).good() && element) {
                OFString strValue;
                if (element->getOFString(strValue, 0).good()) {
                    bool ok;
                    double val = QString::fromStdString(strValue.c_str()).toDouble(&ok);
                    if (ok) {
                        m_currentXRayTubeCurrent = val;
                        m_hasTechnicalParams = true;
                        foundTubeCurrent = true;
                    }
                }
            }
        }
        
        if (!foundTubeCurrent) {
        }
        
        // Try to get KVP - Tag (0018,0060)
        bool foundKVP = false;
        
        // Method 1: Try as Float64
        if (dataset->findAndGetFloat64(DCM_KVP, floatValue).good()) {
            m_currentKVP = floatValue;
            m_hasTechnicalParams = true;
            foundKVP = true;
        }
        
        // Method 2: Try as string and convert
        if (!foundKVP) {
            OFString stringValue;
            if (dataset->findAndGetOFString(DCM_KVP, stringValue).good()) {
                bool ok;
                double val = QString::fromStdString(stringValue.c_str()).toDouble(&ok);
                if (ok) {
                    m_currentKVP = val;
                    m_hasTechnicalParams = true;
                    foundKVP = true;
                }
            }
        }
        
        // Method 3: Try accessing by hex tag directly (0018,0060)
        if (!foundKVP) {
            DcmElement* element = nullptr;
            if (dataset->findAndGetElement(DcmTagKey(0x0018, 0x0060), element).good() && element) {
                OFString strValue;
                if (element->getOFString(strValue, 0).good()) {
                    bool ok;
                    double val = QString::fromStdString(strValue.c_str()).toDouble(&ok);
                    if (ok) {
                        m_currentKVP = val;
                        m_hasTechnicalParams = true;
                        foundKVP = true;
                    }
                }
            }
        }
        
        if (!foundKVP) {
        }
        
        // Extract BitsStored and BitsAllocated for proper scaling
        Uint16 bitsStored = 8;  // Default to 8-bit
        Uint16 bitsAllocated = 8;
        dataset->findAndGetUint16(DCM_BitsStored, bitsStored);
        dataset->findAndGetUint16(DCM_BitsAllocated, bitsAllocated);
        
        // Extract Window/Level values - Tags (0028,1050) and (0028,1051)
        // Store these as the ORIGINAL DICOM values that should never be modified
        bool foundWindowLevel = false;
        double originalDicomCenter = 0.0;
        double originalDicomWidth = 0.0;
        
        // Window Center (Level) - store original DICOM value
        if (dataset->findAndGetFloat64(DCM_WindowCenter, floatValue).good()) {
            originalDicomCenter = floatValue;
            m_originalWindowCenter = floatValue;  // For reset functionality
            foundWindowLevel = true;
        }
        
        // Window Width - store original DICOM value  
        if (dataset->findAndGetFloat64(DCM_WindowWidth, floatValue).good()) {
            originalDicomWidth = floatValue;
            m_originalWindowWidth = floatValue;   // For reset functionality
            foundWindowLevel = foundWindowLevel && true;
        }
        
        // Set current values to original DICOM values for UI display
        // These should always show the actual DICOM values to the user
        m_currentWindowCenter = originalDicomCenter;
        m_currentWindowWidth = originalDicomWidth;
        
        // Store BitsStored for pipeline processing
        m_imagePipeline->setBitsStored(bitsStored);
        
        // If window/level values found, apply them to the display pipeline
        if (foundWindowLevel && originalDicomWidth > 0) {
            // IMPORTANT: DCMTK's getOutputData(8) converts >8-bit data to 8-bit range (0-255)
            // We need to scale the window/level values from original bit depth to 8-bit range
            
            double pipelineCenter, pipelineWidth;
            
            if (bitsStored > 8) {
                // For >8-bit images, scale window values to 8-bit range
                double maxOriginalValue = (1 << bitsStored) - 1;  // e.g., 16383 for 14-bit
                double scaleFactor = 255.0 / maxOriginalValue;    // e.g., 255/16383 ≈ 0.0156
                
                pipelineCenter = originalDicomCenter * scaleFactor;
                pipelineWidth = originalDicomWidth * scaleFactor;
                
                logMessage("DEBUG", QString("Scaled window values: Original C=%1 W=%2 -> 8-bit C=%3 W=%4 (scale=%5, BitsStored=%6)")
                    .arg(originalDicomCenter).arg(originalDicomWidth)
                    .arg(pipelineCenter).arg(pipelineWidth)
                    .arg(scaleFactor).arg(bitsStored));
            } else {
                // For 8-bit images, use original values
                pipelineCenter = originalDicomCenter;
                pipelineWidth = originalDicomWidth;
                
                logMessage("DEBUG", QString("Window values applied directly: C=%1 W=%2 (8-bit image, BitsStored=%3)")
                    .arg(originalDicomCenter).arg(originalDicomWidth).arg(bitsStored));
            }
            
            // Apply scaled values to pipeline (internal processing only)
            // This does NOT affect m_currentWindowCenter/Width which are used for UI
            m_imagePipeline->setWindowLevel(pipelineCenter, pipelineWidth);
            
            // Only enable if toggle button is ON
            if (m_windowLevelModeEnabled) {
                m_imagePipeline->setWindowLevelEnabled(true);
            } else {
            }
        } else {
            // Calculate default window/level for 8-bit display
            // Use 8-bit range (0-255) regardless of original bit depth
            // since we're displaying on 8-bit pipeline
            m_currentWindowWidth = 255.0;      // Full 8-bit range
            m_currentWindowCenter = 127.5;     // Middle of 8-bit range
            m_originalWindowCenter = m_currentWindowCenter;
            m_originalWindowWidth = m_currentWindowWidth;
            m_imagePipeline->setWindowLevel(m_currentWindowCenter, m_currentWindowWidth);
            
            logMessage("DEBUG", QString("Default windowing: C=%1 W=%2 (8-bit defaults)")
                .arg(m_currentWindowCenter).arg(m_currentWindowWidth));
                
            // Only enable if toggle button is ON
            if (m_windowLevelModeEnabled) {
                m_imagePipeline->setWindowLevelEnabled(true);
            } else {
            }
        }
        
        
    } catch (const std::exception& e) {
    } catch (...) {
    }
#else
    Q_UNUSED(filePath);
#endif
}

// Simplified Framework Implementation
void DicomViewer::initializeFramework()
{
    // Create framework components
    m_playbackController = new DicomPlaybackController(this);
    m_inputHandler = new DicomInputHandler(this);
    
    // Configure components
    configurePlaybackController();
    configureInputHandler();
    
    // Connect signals
    connectFrameworkSignals();
}

void DicomViewer::configurePlaybackController()
{
    m_playbackController->setAutoPlayPolicy(DicomPlaybackController::OnFirstFrame);
}

void DicomViewer::configureInputHandler()
{
}

void DicomViewer::connectFrameworkSignals()
{
    // Connect playback signals
    connect(m_playbackController, &DicomPlaybackController::playbackStateChanged,
            this, &DicomViewer::onPlaybackStateChanged);
    connect(m_playbackController, &DicomPlaybackController::currentFrameChanged,
            this, &DicomViewer::onCurrentFrameChanged);
    // Note: frameRequested signal intentionally not connected to avoid duplicate display calls
    // onCurrentFrameChanged() handles all frame display logic including fallbacks
    
    // Connect input signals
    connect(m_inputHandler, &DicomInputHandler::playPauseRequested,
            this, &DicomViewer::onPlayPauseRequested);
    connect(m_inputHandler, &DicomInputHandler::nextFrameRequested,
            this, &DicomViewer::onNextFrameRequested);
    connect(m_inputHandler, &DicomInputHandler::previousFrameRequested,
            this, &DicomViewer::onPreviousFrameRequested);
    connect(m_inputHandler, &DicomInputHandler::nextImageRequested,
            this, &DicomViewer::onNextImageRequested);
    connect(m_inputHandler, &DicomInputHandler::previousImageRequested,
            this, &DicomViewer::onPreviousImageRequested);
    connect(m_inputHandler, &DicomInputHandler::horizontalFlipRequested,
            this, &DicomViewer::onHorizontalFlipRequested);
    connect(m_inputHandler, &DicomInputHandler::verticalFlipRequested,
            this, &DicomViewer::onVerticalFlipRequested);
    connect(m_inputHandler, &DicomInputHandler::invertImageRequested,
            this, &DicomViewer::onInvertImageRequested);
    connect(m_inputHandler, &DicomInputHandler::resetAllRequested,
            this, &DicomViewer::onResetAllRequested);
    
}

// Framework slot implementations
void DicomViewer::onPlaybackStateChanged(DicomPlaybackController::PlaybackState oldState, DicomPlaybackController::PlaybackState newState)
{
    Q_UNUSED(oldState)
    
    switch (newState) {
    case DicomPlaybackController::Playing:
        updatePlayButtonIcon("Pause_96.png");
        m_isPlaying = true;
        break;
    case DicomPlaybackController::Paused:
    case DicomPlaybackController::Stopped:
    case DicomPlaybackController::Ready:
        updatePlayButtonIcon("Play_96.png");
        m_isPlaying = false;
        m_playbackPausedForFrame = false;  // Clear pause flag when framework stops
        break;
    }
}

void DicomViewer::onCurrentFrameChanged(int frameIndex, int totalFrames)
{
    
    m_currentFrame = frameIndex;
    m_totalFrames = totalFrames;
    
    if (m_frameCache.contains(frameIndex)) {
        displayCachedFrame(frameIndex);
        m_currentDisplayedFrame = frameIndex;
        // Update overlay only when frame is actually displayed
        updateOverlayInfo();
    } else {
        // Do not display anything yet - wait for the actual frame to be loaded
        // Do NOT update overlay here - frame number should only change when frame is actually shown
        // When frameReady() is emitted, it will trigger another onCurrentFrameChanged call
        // This ensures each frame is displayed exactly once, no duplicates
    }
}

void DicomViewer::onFrameRequested(int frameIndex)
{
    if (m_frameCache.contains(frameIndex)) {
        displayCachedFrame(frameIndex);
    }
}

void DicomViewer::onPlayPauseRequested()
{
    
    if (m_playbackController) {
        m_playbackController->togglePlayback();
    } else {
        togglePlayback();
    }
}

void DicomViewer::onNextFrameRequested()
{
    logMessage("[USER ACTION] Next frame requested", LOG_DEBUG);
    // Only handle frame navigation for multiframe images
    if (m_totalFrames <= 1) {
        return;
    }
    
    if (m_playbackController) {
        m_playbackController->nextFrame();
    } else {
        nextFrame();
    }
}

void DicomViewer::onPreviousFrameRequested()
{
    logMessage("[USER ACTION] Previous frame requested", LOG_DEBUG);
    // Only handle frame navigation for multiframe images
    if (m_totalFrames <= 1) {
        return;
    }
    
    if (m_playbackController) {
        m_playbackController->previousFrame();
    } else {
        previousFrame();
    }
}

void DicomViewer::onNextImageRequested()
{
    nextImage();
}

void DicomViewer::onPreviousImageRequested()
{
    previousImage();
}

void DicomViewer::onHorizontalFlipRequested()
{
    horizontalFlip();
}

void DicomViewer::onVerticalFlipRequested()
{
    verticalFlip();
}

void DicomViewer::onInvertImageRequested()
{
    invertImage();
}

void DicomViewer::onResetAllRequested()
{
    resetTransformations();
}

// Tree navigation helper functions
QTreeWidgetItem* DicomViewer::findNextSelectableItem(QTreeWidgetItem* currentItem)
{
    if (!currentItem || !m_dicomTree) return nullptr;
    
    // First try to find a sibling after the current item
    QTreeWidgetItem* parent = currentItem->parent();
    if (parent) {
        int currentIndex = parent->indexOfChild(currentItem);
        for (int i = currentIndex + 1; i < parent->childCount(); i++) {
            QTreeWidgetItem* sibling = parent->child(i);
            if (isSelectableItem(sibling)) {
                return sibling;
            }
            // Check children of sibling (if it's expanded or has image children)
            if (sibling->childCount() > 0) {
                QTreeWidgetItem* childItem = findFirstSelectableChild(sibling);
                if (childItem) return childItem;
            }
        }
        
        // No more siblings, go to parent's next sibling
        return findNextSelectableItem(parent);
    } else {
        // This is a top-level item, find next top-level item
        int currentIndex = m_dicomTree->indexOfTopLevelItem(currentItem);
        for (int i = currentIndex + 1; i < m_dicomTree->topLevelItemCount(); i++) {
            QTreeWidgetItem* topItem = m_dicomTree->topLevelItem(i);
            if (isSelectableItem(topItem)) {
                return topItem;
            }
            // Check children
            if (topItem->childCount() > 0) {
                QTreeWidgetItem* childItem = findFirstSelectableChild(topItem);
                if (childItem) return childItem;
            }
        }
    }
    
    return nullptr;
}

QTreeWidgetItem* DicomViewer::findPreviousSelectableItem(QTreeWidgetItem* currentItem)
{
    if (!currentItem || !m_dicomTree) return nullptr;
    
    QTreeWidgetItem* parent = currentItem->parent();
    if (parent) {
        int currentIndex = parent->indexOfChild(currentItem);
        for (int i = currentIndex - 1; i >= 0; i--) {
            QTreeWidgetItem* sibling = parent->child(i);
            // Check last child first (if it has children)
            if (sibling->childCount() > 0) {
                QTreeWidgetItem* lastChild = findLastSelectableChild(sibling);
                if (lastChild) return lastChild;
            }
            if (isSelectableItem(sibling)) {
                return sibling;
            }
        }
        
        // No previous siblings, go to parent if it's selectable
        if (isSelectableItem(parent)) {
            return parent;
        }
        return findPreviousSelectableItem(parent);
    } else {
        // This is a top-level item
        int currentIndex = m_dicomTree->indexOfTopLevelItem(currentItem);
        for (int i = currentIndex - 1; i >= 0; i--) {
            QTreeWidgetItem* topItem = m_dicomTree->topLevelItem(i);
            // Check last child first
            if (topItem->childCount() > 0) {
                QTreeWidgetItem* lastChild = findLastSelectableChild(topItem);
                if (lastChild) return lastChild;
            }
            if (isSelectableItem(topItem)) {
                return topItem;
            }
        }
    }
    
    return nullptr;
}

void DicomViewer::selectFirstImageItem()
{
    if (!m_dicomTree) return;
    
    for (int i = 0; i < m_dicomTree->topLevelItemCount(); i++) {
        QTreeWidgetItem* topItem = m_dicomTree->topLevelItem(i);
        QTreeWidgetItem* firstItem = findFirstImageChild(topItem);
        if (firstItem) {
            m_dicomTree->setCurrentItem(firstItem);
            m_dicomTree->scrollToItem(firstItem);
            return;
        }
    }
}

void DicomViewer::selectLastImageItem()
{
    if (!m_dicomTree) return;
    
    for (int i = m_dicomTree->topLevelItemCount() - 1; i >= 0; i--) {
        QTreeWidgetItem* topItem = m_dicomTree->topLevelItem(i);
        QTreeWidgetItem* lastItem = findLastSelectableChild(topItem);
        if (lastItem) {
            m_dicomTree->setCurrentItem(lastItem);
            m_dicomTree->scrollToItem(lastItem);
            return;
        }
    }
}

bool DicomViewer::isSelectableItem(QTreeWidgetItem* item)
{
    if (!item) return false;
    
    QVariantList userData = item->data(0, Qt::UserRole).toList();
    if (userData.size() >= 2) {
        QString type = userData[0].toString();
        // Allow selection of image items and series items
        return (type == "image" || type == "series");
    }
    return false;
}

bool DicomViewer::isImageItem(QTreeWidgetItem* item)
{
    if (!item) return false;
    
    QVariantList userData = item->data(0, Qt::UserRole).toList();
    if (userData.size() >= 2) {
        QString type = userData[0].toString();
        // Only allow selection of actual image items (not series)
        return (type == "image");
    }
    return false;
}

QTreeWidgetItem* DicomViewer::findFirstSelectableChild(QTreeWidgetItem* parent)
{
    if (!parent) return nullptr;
    
    for (int i = 0; i < parent->childCount(); i++) {
        QTreeWidgetItem* child = parent->child(i);
        if (isSelectableItem(child)) {
            return child;
        }
        // Recursively check children
        QTreeWidgetItem* grandchild = findFirstSelectableChild(child);
        if (grandchild) return grandchild;
    }
    return nullptr;
}

QTreeWidgetItem* DicomViewer::findFirstImageChild(QTreeWidgetItem* parent)
{
    if (!parent) return nullptr;
    
    for (int i = 0; i < parent->childCount(); i++) {
        QTreeWidgetItem* child = parent->child(i);
        if (isImageItem(child)) {
            return child;
        }
        // Recursively check children for image items
        QTreeWidgetItem* grandchild = findFirstImageChild(child);
        if (grandchild) return grandchild;
    }
    return nullptr;
}

QTreeWidgetItem* DicomViewer::findLastSelectableChild(QTreeWidgetItem* parent)
{
    if (!parent) return nullptr;
    
    for (int i = parent->childCount() - 1; i >= 0; i--) {
        QTreeWidgetItem* child = parent->child(i);
        // Check children first (deepest first)
        QTreeWidgetItem* grandchild = findLastSelectableChild(child);
        if (grandchild) return grandchild;
        
        if (isSelectableItem(child)) {
            return child;
        }
    }
    return nullptr;
}

void DicomViewer::performImageExport(const SaveImageDialog::ExportSettings& settings)
{
    try {
        // Create destination directory if it doesn't exist
        QDir dir;
        if (!dir.exists(settings.destination)) {
            if (!dir.mkpath(settings.destination)) {
                throw std::runtime_error("Failed to create destination directory");
            }
        }
        
        // Get the current displayed image through the pipeline
        QImage imageToSave = m_currentPixmap.toImage();
        
        // Create the full file path
        QString filename = settings.filename + ".jpg";
        QString filepath = QDir(settings.destination).absoluteFilePath(filename);
        
        // Save the image as JPEG with 90% quality
        bool success = imageToSave.save(filepath, "JPEG", settings.quality);
        
        if (!success) {
            throw std::runtime_error("Failed to save image file");
        }
        
        // Show success message
        QMessageBox msg;
        msg.setIcon(QMessageBox::Information);
        msg.setWindowTitle("Export Complete");
        msg.setText(QString("Image successfully exported!\n\nSaved to:\n%1").arg(filepath));
        msg.setStandardButtons(QMessageBox::Ok);
        
        // Apply dark theme styling
        msg.setStyleSheet(QString(
            "QMessageBox { background-color: #2b2b2b; color: #ffffff; border: 1px solid #555555; }"
            "QMessageBox QLabel { color: #ffffff; background: transparent; }"
            "QMessageBox QPushButton { background-color: #404040; color: #ffffff; border: 1px solid #666666; padding: 8px 16px; border-radius: 4px; }"
            "QMessageBox QPushButton:hover { background-color: #4a90e2; border-color: #4a90e2; }"
            "QMessageBox QPushButton:pressed { background-color: #357abd; }"
        ));
        msg.exec();
        
        // Open file explorer if requested
        if (settings.openExplorer) {
            QProcess::startDetached("explorer.exe", QStringList() << "/select," << QDir::toNativeSeparators(filepath));
        }
        
        
    } catch (const std::exception& e) {
        
        QMessageBox errorMsg;
        errorMsg.setIcon(QMessageBox::Critical);
        errorMsg.setWindowTitle("Export Failed");
        errorMsg.setText(QString("Failed to export image:\n%1").arg(e.what()));
        errorMsg.setStandardButtons(QMessageBox::Ok);
        errorMsg.setStyleSheet(QString(
            "QMessageBox { background-color: #2b2b2b; color: #ffffff; border: 1px solid #555555; }"
            "QMessageBox QLabel { color: #ffffff; background: transparent; }"
            "QMessageBox QPushButton { background-color: #404040; color: #ffffff; border: 1px solid #666666; padding: 8px 16px; border-radius: 4px; }"
        ));
        errorMsg.exec();
    }
}

void DicomViewer::performVideoExport(const SaveRunDialog::ExportSettings& settings)
{
    try {
        // Check if we have frames to export
        if (m_frameCache.isEmpty()) {
            throw std::runtime_error("No frames available for video export");
        }
        
        // Create destination directory if it doesn't exist
        QDir dir;
        if (!dir.exists(settings.destination)) {
            if (!dir.mkpath(settings.destination)) {
                throw std::runtime_error("Failed to create destination directory");
            }
        }
        
        // Create the full file path
        QString filename = settings.filename + ".mp4";
        QString filepath = QDir(settings.destination).absoluteFilePath(filename);
        
        
        // Create temporary directory for frames
        QString tempDir = QDir(settings.destination).absoluteFilePath("temp_frames_" + QString::number(QDateTime::currentSecsSinceEpoch()));
        if (!dir.exists(tempDir)) {
            if (!dir.mkpath(tempDir)) {
                throw std::runtime_error("Failed to create temporary directory for frames");
            }
        }
        
        // Export each frame as JPEG for FFmpeg processing
        QStringList frameFiles;
        int frameCount = 0;
        for (int i = 0; i < m_totalFrames; ++i) {
            if (m_frameCache.contains(i)) {
                // Get the frame and process it through the pipeline
                QPixmap originalFrame = m_frameCache[i];
                m_originalPixmap = originalFrame;
                
                // Process through pipeline to apply current transformations
                QImage frameImage = m_imagePipeline->processImage(originalFrame.toImage());
                
                // Save frame as JPEG with sequential numbering for video processing
                QString frameFilename = QString("frame_%1.jpg").arg(frameCount, 6, 10, QChar('0'));
                QString frameFilepath = QDir(tempDir).absoluteFilePath(frameFilename);
                
                // Save as JPEG with high quality (90%) for good video quality
                if (frameImage.save(frameFilepath, "JPEG", 90)) {
                    frameFiles.append(frameFilepath);
                    frameCount++;
                } else {
                }
            }
        }
        
        if (frameFiles.isEmpty()) {
            throw std::runtime_error("No frames were exported successfully");
        }
        
        // Try to create video using FFmpeg approach
        bool videoCreated = createMP4Video(tempDir, filepath, settings.framerate);
        
        if (videoCreated) {
            // Clean up temporary frames
            QDir(tempDir).removeRecursively();
            
            // Show success message
            QMessageBox msg;
            msg.setIcon(QMessageBox::Information);
            msg.setWindowTitle("Export Complete");
            msg.setText(QString("MP4 video saved to:\n%1").arg(filepath));
            msg.setStandardButtons(QMessageBox::Ok);
            
            // Apply dark theme styling
            msg.setStyleSheet(QString(
                "QMessageBox { background-color: #2b2b2b; color: #ffffff; border: 1px solid #555555; }"
                "QMessageBox QLabel { color: #ffffff; background: transparent; }"
                "QMessageBox QPushButton { background-color: #404040; color: #ffffff; border: 1px solid #666666; padding: 8px 16px; border-radius: 4px; }"
                "QMessageBox QPushButton:hover { background-color: #4a90e2; border-color: #4a90e2; }"
                "QMessageBox QPushButton:pressed { background-color: #357abd; }"
            ));
            msg.exec();
            
            // Open file explorer to the video file location
            if (settings.openExplorer) {
                QProcess::startDetached("explorer.exe", QStringList() << "/select," << QDir::toNativeSeparators(filepath));
            }
        } else {
            // Fallback: Show completion message with frame files
            QMessageBox msg;
            msg.setIcon(QMessageBox::Warning);
            msg.setWindowTitle("Frames Exported");
            msg.setText(QString("FFmpeg not found - JPEG frames exported instead!\n\n%1 frames saved to:\n%2\n\nTo create MP4 videos:\n1. Install FFmpeg from ffmpeg.org\n2. Add FFmpeg to your system PATH\n3. Re-export from the DICOM viewer\n\nAlternatively, use video editing software to combine frames at %3 FPS")
                       .arg(frameCount)
                       .arg(tempDir)
                       .arg(settings.framerate));
            msg.setStandardButtons(QMessageBox::Ok);
            
            // Apply dark theme styling
            msg.setStyleSheet(QString(
                "QMessageBox { background-color: #2b2b2b; color: #ffffff; border: 1px solid #555555; }"
                "QMessageBox QLabel { color: #ffffff; background: transparent; }"
                "QMessageBox QPushButton { background-color: #404040; color: #ffffff; border: 1px solid #666666; padding: 8px 16px; border-radius: 4px; }"
                "QMessageBox QPushButton:hover { background-color: #4a90e2; border-color: #4a90e2; }"
                "QMessageBox QPushButton:pressed { background-color: #357abd; }"
            ));
            msg.exec();
            
            // Open file explorer to the frames directory
            if (settings.openExplorer) {
                QProcess::startDetached("explorer.exe", QStringList() << QDir::toNativeSeparators(tempDir));
            }
        }
        
        
    } catch (const std::exception& e) {
        
        QMessageBox errorMsg;
        errorMsg.setIcon(QMessageBox::Critical);
        errorMsg.setWindowTitle("Export Failed");
        errorMsg.setText(QString("Failed to export video:\n%1").arg(e.what()));
        errorMsg.setStandardButtons(QMessageBox::Ok);
        errorMsg.setStyleSheet(QString(
            "QMessageBox { background-color: #2b2b2b; color: #ffffff; border: 1px solid #555555; }"
            "QMessageBox QLabel { color: #ffffff; background: transparent; }"
            "QMessageBox QPushButton { background-color: #404040; color: #ffffff; border: 1px solid #666666; padding: 8px 16px; border-radius: 4px; }"
        ));
        errorMsg.exec();
    }
}

bool DicomViewer::createMP4Video(const QString& frameDir, const QString& outputPath, int framerate)
{
    // Use FFmpeg to create MP4 video from JPEG frames
    
    logMessage("DEBUG", "Starting MP4 video creation");
    logMessage("DEBUG", QString("Frame directory: %1").arg(frameDir));
    logMessage("DEBUG", QString("Output path: %1").arg(outputPath));
    logMessage("DEBUG", QString("Framerate: %1").arg(framerate));
    
    // Find ffmpeg executable using our helper function
    QString ffmpegPath = findFfmpegExecutable();
    
    if (ffmpegPath.isEmpty()) {
        logMessage("ERROR", "FFmpeg executable not found in local directory or DVD drive");
        return false;
    }
    
    logMessage("DEBUG", QString("Using FFmpeg executable at: %1").arg(ffmpegPath));
    
    // Test if ffmpeg executable is working
    QProcess testProcess;
    testProcess.start(ffmpegPath, QStringList() << "-version");
    testProcess.waitForFinished(3000); // Wait up to 3 seconds
    
    if (testProcess.exitCode() != 0) {
        logMessage("ERROR", QString("FFmpeg executable test failed, exit code: %1").arg(testProcess.exitCode()));
        return false;
    }
    
    // Prepare FFmpeg command to create MP4 from JPEG frames
    QStringList arguments;
    arguments << "-framerate" << QString::number(framerate);
    arguments << "-i" << QDir(frameDir).absoluteFilePath("frame_%06d.jpg");
    arguments << "-c:v" << "libx264";           // H.264 codec (widely supported)
    arguments << "-pix_fmt" << "yuv420p";       // Pixel format for maximum compatibility
    arguments << "-crf" << "23";                // Constant Rate Factor (good quality: 18-28, 23 is balanced)
    arguments << "-preset" << "medium";         // Encoding speed vs compression ratio
    arguments << "-movflags" << "+faststart";   // Optimize for web playback
    arguments << "-y";                          // Overwrite output file if it exists
    arguments << outputPath;
    
    QString fullCommand = ffmpegPath + " " + arguments.join(" ");
    logMessage("DEBUG", QString("FFmpeg command: %1").arg(fullCommand));
    
    // Check if ffmpeg is running from DVD/CD (slower operation)
    bool isFromDVD = ffmpegPath.length() >= 2 && ffmpegPath.at(1) == ':' && 
                     QString("DEFGH").contains(ffmpegPath.at(0).toUpper());
    
    QProgressDialog* progressDialog = nullptr;
    
    if (isFromDVD) {
        // Show progress dialog for DVD operations
        progressDialog = new QProgressDialog("Creating MP4 video...\nThis may take a while when running from DVD/CD.", 
                                           "Cancel", 0, 0, this);
        progressDialog->setWindowTitle("Video Creation Progress");
        progressDialog->setWindowModality(Qt::WindowModal);
        progressDialog->setMinimumDuration(1000); // Show after 1 second
        progressDialog->setValue(0);
        progressDialog->show();
        
        // Apply dark theme styling
        progressDialog->setStyleSheet(QString(
            "QProgressDialog { background-color: #2b2b2b; color: #ffffff; border: 1px solid #555555; }"
            "QProgressDialog QLabel { color: #ffffff; background: transparent; }"
            "QProgressDialog QPushButton { background-color: #404040; color: #ffffff; border: 1px solid #666666; padding: 8px 16px; border-radius: 4px; }"
            "QProgressDialog QPushButton:hover { background-color: #4a90e2; border-color: #4a90e2; }"
            "QProgressDialog QProgressBar { background-color: #404040; border: 1px solid #666666; border-radius: 3px; }"
            "QProgressBar::chunk { background-color: #4a90e2; }"
        ));
        
        QApplication::processEvents();
    }
    
    // Execute FFmpeg process
    QProcess ffmpegProcess;
    ffmpegProcess.start(ffmpegPath, arguments);
    
    // Monitor progress for DVD operations
    const int timeout = isFromDVD ? 120000 : 60000; // 2 minutes for DVD, 1 minute for local
    const int checkInterval = 1000; // Check every second
    int elapsed = 0;
    
    while (ffmpegProcess.state() == QProcess::Running && elapsed < timeout) {
        if (progressDialog && progressDialog->wasCanceled()) {
            logMessage("INFO", "User canceled video creation");
            ffmpegProcess.kill();
            progressDialog->deleteLater();
            return false;
        }
        
        QApplication::processEvents();
        QThread::msleep(checkInterval);
        elapsed += checkInterval;
        
        if (progressDialog) {
            // Update progress dialog text with elapsed time
            int seconds = elapsed / 1000;
            progressDialog->setLabelText(QString("Creating MP4 video...\nElapsed time: %1 seconds\nThis may take a while when running from DVD/CD.")
                                       .arg(seconds));
        }
    }
    
    if (progressDialog) {
        progressDialog->deleteLater();
    }
    
    // Check if process is still running (timed out)
    if (ffmpegProcess.state() == QProcess::Running) {
        logMessage("ERROR", "FFmpeg process timed out after " + QString::number(timeout/1000) + " seconds");
        ffmpegProcess.kill();
        ffmpegProcess.waitForFinished(5000);
        return false;
    }
    
    // Check if the process completed successfully
    if (ffmpegProcess.exitCode() != 0) {
        QString errorOutput = QString::fromUtf8(ffmpegProcess.readAllStandardError());
        logMessage("ERROR", "FFmpeg process failed with exit code: " + QString::number(ffmpegProcess.exitCode()));
        logMessage("ERROR", "FFmpeg stderr: " + errorOutput);
        return false;
    }
    
    // Verify that the output file was created
    if (!QFile::exists(outputPath)) {
        logMessage("ERROR", "FFmpeg completed but output file not found: " + outputPath);
        return false;
    }
    
    logMessage("DEBUG", QString("FFmpeg video creation successful: %1").arg(outputPath));
    return true;
}

// ========== RDSR (Radiation Dose Structured Report) IMPLEMENTATION ==========

void DicomViewer::displayReport(const QString& filePath)
{
    if (!m_reportArea) {
        return;
    }
    
    QString reportContent = formatSRReport(filePath);
    m_reportArea->setHtml(reportContent);
}

QString DicomViewer::formatSRReport(const QString& filePath)
{
    #ifdef HAVE_DCMTK
    try {
        // First check if the file path is valid
        QFileInfo fileInfo(filePath);
        if (!fileInfo.exists()) {
            return QString("<!DOCTYPE html><html><head><style>.error { color: #e74c3c; font-weight: bold; background-color: #fdf2f2; padding: 15px; border: 1px solid #fadbd8; font-family: Arial, sans-serif; }</style></head><body><div class='error'>Error: File does not exist<br><br>File: %1<br>Check if the file path is correct.</div></body></html>")
                   .arg(filePath);
        }
        
        if (fileInfo.isDir()) {
            return QString("<!DOCTYPE html><html><head><style>.error { color: #e74c3c; font-weight: bold; background-color: #fdf2f2; padding: 15px; border: 1px solid #fadbd8; font-family: Arial, sans-serif; }</style></head><body><div class='error'>Error: Path is a directory, not a file<br><br>Path: %1<br>SR documents must be individual DICOM files.</div></body></html>")
                   .arg(filePath);
        }
        
        DcmFileFormat fileformat;
        OFCondition result = fileformat.loadFile(filePath.toLocal8Bit().constData());
        
        if (result.bad()) {
            return QString("<!DOCTYPE html><html><head><style>.error { color: #e74c3c; font-weight: bold; background-color: #fdf2f2; padding: 15px; border: 1px solid #fadbd8; font-family: Arial, sans-serif; }</style></head><body><div class='error'>Error: Could not load DICOM file<br><br>File: %1<br>Error: %2<br>File size: %3 bytes</div></body></html>")
                   .arg(filePath, result.text())
                   .arg(fileInfo.size());
        }
        
        DcmDataset *dataset = fileformat.getDataset();
        if (!dataset) {
            return QString("<!DOCTYPE html><html><head><style>.error { color: #e74c3c; font-weight: bold; background-color: #fdf2f2; padding: 15px; border: 1px solid #fadbd8; font-family: Arial, sans-serif; }</style></head><body><div class='error'>Error: No dataset found in DICOM file<br><br>File: %1</div></body></html>").arg(filePath);
        }
        
        // Check if it's a Radiation Dose SR
        if (isRadiationDoseSR(filePath)) {
            return formatRadiationDoseReport(filePath);
        }
        
        // For other SR types, show basic info with HTML formatting
        QString report;
        
        // Start HTML document with professional styling
        report += "<!DOCTYPE html><html><head><style>";
        report += "body { font-family: 'Segoe UI', Tahoma, Arial, sans-serif; font-size: 11pt; line-height: 1.4; margin: 20px; background-color: #fdfdfd; }";
        report += "h1 { font-size: 18pt; font-weight: bold; color: #2c3e50; text-align: center; margin: 20px 0; border-bottom: 3px solid #3498db; padding-bottom: 10px; }";
        report += "h2 { font-size: 14pt; font-weight: bold; color: #34495e; margin: 20px 0 10px 0; border-left: 4px solid #3498db; padding-left: 10px; background-color: #f8f9fa; padding: 8px; }";
        report += ".info-row { margin: 8px 0; padding: 4px 0; border-bottom: 1px dotted #ddd; }";
        report += ".label { font-weight: bold; color: #2c3e50; display: inline-block; min-width: 150px; }";
        report += ".value { color: #34495e; }";
        report += ".warning { color: #f39c12; font-weight: bold; background-color: #fef9e7; padding: 10px; border: 1px solid #fcf3cf; margin: 10px 0; }";
        report += "</style></head><body>";
        
        report += "<h1>Structured Report Document</h1>";
        report += "<h2>Basic Information</h2>";
        report += QString("<div class='info-row'><span class='label'>File Path:</span> <span class='value'>%1</span></div>").arg(filePath);
        
        // Try to get basic SR info
        OFString sopClassUID, modality, conceptNameCodeMeaning;
        if (dataset->findAndGetOFString(DCM_SOPClassUID, sopClassUID).good()) {
            report += QString("<div class='info-row'><span class='label'>SOP Class UID:</span> <span class='value'>%1</span></div>").arg(sopClassUID.c_str());
        }
        if (dataset->findAndGetOFString(DCM_Modality, modality).good()) {
            report += QString("<div class='info-row'><span class='label'>Modality:</span> <span class='value'>%1</span></div>").arg(modality.c_str());
        }
        
        report += "<div class='warning'>";
        report += "<strong>Note:</strong> This structured report type is not fully supported for detailed formatting.<br>";
        report += "Please use a dedicated DICOM viewer for complete SR analysis.";
        report += "</div>";
        
        report += "</body></html>";
        
        return report;
        
    } catch (const std::exception& e) {
        return QString("Error: Exception occurred while reading DICOM file\n\nFile: %1\nError: %2")
               .arg(filePath, e.what());
    }
    #else
    return QString("Error: DCMTK support not available\n\nFile: %1\n\nDCMTK library is required for DICOM file reading.")
           .arg(filePath);
    #endif
}

bool DicomViewer::isRadiationDoseSR(const QString& filePath)
{
    #ifdef HAVE_DCMTK
    try {
        DcmFileFormat fileformat;
        OFCondition result = fileformat.loadFile(filePath.toLocal8Bit().constData());
        
        if (result.bad()) {
            return false;
        }
        
        DcmDataset *dataset = fileformat.getDataset();
        if (!dataset) {
            return false;
        }
        
        // Check SOP Class UID for X-Ray Radiation Dose SR
        OFString sopClassUID;
        if (dataset->findAndGetOFString(DCM_SOPClassUID, sopClassUID).good()) {
            // X-Ray Radiation Dose SR SOP Class UID
            if (sopClassUID == "1.2.840.10008.5.1.4.1.1.88.67") {
                return true;
            }
        }
        
        // Also check concept name code meaning in content sequence
        DcmSequenceOfItems *contentSeq = nullptr;
        if (dataset->findAndGetSequence(DCM_ContentSequence, contentSeq).good() && contentSeq) {
            for (unsigned long i = 0; i < contentSeq->card(); i++) {
                DcmItem *item = contentSeq->getItem(i);
                if (item) {
                    DcmSequenceOfItems *conceptNameSeq = nullptr;
                    if (item->findAndGetSequence(DCM_ConceptNameCodeSequence, conceptNameSeq).good() && conceptNameSeq && conceptNameSeq->card() > 0) {
                        DcmItem *conceptItem = conceptNameSeq->getItem(0);
                        if (conceptItem) {
                            OFString codeMeaning;
                            if (conceptItem->findAndGetOFString(DCM_CodeMeaning, codeMeaning).good()) {
                                if (codeMeaning.find("Dose Report") != OFString_npos ||
                                    codeMeaning.find("Radiation Dose") != OFString_npos ||
                                    codeMeaning.find("X-Ray Dose") != OFString_npos) {
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }
        
        return false;
        
    } catch (const std::exception& e) {
        return false;
    }
    #else
    return false;
    #endif
}

QString DicomViewer::formatRadiationDoseReport(const QString& filePath)
{
    QString report;
    
    // Start HTML document with professional styling
    report += "<!DOCTYPE html><html><head><style>";
    report += "body { font-family: 'Segoe UI', Tahoma, Arial, sans-serif; font-size: 11pt; line-height: 1.4; margin: 20px; background-color: #fdfdfd; }";
    report += "h1 { font-size: 18pt; font-weight: bold; color: #2c3e50; text-align: center; margin: 20px 0; border-bottom: 3px solid #3498db; padding-bottom: 10px; }";
    report += "h2 { font-size: 14pt; font-weight: bold; color: #34495e; margin: 20px 0 10px 0; border-left: 4px solid #3498db; padding-left: 10px; background-color: #f8f9fa; padding: 8px; }";
    report += "h3 { font-size: 12pt; font-weight: bold; color: #2c3e50; margin: 15px 0 8px 0; }";
    report += ".info-row { margin: 8px 0; padding: 4px 0; border-bottom: 1px dotted #ddd; }";
    report += ".label { font-weight: bold; color: #2c3e50; display: inline-block; min-width: 150px; }";
    report += ".value { color: #34495e; }";
    report += ".event { background-color: #f8f9fa; margin: 10px 0; padding: 15px; border-left: 3px solid #e74c3c; }";
    report += ".measurement { margin: 5px 0 5px 20px; }";
    report += ".error { color: #e74c3c; font-weight: bold; background-color: #fdf2f2; padding: 10px; border: 1px solid #fadbd8; }";
    report += "</style></head><body>";
    
    // Main Header
    report += "<h1>RADIATION DOSE STRUCTURED REPORT</h1>";
    
    // Add sections
    report += formatRDSRHeader(filePath);
    report += formatRDSRProcedureInfo(filePath);
    report += formatAccumulatedDoseData(filePath);
    report += formatIrradiationEvents(filePath);
    
    // Close HTML
    report += "</body></html>";
    
    return report;
}

QString DicomViewer::formatRDSRHeader(const QString& filePath)
{
    #ifdef HAVE_DCMTK
    try {
        DcmFileFormat fileformat;
        OFCondition result = fileformat.loadFile(filePath.toLocal8Bit().constData());
        
        if (result.bad()) {
            return "Error: Could not load DICOM file for header information";
        }
        
        DcmDataset *dataset = fileformat.getDataset();
        if (!dataset) {
            return "Error: No dataset found";
        }
        
        QString header;
        header += "<h2>Patient & Study Information</h2>";
        
        // Patient Information
        OFString patientName, patientID, patientSex, patientAge;
        if (dataset->findAndGetOFString(DCM_PatientName, patientName).good()) {
            QString cleanName = QString(patientName.c_str()).replace("^", " ").trimmed();
            header += QString("<div class='info-row'><span class='label'>Patient Name:</span> <span class='value'>%1</span></div>").arg(cleanName);
        }
        if (dataset->findAndGetOFString(DCM_PatientID, patientID).good()) {
            header += QString("<div class='info-row'><span class='label'>Patient ID:</span> <span class='value'>%1</span></div>").arg(patientID.c_str());
        }
        if (dataset->findAndGetOFString(DCM_PatientSex, patientSex).good()) {
            header += QString("<div class='info-row'><span class='label'>Patient Sex:</span> <span class='value'>%1</span></div>").arg(patientSex.c_str());
        }
        if (dataset->findAndGetOFString(DCM_PatientAge, patientAge).good()) {
            header += QString("<div class='info-row'><span class='label'>Patient Age:</span> <span class='value'>%1</span></div>").arg(patientAge.c_str());
        }
        
        // Study Information
        OFString studyDate, studyTime, studyDescription, institutionName;
        if (dataset->findAndGetOFString(DCM_StudyDate, studyDate).good()) {
            header += QString("<div class='info-row'><span class='label'>Study Date:</span> <span class='value'>%1</span></div>").arg(formatDateTime(studyDate.c_str()));
        }
        if (dataset->findAndGetOFString(DCM_StudyTime, studyTime).good()) {
            header += QString("<div class='info-row'><span class='label'>Study Time:</span> <span class='value'>%1</span></div>").arg(formatDateTime(studyTime.c_str()));
        }
        if (dataset->findAndGetOFString(DCM_StudyDescription, studyDescription).good()) {
            header += QString("<div class='info-row'><span class='label'>Study Description:</span> <span class='value'>%1</span></div>").arg(studyDescription.c_str());
        }
        if (dataset->findAndGetOFString(DCM_InstitutionName, institutionName).good()) {
            header += QString("<div class='info-row'><span class='label'>Institution:</span> <span class='value'>%1</span></div>").arg(institutionName.c_str());
        }
        
        header += "<h3>Device Information</h3>";
        
        // Device Information
        OFString manufacturer, manufacturerModelName, deviceSerialNumber;
        if (dataset->findAndGetOFString(DCM_Manufacturer, manufacturer).good()) {
            header += QString("<div class='info-row'><span class='label'>Manufacturer:</span> <span class='value'>%1</span></div>").arg(manufacturer.c_str());
        }
        if (dataset->findAndGetOFString(DCM_ManufacturerModelName, manufacturerModelName).good()) {
            header += QString("<div class='info-row'><span class='label'>Device Model:</span> <span class='value'>%1</span></div>").arg(manufacturerModelName.c_str());
        }
        if (dataset->findAndGetOFString(DCM_DeviceSerialNumber, deviceSerialNumber).good()) {
            header += QString("<div class='info-row'><span class='label'>Serial Number:</span> <span class='value'>%1</span></div>").arg(deviceSerialNumber.c_str());
        }
        
        return header;
        
    } catch (const std::exception& e) {
        return QString("<div class='error'>Error reading header: %1</div>").arg(e.what());
    }
    #else
    return "Error: DCMTK support not available";
    #endif
}

QString DicomViewer::formatRDSRProcedureInfo(const QString& filePath)
{
    #ifdef HAVE_DCMTK
    try {
        DcmFileFormat fileformat;
        OFCondition result = fileformat.loadFile(filePath.toLocal8Bit().constData());
        
        if (result.bad()) {
            return "Error: Could not load DICOM file for procedure information";
        }
        
        DcmDataset *dataset = fileformat.getDataset();
        if (!dataset) {
            return "Error: No dataset found";
        }
        
        QString procedure;
        procedure += "<h2>Procedure Information</h2>";
        
        // Basic procedure info
        OFString studyDescription, seriesDescription, protocolName;
        if (dataset->findAndGetOFString(DCM_StudyDescription, studyDescription).good()) {
            procedure += QString("<div class='info-row'><span class='label'>Procedure:</span> <span class='value'>%1</span></div>").arg(studyDescription.c_str());
        }
        if (dataset->findAndGetOFString(DCM_SeriesDescription, seriesDescription).good()) {
            procedure += QString("<div class='info-row'><span class='label'>Series Description:</span> <span class='value'>%1</span></div>").arg(seriesDescription.c_str());
        }
        if (dataset->findAndGetOFString(DCM_ProtocolName, protocolName).good()) {
            procedure += QString("<div class='info-row'><span class='label'>Protocol:</span> <span class='value'>%1</span></div>").arg(protocolName.c_str());
        }
        
        // Try to extract procedure info from content sequence
        DcmSequenceOfItems *contentSeq = nullptr;
        if (dataset->findAndGetSequence(DCM_ContentSequence, contentSeq).good() && contentSeq) {
            for (unsigned long i = 0; i < contentSeq->card(); i++) {
                DcmItem *item = contentSeq->getItem(i);
                if (item) {
                    // Look for procedure-related information in the structured content
                    DcmSequenceOfItems *conceptNameSeq = nullptr;
                    if (item->findAndGetSequence(DCM_ConceptNameCodeSequence, conceptNameSeq).good() && 
                        conceptNameSeq && conceptNameSeq->card() > 0) {
                        DcmItem *conceptItem = conceptNameSeq->getItem(0);
                        if (conceptItem) {
                            OFString codeMeaning;
                            if (conceptItem->findAndGetOFString(DCM_CodeMeaning, codeMeaning).good()) {
                                // Look for procedure-specific information
                                if (codeMeaning.find("Procedure") != OFString_npos ||
                                    codeMeaning.find("Protocol") != OFString_npos) {
                                    
                                    OFString textValue;
                                    if (item->findAndGetOFString(DCM_TextValue, textValue).good()) {
                                        procedure += QString("Protocol Detail: %1\n").arg(textValue.c_str());
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        
        if (procedure.endsWith(QString("-").repeated(30) + "\n")) {
            procedure += "No detailed procedure information available\n";
        }
        
        return procedure;
        
    } catch (const std::exception& e) {
        return QString("Error reading procedure info: %1").arg(e.what());
    }
    #else
    return "Error: DCMTK support not available";
    #endif
}

QString DicomViewer::formatAccumulatedDoseData(const QString& filePath)
{
    #ifdef HAVE_DCMTK
    try {
        DcmFileFormat fileformat;
        OFCondition result = fileformat.loadFile(filePath.toLocal8Bit().constData());
        
        if (result.bad()) {
            return "Error: Could not load DICOM file for dose data";
        }
        
        DcmDataset *dataset = fileformat.getDataset();
        if (!dataset) {
            return "Error: No dataset found";
        }
        
        QString doseData;
        doseData += "<h2>Accumulated Dose Data</h2>";
        
        // Look for dose measurements in content sequence
        DcmSequenceOfItems *contentSeq = nullptr;
        if (dataset->findAndGetSequence(DCM_ContentSequence, contentSeq).good() && contentSeq) {
            
            QStringList foundDoseValues;
            
            for (unsigned long i = 0; i < contentSeq->card(); i++) {
                DcmItem *item = contentSeq->getItem(i);
                if (item) {
                    DcmSequenceOfItems *conceptNameSeq = nullptr;
                    if (item->findAndGetSequence(DCM_ConceptNameCodeSequence, conceptNameSeq).good() && 
                        conceptNameSeq && conceptNameSeq->card() > 0) {
                        
                        DcmItem *conceptItem = conceptNameSeq->getItem(0);
                        if (conceptItem) {
                            OFString codeMeaning;
                            if (conceptItem->findAndGetOFString(DCM_CodeMeaning, codeMeaning).good()) {
                                
                                // Look for various dose measurements
                                QString conceptStr = QString(codeMeaning.c_str());
                                
                                if (conceptStr.contains("Dose", Qt::CaseInsensitive) ||
                                    conceptStr.contains("DAP", Qt::CaseInsensitive) ||
                                    conceptStr.contains("Air Kerma", Qt::CaseInsensitive) ||
                                    conceptStr.contains("Exposure", Qt::CaseInsensitive)) {
                                    
                                    // Get the measured value
                                    DcmSequenceOfItems *measuredValueSeq = nullptr;
                                    if (item->findAndGetSequence(DCM_MeasuredValueSequence, measuredValueSeq).good() && 
                                        measuredValueSeq && measuredValueSeq->card() > 0) {
                                        
                                        DcmItem *measuredItem = measuredValueSeq->getItem(0);
                                        if (measuredItem) {
                                            OFString numericValue;
                                            if (measuredItem->findAndGetOFString(DCM_NumericValue, numericValue).good()) {
                                                
                                                // Get units if available
                                                DcmSequenceOfItems *measurementUnitsSeq = nullptr;
                                                QString unit = "";
                                                if (measuredItem->findAndGetSequence(DCM_MeasurementUnitsCodeSequence, measurementUnitsSeq).good() && 
                                                    measurementUnitsSeq && measurementUnitsSeq->card() > 0) {
                                                    
                                                    DcmItem *unitItem = measurementUnitsSeq->getItem(0);
                                                    if (unitItem) {
                                                        OFString unitMeaning;
                                                        if (unitItem->findAndGetOFString(DCM_CodeMeaning, unitMeaning).good()) {
                                                            unit = QString(unitMeaning.c_str());
                                                        }
                                                    }
                                                }
                                                
                                                QString measurement = formatMeasurement(conceptStr, QString(numericValue.c_str()), unit);
                                                if (!foundDoseValues.contains(measurement)) {
                                                    foundDoseValues.append(measurement);
                                                    doseData += measurement + "\n";
                                                }
                                            }
                                        }
                                    }
                                    
                                    // Also check for simple text values
                                    OFString textValue;
                                    if (item->findAndGetOFString(DCM_TextValue, textValue).good()) {
                                        QString measurement = formatMeasurement(conceptStr, QString(textValue.c_str()));
                                        if (!foundDoseValues.contains(measurement)) {
                                            foundDoseValues.append(measurement);
                                            doseData += measurement + "\n";
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            
            if (foundDoseValues.isEmpty()) {
                doseData += "No dose measurements found in structured report\n";
            }
        } else {
            doseData += "No content sequence found\n";
        }
        
        return doseData;
        
    } catch (const std::exception& e) {
        return QString("Error reading dose data: %1").arg(e.what());
    }
    #else
    return "Error: DCMTK support not available";
    #endif
}

QString DicomViewer::formatIrradiationEvents(const QString& filePath)
{
    #ifdef HAVE_DCMTK
    try {
        DcmFileFormat fileformat;
        OFCondition result = fileformat.loadFile(filePath.toLocal8Bit().constData());
        
        if (result.bad()) {
            return "Error: Could not load DICOM file for irradiation events";
        }
        
        DcmDataset *dataset = fileformat.getDataset();
        if (!dataset) {
            return "Error: No dataset found";
        }
        
        QString events;
        events += "<h2>Irradiation Events</h2>";
        
        // Look for irradiation event data in content sequence
        DcmSequenceOfItems *contentSeq = nullptr;
        if (dataset->findAndGetSequence(DCM_ContentSequence, contentSeq).good() && contentSeq) {
            
            int eventCount = 0;
            
            for (unsigned long i = 0; i < contentSeq->card(); i++) {
                DcmItem *item = contentSeq->getItem(i);
                if (item) {
                    DcmSequenceOfItems *conceptNameSeq = nullptr;
                    if (item->findAndGetSequence(DCM_ConceptNameCodeSequence, conceptNameSeq).good() && 
                        conceptNameSeq && conceptNameSeq->card() > 0) {
                        
                        DcmItem *conceptItem = conceptNameSeq->getItem(0);
                        if (conceptItem) {
                            OFString codeMeaning;
                            if (conceptItem->findAndGetOFString(DCM_CodeMeaning, codeMeaning).good()) {
                                
                                QString conceptStr = QString(codeMeaning.c_str());
                                
                                // Look for irradiation events
                                if (conceptStr.contains("Irradiation Event", Qt::CaseInsensitive) ||
                                    conceptStr.contains("Radiation Event", Qt::CaseInsensitive) ||
                                    conceptStr.contains("Exposure Event", Qt::CaseInsensitive)) {
                                    
                                    eventCount++;
                                    events += QString("<div class='event'><h3>Event %1</h3>").arg(eventCount);
                                    
                                    // Look for child sequences containing event details
                                    DcmSequenceOfItems *childContentSeq = nullptr;
                                    if (item->findAndGetSequence(DCM_ContentSequence, childContentSeq).good() && childContentSeq) {
                                        
                                        for (unsigned long j = 0; j < childContentSeq->card(); j++) {
                                            DcmItem *childItem = childContentSeq->getItem(j);
                                            if (childItem) {
                                                DcmSequenceOfItems *childConceptSeq = nullptr;
                                                if (childItem->findAndGetSequence(DCM_ConceptNameCodeSequence, childConceptSeq).good() && 
                                                    childConceptSeq && childConceptSeq->card() > 0) {
                                                    
                                                    DcmItem *childConceptItem = childConceptSeq->getItem(0);
                                                    if (childConceptItem) {
                                                        OFString childCodeMeaning;
                                                        if (childConceptItem->findAndGetOFString(DCM_CodeMeaning, childCodeMeaning).good()) {
                                                            
                                                            QString childConceptStr = QString(childCodeMeaning.c_str());
                                                            
                                                            // Get value for this concept
                                                            OFString textValue;
                                                            if (childItem->findAndGetOFString(DCM_TextValue, textValue).good()) {
                                                                events += formatMeasurement(childConceptStr, QString(textValue.c_str()), QString(), 2) + "\n";
                                                            } else {
                                                                // Check for measured values
                                                                DcmSequenceOfItems *childMeasuredSeq = nullptr;
                                                                if (childItem->findAndGetSequence(DCM_MeasuredValueSequence, childMeasuredSeq).good() && 
                                                                    childMeasuredSeq && childMeasuredSeq->card() > 0) {
                                                                    
                                                                    DcmItem *childMeasuredItem = childMeasuredSeq->getItem(0);
                                                                    if (childMeasuredItem) {
                                                                        OFString numericValue;
                                                                        if (childMeasuredItem->findAndGetOFString(DCM_NumericValue, numericValue).good()) {
                                                                            
                                                                            QString unit = "";
                                                                            DcmSequenceOfItems *unitSeq = nullptr;
                                                                            if (childMeasuredItem->findAndGetSequence(DCM_MeasurementUnitsCodeSequence, unitSeq).good() && 
                                                                                unitSeq && unitSeq->card() > 0) {
                                                                                DcmItem *unitItem = unitSeq->getItem(0);
                                                                                if (unitItem) {
                                                                                    OFString unitMeaning;
                                                                                    if (unitItem->findAndGetOFString(DCM_CodeMeaning, unitMeaning).good()) {
                                                                                        unit = QString(unitMeaning.c_str());
                                                                                    }
                                                                                }
                                                                            }
                                                                            
                                                                            events += formatMeasurement(childConceptStr, QString(numericValue.c_str()), unit, 2) + "\n";
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    
                                    events += "</div>";
                                }
                            }
                        }
                    }
                }
            }
            
            if (eventCount == 0) {
                events += "<div class='info-row'>No irradiation events found in structured report</div>";
            }
        } else {
            events += "<div class='info-row'>No content sequence found</div>";
        }
        
        return events;
        
    } catch (const std::exception& e) {
        return QString("<div class='error'>Error reading irradiation events: %1</div>").arg(e.what());
    }
    #else
    return "Error: DCMTK support not available";
    #endif
}

QString DicomViewer::formatDateTime(const QString& dtString)
{
    if (dtString.length() >= 8) {
        QString year = dtString.mid(0, 4);
        QString month = dtString.mid(4, 2);
        QString day = dtString.mid(6, 2);
        
        QString formatted = QString("%1-%2-%3").arg(day, month, year);
        
        if (dtString.length() >= 14) {
            QString hour = dtString.mid(8, 2);
            QString minute = dtString.mid(10, 2);
            QString second = dtString.mid(12, 2);
            formatted += QString(" %1:%2:%3").arg(hour, minute, second);
        }
        
        return formatted;
    }
    
    return dtString;
}

QString DicomViewer::formatMeasurement(const QString& name, const QString& value, const QString& unit, int indent)
{
    Q_UNUSED(indent); // Not needed for HTML formatting
    
    QString result = QString("<div class='measurement'><span class='label'>%1:</span> <span class='value'>%2").arg(name, value);
    
    if (!unit.isEmpty() && unit != value) {
        QString cleanUnit = unit;
        // Fix degree symbol encoding issues
        if (cleanUnit.contains("?") || cleanUnit.contains("?") || cleanUnit.toLower().contains("degree")) {
            cleanUnit = "degrees";
        }
        result += " " + cleanUnit;
    }
    
    result += "</span></div>";
    return result;
}

QString DicomViewer::getCodeSequenceValue(const QString& filePath, const QString& tagPath)
{
    // This is a placeholder for more complex code sequence navigation
    // In a full implementation, this would navigate through nested sequences
    // based on the tagPath parameter
    return QString();
}

QString DicomViewer::extractDoseValue(const QString& filePath, const QString& conceptName)
{
    // This is a placeholder for extracting specific dose values
    // In a full implementation, this would search for specific concept names
    // and return their associated values
    return QString();
}

QString DicomViewer::extractEventData(const QString& filePath, int eventIndex)
{
    // This is a placeholder for extracting specific event data
    // In a full implementation, this would extract data for a specific
    // irradiation event by index
    return QString();
}

QString DicomViewer::formatRadiationEvent(const QString& eventData, int eventNum)
{
    // This is a placeholder for formatting individual radiation events
    // In a full implementation, this would parse and format the event data
    return QString("Event %1: %2").arg(eventNum).arg(eventData);
}

QString DicomViewer::formatFilterInfo(const QString& filterData, int indent)
{
    // This is a placeholder for formatting filter information
    // In a full implementation, this would parse and format filter details
    QString indentStr = QString(" ").repeated(indent * 2);
    return indentStr + "Filter: " + filterData;
}



void DicomViewer::createStatusBar()
{
    // Create status bar
    m_statusBar = statusBar(); // QMainWindow provides this
    m_statusBar->setStyleSheet(R"(
        QStatusBar {
            background-color: #2a2a2a;
            color: white;
            border-top: 1px solid #555555;
        }
        QStatusBar::item {
            border: none;
        }
    )");
    
    // Create status label for messages
    m_statusLabel = new QLabel("Ready");
    m_statusLabel->setStyleSheet("color: white; padding: 2px;");
    m_statusBar->addWidget(m_statusLabel, 1); // Stretch factor 1
    
    // Create progress bar for copy operations (initially hidden)
    m_statusProgressBar = new QProgressBar();
    m_statusProgressBar->setRange(0, 100);
    m_statusProgressBar->setVisible(false);
    m_statusProgressBar->setFixedWidth(200);
    m_statusProgressBar->setStyleSheet(R"(
        QProgressBar {
            border: 1px solid #555555;
            border-radius: 3px;
            text-align: center;
            color: white;
            background-color: #333333;
        }
        QProgressBar::chunk {
            background-color: #0078d7;
            border-radius: 2px;
        }
    )");
    
    m_statusBar->addPermanentWidget(m_statusProgressBar);
}

void DicomViewer::updateStatusBar(const QString& message, int progress)
{
    if (m_statusLabel) {
        m_statusLabel->setText(message);
    }
    
    if (m_statusProgressBar) {
        if (progress >= 0) {
            m_statusProgressBar->setValue(progress);
            m_statusProgressBar->setVisible(true);
        } else {
            m_statusProgressBar->setVisible(false);
        }
    }
}

void DicomViewer::toggleDicomInfo()
{
    logMessage("DEBUG", "[DICOM INFO] toggleDicomInfo() called");
    
    if (!m_dicomInfoWidget) {
        logMessage("ERROR", "[DICOM INFO] ERROR: m_dicomInfoWidget is null!");
        return; // Widget should always exist now that it's created in layout
    }
    
    m_dicomInfoVisible = !m_dicomInfoVisible;
    logMessage("DEBUG", QString("[DICOM INFO] Toggled to visible: %1").arg(m_dicomInfoVisible));
    
    if (m_dicomInfoVisible) {
        // Show the panel (it's now inline in the layout like Python version)
        // Populate with current image info if available
        if (!m_currentImagePath.isEmpty()) {
            populateDicomInfo(m_currentImagePath);
        } else {
            // Show default message if no image is loaded
            if (m_dicomInfoTextEdit) {
                m_dicomInfoTextEdit->setHtml("<div style='text-align: center; padding: 20px; color: white; background-color: #2a2a2a;'>"
                                           "<h3>DICOM Tags</h3><p>No image loaded. Please select a DICOM image to view its tags.</p></div>");
            }
        }
        
        m_dicomInfoWidget->show();
        logMessage("DEBUG", QString("[DICOM INFO] Widget should now be visible. IsVisible: %1").arg(m_dicomInfoWidget->isVisible()));
    } else {
        m_dicomInfoWidget->hide();
        logMessage("DEBUG", "[DICOM INFO] Widget hidden");
    }
}

void DicomViewer::populateDicomInfo(const QString& filePath)
{
    if (!m_dicomInfoWidget || !m_dicomInfoTextEdit || filePath.isEmpty()) {
        return;
    }
    
    // Check if we already have cached HTML for this file
    if (m_cachedDicomInfoFilePath == filePath && !m_cachedDicomInfoHtml.isEmpty()) {
        m_dicomInfoTextEdit->setHtml(m_cachedDicomInfoHtml);
        return;
    }
    
#ifdef HAVE_DCMTK
    try {
        DcmFileFormat fileFormat;
        OFCondition status = fileFormat.loadFile(filePath.toLocal8Bit().constData());
        
        if (status.bad()) {
            m_dicomInfoTextEdit->setPlainText(QString("Error reading DICOM file: %1").arg(status.text()));
            return;
        }
        
        DcmDataset* dataset = fileFormat.getDataset();
        if (!dataset) {
            m_dicomInfoTextEdit->setPlainText("Error: No dataset found in DICOM file");
            return;
        }
        
        // Helper lambda to get VR-based color like Python version
        auto getVRColor = [](const QString& vr) -> QString {
            if (vr == "SQ") {
                return "#6496FF";  // Sequences in blue (100, 150, 255)
            } else if (vr == "UI" || vr == "SH" || vr == "LO" || vr == "ST" || vr == "LT" || vr == "UT" || vr == "CS" || vr == "PN") {
                return "#96FF96";  // String types in light green (150, 255, 150) 
            } else if (vr == "US" || vr == "SS" || vr == "UL" || vr == "SL" || vr == "FL" || vr == "FD" || vr == "DS" || vr == "IS") {
                return "#FFFF96";  // Numeric types in yellow (255, 255, 150)
            } else {
                return "#FFFFFF";  // Default white (255, 255, 255)
            }
        };
        
        // Helper lambda to create DICOM tag row with VR-based coloring like Python
        auto formatDicomTag = [&](DcmElement* elem) -> QString {
            DcmTag tag = elem->getTag();
            QString tagStr = QString("(%1,%2)").arg(tag.getGTag(), 4, 16, QChar('0')).arg(tag.getETag(), 4, 16, QChar('0')).toUpper();
            DcmVR vr(elem->getVR());
            QString vrStr = QString::fromLatin1(vr.getVRName());
            QString color = getVRColor(vrStr);
            
            // Get tag name 
            QString tagName = QString::fromLatin1(tag.getTagName());
            if (tagName.isEmpty()) {
                tagName = QString("Unknown Tag %1").arg(tagStr);
            }
            
            // Get value
            QString value;
            OFString ofString;
            if (elem->getOFString(ofString, 0).good()) {
                value = QString::fromLatin1(ofString.c_str());
                if (value.length() > 100) {
                    value = value.left(97) + "...";
                }
            } else {
                value = "[Empty]";
            }
            
            return QString("<tr style='background-color: #2a2a2a;'>"
                          "<td style='color: %1; font-family: Consolas, monospace; font-size: 10px; padding: 2px 6px; border: 1px solid #444;'>%2</td>"
                          "<td style='color: %1; font-weight: bold; padding: 2px 6px; border: 1px solid #444;'>%3</td>"
                          "<td style='color: %1; font-family: Consolas, monospace; font-size: 11px; padding: 2px 6px; border: 1px solid #444; word-break: break-all;'>%4</td>"
                          "</tr>").arg(color, tagStr, tagName, value);
        };
        
        QString htmlContent = "<html><body style='margin: 0; padding: 8px; background-color: #1e1e1e; font-family: \"Segoe UI\", Arial, sans-serif; font-size: 11px;'>";
        
        // Add professional title header like Python version
        htmlContent += "<div style='text-align: center; background: linear-gradient(135deg, #0078d4, #005a9e); "
                      "color: white; padding: 12px; margin: -8px -8px 16px -8px; border-radius: 0 0 8px 8px;'>"
                      "<h2 style='margin: 0; font-size: 14px; font-weight: bold; letter-spacing: 1px;'>DICOM TAGS</h2>"
                      "</div>";
        
        // Create table structure like Python version
        htmlContent += "<table style='width: 100%; border-collapse: collapse; font-family: Consolas, monospace;'>";
        htmlContent += "<tr style='background-color: #404040;'>"
                      "<th style='color: white; font-weight: bold; padding: 6px; border: 1px solid #666; text-align: left; width: 80px;'>Group,Elem.</th>"
                      "<th style='color: white; font-weight: bold; padding: 6px; border: 1px solid #666; text-align: left; width: 180px;'>TAG Description</th>"
                      "<th style='color: white; font-weight: bold; padding: 6px; border: 1px solid #666; text-align: left;'>Value</th>"
                      "</tr>";
        
        // Iterate through all DICOM elements like Python version
        for (unsigned long i = 0; i < dataset->card(); i++) {
            DcmElement* elem = dataset->getElement(i);
            if (elem) {
                htmlContent += formatDicomTag(elem);
            }
        }
        
        htmlContent += "</table>";
        
        htmlContent += "</body></html>";
        
        m_dicomInfoTextEdit->setHtml(htmlContent);
        
        // Cache the HTML content for this file to avoid regenerating it
        m_cachedDicomInfoFilePath = filePath;
        m_cachedDicomInfoHtml = htmlContent;
        
    } catch (...) {
        m_dicomInfoTextEdit->setPlainText("Error: Exception occurred while reading DICOM file");
        // Clear cache on error
        m_cachedDicomInfoFilePath.clear();
        m_cachedDicomInfoHtml.clear();
    }
#else
    m_dicomInfoTextEdit->setPlainText("DICOM support not available (DCMTK not compiled)");
#endif
}

// ==============================
// Copy Monitoring System Implementation
// ==============================

void DicomViewer::onCopyProgressTimeout()
{
    // Periodic tree refresh during copy operations
    if (m_copyInProgress && m_dicomReader) {
        qDebugT() << "[PERIODIC REFRESH] Checking for newly available files...";
        
        // Store current selection before repopulating
        QString selectedFilePath;
        bool wasImageSelected = false;
        QTreeWidgetItem* currentItem = m_dicomTree->currentItem();
        if (currentItem) {
            QVariantList itemData = currentItem->data(0, Qt::UserRole).toList();
            if (itemData.size() >= 2) {
                selectedFilePath = itemData[1].toString();
                wasImageSelected = (itemData[0].toString() == "image");
                qDebugT() << "[PERIODIC REFRESH] Preserving selection:" << selectedFilePath 
                         << "Type:" << (wasImageSelected ? "image" : "series");
            }
        }
        
        // Refresh file existence status in DicomReader
        m_dicomReader->refreshFileExistenceStatus();
        
        // Update the tree display to reflect new file availability
        m_dicomReader->populateTreeWidget(m_dicomTree);
        
        // Restore the previous selection if it existed
        if (!selectedFilePath.isEmpty()) {
            // Temporarily disconnect selection signal to avoid triggering onTreeItemSelected
            disconnect(m_dicomTree, &QTreeWidget::currentItemChanged,
                      this, &DicomViewer::onTreeItemSelected);
            
            QTreeWidgetItemIterator it(m_dicomTree);
            while (*it) {
                QVariantList itemData = (*it)->data(0, Qt::UserRole).toList();
                if (itemData.size() >= 2) {
                    QString itemPath = itemData[1].toString();
                    QString itemType = itemData[0].toString();
                    
                    if (itemPath == selectedFilePath) {
                        if (wasImageSelected && itemType == "image") {
                            m_dicomTree->setCurrentItem(*it);
                            qDebugT() << "[PERIODIC REFRESH] Restored image selection:" << selectedFilePath;
                            break;
                        } else if (!wasImageSelected && itemType == "series") {
                            m_dicomTree->setCurrentItem(*it);
                            qDebugT() << "[PERIODIC REFRESH] Restored series selection:" << selectedFilePath;
                            break;
                        }
                    }
                }
                ++it;
            }
            
            // Reconnect the selection signal
            connect(m_dicomTree, &QTreeWidget::currentItemChanged,
                    this, &DicomViewer::onTreeItemSelected);
        }
        
        // REMOVED: updateThumbnailPanel() to prevent tree selection jumping
        // Old behavior: updateThumbnailPanel() called every 2-3 seconds during copy
        // Issue: This caused tree selection to reset to first item, disrupting user navigation
        // Fix: Thumbnails are properly triggered when all files complete via areAllFilesComplete()
        // Result: User can now navigate to any completed file without selection jumping
        
        qDebugT() << "[SELECTION FIX] Skipping thumbnail panel update to preserve user selection";
        
        // Update the header to show current progress
        int totalPatients = m_dicomReader->getTotalPatients();
        int totalImages = m_dicomReader->getTotalImages();
        double overallProgress = m_dicomReader->calculateProgress();
        
        QString headerText = QString("All patients (Patients: %1, Images: %2) - %3% loaded")
                           .arg(totalPatients)
                           .arg(totalImages)
                           .arg(QString::number(overallProgress * 100, 'f', 1));
        
        m_dicomTree->setHeaderLabel(headerText);
        
        qDebugT() << "[PERIODIC REFRESH] Overall progress:" << overallProgress * 100 << "% (" 
                 << (int)(overallProgress * totalImages) << "/" << totalImages << " files)";
    } else {
        // Stop timer if copy is no longer in progress
        if (m_copyProgressTimer && m_copyProgressTimer->isActive()) {
            m_copyProgressTimer->stop();
            logMessage("DEBUG", "Stopped periodic tree refresh timer - copy completed");
        }
    }
}

void DicomViewer::handleMissingFile(const QString& path)
{
    // Detect if this is likely a DVD/media copy scenario
    QString filename = QFileInfo(path).fileName();
    
    // Show appropriate loading message based on context
    if (m_copyInProgress) {
        // DVD/media copy detected - show loading message
        m_imageLabel->setText(QString("Loading from media...\n\nFile: %1").arg(filename));
    } else {
        // Check if parent directory exists (copy might start soon)
        QDir parentDir = QFileInfo(path).dir();
        if (parentDir.exists()) {
            m_imageLabel->setText(QString("Waiting for file...\n\nFile: %1").arg(filename));
            
            // File is missing - trigger copy from DVD if not already in progress
            logMessage("DEBUG", QString("File missing, starting DVD copy: %1").arg(path));
            detectAndStartDvdCopy();
        } else {
            m_imageLabel->setText(QString("File not found\n\nFile: %1").arg(filename));
        }
    }
}











qint64 DicomViewer::getExpectedFileSize(const QString& filePath)
{
    // Simplified file size estimation for new system
    // The new DVD copy system provides real-time progress without needing log file parsing
    return 0;
}

bool DicomViewer::hasActuallyMissingFiles()
{
    logMessage("DEBUG", "[MISSING FILES CHECK] Function called");
    
    // Check if we have loaded a DICOM tree and if any files are actually missing
    if (!m_dicomReader || !m_dicomTree) {
        logMessage("DEBUG", "[MISSING FILES CHECK] No DICOM reader or tree available");
        return false;
    }
    
    // Count missing files in the tree
    int missingCount = 0;
    int totalCount = 0;
    
    std::function<void(QTreeWidgetItem*)> checkItemsRecursively = [&](QTreeWidgetItem* item) {
        for (int i = 0; i < item->childCount(); ++i) {
            QTreeWidgetItem* child = item->child(i);
            checkItemsRecursively(child);
            
            // Check if this is a file item (image or report)
            QVariantList data = child->data(0, Qt::UserRole).toList();
            if (data.size() >= 2 && (data[0].toString() == "image" || data[0].toString() == "report")) {
                totalCount++;
                QString filePath = data[1].toString();
                if (!QFile::exists(filePath)) {
                    missingCount++;
                }
            }
        }
    };
    
    checkItemsRecursively(m_dicomTree->invisibleRootItem());
    
    logMessage("DEBUG", QString("File check: %1 missing out of %2 total files").arg(missingCount).arg(totalCount));
    
    // Only consider it "missing files" if we have a significant number missing
    // This avoids triggering DVD copy for just a few missing files
    bool result = missingCount > 0 && (missingCount > totalCount * 0.1 || missingCount > 5);
    logMessage("DEBUG", QString("[MISSING FILES CHECK] Result: %1 - Missing: %2 Total: %3").arg(result).arg(missingCount).arg(totalCount));
    return result;
}

QStringList DicomViewer::getOrderedFileList()
{
    QStringList orderedFiles;
    
    if (!m_dicomTree) {
        logMessage("ERROR", "[ERROR] No DICOM tree available for ordered file list");
        return orderedFiles;
    }
    
    // Safety check - ensure tree has items
    if (m_dicomTree->topLevelItemCount() == 0) {
        logMessage("WARN", "[WARNING] DICOM tree is empty");
        return orderedFiles;
    }
    
    logMessage("DEBUG", "Extracting ordered file list from tree view...");
    
    // Walk through tree in display order (top to bottom) to get files as they appear to user
    QTreeWidgetItemIterator it(m_dicomTree);
    int itemCount = 0;
    
    while (*it) {
        QTreeWidgetItem* item = *it;
        
        // Safety check for null item
        if (!item) {
            ++it;
            continue;
        }
        
        itemCount++;
        
        // Check if this item represents a file (image or report, not patient/study/series headers)
        QVariantList userData = item->data(0, Qt::UserRole).toList();
        if (userData.size() >= 2) {
            QString itemType = userData[0].toString();
            QString filePath = userData[1].toString();
            
            if (itemType == "image" || itemType == "report") {
                // Extract just the filename from the full path for robocopy
                QFileInfo fileInfo(filePath);
                QString fileName = fileInfo.fileName();
                
                if (!fileName.isEmpty()) {
                    orderedFiles.append(fileName);
                    logMessage("DEBUG", QString("[ORDERED FILE %1] %2 (type: %3)").arg(orderedFiles.size()).arg(fileName).arg(itemType));
                }
            }
        }
        ++it;
    }
    
    logMessage("DEBUG", QString("Extracted %1 files from tree view in display order").arg(orderedFiles.size()));
    return orderedFiles;
}

void DicomViewer::detectAndStartDvdCopy()
{
    qDebugT() << "[DVD DETECTION] detectAndStartDvdCopy() called";
    
    // Prevent multiple simultaneous detection operations
    if (m_dvdDetectionInProgress) {
        qDebugT() << "[DVD DETECTION] Already in progress, skipping duplicate request";
        return;
    }
    
    qDebugT() << "=== DVD Detection Started ===";
    
    // Only run DVD detection if we have missing files that need to be copied
    if (!hasActuallyMissingFiles()) {
        logMessage("DEBUG", "[DVD CHECK] No missing files detected, skipping DVD detection");
        logMessage("DEBUG", "[DVD CHECK] All required files appear to be available locally");
        logMessage("DEBUG", QString("[DVD CHECK] m_dvdDetectionInProgress remains: %1").arg(m_dvdDetectionInProgress));
        return;
    }
    
    logMessage("DEBUG", "[DVD CHECK] Missing files detected, proceeding with DVD detection");
    
    // Check if worker thread is already running
    if (m_dvdWorkerThread && m_dvdWorkerThread->isRunning()) {
        logMessage("DEBUG", "[DVD CHECK] DVD worker already running, skipping new detection");
        return;
    }
    
    if (m_copyInProgress) {
        logMessage("DEBUG", "[DVD CHECK] Copy already in progress, skipping DVD detection");
        return;
    }
    
    // Set detection in progress flag ONLY after confirming missing files need copying
    m_dvdDetectionInProgress = true;
    
    logMessage("DEBUG", "[DVD WORKER] Starting background DVD detection and copy...");
    logMessage("DEBUG", "[DVD WORKER] Looking for DVD drives with DicomFiles folder...");
    
    // Start worker thread for all DVD operations
    if (m_dvdWorkerThread && m_dvdWorker && !m_dvdWorkerThread->isRunning()) {
        logMessage("DEBUG", "[DVD WORKER] Starting worker thread for DVD operations...");
        m_dvdWorkerThread->start();
        
        // Give the thread a moment to start properly
        QThread::msleep(100);
        logMessage("DEBUG", "[DVD WORKER] Worker thread started successfully");
    }
    
    // Worker thread will handle all DVD detection and copying
    logMessage("DEBUG", "[DVD DETECTION] Letting worker thread handle DVD detection and copying...");
}





QString DicomViewer::findDvdWithDicomFiles()
{
    logMessage("DEBUG", "[DVD SCAN] Scanning for DVD drives with DicomFiles folder...");
    
    // Check common DVD drive letters
    QStringList drivesToCheck = {"D:", "E:", "F:", "G:", "H:"};
    
    for (const QString& drive : drivesToCheck) {
        QString dicomPath = drive + "/DicomFiles";
        QDir dir(dicomPath);
        
        logMessage("DEBUG", QString("[DVD SCAN] Checking %1...").arg(dicomPath));
        
        if (dir.exists()) {
            logMessage("DEBUG", QString("[DVD FOUND] DicomFiles folder exists at %1").arg(dicomPath));
            
            // Check if it contains DICOM files
            QStringList filters;
            filters << "*.dcm" << "*.DCM" << "*"; // Include files without extension
            QStringList files = dir.entryList(filters, QDir::Files);
            
            logMessage("DEBUG", QString("[DVD CONTENT] Found %1 files in DicomFiles folder").arg(files.count()));
            
            if (!files.isEmpty()) {
                logMessage("DEBUG", QString("[DVD SUCCESS] ? Found %1 DICOM files at: %2").arg(files.size()).arg(dicomPath));
                
                // Log first few filenames for verification
                for (int i = 0; i < qMin(3, files.size()); i++) {
                    logMessage("DEBUG", QString("[DVD FILES]   - %1").arg(files[i]));
                }
                if (files.size() > 3) {
                    logMessage("DEBUG", QString("[DVD FILES]   ... and %1 more files").arg(files.size() - 3));
                }
                
                return drive; // Return drive letter, not full path
            } else {
                logMessage("DEBUG", QString("[DVD EMPTY] DicomFiles folder is empty at %1").arg(dicomPath));
            }
        } else {
            logMessage("DEBUG", QString("[DVD SCAN] No DicomFiles folder at %1").arg(dicomPath));
        }
    }
    
    logMessage("DEBUG", "[DVD SCAN] ? No DVD with DICOM files found in any drive");
    return QString(); // No DVD with DICOM files found
}







// DVD Worker Slot Implementations
void DicomViewer::onWorkerReady()
{
    qDebugT() << "[WORKER READY] DVD worker thread is ready";
    m_workerReady = true;
    
    // If we have pending sequential copy data, start it now
    if (!m_pendingDvdPath.isEmpty() && !m_pendingOrderedFiles.isEmpty()) {
        logMessage("DEBUG", QString("[PENDING COPY] Starting pending sequential copy for: %1").arg(m_pendingDvdPath));
        logMessage("DEBUG", QString("[PENDING COPY] Files to copy: %1").arg(m_pendingOrderedFiles.size()));
        
        emit requestSequentialRobocopyStart(m_pendingDvdPath, m_pendingOrderedFiles);
        
        // START FIRST IMAGE MONITOR EARLY - during copy initiation
        // This ensures first image displays as soon as first file is ready,
        // not after thumbnail panel becomes visible
        logMessage("DEBUG", "[DVD COPY] Starting first image monitor during pending copy initiation");
        startFirstImageMonitor();
        
        // Clear pending data
        m_pendingDvdPath.clear();
        m_pendingOrderedFiles.clear();
    } else {
        logMessage("DEBUG", "[WORKER READY] No pending copy data");
    }
}

void DicomViewer::onDvdDetected(const QString& dvdPath)
{
    logMessage("INFO", "DVD detected at: " + dvdPath);
    
    // Only update m_dvdSourcePath if we don't have a preferred source drive from command line
    if (m_providedSourceDrive.isEmpty()) {
        m_dvdSourcePath = dvdPath;
        logMessage("INFO", "Using auto-detected DVD path: " + dvdPath);
    } else {
        // Use the provided source drive for consistency
        QString preferredDrive = m_providedSourceDrive;
        if (!preferredDrive.endsWith(":")) {
            preferredDrive += ":";
        }
        m_dvdSourcePath = preferredDrive;
        logMessage("INFO", "Using provided source drive instead of detected: " + preferredDrive + " (detected was: " + dvdPath + ")");
    }
    
    // Clear any previous completion tracking to start fresh
    logMessage("DEBUG", "[INIT DEBUG] Clearing completed files set at DVD detection");
    m_fullyCompletedFiles.clear();
    m_firstImageAutoSelected = false;  // Reset auto-selection flag for new session
    
    // Get ordered file list from tree view for sequential copying
    QStringList orderedFiles = getOrderedFileList();
    
    if (!orderedFiles.isEmpty()) {
        logMessage("DEBUG", QString("[SEQUENTIAL COPY] Storing sequential copy data for path: %1").arg(dvdPath));
        logMessage("DEBUG", QString("[SEQUENTIAL COPY] Files to copy in order: %1").arg(orderedFiles.size()));
        
        m_pendingDvdPath = dvdPath;
        m_pendingOrderedFiles = orderedFiles;
        
        // Check if worker is already ready - if so, start immediately
        if (m_workerReady) {
            logMessage("DEBUG", "[IMMEDIATE START] Worker is ready, starting sequential copy immediately");
            emit requestSequentialRobocopyStart(m_pendingDvdPath, m_pendingOrderedFiles);
            
            // START FIRST IMAGE MONITOR EARLY - during copy initiation
            // This ensures first image displays as soon as first file is ready,
            // not after thumbnail panel becomes visible
            logMessage("DEBUG", "[DVD COPY] Starting first image monitor during immediate copy initiation");
            startFirstImageMonitor();
            
            // Clear pending data since we started immediately
            m_pendingDvdPath.clear();
            m_pendingOrderedFiles.clear();
        } else {
            logMessage("DEBUG", "[SEQUENTIAL COPY] Worker not ready yet, waiting for worker ready signal");
        }
    } else {
        logMessage("WARN", "[WARNING] No ordered files found in tree view - DVD copying may not work properly");
        logMessage("DEBUG", "[INFO] Ensure DICOMDIR is loaded and tree view is populated before DVD detection");
    }
    
    if (m_imageLabel) {
        m_imageLabel->setText("DVD detected. Loading...");
    }
}

void DicomViewer::onCopyStarted()
{
    logMessage("INFO", "DVD copy started");
    m_copyInProgress = true;
    m_currentCopyProgress = 0;
    m_dvdDetectionInProgress = false;  // Reset detection flag when copy starts
    
    // Ensure completed files set is clear at copy start
    qDebugT() << "[COPY START DEBUG] Completed files count before clear:" << m_fullyCompletedFiles.size();
    m_fullyCompletedFiles.clear();
    qDebugT() << "[COPY START DEBUG] Completed files set cleared";
    
    // Debug: Check tree items one more time at copy start
    if (m_dicomTree) {
        QTreeWidgetItemIterator it(m_dicomTree);
        int itemsWithProgress = 0;
        while (*it) {
            QTreeWidgetItem* item = *it;
            QString itemText = item->text(0);
            if (itemText.contains("%") || itemText.contains("Loading")) {
                itemsWithProgress++;
                logMessage("DEBUG", QString("[COPY START DEBUG] Item with progress detected: %1").arg(itemText));
            }
            ++it;
        }
        logMessage("DEBUG", QString("[COPY START DEBUG] Total items with progress indicators: %1").arg(itemsWithProgress));
    }
    
    // Update status bar instead of blocking image display
    updateStatusBar("Loading from media...", 0);
    
    // Start progress monitoring
    if (m_copyProgressTimer) {
        m_copyProgressTimer->start(1000);
    }
}

void DicomViewer::onFileProgress(const QString& fileName, int progress)
{
    m_currentCopyProgress = progress;
    
    // Update file state based on copy progress
    QString fullPath = PathNormalizer::constructFilePath(m_localDestPath, fileName); // Construct normalized full path
    logMessage(LOG_DEBUG, QString("PathNormalizer: Constructed file path for progress tracking: %1").arg(fullPath));
    
    if (progress >= 100) {
        setFileState(fullPath, FileState::Available);
        
        // Trigger thumbnail generation if not already queued
        ThumbnailState thumbState = getThumbnailState(fullPath);
        if (thumbState == ThumbnailState::NotGenerated) {
            setThumbnailState(fullPath, ThumbnailState::Queued);
        }
        
        // Auto-select first available image if none selected
        if (m_currentDisplayReadyFile.isEmpty()) {
            QTimer::singleShot(500, this, &DicomViewer::autoSelectFirstAvailableImage);
        }
    } else {
        // IMPORTANT: Only set to Copying if file isn't already Available or DisplayReady
        // This prevents completed files from being downgraded during DVD copy operations
        FileState currentState = getFileState(fullPath);
        if (currentState == FileState::NotReady) {
            setFileState(fullPath, FileState::Copying);
        }
        // For files already Available/DisplayReady, preserve their state during copying
    }
    
    // Update status bar instead of blocking image display
    QString statusMessage = QString("Loading: %1 (%2%)")
                           .arg(QFileInfo(fileName).fileName())
                           .arg(progress);
    updateStatusBar(statusMessage, progress);
    
    // Update tree item with Loading.png icon if we can identify it
    updateTreeItemWithProgress(fileName, progress);
    
    // Start periodic tree refresh during copy operations (every 2 seconds)
    if (!m_copyProgressTimer->isActive() && m_copyInProgress) {
        m_copyProgressTimer->start(2000); // Check every 2 seconds
        logMessage("DEBUG", "Started periodic tree refresh timer during copy operation");
    }
}

void DicomViewer::onOverallProgress(int percentage, const QString& statusText)
{
    logMessage("DEBUG", QString("Overall DVD copy progress: %1% - %2").arg(percentage).arg(statusText));
    
    // Update status bar instead of blocking image display
    updateStatusBar(statusText, percentage);
    
    // Update tree header with overall progress
    if (m_dicomTree) {
        QString headerText = QString("Loading from DVD: %1").arg(statusText);
        m_dicomTree->setHeaderLabel(headerText);
    }
}

void DicomViewer::onCopyCompleted(bool success)
{
    logMessage("DEBUG", QString("*** RECEIVED onCopyCompleted signal with success: %1 ***").arg(success));
    logMessage("DEBUG", QString("[DVD COPY] Copy completed. Success: %1").arg(success));
    
    m_copyInProgress = false;
    m_dvdDetectionInProgress = false; // Reset detection flag
    
    // Stop progress monitoring
    if (m_copyProgressTimer && m_copyProgressTimer->isActive()) {
        m_copyProgressTimer->stop();
    }
    
    // Check if we should show thumbnail panel now that copy is complete
    checkAndShowThumbnailPanel();
    
    // Stop worker thread
    if (m_dvdWorkerThread) {
        m_dvdWorkerThread->quit();
        m_dvdWorkerThread->wait();
    }
    
    if (success) {
        logMessage("INFO", "*** UNIQUE: DICOM files copy completed successfully - ENHANCED VERSION ***");
        logMessage("INFO", "*** SUCCESS: onCopyCompleted function executed successfully ***");
        logMessage("DEBUG", "[DVD COPY] About to handle DICOMDIR reloading and auto-selection");
        
        // Clear the completed files tracking since all files are now available
        m_fullyCompletedFiles.clear();
        logMessage("DEBUG", "Cleared completed files set - all files now fully available after DVD copy");
        
        // Reset thumbnail completion flag for regeneration
        m_allThumbnailsComplete = false;
        
        logMessage("INFO", "[DVD COPY] *** COPY COMPLETED - Will update file states IMMEDIATELY for user selections ***");
        
        // CRITICAL FIX: Update file states IMMEDIATELY when copy completes
        // This prevents the timing gap where users can select files before state updates
        if (m_dicomTree) {
            QMutexLocker locker(&m_fileStatesMutex);
            QTreeWidgetItemIterator fileIt(m_dicomTree);
            int immediatelyMarkedAvailable = 0;
            while (*fileIt) {
                QVariantList userData = (*fileIt)->data(0, Qt::UserRole).toList();
                if (userData.size() >= 2 && (userData[0].toString() == "image" || userData[0].toString() == "report")) {
                    QString filePath = userData[1].toString();
                    QString normalizedPath = PathNormalizer::normalize(filePath);
                    
                    // Check if copied file exists at destination
                    if (QFile::exists(filePath) || QFile::exists(normalizedPath)) {
                        FileState oldState = m_fileStates.value(filePath, FileState::NotReady);
                        if (oldState != FileState::Available) {
                            m_fileStates[filePath] = FileState::Available;
                            immediatelyMarkedAvailable++;
                        }
                        
                        // Also update normalized path state if different
                        if (normalizedPath != filePath) {
                            FileState normalizedOldState = m_fileStates.value(normalizedPath, FileState::NotReady);
                            if (normalizedOldState != FileState::Available) {
                                m_fileStates[normalizedPath] = FileState::Available;
                            }
                        }
                    }
                }
                ++fileIt;
            }
            locker.unlock();
            
            logMessage("INFO", QString("[DVD COPY] IMMEDIATELY marked %1 files as Available - users can now select them").arg(immediatelyMarkedAvailable));
        }
        
        // Defer tree repopulation and auto-selection to avoid UI freeze
        QTimer::singleShot(100, this, [this]() {
            logMessage("DEBUG", "[UI] Starting deferred tree repopulation and auto-selection");
            
            // Force check if all files are complete and trigger thumbnails if needed
            if (areAllFilesComplete()) {
                logMessage("INFO", "[DVD COPY] All files complete after copy - triggering thumbnails");
                updateThumbnailPanel();
            } else {
                // Even if not all files are tracked as complete, try to trigger thumbnails anyway
                // since the copy operation has completed successfully
                logMessage("INFO", "[DVD COPY] Copy completed but not all files tracked - forcing thumbnail check");
                
                // Log debug info about file states
                QMutexLocker locker(&m_fileStatesMutex);
                int totalFiles = getTotalFileCount();
                int availableFiles = 0;
                for (auto it = m_fileStates.begin(); it != m_fileStates.end(); ++it) {
                    if (it.value() == FileState::Available || it.value() == FileState::DisplayReady) {
                        availableFiles++;
                    }
                }
                locker.unlock();
                
                logMessage("DEBUG", QString("[DVD COPY] File state summary: %1 available of %2 total").arg(availableFiles).arg(totalFiles));
                
                // Force thumbnail update even if not all files are tracked as complete
                // The copy is done, so we should show what we have
                updateThumbnailPanel();
            }
            
            // Refresh DICOM tree to show newly copied files
        if (m_dicomReader) {
            QString dicomdirPath = PathNormalizer::constructRelativePath(m_localDestPath, "../DICOMDIR");
            logMessage(LOG_DEBUG, QString("PathNormalizer: Constructed DICOMDIR path: %1").arg(dicomdirPath));
            if (QFile::exists(dicomdirPath)) {
                // Store current selection before repopulating tree
                QTreeWidgetItem* currentSelection = m_dicomTree->currentItem();
                QString selectedFilePath;
                bool wasImageSelected = false;
                if (currentSelection) {
                    QVariantList userData = currentSelection->data(0, Qt::UserRole).toList();
                    if (userData.size() >= 2) {
                        QString type = userData[0].toString();
                        selectedFilePath = userData[1].toString();
                        wasImageSelected = (type == "image");
                    }
                }
                
                m_dicomReader->loadDicomDir(dicomdirPath);
                m_dicomReader->populateTreeWidget(m_dicomTree);
                
                // Initialize file states after tree repopulation
                logMessage("DEBUG", "[DVD COPY] Initializing file states after tree repopulation");
                initializeFileStatesFromTree();
                
                // File states were already updated immediately when copy completed
                // No need to mark them as Available again here
                logMessage("INFO", "[DVD COPY] File states already updated immediately when copy completed - ready for thumbnails");
                
                // The file availability monitor will automatically trigger thumbnails when ready
                // No manual intervention needed - clean separation of concerns
                
                // First image monitor already started during copy initiation
                logMessage("DEBUG", "[DVD COPY] First image monitor already active from copy initiation");
                
                // Temporarily disconnect the selection signal to avoid reloading
                disconnect(m_dicomTree, &QTreeWidget::currentItemChanged,
                          this, &DicomViewer::onTreeItemSelected);
                
                // Restore user selection or select first image if no user selection
                bool selectionRestored = false;
                if (!selectedFilePath.isEmpty()) {
                    // Try to find and select the previously selected item
                    QTreeWidgetItemIterator it(m_dicomTree);
                    while (*it) {
                        QVariantList itemData = (*it)->data(0, Qt::UserRole).toList();
                        if (itemData.size() >= 2) {
                            QString itemPath = itemData[1].toString();
                            QString itemType = itemData[0].toString();
                            
                            // If we had an image selected, only restore to same image
                            // If we had a series selected, restore to same series or first image in series
                            if (itemPath == selectedFilePath) {
                                if (wasImageSelected && itemType == "image") {
                                    m_dicomTree->setCurrentItem(*it);
                                    selectionRestored = true;
                                    break;
                                } else if (!wasImageSelected && itemType == "series") {
                                    // User had series selected, but now select first image in that series
                                    QTreeWidgetItem* firstImage = findFirstImageChild(*it);
                                    if (firstImage) {
                                        m_dicomTree->setCurrentItem(firstImage);
                                    } else {
                                        m_dicomTree->setCurrentItem(*it);
                                    }
                                    selectionRestored = true;
                                    break;
                                }
                            }
                        }
                        ++it;
                    }
                }
                
                // If no previous selection or couldn't restore it, select first image
                if (!selectionRestored) {
                    logMessage("DEBUG", "[DVD COPY] No previous selection restored - triggering auto-selection");
                    autoSelectFirstCompletedImage();
                }
                
                // Start display monitor now that DVD copy is complete and tree is populated
                logMessage("DEBUG", "[DVD COPY] Starting display monitor after successful copy completion");
                startDisplayMonitor();
                
                // Reconnect the selection signal for future user interactions
                connect(m_dicomTree, &QTreeWidget::currentItemChanged,
                        this, &DicomViewer::onTreeItemSelected);
            } else {
                logMessage("WARNING", QString("[DVD COPY] DICOMDIR not found at: %1").arg(dicomdirPath));
            }
        } else {
            logMessage("WARNING", "[DVD COPY] DICOM reader not available");
        }
        
        // Update status bar to show completion
        updateStatusBar("Media loading completed", -1);
        
            logMessage("DEBUG", "[UI] Deferred tree repopulation and auto-selection completed");
        }); // End deferred lambda
        
        // Start ffmpeg copy in a separate thread after DICOM files are copied and status updated
        QThread* ffmpegCopyThread = QThread::create([this]() {
            copyFfmpegExe();
        });
        ffmpegCopyThread->start();
        
        // Clean up thread when it finishes
        connect(ffmpegCopyThread, &QThread::finished, ffmpegCopyThread, &QObject::deleteLater);
    } else {
        // Update status bar to show failure
        updateStatusBar("Failed to load from media", -1);
    }
}

void DicomViewer::onWorkerError(const QString& error)
{
    logMessage("ERROR", QString("DVD worker error: %1").arg(error));
    
    m_copyInProgress = false;
    m_dvdDetectionInProgress = false; // Reset detection flag
    
    if (m_imageLabel) {
        m_imageLabel->setText(QString("Error: %1").arg(error));
    }
    
    // Stop worker thread
    if (m_dvdWorkerThread) {
        m_dvdWorkerThread->quit();
        m_dvdWorkerThread->wait();
    }
}

void DicomViewer::onFfmpegCopyCompleted(bool success)
{
    logMessage("DEBUG", QString("[FFMPEG COPY] FFmpeg copy completed. Success: %1").arg(success));
    
    m_ffmpegCopyCompleted = success;
    
    if (success && m_saveRunAction) {
        m_saveRunAction->setEnabled(true);
        logMessage("INFO", "Video export functionality now available - FFmpeg ready");
    } else if (!success) {
        if (m_saveRunAction) {
            m_saveRunAction->setEnabled(false);
        }
        logMessage("WARN", "FFmpeg copy failed - Video export will remain disabled");
    }
}

void DicomViewer::updateTreeItemWithProgress(const QString& fileName, int progress)
{
    if (!m_dicomTree || !m_dicomReader) return;
    
    // Extract just the filename from the full path for tree matching
    QString baseFileName = QFileInfo(fileName).fileName();
    
    logMessage("DEBUG", QString("File progress update: %1 %2%").arg(fileName).arg(progress));
    logMessage("DEBUG", QString("Extracted filename for tree matching: %1").arg(baseFileName));
    
    // Find and update the specific tree item for this file using just the filename
    updateSpecificTreeItemProgress(baseFileName, progress);
    
    // When a file completes (reaches 100%), refresh tree to show it's available
    if (progress >= 100) {
        QString baseFileName = QFileInfo(fileName).fileName();
        logMessage("DEBUG", "=== FILE COMPLETION DEBUG ===");
        logMessage("DEBUG", QString("File completed: %1").arg(baseFileName));
        logMessage("DEBUG", QString("m_firstImageAutoSelected: %1").arg(m_firstImageAutoSelected));
        logMessage("DEBUG", QString("Current m_fullyCompletedFiles size: %1").arg(m_fullyCompletedFiles.size()));
        
        // Prevent duplicate completions
        if (m_fullyCompletedFiles.contains(baseFileName)) {
            logMessage("DEBUG", QString("File already completed, skipping: %1").arg(baseFileName));
            return;
        }
        
        // Add to the set of fully completed files
        m_fullyCompletedFiles.insert(baseFileName);
        logMessage("DEBUG", QString("After adding, m_fullyCompletedFiles size: %1").arg(m_fullyCompletedFiles.size()));
        
        // Refresh file existence status in DicomReader
        m_dicomReader->refreshFileExistenceStatus();
        
        // Update frame count for this specific file now that it's available
        m_dicomReader->updateFrameCountForFile(baseFileName);
        
        // Update the tree display to reflect new file availability with correct icons
        m_dicomReader->populateTreeWidget(m_dicomTree);
        
        logMessage("DEBUG", QString("Tree refreshed after file completion with updated frame count: %1").arg(fileName));
        
        // NEW: Check if ALL files are now complete and trigger thumbnail creation if so
        if (areAllFilesComplete()) {
            logMessage("INFO", "[ALL FILES COMPLETE] All files now have cine/image icons - triggering thumbnail creation");
            updateThumbnailPanel();
        }
        
        // Auto-select and display the first completed image for better UX
        if (!m_firstImageAutoSelected) {
            logMessage("DEBUG", "[EARLY AUTO-SELECT] First file completed, attempting immediate auto-selection");
            autoSelectFirstCompletedImage();
            
            // If auto-selection failed, try a more aggressive approach for the very first file
            if (!m_firstImageAutoSelected && m_fullyCompletedFiles.size() == 1) {
                logMessage("DEBUG", "[IMMEDIATE SELECT] This is the very first file - forcing immediate selection");
                
                // Find any tree item that matches this completed file
                QTreeWidgetItemIterator it(m_dicomTree);
                int itemCount = 0;
                while (*it) {
                    itemCount++;
                    QTreeWidgetItem* item = *it;
                    QVariantList userData = item->data(0, Qt::UserRole).toList();
                    
                    if (itemCount <= 5) {
                        logMessage("DEBUG", QString("[DEBUG ITEM %1] Text: %2, UserData size: %3")
                               .arg(itemCount).arg(item->text(0)).arg(userData.size()));
                        if (userData.size() >= 2) {
                            logMessage("DEBUG", QString("  Type: %1 Path: %2").arg(userData[0].toString()).arg(userData[1].toString()));
                        }
                    }
                    
                    if (userData.size() >= 2 && userData[0].toString() == "image") {
                        QString itemFilename = QFileInfo(userData[1].toString()).fileName();
                        logMessage("DEBUG", QString("[CHECKING ITEM] %1 -> filename: %2").arg(item->text(0)).arg(itemFilename));
                        
                        if (m_fullyCompletedFiles.contains(itemFilename)) {
                            logMessage("DEBUG", QString("[IMMEDIATE SELECT] Found completed item, selecting: %1").arg(item->text(0)));
                            
                            // Expand parents
                            QTreeWidgetItem* parent = item->parent();
                            while (parent) {
                                logMessage("DEBUG", QString("[EXPANDING] Parent: %1").arg(parent->text(0)));
                                parent->setExpanded(true);
                                parent = parent->parent();
                            }
                            
                            // Select and trigger loading immediately
                            m_dicomTree->setCurrentItem(item);
                            m_dicomTree->scrollToItem(item);
                            logMessage("DEBUG", "[IMMEDIATE SELECT] About to call onTreeItemSelected");
                            onTreeItemSelected(item, nullptr);
                            m_firstImageAutoSelected = true;
                            
                            logMessage("DEBUG", "[IMMEDIATE SELECT] Successfully selected first completed file!");
                            break;
                        }
                    }
                    ++it;
                }
                
                logMessage("DEBUG", QString("[IMMEDIATE SELECT] Checked %1 total tree items").arg(itemCount));
            }
        } else {
            logMessage("DEBUG", "[EARLY AUTO-SELECT] Skipping auto-selection - already done");
        }
        
        // Also update the header to show current progress
        int totalPatients = m_dicomReader->getTotalPatients();
        int totalImages = m_dicomReader->getTotalImages();
        double overallProgress = m_dicomReader->calculateProgress();
        
        QString headerText = QString("All patients (Patients: %1, Images: %2) - %3% loaded")
                           .arg(totalPatients)
                           .arg(totalImages)
                           .arg(QString::number(overallProgress * 100, 'f', 1));
        
        m_dicomTree->setHeaderLabel(headerText);
        
        // Fix progress calculation - cap at 100% and avoid double multiplication
        double displayProgress = qMin(overallProgress * 100.0, 100.0);
        int completedFiles = qMin(int(overallProgress * totalImages), totalImages);
        
        logMessage("DEBUG", QString("Overall progress: %1% (%2/%3 files)")
                 .arg(QString::number(displayProgress, 'f', 1)).arg(completedFiles).arg(totalImages));
    }
}

void DicomViewer::updateSpecificTreeItemProgress(const QString& fileName, int progress)
{
    if (!m_dicomTree) return;
    
    logMessage("DEBUG", QString("[TREE UPDATE] Searching for file: %1 progress: %2%").arg(fileName).arg(progress));
    
    // Recursively search through all tree items to find the one corresponding to this file
    QTreeWidgetItemIterator it(m_dicomTree);
    bool found = false;
    int itemCount = 0;
    
    while (*it) {
        QTreeWidgetItem* item = *it;
        itemCount++;
        
        // Check if this item represents an image or report
        QVariantList userData = item->data(0, Qt::UserRole).toList();
        if (userData.size() >= 2) {
            QString itemType = userData[0].toString();
            QString filePath = userData[1].toString();
            
            // Debug: Show first few items to understand the data structure
            if (itemCount <= 5 && (itemType == "image" || itemType == "report")) {
                logMessage("DEBUG", QString("[TREE DEBUG] %1 Type: %2 Path: %3 Text: %4").arg(itemCount).arg(itemType).arg(filePath).arg(item->text(0)));
            }
            
            // Check if this item corresponds to the file being copied
            // Use exact filename matching only for DICOM files
            bool isMatch = false;
            
            if ((itemType == "image" || itemType == "report")) {
                // Get the actual filename from the tree item's file path
                QFileInfo filePathInfo(filePath);
                QString itemFileName = filePathInfo.fileName();
                
                // CRITICAL: Use exact string comparison for DICOM filenames
                // This prevents all files starting with "1.2.392..." from matching the same item
                if (itemFileName == fileName) {
                    isMatch = true;
                    logMessage("DEBUG", QString("[MATCH] Exact filename match: %1 vs %2").arg(fileName).arg(itemFileName));
                } else {
                    // Debug mismatches to help troubleshoot
                    logMessage("DEBUG", QString("[NO MATCH] %1 != %2").arg(fileName).arg(itemFileName));
                }
            }
            
            if (isMatch) {
                
                // Store original text if not already stored
                if (!item->data(0, Qt::UserRole + 1).isValid()) {
                    QString originalText = item->text(0);
                    item->setData(0, Qt::UserRole + 1, originalText);
                    logMessage("DEBUG", QString("Stored original text for item: %1").arg(originalText));
                }
                
                QString originalText = item->data(0, Qt::UserRole + 1).toString();
                
                if (progress < 100) {
                    // Show progress percentage in the item text
                    QString progressText = QString("%1 - Loading... %2%").arg(originalText).arg(progress);
                    item->setText(0, progressText);
                    
                    // Keep the loading icon and gray color
                    item->setIcon(0, QIcon(":/icons/Loading.png"));
                    item->setForeground(0, QColor(180, 180, 180));
                    
                    logMessage("DEBUG", QString("Updated tree item progress: %1 %2%").arg(fileName).arg(progress));
                } else {
                    // File completed - verify it actually exists and is readable before marking complete
                    QString fullFilePath = QFileInfo(filePath).absoluteFilePath();
                    QFileInfo completedFile(fullFilePath);
                    
                    if (completedFile.exists() && completedFile.size() > 0) {
                        logMessage("DEBUG", QString("[FILE VERIFIED] File exists and has size: %1 bytes").arg(completedFile.size()));
                        
                        // File completed - restore original text and icon
                        item->setText(0, originalText);
                        item->setForeground(0, QColor(0, 0, 0)); // Black text
                        
                        // Set appropriate icon based on file type and actual frame count
                        if (itemType == "report") {
                            item->setIcon(0, QIcon(":/icons/List.png"));
                        } else {
                            // Get frame count from cached DICOM data instead of re-reading file
                            DicomImageInfo imageInfo = m_dicomReader->getImageInfoForFile(fileName);
                            int cachedFrameCount = imageInfo.frameCount;
                            
                            logMessage("DEBUG", QString("[ICON SELECTION] File: %1 Cached Frames: %2 Path: %3").arg(fileName).arg(cachedFrameCount).arg(imageInfo.filePath));
                            
                            if (cachedFrameCount > 1) {
                                item->setIcon(0, QIcon(":/icons/AcquisitionHeader.png"));
                                logMessage("DEBUG", QString("Set multiframe icon for %1 (%2 frames)").arg(fileName).arg(cachedFrameCount));
                            } else {
                                item->setIcon(0, QIcon(":/icons/Camera.png"));
                                logMessage("DEBUG", QString("Set single frame icon for %1").arg(fileName));
                            }
                        }
                        
                        logMessage("DEBUG", QString("File completed, restored original text: %1").arg(originalText));
                    } else {
                        logMessage("WARN", QString("[FILE NOT READY] File %1 marked as 100% but doesn't exist or is empty. Keeping loading state.").arg(fileName));
                        // Keep the loading state - don't mark as complete yet
                        QString progressText = QString("%1 - Finalizing...").arg(originalText);
                        item->setText(0, progressText);
                        item->setIcon(0, QIcon(":/icons/Loading.png"));
                        item->setForeground(0, QColor(180, 180, 180));
                    }
                }
                break;
            }
        }
        ++it;
    }
}

void DicomViewer::parseRobocopyOutput(const QString& output)
{
    // Parse robocopy output for progress information with detailed logging
    QStringList lines = output.split('\n');
    static int s_filesProcessed = 0;
    static QElapsedTimer s_copyTimer;
    static bool s_timerStarted = false;
    
    if (!s_timerStarted) {
        s_copyTimer.start();
        s_timerStarted = true;
    }
    
    for (const QString& line : lines) {
        QString trimmedLine = line.trimmed();
        if (trimmedLine.isEmpty()) continue;
        
        // Log interesting robocopy status messages
        if (trimmedLine.contains("Started :") || 
            trimmedLine.contains("Source =") ||
            trimmedLine.contains("Dest :") ||
            trimmedLine.contains("Options :")) {
            logMessage("DEBUG", QString("[ROBOCOPY] %1").arg(trimmedLine));
        }
        
        // Skip files that robocopy reports as "same" (already exist and identical)
        if (trimmedLine.contains("same\t\t")) {
            logMessage("DEBUG", QString("[ROBOCOPY SAME] Skipping file that already exists: %1").arg(trimmedLine));
            continue;
        }
        
        // Look for file progress with enhanced parsing
        if (trimmedLine.contains("%")) {
            // Parse progress percentage and filename for any % value
            static QRegularExpression progressPattern(R"(\s*(\d+)%\s+(.+)$)");
            QRegularExpressionMatch match = progressPattern.match(trimmedLine);
            if (match.hasMatch()) {
                int progress = match.captured(1).toInt();
                QString fileInfo = match.captured(2).trimmed();
                
                m_currentCopyProgress = progress;
                
                // Extract clean filename from the file info
                QString filename;
                if (fileInfo.contains("New File")) {
                    QStringList parts = fileInfo.split(QRegularExpression("\\s+"));
                    if (parts.size() >= 4) {
                        filename = QFileInfo(parts.last()).fileName();
                    }
                } else {
                    filename = QFileInfo(fileInfo).fileName();
                }
                
                // Update status bar with progress
                if (!filename.isEmpty()) {
                    QString statusMessage = QString("Loading: %1 (%2%)")
                                           .arg(filename).arg(progress);
                    updateStatusBar(statusMessage, progress);
                } else {
                    updateStatusBar(QString("Loading from media... %1%").arg(progress), progress);
                }
                
                // Enhanced progress logging
                qint64 elapsed = s_copyTimer.elapsed();
                logMessage("DEBUG", QString("[DVD COPY] %1% - %2 (elapsed: %3s)")
                           .arg(progress, 3)
                           .arg(filename.isEmpty() ? "processing..." : filename)
                           .arg(elapsed / 1000.0, 0, 'f', 1));
                
                // Debug any non-zero progress immediately
                if (!filename.isEmpty() && progress > 0) {
                    logMessage("DEBUG", QString("[PROGRESS DEBUG] File progress detected: %1 %2% from line: %3").arg(filename).arg(progress).arg(trimmedLine));
                    updateTreeItemWithProgress(filename, progress);
                }
                
                if (progress >= 100) {
                    s_filesProcessed++;
                    logMessage("DEBUG", QString("[DVD COPY] ? Completed file #%1: %2")
                               .arg(s_filesProcessed)
                               .arg(filename));
                    
                    // Additional debug: Track which files are being marked as complete and when
                    logMessage("DEBUG", QString("[100% DEBUG] File marked complete: %1").arg(filename));
                    logMessage("DEBUG", QString("[100% DEBUG] Robocopy line was: %1").arg(trimmedLine));
                    
                    // Verify file actually exists before marking as complete
                    QString expectedPath = QString("C:/Users/gurup/AppData/Local/Temp/Ekn_TempData/DicomFiles/%1").arg(filename);
                    QFileInfo checkFile(expectedPath);
                    
                    if (checkFile.exists() && checkFile.size() > 0) {
                        logMessage("DEBUG", QString("[VERIFICATION PASS] File exists with size: %1").arg(checkFile.size()));
                        updateTreeItemWithProgress(filename, progress);
                    } else {
                        logMessage("ERROR", QString("[VERIFICATION FAIL] File %1 reported 100% but doesn't exist or is empty!").arg(filename));
                        // Don't mark as complete yet - keep at 99%
                        updateTreeItemWithProgress(filename, 99);
                    }
                }
            }
        }
        
        // Look for file being copied (start of copy)
        if (trimmedLine.contains("New File") && !trimmedLine.contains("%")) {
            // Extract file size and filename
            static QRegularExpression newFilePattern(R"(\s*New File\s+(\d+)\s+(.+))");
            QRegularExpressionMatch match = newFilePattern.match(trimmedLine);
            if (match.hasMatch()) {
                qint64 fileSize = match.captured(1).toLongLong();
                QString filepath = match.captured(2).trimmed();
                QString filename = QFileInfo(filepath).fileName();
                
                logMessage("DEBUG", QString("[DVD COPY] ? Starting: %1 (%2 KB)")
                           .arg(filename)
                           .arg(fileSize / 1024));
            }
        }
        
        // Look for completion summary
        if (trimmedLine.contains("Total") || 
            trimmedLine.contains("Files :") ||
            trimmedLine.contains("Bytes :") ||
            trimmedLine.contains("Speed :") ||
            trimmedLine.contains("Ended :")) {
            logMessage("DEBUG", QString("[ROBOCOPY SUMMARY] %1").arg(trimmedLine));
        }
        
        // Log any errors or warnings
        if (trimmedLine.contains("ERROR") || 
            trimmedLine.contains("FAILED") ||
            trimmedLine.contains("Access denied")) {
            logMessage("ERROR", QString("[ROBOCOPY ERROR] %1").arg(trimmedLine));
        }
    }
}

void DicomViewer::autoSelectFirstCompletedImage()
{
    logMessage("DEBUG", "[AUTO SELECT] === Function called ===");
    logMessage("DEBUG", QString("[AUTO SELECT] m_dicomTree exists: %1").arg(m_dicomTree != nullptr));
    logMessage("DEBUG", QString("[AUTO SELECT] m_firstImageAutoSelected: %1").arg(m_firstImageAutoSelected));
    
    if (!m_dicomTree || m_firstImageAutoSelected) {
        logMessage("DEBUG", "[AUTO SELECT] Early return - tree null or already selected");
        return;
    }
    
    logMessage("DEBUG", "[AUTO SELECT] Looking for first completed image to auto-select...");
    logMessage("DEBUG", QString("[AUTO SELECT] Tree has %1 top level items").arg(m_dicomTree->topLevelItemCount()));
    
    // Recursive function to find the first DICOM image item (leaf node)
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> findFirstImageItem = 
        [&](QTreeWidgetItem* item) -> QTreeWidgetItem* {
        
        if (!item) {
            logMessage("DEBUG", "[AUTO SELECT] Null item passed to findFirstImageItem");
            return nullptr;
        }
        
        // Check if this is a leaf item (DICOM image) by checking if it has no children
        if (item->childCount() == 0) {
            logMessage("DEBUG", QString("[AUTO SELECT] Checking leaf item: %1").arg(item->text(0)));
            
            // Check if this is an image item from user data (more reliable than icon)
            QVariantList userData = item->data(0, Qt::UserRole).toList();
            logMessage("DEBUG", QString("[AUTO SELECT]   UserData size: %1").arg(userData.size()));
            
            if (userData.size() >= 2 && userData[0].toString() == "image") {
                QString filePath = userData[1].toString();
                QString fileName = QFileInfo(filePath).fileName();
                
                logMessage("DEBUG", QString("[AUTO SELECT]   Is image item, file: %1").arg(fileName));
                logMessage("DEBUG", QString("[AUTO SELECT]   In completed files: %1").arg(m_fullyCompletedFiles.contains(fileName)));
                logMessage("DEBUG", QString("[AUTO SELECT]   File exists: %1").arg(QFile::exists(filePath)));
                
                // Check if this file is completed or if it's available locally
                if (m_fullyCompletedFiles.contains(fileName) || QFile::exists(filePath)) {
                    logMessage("DEBUG", QString("[AUTO SELECT] ? Found completed image item: %1 (file: %2)").arg(item->text(0)).arg(fileName));
                    return item;
                }
            }
            
            // Fallback: Check icon (original logic for cases where user data isn't set)
            QIcon itemIcon = item->icon(0);
            if (!itemIcon.isNull()) {
                logMessage("DEBUG", QString("[AUTO SELECT] Found potential image item by icon: %1").arg(item->text(0)));
                return item;
            }
            
            logMessage("DEBUG", QString("[AUTO SELECT]   No match for: %1").arg(item->text(0)));
        }
        
        // Recursively search children
        for (int i = 0; i < item->childCount(); ++i) {
            QTreeWidgetItem* result = findFirstImageItem(item->child(i));
            if (result) {
                return result;
            }
        }
        
        return nullptr;
    };
    
    // Search from root level
    for (int i = 0; i < m_dicomTree->topLevelItemCount(); ++i) {
        logMessage("DEBUG", QString("[AUTO SELECT] Searching top level item %1: %2").arg(i).arg(m_dicomTree->topLevelItem(i)->text(0)));
        QTreeWidgetItem* firstImage = findFirstImageItem(m_dicomTree->topLevelItem(i));
        if (firstImage) {
            logMessage("DEBUG", QString("[AUTO SELECT] ? Auto-selecting first completed image: %1").arg(firstImage->text(0)));
            
            // Expand parent items to make the selection visible
            QTreeWidgetItem* parent = firstImage->parent();
            while (parent) {
                logMessage("DEBUG", QString("[AUTO SELECT]   Expanding parent: %1").arg(parent->text(0)));
                parent->setExpanded(true);
                parent = parent->parent();
            }
            
            // Select the item - this will trigger onTreeItemSelected and load the image
            logMessage("DEBUG", "[AUTO SELECT] Setting current item and scrolling to it");
            m_dicomTree->setCurrentItem(firstImage);
            m_dicomTree->scrollToItem(firstImage);
            
            // Mark that we've auto-selected the first image
            m_firstImageAutoSelected = true;
            
            logMessage("DEBUG", "[AUTO SELECT] ? First image auto-selected and displayed!");
            return;
        } else {
            logMessage("DEBUG", QString("[AUTO SELECT] No suitable image found in top level item %1").arg(i));
        }
    }
    
    logMessage("DEBUG", "[AUTO SELECT] ? No completed images found yet for auto-selection");
}

void DicomViewer::onThumbnailGeneratedWithMetadata(const QString& filePath, const QPixmap& thumbnail, const QString& instanceNumber)
{
    // Find the corresponding list item and update it
    for (int i = 0; i < m_thumbnailList->count(); ++i) {
        QListWidgetItem* item = m_thumbnailList->item(i);
        if (item && item->data(Qt::UserRole).toString() == filePath) {
            if (!thumbnail.isNull()) {
                // Create final thumbnail with patient name overlay
                QPixmap finalThumbnail = thumbnail;
                QString patientName = item->data(Qt::UserRole + 2).toString();
                
                if (!patientName.isEmpty()) {
                    QPainter painter(&finalThumbnail);
                    painter.setRenderHint(QPainter::Antialiasing);
                    painter.setRenderHint(QPainter::TextAntialiasing);
                    
                    // Create taller overlay to prevent text clipping
                    int overlayHeight = 26;
                    QRect textRect(0, finalThumbnail.height() - overlayHeight, finalThumbnail.width(), overlayHeight);
                    painter.fillRect(textRect, QColor(0, 0, 0, 180)); // Semi-transparent black
                    
                    // Setup font with proper size for the overlay height
                    QFont font("Arial", 7, QFont::Bold);
                    painter.setFont(font);
                    QFontMetrics fontMetrics(font);
                    
                    // Elide text if it's too long to fit with padding
                    int textPadding = 6;
                    QString elidedText = fontMetrics.elidedText(patientName, Qt::ElideRight, textRect.width() - textPadding);
                    
                    // Create a slightly smaller rect for text to ensure proper vertical centering
                    QRect adjustedTextRect = textRect.adjusted(textPadding/2, 2, -textPadding/2, -2);
                    
                    // Draw patient name in white text with proper alignment
                    painter.setPen(QPen(Qt::white));
                    painter.drawText(adjustedTextRect, Qt::AlignCenter | Qt::AlignVCenter, elidedText);
                    
                    painter.end();
                }
                
                item->setIcon(QIcon(finalThumbnail));
                
                // Ensure consistent item size to prevent stretching - updated for new thumbnail size
                item->setSizeHint(QSize(190, 150));
                
                // Store metadata in item data for later use
                QVariantList metadata;
                metadata << filePath << instanceNumber;
                item->setData(Qt::UserRole + 1, metadata);
                
                // Don't set display text - instance number is already drawn in the thumbnail overlay
                // item->setText(instanceNumber);  // Removed to prevent duplicate instance number display
                
                logMessage("DEBUG", QString("Updated thumbnail for: %1 with instance number: %2").arg(QFileInfo(filePath).baseName()).arg(instanceNumber));
            }
            break;
        }
    }
    
    ++m_completedThumbnails;
    int completed = m_completedThumbnails;
    int total = m_totalThumbnails;
    logMessage("DEBUG", QString("Thumbnail progress: %1 of %2").arg(completed).arg(total));
    
    // Update status bar with thumbnail generation progress (batch updates to avoid UI spam)
    if (completed % 3 == 0 || completed == total) { // Update every 3rd completion or on final
        int progressPercent = (completed * 100) / total;
        updateStatusBar(QString("Generating thumbnails... (%1/%2)").arg(completed).arg(total), progressPercent);
    }
}

void DicomViewer::onThumbnailGenerated(const QString& filePath, const QPixmap& thumbnail)
{
    // Find the corresponding list item and update it
    for (int i = 0; i < m_thumbnailList->count(); ++i) {
        QListWidgetItem* item = m_thumbnailList->item(i);
        if (item && item->data(Qt::UserRole).toString() == filePath) {
            if (!thumbnail.isNull()) {
                item->setIcon(QIcon(thumbnail));
                
                // Ensure consistent item size to prevent stretching
                item->setSizeHint(QSize(250, 180));
                
                // Use instance number from thumbnail data (passed from background thread)
                // Extract from item data that was set during thumbnail generation
                QVariantList thumbnailData = item->data(Qt::UserRole + 1).toList();
                QString instanceNumber = "1"; // Default fallback
                
                if (thumbnailData.size() > 1) {
                    instanceNumber = thumbnailData[1].toString();
                }
                
                item->setText(instanceNumber);
                
                logMessage("DEBUG", QString("Updated thumbnail for: %1 with instance number: %2").arg(QFileInfo(filePath).baseName()).arg(instanceNumber));
            }
            break;
        }
    }
    
    m_completedThumbnails++;
    logMessage("DEBUG", QString("Thumbnail progress: %1 of %2").arg(m_completedThumbnails).arg(m_totalThumbnails));
}

void DicomViewer::onAllThumbnailsGenerated()
{
    logMessage("DEBUG", "Thumbnail generation completed! Showing thumbnail panel.");
    
    // Stop file availability monitoring since thumbnails are complete
    stopFileAvailabilityMonitoring();
    
    // Clean up thread pool tasks (they auto-delete)
    // Mark thumbnails as complete
    m_allThumbnailsComplete = true;
    
    // Only show the thumbnail panel if NO DVD/copy operations are in progress
    // Wait until all operations (detection, copy, and thumbnails) are complete
    if (!m_copyInProgress && !m_dvdDetectionInProgress) {
        // Show the thumbnail panel after all thumbnails are generated and no copy operations
        m_thumbnailPanel->setVisible(true);
        updateStatusBar("Ready", -1);
        logMessage("DEBUG", "Thumbnail panel shown - no operations in progress and all thumbnails generated");
        logMessage("DEBUG", "[THUMBNAIL PANEL] *** PANEL NOW VISIBLE *** - All thumbnails generated and no copy operations");
    } else {
        logMessage("DEBUG", QString("Thumbnail generation complete, but operations still in progress - panel stays hidden: copyInProgress: %1, dvdDetectionInProgress: %2")
                 .arg(m_copyInProgress).arg(m_dvdDetectionInProgress));
        logMessage("DEBUG", "[THUMBNAIL PANEL] Generation complete but panel stays hidden - operations in progress");
    }
    
    // Apply pending tree selection if any
    if (!m_pendingTreeSelection.isEmpty() && m_thumbnailList) {
        for (int i = 0; i < m_thumbnailList->count(); ++i) {
            QListWidgetItem* item = m_thumbnailList->item(i);
            if (item && item->data(Qt::UserRole).toString() == m_pendingTreeSelection) {
                m_thumbnailList->setCurrentItem(item);
                break;
            }
        }
        m_pendingTreeSelection.clear();
    } else if (m_thumbnailList && m_thumbnailList->count() > 0 && !m_thumbnailList->currentItem()) {
        // Only auto-select first image if no image is currently selected
        // This prevents interrupting an already playing first image
        for (int i = 0; i < m_thumbnailList->count(); ++i) {
            QListWidgetItem* item = m_thumbnailList->item(i);
            if (item) {
                m_thumbnailList->setCurrentItem(item);
                logMessage("DEBUG", QString("Auto-selected first image from DICOMDIR (no previous selection): %1").arg(item->data(Qt::UserRole).toString()));
                break;
            }
        }
    }
    
    // Count how many thumbnails were actually generated vs loading placeholders
    int actualThumbnails = 0;
    for (int i = 0; i < m_thumbnailList->count(); ++i) {
        QListWidgetItem* item = m_thumbnailList->item(i);
        if (item) {
            QIcon icon = item->icon();
            // Check if this is still a loading placeholder (basic check)
            if (!icon.isNull()) {
                actualThumbnails++;
            }
        }
    }
    
    logMessage("DEBUG", QString("Thumbnail panel is now visible with %1 actual thumbnails out of %2 items").arg(actualThumbnails).arg(m_thumbnailList->count()));
    
    // If running from DVD and some thumbnails failed, they will be regenerated when copy completes
    if (m_copyInProgress && actualThumbnails < m_thumbnailList->count()) {
        logMessage("DEBUG", "Some thumbnails missing due to DVD copy in progress - will regenerate after copy completion");
    }
}

void DicomViewer::onFileReadyForThumbnail(const QString& fileName)
{
    // Mark file as ready for access in legacy system
    QMutexLocker fileLocker(&m_fileStatesMutex);
    m_fileReadyStates[fileName] = true;
    fileLocker.unlock(); // Release lock before other operations
    
    logMessage("DEBUG", QString("[FILE READY] File ready for thumbnail generation: %1").arg(fileName));
    
    // IMPORTANT: Update the proper FileState system
    // Construct the full file path using PathNormalizer for consistent path handling
    QString fullPath = PathNormalizer::constructFilePath(m_localDestPath, fileName);
    logMessage(LOG_DEBUG, QString("PathNormalizer: Constructed file path for ready notification: %1").arg(fullPath));
    if (QFile::exists(fullPath)) {
        // Update file state to Available - this is key for display monitor
        setFileState(fullPath, FileState::Available);
        logMessage("DEBUG", QString("[FILE READY] File state updated to Available: %1").arg(fullPath));
        
        // IMMEDIATE FIRST IMAGE DISPLAY: Check if this is the very first available image
        // This triggers when the tree icon changes from waiting to image/run icon
        if (!m_firstImageFound && m_currentlyDisplayedPath.isEmpty() && !isDisplayingAnything()) {
            // This is the first file to become available - display it immediately!
            m_firstImageFound = true;
            logMessage("DEBUG", QString("[FILE READY] *** FIRST AVAILABLE FILE *** - Triggering immediate display: %1").arg(fullPath));
            
            // Stop the FirstImageMonitor since we found and are displaying the first image
            stopFirstImageMonitor();
            
            // Request immediate display of first available image
            requestDisplay(fullPath);
        } else {
            // Log for timing analysis
            static bool firstFileLogged = false;
            if (!firstFileLogged) {
                firstFileLogged = true;
                logMessage("DEBUG", QString("[FILE READY] First file available but conditions not met - FirstImageFound: %1, DisplayPath: %2, IsDisplaying: %3").arg(m_firstImageFound).arg(m_currentlyDisplayedPath).arg(isDisplayingAnything()));
            }
        }
        
        // NOTE: FirstImageMonitor is backup - immediate display above should handle first image
        // This avoids triggering display requests for every single file completion
        
        /*
        // OLD APPROACH - DISABLED: Too many triggers for large DVD copies
        // Check if this is the first available file and no image is displayed yet
        if (m_currentlyDisplayedPath.isEmpty() && !isDisplayingAnything()) {
            logMessage("DEBUG", QString("[FILE READY] First available file - requesting immediate display: %1").arg(fullPath));
            // Request immediate display of first available image
            requestDisplay(fullPath);
        }
        */
    } else {
        logMessage("WARNING", QString("[FILE READY] File does not exist at expected path: %1").arg(fullPath));
    }
    
    // Process any pending selections for this file
    QMutexLocker pendingLocker(&m_pendingSelectionsMutex);
    QQueue<QString> remainingSelections;
    
    while (!m_pendingSelections.isEmpty()) {
        QString pendingPath = m_pendingSelections.dequeue();
        if (QFileInfo(pendingPath).fileName() == fileName) {
            // Process this queued selection
            QTimer::singleShot(0, [this, pendingPath]() {
                // Find and select the thumbnail item
                for (int i = 0; i < m_thumbnailList->count(); ++i) {
                    QListWidgetItem* item = m_thumbnailList->item(i);
                    if (item && item->data(Qt::UserRole).toString() == pendingPath) {
                        m_thumbnailList->setCurrentItem(item);
                        break;
                    }
                }
            });
        } else {
            remainingSelections.enqueue(pendingPath);
        }
    }
    
    m_pendingSelections = remainingSelections;
}

// ========== STATE MANAGEMENT IMPLEMENTATION ==========

FileState DicomViewer::getFileState(const QString& filePath) const
{
    QMutexLocker locker(&m_fileStatesMutex);
    
    // Normalize path case to ensure consistent lookups
    QString normalizedPath = PathNormalizer::normalize(filePath);
    return m_fileStates.value(normalizedPath, FileState::NotReady);
}

void DicomViewer::setFileState(const QString& filePath, FileState state)
{
    QMutexLocker locker(&m_fileStatesMutex);
    
    // Normalize path case to ensure consistent storage
    QString normalizedPath = PathNormalizer::normalize(filePath);
    FileState oldState = m_fileStates.value(normalizedPath, FileState::NotReady);
    if (oldState != state) {
        m_fileStates[normalizedPath] = state;
        
        // ENHANCED LOGGING FOR FILE STATE CHANGES
        QString stateNames[] = {"NotReady", "Copying", "Available", "DisplayReady"};
        QString oldStateName = (static_cast<int>(oldState) < 4) ? stateNames[static_cast<int>(oldState)] : "Unknown";
        QString newStateName = (static_cast<int>(state) < 4) ? stateNames[static_cast<int>(state)] : "Unknown";
        
        logMessage("INFO", QString("[FILE STATE CHANGE] %1").arg(QFileInfo(filePath).fileName()));
        logMessage("INFO", QString("[FILE STATE CHANGE] Path: %1").arg(filePath));
        logMessage("INFO", QString("[FILE STATE CHANGE] %1 (%2) -> %3 (%4)")
                 .arg(oldStateName).arg(static_cast<int>(oldState))
                 .arg(newStateName).arg(static_cast<int>(state)));
        logMessage("DEBUG", QString("[FILE STATE] %1: %2 -> %3")
                 .arg(filePath).arg(static_cast<int>(oldState)).arg(static_cast<int>(state)));
        
        // Handle DisplayReady state transitions
        if (state == FileState::DisplayReady) {
            // Clear previous DisplayReady file
            if (!m_currentDisplayReadyFile.isEmpty() && m_currentDisplayReadyFile != filePath) {
                m_fileStates[m_currentDisplayReadyFile] = FileState::Available;
                logMessage("DEBUG", QString("[FILE STATE] Cleared DisplayReady: %1").arg(m_currentDisplayReadyFile));
            }
            m_currentDisplayReadyFile = filePath;
        } else if (m_currentDisplayReadyFile == filePath) {
            m_currentDisplayReadyFile.clear();
        }
        
        // NEW: Check if all files are now Available and trigger thumbnails if so
        if (state == FileState::Available) {
            // Use a timer to defer the check to avoid blocking and allow batch updates
            QTimer::singleShot(10, this, [this]() {
                checkAllFilesAvailableAndTriggerThumbnails();
            });
        }
    }
}

bool DicomViewer::isFileAvailable(const QString& filePath) const
{
    FileState state = getFileState(filePath);
    return state == FileState::Available || state == FileState::DisplayReady;
}

bool DicomViewer::isFileDisplayReady(const QString& filePath) const
{
    return getFileState(filePath) == FileState::DisplayReady;
}

ThumbnailState DicomViewer::getThumbnailState(const QString& filePath) const
{
    QMutexLocker locker(&m_thumbnailStatesMutex);
    return m_thumbnailStates.value(filePath, ThumbnailState::NotGenerated);
}

void DicomViewer::setThumbnailState(const QString& filePath, ThumbnailState state)
{
    QMutexLocker locker(&m_thumbnailStatesMutex);
    
    ThumbnailState oldState = m_thumbnailStates.value(filePath, ThumbnailState::NotGenerated);
    if (oldState != state) {
        m_thumbnailStates[filePath] = state;
        logMessage("DEBUG", QString("[THUMBNAIL STATE] %1: %2 -> %3")
                 .arg(filePath).arg(static_cast<int>(oldState)).arg(static_cast<int>(state)));
    }
}

bool DicomViewer::areAllThumbnailsReady() const
{
    QMutexLocker locker(&m_thumbnailStatesMutex);
    
    // Check if all thumbnail states are Ready
    for (auto it = m_thumbnailStates.begin(); it != m_thumbnailStates.end(); ++it) {
        if (it.value() != ThumbnailState::Ready) {
            logMessage("DEBUG", QString("[THUMBNAIL CHECK] Not ready: %1 State: %2")
                     .arg(it.key()).arg(static_cast<int>(it.value())));
            return false;
        }
    }
    
    // If we have no thumbnails tracked yet, they're not ready
    if (m_thumbnailStates.isEmpty()) {
        logMessage("DEBUG", "[THUMBNAIL CHECK] No thumbnails tracked yet");
        return false;
    }
    
    logMessage("DEBUG", QString("[THUMBNAIL CHECK] All %1 thumbnails are Ready").arg(m_thumbnailStates.size()));
    return true;
}

void DicomViewer::checkAllFilesAvailableAndTriggerThumbnails()
{
    // Skip if monitoring is not active
    if (!m_fileAvailabilityMonitoringActive) {
        logMessage("DEBUG", "[FILE AVAILABILITY MONITOR] Monitoring not active - skipping check");
        return;
    }
    
    logMessage("DEBUG", "[FILE AVAILABILITY MONITOR] Checking if all files are now Available...");
    
    // Skip if thumbnail generation is already active
    if (m_thumbnailGenerationActive) {
        logMessage("DEBUG", "[FILE AVAILABILITY MONITOR] Thumbnail generation already active - skipping check");
        return;
    }
    
    // Skip if thumbnails are already complete
    if (m_allThumbnailsComplete) {
        logMessage("DEBUG", "[FILE AVAILABILITY MONITOR] Thumbnails already complete - skipping check");
        return;
    }
    
    if (areAllFilesComplete()) {
        logMessage("INFO", "[FILE AVAILABILITY MONITOR] *** ALL FILES NOW AVAILABLE *** - Triggering thumbnail generation");
        
        // Update status and trigger thumbnail generation
        updateStatusBar("All files available - Generating thumbnails...", 0);
        
        // Trigger thumbnail generation through the existing panel update mechanism
        updateThumbnailPanel();
    } else {
        // Log current availability status for debugging
        QMutexLocker locker(&m_fileStatesMutex);
        int totalFiles = getTotalFileCount();
        int availableFiles = 0;
        for (auto it = m_fileStates.begin(); it != m_fileStates.end(); ++it) {
            if (it.value() == FileState::Available || it.value() == FileState::DisplayReady) {
                availableFiles++;
            }
        }
        logMessage("DEBUG", QString("[FILE AVAILABILITY MONITOR] Still waiting: %1/%2 files available")
                 .arg(availableFiles).arg(totalFiles));
    }
}

void DicomViewer::startFileAvailabilityMonitoring()
{
    logMessage("INFO", "[FILE AVAILABILITY MONITOR] *** STARTING file availability monitoring ***");
    m_fileAvailabilityMonitoringActive = true;
}

void DicomViewer::stopFileAvailabilityMonitoring()
{
    logMessage("INFO", "[FILE AVAILABILITY MONITOR] *** STOPPING file availability monitoring ***");
    m_fileAvailabilityMonitoringActive = false;
}

bool DicomViewer::areAllFilesComplete() const
{
    QMutexLocker locker(&m_fileStatesMutex);
    
    int totalFiles = getTotalFileCount();
    int completeFiles = 0;
    
    // Count files that are Available or DisplayReady
    for (auto it = m_fileStates.begin(); it != m_fileStates.end(); ++it) {
        FileState state = it.value();
        if (state == FileState::Available || state == FileState::DisplayReady) {
            completeFiles++;
        }
    }
    
    bool allComplete = (completeFiles >= totalFiles) && (totalFiles > 0);
    logMessage("DEBUG", QString("[FILE COMPLETION] %1 of %2 files complete (All complete: %3)")
             .arg(completeFiles).arg(totalFiles).arg(allComplete ? "YES" : "NO"));
    
    return allComplete;
}

int DicomViewer::getTotalFileCount() const
{
    if (!m_dicomReader) {
        logMessage("DEBUG", "[TOTAL FILE COUNT] DicomReader not available");
        return 0;
    }
    
    // Get total file count from the DicomReader
    int totalCount = m_dicomReader->getTotalImages();
    logMessage("DEBUG", QString("[TOTAL FILE COUNT] Total files from DicomReader: %1").arg(totalCount));
    
    return totalCount;
}

// ========== SELECTION GUARD IMPLEMENTATION ==========

bool DicomViewer::beginSelection(const QString& filePath)
{
    QMutexLocker locker(&m_selectionMutex);
    
    if (m_selectionInProgress) {
        logMessage("DEBUG", QString("[SELECTION GUARD] Selection already in progress - ignoring %1").arg(filePath));
        return false;
    }
    
    // Check for DisplayReady optimization
    FileState currentState = getFileState(filePath);
    if (currentState == FileState::DisplayReady && m_currentDisplayReadyFile == filePath) {
        logMessage("DEBUG", QString("[DISPLAY READY] File already displayed and ready - ignoring %1").arg(filePath));
        return false;
    }
    
    // Check for duplicate selection
    if (m_lastSelectedFilePath == filePath) {
        logMessage("DEBUG", QString("[DUPLICATE] Same file selected again - ignoring %1").arg(filePath));
        return false;
    }
    
    m_selectionInProgress = true;
    m_lastSelectedFilePath = filePath;
    logMessage("DEBUG", QString("[SELECTION GUARD] Beginning selection for: %1").arg(filePath));
    return true;
}

void DicomViewer::endSelection()
{
    QMutexLocker locker(&m_selectionMutex);
    m_selectionInProgress = false;
    logMessage("DEBUG", "[SELECTION GUARD] Selection completed");
}

bool DicomViewer::isSelectionInProgress() const
{
    QMutexLocker locker(&m_selectionMutex);
    return m_selectionInProgress;
}

void DicomViewer::autoSelectFirstAvailableImage()
{
    if (!m_dicomTree) return;
    
    logMessage("DEBUG", "[AUTO SELECT] Searching for first available image...");
    
    // Find first Available file
    QTreeWidgetItemIterator it(m_dicomTree);
    while (*it) {
        QTreeWidgetItem* item = *it;
        
        if (isImageItem(item)) {
            QVariantList userData = item->data(0, Qt::UserRole).toList();
            if (userData.size() >= 2) {
                QString filePath = userData[1].toString();
                FileState state = getFileState(filePath);
                
                if (state == FileState::Available) {
                    logMessage("DEBUG", QString("[AUTO SELECT] Selecting first available image: %1").arg(filePath));
                    
                    // Use Qt's selection mechanism instead of direct function call
                    m_dicomTree->setCurrentItem(item);
                    // This will trigger onTreeItemSelected through Qt's signal/slot mechanism
                    // which properly respects our selection guards
                    return;
                }
            }
        }
        ++it;
    }
    
    logMessage("DEBUG", "[AUTO SELECT] No available images found for auto-selection");
}

void DicomViewer::synchronizeThumbnailSelection(const QString& filePath)
{
    if (!m_thumbnailList) {
        return;
    }
    
    // Try to find thumbnail by exact path match first
    bool found = false;
    for (int i = 0; i < m_thumbnailList->count(); ++i) {
        QListWidgetItem* item = m_thumbnailList->item(i);
        if (item && item->data(Qt::UserRole).toString() == filePath) {
            m_thumbnailList->setCurrentItem(item);
            logMessage("DEBUG", QString("[THUMBNAIL SYNC] Selected thumbnail for: %1").arg(filePath));
            found = true;
            break;
        }
    }
    
    // If not found, try with normalized path comparison for cross-compatibility
    if (!found) {
        QString normalizedPath = PathNormalizer::normalize(filePath);
        for (int i = 0; i < m_thumbnailList->count(); ++i) {
            QListWidgetItem* item = m_thumbnailList->item(i);
            QString thumbnailPath = item->data(Qt::UserRole).toString();
            if (item && (PathNormalizer::normalize(thumbnailPath) == normalizedPath)) {
                m_thumbnailList->setCurrentItem(item);
                logMessage("DEBUG", QString("[THUMBNAIL SYNC] Selected thumbnail via normalized match: %1 -> %2").arg(filePath).arg(thumbnailPath));
                found = true;
                break;
            }
        }
    }
    
    if (!found) {
        logMessage("DEBUG", QString("[THUMBNAIL SYNC] No thumbnail found for: %1").arg(filePath));
    }
}

void DicomViewer::initializeFileStatesFromTree()
{
    if (!m_dicomTree) {
        return;
    }
    
    logMessage("DEBUG", "[FILE STATE INIT] Initializing file states from tree...");
    
    // Iterate through all tree items and mark existing files as Available
    QTreeWidgetItemIterator it(m_dicomTree);
    int availableCount = 0;
    
    while (*it) {
        QTreeWidgetItem* item = *it;
        QVariantList userData = item->data(0, Qt::UserRole).toList();
        
        if (userData.size() >= 2) {
            QString itemType = userData[0].toString();
            QString filePath = userData[1].toString();
            
            if (itemType == "image" || itemType == "report") {
                // Check current state first - preserve existing Available states
                FileState currentState = getFileState(filePath);
                QFileInfo fileInfo(filePath);
                
                if (fileInfo.exists() && fileInfo.isReadable()) {
                    // Only update to Available if not already Available (preserve dynamic tracking)
                    if (currentState != FileState::Available && currentState != FileState::DisplayReady) {
                        setFileState(filePath, FileState::Available);
                        setThumbnailState(filePath, ThumbnailState::Queued); // Ready for thumbnail generation
                        availableCount++;
                        logMessage("DEBUG", QString("[FILE STATE INIT] Marked as Available: %1").arg(filePath));
                    } else {
                        logMessage("DEBUG", QString("[FILE STATE INIT] Preserved existing state (%1): %2").arg(static_cast<int>(currentState)).arg(filePath));
                        if (currentState == FileState::Available) availableCount++;
                    }
                } else {
                    // NEVER downgrade Available or DisplayReady files - preserve copy progress
                    if (currentState == FileState::NotReady) {
                        setFileState(filePath, FileState::NotReady);
                        logMessage("DEBUG", QString("[FILE STATE INIT] File not accessible: %1").arg(filePath));
                    } else {
                        logMessage("DEBUG", QString("[FILE STATE INIT] Preserving existing state (%1) for file that appears missing: %2").arg(static_cast<int>(currentState)).arg(filePath));
                    }
                }
            }
        }
        
        ++it;
    }
    
    logMessage("DEBUG", QString("[FILE STATE INIT] Initialized %1 files as Available").arg(availableCount));
}

// ========== DISPLAY MONITOR SYSTEM ==========

void DicomViewer::initializeDisplayMonitor()
{
    logMessage("DEBUG", "[DISPLAY MONITOR] Initializing display monitor system...");
    
    // Create the display monitor timer
    m_displayMonitor = new QTimer(this);
    m_displayMonitor->setSingleShot(false);
    m_displayMonitor->setInterval(1000); // Check every 1 second
    
    // Connect timer to monitor function
    connect(m_displayMonitor, &QTimer::timeout, this, &DicomViewer::checkAndUpdateDisplay);
    
    m_displayMonitorActive = false;
    m_currentlyDisplayedPath.clear();
    m_requestedDisplayPath.clear();
    
    logMessage("DEBUG", "[DISPLAY MONITOR] Display monitor initialized");
}

void DicomViewer::startDisplayMonitor()
{
    if (m_displayMonitor && !m_displayMonitor->isActive()) {
        m_displayMonitorActive = true;
        m_displayMonitor->start();
        logMessage("DEBUG", "[DISPLAY MONITOR] Display monitor started");
    }
}

void DicomViewer::stopDisplayMonitor()
{
    if (m_displayMonitor && m_displayMonitor->isActive()) {
        m_displayMonitor->stop();
        m_displayMonitorActive = false;
        logMessage("DEBUG", "[DISPLAY MONITOR] Display monitor stopped");
    }
}

void DicomViewer::requestDisplay(const QString& filePath)
{
    QMutexLocker locker(&m_displayRequestMutex);
    
    if (m_requestedDisplayPath == filePath) {
        logMessage("DEBUG", QString("[DISPLAY MONITOR] Same display request - ignoring: %1").arg(filePath));
        return;
    }
    
    m_requestedDisplayPath = filePath;
    logMessage("DEBUG", QString("[DISPLAY MONITOR] Display requested: %1").arg(filePath));
    
    // Start monitor if not already running
    if (!m_displayMonitor->isActive()) {
        startDisplayMonitor();
    }
}

void DicomViewer::checkAndUpdateDisplay()
{
    QMutexLocker locker(&m_displayRequestMutex);
    
    if (!m_displayMonitorActive) {
        return;
    }
    
    // Check if we have a specific display request
    if (!m_requestedDisplayPath.isEmpty()) {
        // Validate the requested file state
        FileState fileState = getFileState(m_requestedDisplayPath);
        
        if (fileState == FileState::Available || fileState == FileState::DisplayReady) {
            if (m_currentlyDisplayedPath != m_requestedDisplayPath) {
                logMessage("DEBUG", QString("[DISPLAY MONITOR] Displaying requested image: %1").arg(m_requestedDisplayPath));
                
                // Release mutex before calling loadDicomImage to prevent deadlock
                locker.unlock();
                
                // Actually load the image
                loadDicomImage(m_requestedDisplayPath);
                
                // Relock and update state
                locker.relock();
                m_currentlyDisplayedPath = m_requestedDisplayPath;
                setFileState(m_requestedDisplayPath, FileState::DisplayReady);
                
                // Clear the request since it's fulfilled
                m_requestedDisplayPath.clear();
                
                logMessage("DEBUG", QString("[DISPLAY MONITOR] Display completed: %1").arg(m_currentlyDisplayedPath));
                
                // Start file availability monitoring after first successful display
                // This delays thumbnail generation until first image is shown
                if (!m_fileAvailabilityMonitoringActive) {
                    logMessage("DEBUG", "[DISPLAY MONITOR] Starting file availability monitoring after first image display");
                    startFileAvailabilityMonitoring();
                    // Trigger immediate check since all files might already be available
                    QTimer::singleShot(50, this, [this]() {
                        checkAllFilesAvailableAndTriggerThumbnails();
                    });
                }
            } else {
                // Already displaying this image
                m_requestedDisplayPath.clear();
                logMessage("DEBUG", "[DISPLAY MONITOR] Already displaying requested image");
            }
        } else {
            logMessage("DEBUG", QString("[DISPLAY MONITOR] Requested file not available yet: %1 State: %2").arg(m_requestedDisplayPath).arg(static_cast<int>(fileState)));
        }
        return;
    }
    
    // No specific request - check if we need to display something automatically
    if (isDisplayingAnything()) {
        return; // Already displaying something
    }
    
    // Nothing displayed - find first available image
    if (!m_dicomTree) {
        return;
    }
    
    QTreeWidgetItemIterator it(m_dicomTree);
    while (*it) {
        QTreeWidgetItem* item = *it;
        QVariantList userData = item->data(0, Qt::UserRole).toList();
        
        if (userData.size() >= 2 && userData[0].toString() == "image") {
            QString filePath = userData[1].toString();
            FileState state = getFileState(filePath);
            
            if (state == FileState::Available) {
                logMessage("DEBUG", QString("[DISPLAY MONITOR] Auto-displaying first available image: %1").arg(filePath));
                
                // Release mutex before calling loadDicomImage
                locker.unlock();
                
                // Load the image
                loadDicomImage(filePath);
                
                // Update tree selection to match
                m_dicomTree->setCurrentItem(item);
                synchronizeThumbnailSelection(filePath);
                
                // Relock and update state
                locker.relock();
                m_currentlyDisplayedPath = filePath;
                setFileState(filePath, FileState::DisplayReady);
                
                logMessage("DEBUG", QString("[DISPLAY MONITOR] Auto-display completed: %1").arg(filePath));
                return;
            }
        }
        ++it;
    }
    
    logMessage("DEBUG", "[DISPLAY MONITOR] No available images found for auto-display");
}

bool DicomViewer::isDisplayingAnything() const
{
    // Check if we have a valid image displayed
    if (!m_currentlyDisplayedPath.isEmpty()) {
        return true;
    }
    
    // Also check if the main image widget is showing something meaningful
    if (m_mainStack && m_mainStack->currentWidget() == m_imageWidget) {
        if (m_pixmapItem && !m_pixmapItem->pixmap().isNull()) {
            return true;
        }
    }
    
    return false;
}

void DicomViewer::clearCurrentDisplay()
{
    QMutexLocker locker(&m_displayRequestMutex);
    m_currentlyDisplayedPath.clear();
    logMessage("DEBUG", "[DISPLAY MONITOR] Current display cleared");
}

void DicomViewer::startFirstImageMonitor()
{
    if (m_firstImageFound || (m_firstImageMonitor && m_firstImageMonitor->isActive())) {
        return; // Already found or monitoring
    }
    
    logMessage("DEBUG", "[FIRST IMAGE MONITOR] Starting first image monitoring timer");
    
    // Create timer in main thread
    if (!m_firstImageMonitor) {
        m_firstImageMonitor = new QTimer(this);
        m_firstImageMonitor->setInterval(150); // Check every 150ms
        m_firstImageMonitor->setSingleShot(false);
        
        // Connect timer to checking function
        connect(m_firstImageMonitor, &QTimer::timeout, this, &DicomViewer::checkForFirstAvailableImage);
    }
    
    // Do an immediate check before starting the timer
    logMessage("DEBUG", "[FIRST IMAGE MONITOR] Performing immediate initial check");
    checkForFirstAvailableImage();
    
    // Start the timer only if we haven't found the first image yet
    if (!m_firstImageFound) {
        m_firstImageMonitor->start();
        logMessage("DEBUG", "[FIRST IMAGE MONITOR] Timer started");
    }
}

void DicomViewer::stopFirstImageMonitor()
{
    if (m_firstImageMonitor && m_firstImageMonitor->isActive()) {
        logMessage("DEBUG", "[FIRST IMAGE MONITOR] Stopping first image monitoring timer");
        m_firstImageMonitor->stop();
    }
}

void DicomViewer::checkForFirstAvailableImage()
{
    if (m_firstImageFound || !m_dicomTree) {
        return;
    }
    
    // Check if any image is currently being displayed
    if (isDisplayingAnything()) {
        m_firstImageFound = true;
        stopFirstImageMonitor();
        logMessage("DEBUG", "[FIRST IMAGE MONITOR] Image already displaying - stopping monitor");
        return;
    }
    
    // Log current state for debugging timing
    static int checkCount = 0;
    checkCount++;
    if (checkCount % 10 == 1) { // Log every 1.5 seconds to avoid spam
        logMessage("DEBUG", QString("[FIRST IMAGE MONITOR] Check #%1 - looking for first available image...").arg(checkCount));
    }
    
    // Look for the first available image in the tree
    QTreeWidgetItemIterator it(m_dicomTree);
    while (*it) {
        QTreeWidgetItem* item = *it;
        QVariantList userData = item->data(0, Qt::UserRole).toList();
        
        if (userData.size() >= 2 && userData[0].toString() == "image") {
            QString filePath = userData[1].toString();
            
            // Check file state with both original and normalized paths (like tree click handler)
            FileState originalState = FileState::NotReady;
            {
                QMutexLocker locker(&m_fileStatesMutex);
                originalState = m_fileStates.value(filePath, FileState::NotReady);
            }
            
            FileState normalizedState = getFileState(filePath); // This normalizes internally
            
            // Use the path that has a valid file state
            FileState effectiveState = originalState;
            QString effectivePath = filePath;
            
            // CRITICAL: In non-DVD mode, files exist at original path, not normalized path
            // Check file existence to determine which path to use
            bool originalExists = QFile::exists(filePath);
            QString normalizedPath = PathNormalizer::normalize(filePath);
            bool normalizedExists = QFile::exists(normalizedPath);
            
            if (originalExists && (originalState == FileState::Available || originalState == FileState::DisplayReady)) {
                // Best case: Original path exists and has valid state
                effectivePath = filePath;
                effectiveState = originalState;
            } else if (originalExists && originalState == FileState::NotReady && 
                       (normalizedState == FileState::Available || normalizedState == FileState::DisplayReady)) {
                // Non-DVD mode: File exists at original path but state stored under normalized path
                effectivePath = filePath;  // Use original path where file exists
                effectiveState = normalizedState;  // But use normalized state for validation
            } else if (normalizedExists && (normalizedState == FileState::Available || normalizedState == FileState::DisplayReady)) {
                // DVD mode: File exists at normalized path
                effectivePath = normalizedPath;
                effectiveState = normalizedState;
            } else if (originalExists) {
                // Fallback: Use original path if it exists, regardless of state
                effectivePath = filePath;
                effectiveState = originalState;
            } else {
                // Final fallback: Use normalized path
                effectivePath = normalizedPath;
                effectiveState = normalizedState;
            }
            
            if (checkCount % 10 == 1) { // Detailed logging every 1.5 seconds
                logMessage("DEBUG", QString("[FIRST IMAGE MONITOR] Checking file: %1 - Original State: %2, Normalized State: %3").arg(filePath).arg(static_cast<int>(originalState)).arg(static_cast<int>(normalizedState)));
            }
            
            if (effectiveState == FileState::Available) {
                m_firstImageFound = true;
                stopFirstImageMonitor();
                
                logMessage("DEBUG", QString("[FIRST IMAGE MONITOR] Found first available image after %1 checks: %2").arg(checkCount).arg(effectivePath));
                
                // Request display of the first available image
                requestDisplay(effectivePath);
                return;
            }
        }
        ++it;
    }
}


