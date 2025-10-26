#include "dicomviewer.h"
#include "DicomFrameProcessor.h"
#include "saveimagedialog.h"
#include "saverundialog.h"
#include "dvdcopyworker.h"

#include <chrono>
#include <cstdlib> // For std::exit
#include <QtWidgets/QApplication>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QToolButton>
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

    
    // Pipeline: Decompressed Image â†’ Window/Level â†’ H-Flip â†’ V-Flip â†’ Invert â†’ Display
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
            
            // Get the grayscale value from the display image (0-255)
            int displayGrayValue = qGray(pixel);
            
            // Scale back to original DICOM pixel value range based on BitsStored
            // For BitsStored=8: scale 0-255 stays as 0-255
            // For BitsStored=12: scale 0-255 to 0-4095
            // For BitsStored=16: scale 0-255 to 0-65535
            double maxPixelValue = (1 << m_bitsStored) - 1;  // 2^BitsStored - 1
            double originalPixelValue = (displayGrayValue / 255.0) * maxPixelValue;
            
            // Apply window/level algorithm using original DICOM pixel value range
            double windowedValue;
            if (m_windowWidth > 1) {  // Minimum meaningful width
                if (originalPixelValue <= minValue) {
                    windowedValue = 0.0;
                } else if (originalPixelValue >= maxValue) {
                    windowedValue = 255.0;
                } else {
                    // Scale within window range to 0-255
                    windowedValue = ((originalPixelValue - minValue) / m_windowWidth) * 255.0;
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

DicomViewer::DicomViewer(QWidget *parent)
    : QMainWindow(parent)
    , m_playbackController(nullptr)
    , m_inputHandler(nullptr)
    , m_centralWidget(nullptr)
    , m_topToolbar(nullptr)
    , m_closeButton(nullptr)
    , m_playAction(nullptr)
    , m_windowLevelToggleAction(nullptr)
    , m_leftSidebar(nullptr)
    , m_dicomTree(nullptr)
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
    , m_copyProgressTimer(nullptr)
    , m_copyInProgress(false)
    , m_currentCopyProgress(0)
    , m_dvdDetectionInProgress(false)
    , m_completedFiles()
    , m_fullyCompletedFiles()
    , m_workerReady(false)
    , m_firstImageAutoSelected(false)
    , m_progressWidget(nullptr)
    , m_progressLabel(nullptr)
    , m_progressBar(nullptr)
    , m_persistentSelectedStudyId("")
    , m_persistentSelectedSeriesId("")
    , m_persistentSelectedFileName("")
    , m_persistentSelectedPath()
    , m_selectionPersistenceEnabled(true)  // Enable selection persistence by default
    , m_statusBar(nullptr)
    , m_statusLabel(nullptr)
    , m_statusProgressBar(nullptr)
{
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
    
    // Setup paths for DVD copying
    m_localDestPath = QDir::tempPath() + "/Ekn_TempData/DicomFiles";
    
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
            background-color: #0078d7 !important;
            color: white !important;
        }
        QTreeWidget::item:selected:!focus {
            background-color: #0078d7 !important;
            color: white !important;
        }
        QTreeWidget::item:selected:focus {
            background-color: #0078d7 !important;
            color: white !important;
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
    qDebug() << "[DICOM INFO] DICOM info widget created successfully in constructor. Widget pointer:" << m_dicomInfoWidget;
    
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
    
    // Step 4: Add overlay labels
    createOverlayLabels(m_imageWidget);
    
    // Step 5: Create status bar
    createStatusBar();
    
    // Step 7: Initialize DVD worker thread first
    initializeDvdWorker();
    
    // Step 7: Auto-load DICOMDIR if present in executable directory
    // Use QTimer::singleShot to defer DVD operations until after constructor completes
    QTimer::singleShot(0, this, [this]() {
        autoLoadDicomdir();
    });
    
    // Step 8: Install event filters after full initialization
    installEventFilters();
}

DicomViewer::~DicomViewer()
{
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
    connect(m_dvdWorker, &DvdCopyWorker::workerError, 
            this, &DicomViewer::onWorkerError);
    connect(m_dvdWorker, &DvdCopyWorker::statusChanged, 
            this, [this](const QString& status) {
                qDebug() << "DVD Worker Status:" << status;
            });
    
    // Connect signal for sequential robocopy (only method used)
    bool seqConnected = connect(this, &DicomViewer::requestSequentialRobocopyStart,
                               m_dvdWorker, &DvdCopyWorker::startSequentialRobocopy,
                               Qt::QueuedConnection);
    qDebug() << "[DVD WORKER] Sequential robocopy signal connection established:" << (seqConnected ? "SUCCESS" : "FAILED");
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
    double currentCenter = m_imagePipeline->getWindowCenter();
    double currentWidth = m_imagePipeline->getWindowWidth();
    
    QString message = QString("Current Window/Level:\nCenter: %1\nWidth: %2\n\n(Custom dialog not yet implemented)")
                     .arg(currentCenter).arg(currentWidth);
    
    QMessageBox::information(this, "Window/Level", message);
}



void DicomViewer::zoomIn()
{
    if (m_graphicsView && m_zoomFactor < m_maxZoomFactor) {
        m_zoomFactor *= m_zoomIncrement;
        m_graphicsView->scale(m_zoomIncrement, m_zoomIncrement);
        updateZoomOverlay();
    }
}

void DicomViewer::zoomOut()
{
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
    }
}

void DicomViewer::onTreeItemSelected(QTreeWidgetItem *current, QTreeWidgetItem *previous)
{
    Q_UNUSED(previous)
    if (!current) {
        return;
    }
    
    // Store current selection for persistence across tree refreshes
    storeCurrentSelection();
    
    // Check what type of item this is
    QVariantList userData = current->data(0, Qt::UserRole).toList();
    
    if (userData.size() >= 2) {
        QString itemType = userData[0].toString();
        QString filePath = userData[1].toString();
        
        if (itemType == "image") {
            // This is an actual DICOM image
            loadDicomImage(filePath);
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
    } else {
        // No proper user data, just show selection
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
    qDebug() << "CloseEvent: Starting application shutdown...";
    
    // Stop all timers first
    if (m_playbackTimer && m_playbackTimer->isActive()) {
        m_playbackTimer->stop();
        qDebug() << "CloseEvent: Playback timer stopped";
    }
    
    if (m_progressiveTimer && m_progressiveTimer->isActive()) {
        m_progressiveTimer->stop();
        qDebug() << "CloseEvent: Progressive timer stopped";
    }
    
    if (m_copyProgressTimer && m_copyProgressTimer->isActive()) {
        m_copyProgressTimer->stop();
        qDebug() << "CloseEvent: Copy progress timer stopped";
    }
    
    // Stop and clean up progressive loader thread
    if (m_progressiveLoader) {
        qDebug() << "CloseEvent: Stopping progressive loader...";
        m_progressiveLoader->stop();
        m_progressiveLoader->wait(3000); // 3 second timeout
        delete m_progressiveLoader;
        m_progressiveLoader = nullptr;
        qDebug() << "CloseEvent: Progressive loader cleaned up";
    }
    
    // Robocopy process is now handled by DvdCopyWorker
    
    // Clean up DVD worker thread
    if (m_dvdWorkerThread) {
        qDebug() << "CloseEvent: Stopping DVD worker thread...";
        m_dvdWorkerThread->quit();
        if (!m_dvdWorkerThread->wait(3000)) { // 3 second timeout
            qDebug() << "CloseEvent: Force terminating DVD worker thread...";
            m_dvdWorkerThread->terminate();
            m_dvdWorkerThread->wait(1000);
        }
        qDebug() << "CloseEvent: DVD worker thread stopped";
    }
    
    // Force cleanup of any remaining resources
    qDebug() << "CloseEvent: Final cleanup and quit...";
    
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
    } else {
        // Show current pipeline values as fallback
        double pipelineCenter = m_imagePipeline->getWindowCenter();
        double pipelineWidth = m_imagePipeline->getWindowWidth();
        if (pipelineWidth > 0) {
            bottomRightText += QString("WL: %1 WW: %2").arg(pipelineCenter, 0, 'f', 0).arg(pipelineWidth, 0, 'f', 0);
        }
    }
    
    // Update the overlay labels
    m_overlayTopLeft->setText(topLeftText);
    m_overlayTopRight->setText(topRightText);
    m_overlayBottomLeft->setText(bottomLeftText);
    m_overlayBottomRight->setText(bottomRightText);
    
    // Update DICOM info panel if visible and we have a current image
    if (m_dicomInfoVisible && !m_currentImagePath.isEmpty()) {
        populateDicomInfo(m_currentImagePath);
    }
    
    // Update DICOM info panel if it's visible
    if (m_dicomInfoVisible && !m_currentImagePath.isEmpty()) {
        populateDicomInfo(m_currentImagePath);
    }
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
    
    // Sync current W/L values with the pipeline after processing
    m_currentWindowCenter = m_imagePipeline->getWindowCenter();
    m_currentWindowWidth = m_imagePipeline->getWindowWidth();
    
    updateImageDisplay();
}

void DicomViewer::resetZoomToFit()
{
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
    
    // Initialize with current values or defaults
    m_currentWindowCenter = m_imagePipeline->getWindowCenter();
    m_currentWindowWidth = m_imagePipeline->getWindowWidth();
    m_originalWindowCenter = m_currentWindowCenter;
    m_originalWindowWidth = m_currentWindowWidth;
    
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
    
    // Update values
    m_currentWindowCenter = newCenter;
    m_currentWindowWidth = newWidth;
    
    
    // Update the pipeline with new values
    m_imagePipeline->setWindowLevel(newCenter, newWidth);
    
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
    m_windowLevelModeEnabled = !m_windowLevelModeEnabled;
    

    
    // Enable/disable the pipeline's window/level processing
    if (m_windowLevelModeEnabled) {
        // Enable window/level interaction mode, but don't apply any values yet
        // The user needs to drag the mouse to start applying window/level
        // Don't enable pipeline processing yet - wait for user to drag
    } else {
        // Disable window/level processing - show original image
        m_imagePipeline->setWindowLevelEnabled(false);
        // Process through pipeline to show original image
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
    m_imagePipeline->setWindowLevel(center, width);
    // Only enable if toggle button is ON
    if (m_windowLevelModeEnabled) {
        m_imagePipeline->setWindowLevelEnabled(true);
    } else {
    }
    processThroughPipeline();
}

void DicomViewer::loadDicomDir(const QString& dicomdirPath)
{
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
        
        qDebug() << "[LOAD DICOMDIR] Tree populated, about to call detectAndStartDvdCopy()";
        
        // Start DVD detection and copy if needed
        detectAndStartDvdCopy();
        
        qDebug() << "[LOAD DICOMDIR] detectAndStartDvdCopy() completed";
        
        // Expand first level items and select first image if available
        expandFirstItems();
        
        // Update display message
        if (m_dicomReader->getTotalImages() > 0) {
            m_imageLabel->setText("DICOMDIR loaded successfully. Select an image to view.");
            // Clear status bar when DICOMDIR is loaded successfully
            updateStatusBar("Ready", -1);
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

void DicomViewer::expandFirstItems()
{
    if (m_dicomTree->topLevelItemCount() > 0) {
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
                    
                    // Select first image if this is the first patient
                    if (i == 0 && firstSeries->childCount() > 0) {
                        QTreeWidgetItem* firstImage = firstSeries->child(0);
                        m_dicomTree->setCurrentItem(firstImage);
                    }
                }
            }
        }
    } else {
    }
}

void DicomViewer::loadDicomImage(const QString& filePath)
{
#ifdef HAVE_DCMTK
    
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
        qDebug() << "[FILE ACCESS] File not yet completed:" << filename << "- copy still in progress";
        
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
    
    qDebug() << "[FILE ACCESS] File is ready for access:" << filename << "completed:" << fileIsCompleted;
    
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
                qDebug() << "DICOM file SOP Class UID:" << sopClassUID.c_str();
                
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
        bool foundWindowLevel = false;
        
        // Window Center (Level)
        if (dataset->findAndGetFloat64(DCM_WindowCenter, floatValue).good()) {
            m_currentWindowCenter = floatValue;
            m_originalWindowCenter = floatValue;
            foundWindowLevel = true;
        }
        
        // Window Width
        if (dataset->findAndGetFloat64(DCM_WindowWidth, floatValue).good()) {
            m_currentWindowWidth = floatValue;
            m_originalWindowWidth = floatValue;
            foundWindowLevel = foundWindowLevel && true;
        }
        
        // Store BitsStored for pipeline processing
        m_imagePipeline->setBitsStored(bitsStored);
        
        // If window/level values found, use them for pipeline processing

        
        if (foundWindowLevel && m_currentWindowWidth > 0) {
            m_imagePipeline->setWindowLevel(m_currentWindowCenter, m_currentWindowWidth);
            // Only enable if toggle button is ON
            if (m_windowLevelModeEnabled) {
                m_imagePipeline->setWindowLevelEnabled(true);
            } else {
            }
        } else {
            // Calculate window/level based on BitsStored when no DICOM values present
            // Formula: WindowWidth = 2^BitsStored - 1, WindowCenter = WindowWidth / 2
            // This utilizes the full dynamic range of the image data
            double maxPixelValue = (1 << bitsStored) - 1;  // 2^BitsStored - 1
            m_currentWindowWidth = maxPixelValue;
            m_currentWindowCenter = maxPixelValue / 2.0;
            m_originalWindowCenter = m_currentWindowCenter;
            m_originalWindowWidth = m_currentWindowWidth;
            m_imagePipeline->setWindowLevel(m_currentWindowCenter, m_currentWindowWidth);
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
        QTreeWidgetItem* firstItem = findFirstSelectableChild(topItem);
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

QTreeWidgetItem* DicomViewer::findItemByPath(const QStringList& path)
{
    if (!m_dicomTree || path.isEmpty()) {
        return nullptr;
    }
    
    // Start from top level items
    QTreeWidgetItem* currentItem = nullptr;
    
    // Find the root item (patient level)
    for (int i = 0; i < m_dicomTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* topItem = m_dicomTree->topLevelItem(i);
        if (topItem->text(0) == path[0]) {
            currentItem = topItem;
            break;
        }
    }
    
    if (!currentItem) {
        return nullptr;
    }
    
    // Navigate down the path
    for (int pathIndex = 1; pathIndex < path.size(); ++pathIndex) {
        const QString& targetText = path[pathIndex];
        QTreeWidgetItem* foundChild = nullptr;
        
        // Search through children
        for (int childIndex = 0; childIndex < currentItem->childCount(); ++childIndex) {
            QTreeWidgetItem* child = currentItem->child(childIndex);
            if (child->text(0) == targetText) {
                foundChild = child;
                break;
            }
        }
        
        if (!foundChild) {
            return nullptr;  // Path not found
        }
        
        currentItem = foundChild;
    }
    
    return currentItem;
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
    
    // First, check if ffmpeg is available in the system PATH
    QProcess testProcess;
    testProcess.start("ffmpeg", QStringList() << "-version");
    testProcess.waitForFinished(3000); // Wait up to 3 seconds
    
    if (testProcess.exitCode() != 0) {
        return false;
    }
    
    // Prepare FFmpeg command to create MP4 from JPEG frames
    // Command format: ffmpeg -framerate <fps> -i frame_%06d.jpg -c:v libx264 -pix_fmt yuv420p -crf 23 -y output.mp4
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
    
    
    // Execute FFmpeg process
    QProcess ffmpegProcess;
    ffmpegProcess.start("ffmpeg", arguments);
    
    // Wait for the process to complete (up to 60 seconds for video creation)
    if (!ffmpegProcess.waitForFinished(60000)) {
        ffmpegProcess.kill();
        return false;
    }
    
    // Check if the process completed successfully
    if (ffmpegProcess.exitCode() != 0) {
        return false;
    }
    
    // Verify that the output file was created
    if (!QFile::exists(outputPath)) {
        return false;
    }
    
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
        if (cleanUnit.contains("°") || cleanUnit.contains("�") || cleanUnit.toLower().contains("degree")) {
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
    qDebug() << "[DICOM INFO] toggleDicomInfo() called";
    
    if (!m_dicomInfoWidget) {
        qDebug() << "[DICOM INFO] ERROR: m_dicomInfoWidget is null!";
        return; // Widget should always exist now that it's created in layout
    }
    
    m_dicomInfoVisible = !m_dicomInfoVisible;
    qDebug() << "[DICOM INFO] Toggled to visible:" << m_dicomInfoVisible;
    
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
        qDebug() << "[DICOM INFO] Widget should now be visible. IsVisible:" << m_dicomInfoWidget->isVisible();
    } else {
        m_dicomInfoWidget->hide();
        qDebug() << "[DICOM INFO] Widget hidden";
    }
}

void DicomViewer::populateDicomInfo(const QString& filePath)
{
    if (!m_dicomInfoWidget || !m_dicomInfoTextEdit || filePath.isEmpty()) {
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
        
    } catch (...) {
        m_dicomInfoTextEdit->setPlainText("Error: Exception occurred while reading DICOM file");
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
        qDebug() << "[PERIODIC REFRESH] Checking for newly available files...";
        
        // Store current selection before tree refresh
        storeCurrentSelection();
        
        // Refresh file existence status in DicomReader
        m_dicomReader->refreshFileExistenceStatus();
        
        // Update the tree display to reflect new file availability
        m_dicomReader->populateTreeWidget(m_dicomTree);
        
        // Restore selection after tree refresh
        restoreStoredSelection();
        
        // Update the header to show current progress
        int totalPatients = m_dicomReader->getTotalPatients();
        int totalImages = m_dicomReader->getTotalImages();
        double overallProgress = m_dicomReader->calculateProgress();
        
        QString headerText = QString("All patients (Patients: %1, Images: %2) - %3% loaded")
                           .arg(totalPatients)
                           .arg(totalImages)
                           .arg(QString::number(overallProgress * 100, 'f', 1));
        
        m_dicomTree->setHeaderLabel(headerText);
        
        qDebug() << "[PERIODIC REFRESH] Overall progress:" << overallProgress * 100 << "% (" 
                 << (int)(overallProgress * totalImages) << "/" << totalImages << " files)";
    } else {
        // Stop timer if copy is no longer in progress
        if (m_copyProgressTimer && m_copyProgressTimer->isActive()) {
            m_copyProgressTimer->stop();
            qDebug() << "Stopped periodic tree refresh timer - copy completed";
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
            qDebug() << "File missing, starting DVD copy:" << path;
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
    qDebug() << "[MISSING FILES CHECK] Function called";
    
    // Check if we have loaded a DICOM tree and if any files are actually missing
    if (!m_dicomReader || !m_dicomTree) {
        qDebug() << "[MISSING FILES CHECK] No DICOM reader or tree available";
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
    
    qDebug() << "File check: " << missingCount << "missing out of" << totalCount << "total files";
    
    // Only consider it "missing files" if we have a significant number missing
    // This avoids triggering DVD copy for just a few missing files
    bool result = missingCount > 0 && (missingCount > totalCount * 0.1 || missingCount > 5);
    qDebug() << "[MISSING FILES CHECK] Result:" << result << "- Missing:" << missingCount << "Total:" << totalCount;
    return result;
}

QStringList DicomViewer::getOrderedFileList()
{
    QStringList orderedFiles;
    
    if (!m_dicomTree) {
        qDebug() << "[ERROR] No DICOM tree available for ordered file list";
        return orderedFiles;
    }
    
    // Safety check - ensure tree has items
    if (m_dicomTree->topLevelItemCount() == 0) {
        qDebug() << "[WARNING] DICOM tree is empty";
        return orderedFiles;
    }
    
    qDebug() << "Extracting ordered file list from tree view...";
    
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
                    qDebug() << QString("[ORDERED FILE %1] %2 (type: %3)").arg(orderedFiles.size()).arg(fileName).arg(itemType);
                }
            }
        }
        ++it;
    }
    
    qDebug() << QString("Extracted %1 files from tree view in display order").arg(orderedFiles.size());
    return orderedFiles;
}

void DicomViewer::detectAndStartDvdCopy()
{
    qDebug() << "[DVD DETECTION] detectAndStartDvdCopy() called";
    
    // Prevent multiple simultaneous detection operations
    if (m_dvdDetectionInProgress) {
        qDebug() << "[DVD DETECTION] Already in progress, skipping duplicate request";
        return;
    }
    
    qDebug() << "=== DVD Detection Started ===";
    
    // Only run DVD detection if we have missing files that need to be copied
    if (!hasActuallyMissingFiles()) {
        qDebug() << "[DVD CHECK] No missing files detected, skipping DVD detection";
        qDebug() << "[DVD CHECK] All required files appear to be available locally";
        return;
    }
    
    qDebug() << "[DVD CHECK] Missing files detected, proceeding with DVD detection";
    
    // Check if worker thread is already running
    if (m_dvdWorkerThread && m_dvdWorkerThread->isRunning()) {
        qDebug() << "[DVD CHECK] DVD worker already running, skipping new detection";
        return;
    }
    
    if (m_copyInProgress) {
        qDebug() << "[DVD CHECK] Copy already in progress, skipping DVD detection";
        return;
    }
    
    // Set detection in progress flag
    m_dvdDetectionInProgress = true;
    
    qDebug() << "[DVD WORKER] Starting background DVD detection and copy...";
    qDebug() << "[DVD WORKER] Looking for DVD drives with DicomFiles folder...";
    
    // Start worker thread for all DVD operations
    if (m_dvdWorkerThread && m_dvdWorker && !m_dvdWorkerThread->isRunning()) {
        qDebug() << "[DVD WORKER] Starting worker thread for DVD operations...";
        m_dvdWorkerThread->start();
        
        // Give the thread a moment to start properly
        QThread::msleep(100);
        qDebug() << "[DVD WORKER] Worker thread started successfully";
    }
    
    // Worker thread will handle all DVD detection and copying
    qDebug() << "[DVD DETECTION] Letting worker thread handle DVD detection and copying...";
}





QString DicomViewer::findDvdWithDicomFiles()
{
    qDebug() << "[DVD SCAN] Scanning for DVD drives with DicomFiles folder...";
    
    // Check common DVD drive letters
    QStringList drivesToCheck = {"D:", "E:", "F:", "G:", "H:"};
    
    for (const QString& drive : drivesToCheck) {
        QString dicomPath = drive + "/DicomFiles";
        QDir dir(dicomPath);
        
        qDebug() << QString("[DVD SCAN] Checking %1...").arg(dicomPath);
        
        if (dir.exists()) {
            qDebug() << QString("[DVD FOUND] DicomFiles folder exists at %1").arg(dicomPath);
            
            // Check if it contains DICOM files
            QStringList filters;
            filters << "*.dcm" << "*.DCM" << "*"; // Include files without extension
            QStringList files = dir.entryList(filters, QDir::Files);
            
            qDebug() << QString("[DVD CONTENT] Found %1 files in DicomFiles folder").arg(files.count());
            
            if (!files.isEmpty()) {
                qDebug() << QString("[DVD SUCCESS] ✓ Found %1 DICOM files at: %2").arg(files.size()).arg(dicomPath);
                
                // Log first few filenames for verification
                for (int i = 0; i < qMin(3, files.size()); i++) {
                    qDebug() << QString("[DVD FILES]   - %1").arg(files[i]);
                }
                if (files.size() > 3) {
                    qDebug() << QString("[DVD FILES]   ... and %1 more files").arg(files.size() - 3);
                }
                
                return drive; // Return drive letter, not full path
            } else {
                qDebug() << QString("[DVD EMPTY] DicomFiles folder is empty at %1").arg(dicomPath);
            }
        } else {
            qDebug() << QString("[DVD SCAN] No DicomFiles folder at %1").arg(dicomPath);
        }
    }
    
    qDebug() << "[DVD SCAN] ✗ No DVD with DICOM files found in any drive";
    return QString(); // No DVD with DICOM files found
}







// DVD Worker Slot Implementations
void DicomViewer::onWorkerReady()
{
    qDebug() << "[WORKER READY] DVD worker thread is ready";
    m_workerReady = true;
    
    // If we have pending sequential copy data, start it now
    if (!m_pendingDvdPath.isEmpty() && !m_pendingOrderedFiles.isEmpty()) {
        qDebug() << "[PENDING COPY] Starting pending sequential copy for:" << m_pendingDvdPath;
        qDebug() << "[PENDING COPY] Files to copy:" << m_pendingOrderedFiles.size();
        
        emit requestSequentialRobocopyStart(m_pendingDvdPath, m_pendingOrderedFiles);
        
        // Clear pending data
        m_pendingDvdPath.clear();
        m_pendingOrderedFiles.clear();
    } else {
        qDebug() << "[WORKER READY] No pending copy data";
    }
}

void DicomViewer::onDvdDetected(const QString& dvdPath)
{
    qDebug() << "DVD detected at:" << dvdPath;
    m_dvdSourcePath = dvdPath;
    
    // Clear any previous completion tracking to start fresh
    qDebug() << "[INIT DEBUG] Clearing completed files set at DVD detection";
    m_fullyCompletedFiles.clear();
    m_firstImageAutoSelected = false;  // Reset auto-selection flag for new session
    
    // Get ordered file list from tree view for sequential copying
    QStringList orderedFiles = getOrderedFileList();
    
    if (!orderedFiles.isEmpty()) {
        qDebug() << "[SEQUENTIAL COPY] Storing sequential copy data for path:" << dvdPath;
        qDebug() << "[SEQUENTIAL COPY] Files to copy in order:" << orderedFiles.size();
        
        m_pendingDvdPath = dvdPath;
        m_pendingOrderedFiles = orderedFiles;
        
        // Check if worker is already ready - if so, start immediately
        if (m_workerReady) {
            qDebug() << "[IMMEDIATE START] Worker is ready, starting sequential copy immediately";
            emit requestSequentialRobocopyStart(m_pendingDvdPath, m_pendingOrderedFiles);
            
            // Clear pending data since we started immediately
            m_pendingDvdPath.clear();
            m_pendingOrderedFiles.clear();
        } else {
            qDebug() << "[SEQUENTIAL COPY] Worker not ready yet, waiting for worker ready signal";
        }
    } else {
        qDebug() << "[WARNING] No ordered files found in tree view - DVD copying may not work properly";
        qDebug() << "[INFO] Ensure DICOMDIR is loaded and tree view is populated before DVD detection";
    }
    
    if (m_imageLabel) {
        m_imageLabel->setText("DVD detected. Loading...");
    }
}

void DicomViewer::onCopyStarted()
{
    qDebug() << "DVD copy started";
    m_copyInProgress = true;
    m_currentCopyProgress = 0;
    m_dvdDetectionInProgress = false;  // Reset detection flag when copy starts
    
    // Ensure completed files set is clear at copy start
    qDebug() << "[COPY START DEBUG] Completed files count before clear:" << m_fullyCompletedFiles.size();
    m_fullyCompletedFiles.clear();
    qDebug() << "[COPY START DEBUG] Completed files set cleared";
    
    // Debug: Check tree items one more time at copy start
    if (m_dicomTree) {
        QTreeWidgetItemIterator it(m_dicomTree);
        int itemsWithProgress = 0;
        while (*it) {
            QTreeWidgetItem* item = *it;
            QString itemText = item->text(0);
            if (itemText.contains("%") || itemText.contains("Loading")) {
                itemsWithProgress++;
                qDebug() << "[COPY START DEBUG] Item with progress detected:" << itemText;
            }
            ++it;
        }
        qDebug() << "[COPY START DEBUG] Total items with progress indicators:" << itemsWithProgress;
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
        qDebug() << "Started periodic tree refresh timer during copy operation";
    }
}

void DicomViewer::onOverallProgress(int percentage, const QString& statusText)
{
    qDebug() << "Overall DVD copy progress:" << percentage << "% -" << statusText;
    
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
    qDebug() << "DVD copy completed. Success:" << success;
    
    m_copyInProgress = false;
    m_dvdDetectionInProgress = false; // Reset detection flag
    
    // Stop progress monitoring
    if (m_copyProgressTimer && m_copyProgressTimer->isActive()) {
        m_copyProgressTimer->stop();
    }
    
    // Stop worker thread
    if (m_dvdWorkerThread) {
        m_dvdWorkerThread->quit();
        m_dvdWorkerThread->wait();
    }
    
    if (success) {
        // Clear the completed files tracking since all files are now available
        m_fullyCompletedFiles.clear();
        qDebug() << "Cleared completed files set - all files now fully available";
        
        // Refresh DICOM tree to show newly copied files
        if (m_dicomReader) {
            // Store current selection before tree refresh
            storeCurrentSelection();
            
            QString dicomdirPath = m_localDestPath + "/../DICOMDIR";
            if (QFile::exists(dicomdirPath)) {
                m_dicomReader->loadDicomDir(dicomdirPath);
                m_dicomReader->populateTreeWidget(m_dicomTree);
                
                // Restore selection after tree refresh
                restoreStoredSelection();
            }
        }
        
        // Update status bar to show completion
        updateStatusBar("Media loading completed", -1);
    } else {
        // Update status bar to show failure
        updateStatusBar("Failed to load from media", -1);
    }
}

void DicomViewer::onWorkerError(const QString& error)
{
    qDebug() << "DVD worker error:" << error;
    
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

void DicomViewer::updateTreeItemWithProgress(const QString& fileName, int progress)
{
    if (!m_dicomTree || !m_dicomReader) return;
    
    // Extract just the filename from the full path for tree matching
    QString baseFileName = QFileInfo(fileName).fileName();
    
    qDebug() << "File progress update:" << fileName << progress << "%";
    qDebug() << "Extracted filename for tree matching:" << baseFileName;
    
    // Find and update the specific tree item for this file using just the filename
    updateSpecificTreeItemProgress(baseFileName, progress);
    
    // When a file completes (reaches 100%), refresh tree to show it's available
    if (progress >= 100) {
        QString baseFileName = QFileInfo(fileName).fileName();
        qDebug() << "File completed, adding to fully completed set:" << baseFileName;
        
        // Add to the set of fully completed files
        m_fullyCompletedFiles.insert(baseFileName);
        
        // Refresh file existence status in DicomReader
        m_dicomReader->refreshFileExistenceStatus();
        
        // Update frame count for this specific file now that it's available
        m_dicomReader->updateFrameCountForFile(baseFileName);
        
        // Store current selection using persistent selection system
        storeCurrentSelection();
        
        // Update the tree display to reflect new file availability with correct icons
        m_dicomReader->populateTreeWidget(m_dicomTree);
        
        // Restore selection using persistent selection system
        restoreStoredSelection();
        
        qDebug() << "Tree refreshed after file completion with updated frame count:" << fileName;
        
        // Auto-select and display the first completed image for better UX
        if (!m_firstImageAutoSelected) {
            autoSelectFirstCompletedImage();
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
        
        qDebug() << "Overall progress:" << overallProgress * 100 << "% (" 
                 << (int)(overallProgress * totalImages) << "/" << totalImages << " files)";
    }
}

void DicomViewer::updateSpecificTreeItemProgress(const QString& fileName, int progress)
{
    if (!m_dicomTree) return;
    
    qDebug() << "[TREE UPDATE] Searching for file:" << fileName << "progress:" << progress << "%";
    
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
                qDebug() << "[TREE DEBUG]" << itemCount << "Type:" << itemType << "Path:" << filePath << "Text:" << item->text(0);
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
                    qDebug() << "[MATCH] Exact filename match:" << fileName << "vs" << itemFileName;
                } else {
                    // Debug mismatches to help troubleshoot
                    qDebug() << "[NO MATCH]" << fileName << "!=" << itemFileName;
                }
            }
            
            if (isMatch) {
                
                // Store original text if not already stored
                if (!item->data(0, Qt::UserRole + 1).isValid()) {
                    QString originalText = item->text(0);
                    item->setData(0, Qt::UserRole + 1, originalText);
                    qDebug() << "Stored original text for item:" << originalText;
                }
                
                QString originalText = item->data(0, Qt::UserRole + 1).toString();
                
                if (progress < 100) {
                    // Show progress percentage in the item text
                    QString progressText = QString("%1 - Loading... %2%").arg(originalText).arg(progress);
                    item->setText(0, progressText);
                    
                    // Keep the loading icon and gray color
                    item->setIcon(0, QIcon(":/icons/Loading.png"));
                    item->setForeground(0, QColor(180, 180, 180));
                    
                    qDebug() << "Updated tree item progress:" << fileName << progress << "%";
                } else {
                    // File completed - verify it actually exists and is readable before marking complete
                    QString fullFilePath = QFileInfo(filePath).absoluteFilePath();
                    QFileInfo completedFile(fullFilePath);
                    
                    if (completedFile.exists() && completedFile.size() > 0) {
                        qDebug() << "[FILE VERIFIED] File exists and has size:" << completedFile.size() << "bytes";
                        
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
                            
                            qDebug() << "[ICON SELECTION] File:" << fileName << "Cached Frames:" << cachedFrameCount << "Path:" << imageInfo.filePath;
                            
                            if (cachedFrameCount > 1) {
                                item->setIcon(0, QIcon(":/icons/AcquisitionHeader.png"));
                                qDebug() << "Set multiframe icon for" << fileName << "(" << cachedFrameCount << "frames)";
                            } else {
                                item->setIcon(0, QIcon(":/icons/Camera.png"));
                                qDebug() << "Set single frame icon for" << fileName;
                            }
                        }
                        
                        qDebug() << "File completed, restored original text:" << originalText;
                    } else {
                        qDebug() << "[FILE NOT READY] File" << fileName << "marked as 100% but doesn't exist or is empty. Keeping loading state.";
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
            qDebug() << "[ROBOCOPY]" << trimmedLine;
        }
        
        // Skip files that robocopy reports as "same" (already exist and identical)
        if (trimmedLine.contains("same\t\t")) {
            qDebug() << "[ROBOCOPY SAME] Skipping file that already exists:" << trimmedLine;
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
                qDebug() << QString("[DVD COPY] %1% - %2 (elapsed: %3s)")
                           .arg(progress, 3)
                           .arg(filename.isEmpty() ? "processing..." : filename)
                           .arg(elapsed / 1000.0, 0, 'f', 1);
                
                // Debug any non-zero progress immediately
                if (!filename.isEmpty() && progress > 0) {
                    qDebug() << "[PROGRESS DEBUG] File progress detected:" << filename << progress << "% from line:" << trimmedLine;
                    updateTreeItemWithProgress(filename, progress);
                }
                
                if (progress >= 100) {
                    s_filesProcessed++;
                    qDebug() << QString("[DVD COPY] ✓ Completed file #%1: %2")
                               .arg(s_filesProcessed)
                               .arg(filename);
                    
                    // Additional debug: Track which files are being marked as complete and when
                    qDebug() << "[100% DEBUG] File marked complete:" << filename;
                    qDebug() << "[100% DEBUG] Robocopy line was:" << trimmedLine;
                    
                    // Verify file actually exists before marking as complete
                    QString expectedPath = QString("C:/Users/gurup/AppData/Local/Temp/Ekn_TempData/DicomFiles/%1").arg(filename);
                    QFileInfo checkFile(expectedPath);
                    
                    if (checkFile.exists() && checkFile.size() > 0) {
                        qDebug() << "[VERIFICATION PASS] File exists with size:" << checkFile.size();
                        updateTreeItemWithProgress(filename, progress);
                    } else {
                        qDebug() << "[VERIFICATION FAIL] File" << filename << "reported 100% but doesn't exist or is empty!";
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
                
                qDebug() << QString("[DVD COPY] → Starting: %1 (%2 KB)")
                           .arg(filename)
                           .arg(fileSize / 1024);
            }
        }
        
        // Look for completion summary
        if (trimmedLine.contains("Total") || 
            trimmedLine.contains("Files :") ||
            trimmedLine.contains("Bytes :") ||
            trimmedLine.contains("Speed :") ||
            trimmedLine.contains("Ended :")) {
            qDebug() << "[ROBOCOPY SUMMARY]" << trimmedLine;
        }
        
        // Log any errors or warnings
        if (trimmedLine.contains("ERROR") || 
            trimmedLine.contains("FAILED") ||
            trimmedLine.contains("Access denied")) {
            qDebug() << "[ROBOCOPY ERROR]" << trimmedLine;
        }
    }
}

void DicomViewer::autoSelectFirstCompletedImage()
{
    if (!m_dicomTree || m_firstImageAutoSelected) {
        return;
    }
    
    qDebug() << "[AUTO SELECT] Looking for first completed image to auto-select...";
    
    // Recursive function to find the first DICOM image item (leaf node)
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> findFirstImageItem = 
        [&](QTreeWidgetItem* item) -> QTreeWidgetItem* {
        
        if (!item) return nullptr;
        
        // Check if this is a leaf item (DICOM image) by checking if it has no children
        if (item->childCount() == 0) {
            // Verify it's a DICOM file by checking if it has a Camera or AcquisitionHeader icon
            QIcon itemIcon = item->icon(0);
            if (!itemIcon.isNull()) {
                qDebug() << QString("[AUTO SELECT] Found potential image item: %1").arg(item->text(0));
                return item;
            }
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
        QTreeWidgetItem* firstImage = findFirstImageItem(m_dicomTree->topLevelItem(i));
        if (firstImage) {
            qDebug() << QString("[AUTO SELECT] Auto-selecting first completed image: %1").arg(firstImage->text(0));
            
            // Expand parent items to make the selection visible
            QTreeWidgetItem* parent = firstImage->parent();
            while (parent) {
                parent->setExpanded(true);
                parent = parent->parent();
            }
            
            // Multiple approaches to ensure selection is visible
            qDebug() << "[AUTO SELECT] Attempting to select item with multiple methods...";
            
            // Method 1: Use selection model directly
            QModelIndex index = m_dicomTree->indexFromItem(firstImage);
            if (index.isValid()) {
                m_dicomTree->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
                qDebug() << "[AUTO SELECT] Selection model method applied";
            }
            
            // Method 2: Traditional Qt selection
            m_dicomTree->setCurrentItem(firstImage);
            firstImage->setSelected(true);
            
            // Method 3: Force update and repaint
            m_dicomTree->viewport()->update();
            m_dicomTree->repaint();
            
            // Scroll to make it visible
            m_dicomTree->scrollToItem(firstImage, QAbstractItemView::EnsureVisible);
            
            // Ensure tree has focus
            m_dicomTree->setFocus(Qt::OtherFocusReason);
            
            // Store this auto-selection for persistence across tree refreshes
            storeCurrentSelection();
            
            // Debug: Check if selection is actually set
            QTreeWidgetItem* currentSelected = m_dicomTree->currentItem();
            bool isSelected = firstImage->isSelected();
            qDebug() << "[AUTO SELECT] Post-selection state - Current item:" << (currentSelected ? currentSelected->text(0) : "NULL");
            qDebug() << "[AUTO SELECT] Post-selection state - Is selected:" << isSelected;
            
            // Mark that we've auto-selected the first image
            m_firstImageAutoSelected = true;
            
            qDebug() << "[AUTO SELECT] First image auto-selected and displayed!";
            return;
        }
    }
    
    qDebug() << "[AUTO SELECT] No completed images found yet for auto-selection";
}

// ===== SELECTION PERSISTENCE METHODS =====

void DicomViewer::storeCurrentSelection()
{
    if (!m_selectionPersistenceEnabled || !m_dicomTree) {
        qDebug() << "[SELECTION PERSISTENCE] Disabled or no tree available";
        return;
    }
    
    QTreeWidgetItem* currentItem = m_dicomTree->currentItem();
    if (!currentItem) {
        qDebug() << "[SELECTION PERSISTENCE] No current selection to store";
        m_persistentSelectedStudyId.clear();
        m_persistentSelectedSeriesId.clear();
        m_persistentSelectedFileName.clear();
        m_persistentSelectedPath.clear();
        return;
    }
    
    // Store the path to the selected item for precise restoration
    m_persistentSelectedPath = getItemPath(currentItem);
    
    // Also store specific identifiers for robustness
    m_persistentSelectedFileName = currentItem->text(0);
    
    // If it's a child item (image), get parent identifiers
    if (currentItem->parent()) {
        if (currentItem->parent()->parent()) {
            // It's an image item (grandchild), store study and series
            m_persistentSelectedStudyId = currentItem->parent()->parent()->text(0);
            m_persistentSelectedSeriesId = currentItem->parent()->text(0);
        } else {
            // It's a series item (child), store study
            m_persistentSelectedStudyId = currentItem->parent()->text(0);
            m_persistentSelectedSeriesId = currentItem->text(0);
        }
    } else {
        // It's a study item (root)
        m_persistentSelectedStudyId = currentItem->text(0);
        m_persistentSelectedSeriesId.clear();
    }
    
    qDebug() << "[SELECTION PERSISTENCE] Stored selection:";
    qDebug() << "  Path:" << m_persistentSelectedPath;
    qDebug() << "  Study ID:" << m_persistentSelectedStudyId;
    qDebug() << "  Series ID:" << m_persistentSelectedSeriesId;
    qDebug() << "  Filename:" << m_persistentSelectedFileName;
}

void DicomViewer::restoreStoredSelection()
{
    if (!m_selectionPersistenceEnabled || !m_dicomTree) {
        qDebug() << "[SELECTION PERSISTENCE] Disabled or no tree available for restore";
        return;
    }
    
    if (m_persistentSelectedPath.isEmpty()) {
        qDebug() << "[SELECTION PERSISTENCE] No stored selection to restore";
        return;
    }
    
    qDebug() << "[SELECTION PERSISTENCE] Attempting to restore selection:";
    qDebug() << "  Path:" << m_persistentSelectedPath;
    
    // Try to find the item by its stored path
    QTreeWidgetItem* targetItem = findItemByPath(m_persistentSelectedPath);
    
    if (targetItem) {
        qDebug() << "[SELECTION PERSISTENCE] Found target item by path, applying triple selection method";
        
        // Triple selection method for maximum visibility
        
        // Method 1: QSelectionModel selection
        QModelIndex index = m_dicomTree->indexFromItem(targetItem);
        if (index.isValid()) {
            m_dicomTree->selectionModel()->setCurrentIndex(
                index, 
                QItemSelectionModel::SelectCurrent | QItemSelectionModel::Clear
            );
            qDebug() << "[SELECTION PERSISTENCE] Applied QSelectionModel selection";
        }
        
        // Method 2: Traditional setCurrentItem
        m_dicomTree->setCurrentItem(targetItem);
        qDebug() << "[SELECTION PERSISTENCE] Applied setCurrentItem";
        
        // Method 3: Ensure visibility and focus
        m_dicomTree->scrollToItem(targetItem, QAbstractItemView::EnsureVisible);
        m_dicomTree->setFocus();
        
        // Method 4: Force viewport update
        m_dicomTree->viewport()->update();
        
        qDebug() << "[SELECTION PERSISTENCE] Successfully restored selection with enhanced visibility";
        
        // Verify selection was applied
        QTreeWidgetItem* verifyItem = m_dicomTree->currentItem();
        qDebug() << "[SELECTION PERSISTENCE] Verification - Current item:" 
                 << (verifyItem ? verifyItem->text(0) : "NULL");
                 
    } else {
        qDebug() << "[SELECTION PERSISTENCE] Could not find target item by path, selection lost";
    }
}

QStringList DicomViewer::getItemPath(QTreeWidgetItem* item)
{
    QStringList path;
    if (!item) return path;
    
    // Build path from root to item
    QTreeWidgetItem* current = item;
    while (current) {
        path.prepend(current->text(0));
        current = current->parent();
    }
    
    return path;
}

#include "dicomviewer.moc"
