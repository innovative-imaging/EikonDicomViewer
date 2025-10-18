#include "dicomviewer.h"
#include "DicomFrameProcessor.h"
#include "saveimagedialog.h"
#include "saverundialog.h"

#include <chrono>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHeaderView>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtGui/QPainter>
#include <QtCore/QDir>
#include <QtCore/QStandardPaths>
#include <QtCore/QFile>

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

DicomViewer::DicomViewer(QWidget *parent)
    : QMainWindow(parent)
    , m_playbackController(nullptr)
    , m_inputHandler(nullptr)
    , m_centralWidget(nullptr)
    , m_topToolbar(nullptr)
    , m_closeButton(nullptr)
    , m_playAction(nullptr)
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
    , m_isHorizontallyFlipped(false)
    , m_isVerticallyFlipped(false)
    , m_isInverted(false)
    , m_transformationsEnabled(true)
    , m_zoomFactor(1.0)
    , m_minZoomFactor(0.1)
    , m_maxZoomFactor(4.0)
    , m_zoomIncrement(1.05)
    , m_windowingActive(false)
    , m_originalWindowCenter(0)
    , m_originalWindowWidth(0)
    , m_currentWindowCenter(0)
    , m_currentWindowWidth(0)
    , m_windowingSensitivity(5.0)
    , m_currentPositionerPrimaryAngle(0.0)
    , m_currentPositionerSecondaryAngle(0.0)
    , m_currentXRayTubeCurrent(0.0)
    , m_currentKVP(0.0)
    , m_hasPositionerAngles(false)
    , m_hasTechnicalParams(false)
    , m_dicomReader(nullptr)
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
    
    // Skip event filter for now: m_dicomTree->installEventFilter(this);
    
    if (sidebarLayout && m_dicomTree) {
        sidebarLayout->addWidget(m_dicomTree);
    }
    
    if (mainLayout && m_leftSidebar) {
        mainLayout->addWidget(m_leftSidebar);
    }
    
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
    
    // Step 5: Auto-load DICOMDIR if present in executable directory
    autoLoadDicomdir();
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
    
    delete m_dicomReader;
    delete m_frameProcessor;
    
#ifdef HAVE_DCMTK
    // Clean up JPEG decompression codecs
    DJDecoderRegistration::cleanup();
#endif
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
        {"ResetSettings_96.png", "Reset All", "Reset All (Ctrl+R / Esc)", &DicomViewer::resetTransformations},
        {"", "", "", nullptr}, // Separator
        {"ImageSave_96.png", "Save Image", "Save Image", &DicomViewer::saveImage},
        {"RunSave_96.png", "Save Run", "Save Run", &DicomViewer::saveRun},
        {"", "", "", nullptr}, // Separator
        {"Info_96.png", "Info", "Info", nullptr},
    };
    
    for (const auto& action : actions) {
        if (action.iconName.isEmpty()) {
            // Add separator
            m_topToolbar->addSeparator();
            continue;
        }
        
        QString iconPath = m_iconPath + "/" + action.iconName;
        QIcon icon(iconPath);
        
        // Check if the icon is valid (resource-based icons should always be valid)
        if (icon.isNull()) {
            // Create a simple colored rectangle as fallback
            QPixmap pixmap(48, 48);
            pixmap.fill(QColor(100, 100, 100));
            icon = QIcon(pixmap);
        }
        
        QAction* toolAction = m_topToolbar->addAction(icon, action.text);
        toolAction->setToolTip(action.tooltip);
        
        // Store reference to play button for icon updates
        if (action.iconName == "Play_96.png") {
            m_playAction = toolAction;
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
    
    // Install event filter
    m_graphicsView->installEventFilter(this);
    m_graphicsView->viewport()->installEventFilter(this);
    
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
                           ", isLoadingProgressively=" << m_isLoadingProgressively << 
                           ", allFramesCached=" << m_allFramesCached;
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
    m_isHorizontallyFlipped = !m_isHorizontallyFlipped;
    applyAllTransformations();
}

void DicomViewer::verticalFlip()
{
    m_isVerticallyFlipped = !m_isVerticallyFlipped;
    applyAllTransformations();
}

void DicomViewer::invertImage()
{
    m_isInverted = !m_isInverted;
    applyAllTransformations();
}

void DicomViewer::resetTransformations()
{
    m_isHorizontallyFlipped = false;
    m_isVerticallyFlipped = false;
    m_isInverted = false;
    m_zoomFactor = 1.0;
    applyAllTransformations();
    fitToWindow();
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
    SaveImageDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        // TODO: Implement image saving
    }
}

void DicomViewer::saveRun()
{
    SaveRunDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        // TODO: Implement video saving
    }
}

void DicomViewer::onTreeItemSelected(QTreeWidgetItem *current, QTreeWidgetItem *previous)
{
    Q_UNUSED(previous)
    if (!current) {
        return;
    }
    
    // Check if this is an image item
    QVariantList userData = current->data(0, Qt::UserRole).toList();
    
    if (userData.size() >= 2 && userData[0].toString() == "image") {
        QString filePath = userData[1].toString();
        loadDicomImage(filePath);
    } else {
        // Not an image, just show the selection
        m_imageLabel->setText("Selected: " + current->text(0));
        
        // Switch to report area if it's an SR document
        if (current->text(0).startsWith("SR DOC")) {
            m_mainStack->setCurrentWidget(m_reportArea);
            m_reportArea->setPlainText("Structured Report Document\n\nFile: " + userData.value(1).toString());
        } else {
            m_mainStack->setCurrentWidget(m_imageWidget);
        }
    }
}

bool DicomViewer::eventFilter(QObject *obj, QEvent *event)
{
    // Handle mouse events for zoom and windowing
    if (obj == m_graphicsView || obj == m_graphicsView->viewport()) {
        if (event->type() == QEvent::Wheel) {
            QWheelEvent* wheelEvent = static_cast<QWheelEvent*>(event);
            if (wheelEvent->angleDelta().y() > 0) {
                zoomIn();
            } else {
                zoomOut();
            }
            return true;
        }
        // TODO: Handle mouse press/move/release for windowing
    }
    
    // Handle Left/Right arrow keys for frame navigation when tree has focus
    // These should only be used for frame navigation, not tree navigation
    if (obj == m_dicomTree && event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Left || keyEvent->key() == Qt::Key_Right) {
            // Handle frame navigation directly since tree widget has focus
            if (m_totalFrames > 1) {
                if (keyEvent->key() == Qt::Key_Left) {
                    onPreviousFrameRequested();
                } else {
                    onNextFrameRequested();
                }
            } else {
            }
            // Block the event from affecting the tree widget
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
    // Stop playback
    if (m_playbackTimer && m_playbackTimer->isActive()) {
        m_playbackTimer->stop();
    }
    
    // Stop progressive loading if active
    if (m_progressiveLoader) {
        m_progressiveLoader->stop();
        m_progressiveLoader->wait();
    }
    
    QMainWindow::closeEvent(event);
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
            bottomLeftText += QString("LAO: %1Â°\n").arg(m_currentPositionerPrimaryAngle, 0, 'f', 1);
        } else if (m_currentPositionerPrimaryAngle < 0) {
            bottomLeftText += QString("RAO: %1Â°\n").arg(qAbs(m_currentPositionerPrimaryAngle), 0, 'f', 1);
        } else {
            bottomLeftText += "LAO: 0Â°\n";
        }
        
        // CRAN/CAUD (Secondary Angle)
        if (m_currentPositionerSecondaryAngle > 0) {
            bottomLeftText += QString("CAUD: %1Â°\n").arg(m_currentPositionerSecondaryAngle, 0, 'f', 1);
        } else if (m_currentPositionerSecondaryAngle < 0) {
            bottomLeftText += QString("CRAN: %1Â°\n").arg(qAbs(m_currentPositionerSecondaryAngle), 0, 'f', 1);
        } else {
            bottomLeftText += "CRAN: 0Â°\n";
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
    
    // Window/Level values
    if (m_currentWindowCenter > 0 && m_currentWindowWidth > 0) {
        bottomRightText += QString("WL: %1 WW: %2").arg(m_currentWindowCenter, 0, 'f', 0).arg(m_currentWindowWidth, 0, 'f', 0);
    }
    
    // Update the overlay labels
    m_overlayTopLeft->setText(topLeftText);
    m_overlayTopRight->setText(topRightText);
    m_overlayBottomLeft->setText(bottomLeftText);
    m_overlayBottomRight->setText(bottomRightText);
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

void DicomViewer::applyAllTransformations()
{
    if (!m_transformationsEnabled || m_currentPixmap.isNull()) {
        return;
    }
    
    // Convert to QImage for manipulation
    QImage image = m_currentPixmap.toImage();
    
    // Apply transformations in the correct order
    if (m_isHorizontallyFlipped) {
        // Note: Following Python behavior where horizontal_flip uses vertical flip (np.flipud)
        image = image.mirrored(false, true);  // Flip vertically
    }
    
    if (m_isVerticallyFlipped) {
        // Note: Following Python behavior where vertical_flip uses horizontal flip (np.fliplr)
        image = image.mirrored(true, false);  // Flip horizontally
    }
    
    if (m_isInverted) {
        image.invertPixels();
    }
    
    // Convert back to pixmap and update display
    m_currentPixmap = QPixmap::fromImage(image);
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
    Q_UNUSED(pos)
    // TODO: Start windowing operation
}

void DicomViewer::updateWindowing(const QPoint& pos)
{
    Q_UNUSED(pos)
    // TODO: Update windowing based on mouse movement
}

void DicomViewer::endWindowing()
{
    // TODO: End windowing operation
}

void DicomViewer::resetWindowLevel()
{
    // TODO: Reset window/level to defaults
}

void DicomViewer::applyWindowLevel(double center, double width)
{
    Q_UNUSED(center)
    Q_UNUSED(width)
    // TODO: Apply window/level settings
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
        
        // Debug output
                 << "Images:" << m_dicomReader->getTotalImages();
        
        // Expand first level items and select first image if available
        expandFirstItems();
        
        // Update display message
        if (m_dicomReader->getTotalImages() > 0) {
            m_imageLabel->setText("DICOMDIR loaded successfully. Select an image to view.");
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
        m_imageLabel->setText("File not found: " + QFileInfo(actualFilePath).fileName());
        return;
    }
    
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
            m_imageLabel->setText("Selected file is not an image.");
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
        
        // Convert to 8-bit for display (automatically handles window/level)
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
        
        // Use the cached frame
        QPixmap pixmap = m_frameCache[frameIndex];
        m_currentPixmap = pixmap;
        m_currentDisplayedFrame = frameIndex;
        
        // Update display
        updateImageDisplay();
        
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

// ==============================
// Transformation Methods
// ==============================

void DicomViewer::horizontalFlip()
{
    // Check if we have a current pixmap loaded
    if (m_currentPixmap.isNull()) {
        return;
    }
    
    // Check if we have original pixel data available for the current frame
    if (!hasOriginalPixelData()) {
        return;
    }
    
    // Toggle horizontal flip state
    m_isHorizontallyFlipped = !m_isHorizontallyFlipped;
    
    // Apply all transformations
    applyAllTransformations();
    
    // Force refresh of current display when paused
    refreshCurrentFrameDisplay();
}

void DicomViewer::verticalFlip()
{
    // Check if we have a current pixmap loaded
    if (m_currentPixmap.isNull()) {
        return;
    }
    
    // Check if we have original pixel data available for the current frame
    if (!hasOriginalPixelData()) {
        return;
    }
    
    // Toggle vertical flip state
    m_isVerticallyFlipped = !m_isVerticallyFlipped;
    
    // Apply all transformations
    applyAllTransformations();
    
    // Force refresh of current display when paused
    refreshCurrentFrameDisplay();
}

void DicomViewer::invertImage()
{
    // Check if we have a current pixmap loaded
    if (m_currentPixmap.isNull()) {
        return;
    }
    
    // Check if we have original pixel data available for the current frame
    if (!hasOriginalPixelData()) {
        return;
    }
    
    // Toggle invert state
    m_isInverted = !m_isInverted;
    
    // Apply all transformations
    applyAllTransformations();
    
    // Force refresh of current display when paused
    refreshCurrentFrameDisplay();
}

void DicomViewer::resetTransformations()
{
    // Check if we have a current pixmap loaded
    if (m_currentPixmap.isNull()) {
        return;
    }
    
    try {
        // Reset all transformation states
        m_isHorizontallyFlipped = false;
        m_isVerticallyFlipped = false;
        m_isInverted = false;
        
        // Reset windowing to original values
        // You might want to restore original window/level values here
        
        // Reset zoom to fit
        resetZoomToFit();
        
        // Apply transformations (which will be none, essentially restoring original)
        applyAllTransformations();
        
        // Force refresh of current display
        refreshCurrentFrameDisplay();
        
    } catch (const std::exception& e) {
    }
}

void DicomViewer::setTransformationActionsEnabled(bool enabled)
{
    m_transformationsEnabled = enabled;
    
    // Enable/disable all transformation actions
    for (auto it = m_transformationActions.begin(); it != m_transformationActions.end(); ++it) {
        if (it.value()) {
            it.value()->setEnabled(enabled);
        }
    }
}

void DicomViewer::applyAllTransformations()
{
    if (!hasOriginalPixelData()) {
        return;
    }
    
    try {
        // Start with original pixel data
        QByteArray transformedData = m_originalPixelData;
        
        // Apply transformations with SAME REVERSED LOGIC as Python:
        
        // Horizontal flip uses vertical flip operation (flipud equivalent)
        if (m_isHorizontallyFlipped) {
            transformedData = flipVerticallyInternal(transformedData);
        }
        
        // Vertical flip uses horizontal flip operation (fliplr equivalent) 
        if (m_isVerticallyFlipped) {
            transformedData = flipHorizontallyInternal(transformedData);
        }
        
        // Invert
        if (m_isInverted) {
            transformedData = invertPixelDataInternal(transformedData);
        }
        
        // Apply windowing if active
        if (m_currentWindowCenter != 0 && m_currentWindowWidth != 0) {
            applyWindowingToTransformedData(transformedData, m_currentWindowCenter, m_currentWindowWidth);
        } else {
            convertToDisplayFormat(transformedData);
        }
        
        // Apply transformations to all cached frames
        applyTransformationsToAllCachedFrames();
        
    } catch (const std::exception& e) {
    }
}

// ==============================
// Helper Methods for Transformations
// ==============================

bool DicomViewer::hasOriginalPixelData() const
{
    return !m_originalPixelData.isEmpty() || 
           (!m_originalPixelCache.isEmpty() && m_originalPixelCache.contains(m_currentFrame));
}

void DicomViewer::refreshCurrentFrameDisplay()
{
    // This method should trigger a display update
    // Implementation depends on your display system
    updateImageDisplay();
}

QByteArray DicomViewer::flipVerticallyInternal(const QByteArray& data)
{
    // This is a placeholder implementation
    // You'll need to implement actual pixel data flipping based on your image format
    // This should mirror the np.flipud() behavior from Python
    
    // For now, return the data unchanged - you'll need to implement based on your pixel format
    return data;
}

QByteArray DicomViewer::flipHorizontallyInternal(const QByteArray& data)
{
    // This is a placeholder implementation
    // You'll need to implement actual pixel data flipping based on your image format
    // This should mirror the np.fliplr() behavior from Python
    
    // For now, return the data unchanged - you'll need to implement based on your pixel format
    return data;
}

QByteArray DicomViewer::invertPixelDataInternal(const QByteArray& data)
{
    // This is a placeholder implementation
    // You'll need to implement actual pixel data inversion based on your image format
    // This should mirror the "data_max + data_min - transformed_data" behavior from Python
    
    // For now, return the data unchanged - you'll need to implement based on your pixel format
    return data;
}

void DicomViewer::convertToDisplayFormat(const QByteArray& transformedData)
{
    // This method should convert the transformed pixel data to display format
    // Implementation depends on your display system
    
    // Placeholder - you'll need to implement based on your display pipeline
}

void DicomViewer::applyWindowingToTransformedData(const QByteArray& data, double center, double width)
{
    // This method should apply windowing to the transformed data
    // Implementation depends on your windowing system
    
    // Placeholder - you'll need to implement based on your windowing pipeline
}

void DicomViewer::applyTransformationsToAllCachedFrames()
{
    // This method should apply current transformations to all cached frames
    // Implementation depends on your caching system
    
    // Placeholder - you'll need to implement based on your frame caching system
}

// ==============================
// Loading Event Handlers (with enable/disable logic)
// ==============================

void DicomViewer::onFrameReady(int frameNumber)
{
    // Handle frame ready event
    // Enable transformations after first frame is ready
    if (frameNumber == 0) {
        setTransformationActionsEnabled(true);
    }
    
    // Your existing frame ready logic here...
}

void DicomViewer::onAllFramesLoaded(int totalFrames)
{
    // Handle all frames loaded event
    // Ensure transformations are enabled
    setTransformationActionsEnabled(true);
    
    // Your existing all frames loaded logic here...
}

void DicomViewer::onFirstFrameInfo(const QString& patientName, const QString& patientId, int totalFrames)
{
    // Handle first frame info event
    // Enable transformations as we now have image data
    setTransformationActionsEnabled(true);
    
    // Your existing first frame info logic here...
}

void DicomViewer::onLoadingError(const QString& errorMessage)
{
    // Handle loading error
    // Re-enable transformations even on error
    setTransformationActionsEnabled(true);
    
    // Your existing error handling logic here...
}

void DicomViewer::onLoadingProgress(int currentFrame, int totalFrames)
{
    // Handle loading progress
    // Keep transformations disabled during loading
    // They will be enabled when loading completes
    
    // Your existing progress handling logic here...
}

// ==============================
// Image Loading Methods (with enable/disable logic)
// ==============================

void DicomViewer::loadDicomImage(const QString& filePath)
{
    // Disable transformation actions during loading
    setTransformationActionsEnabled(false);
    
    // Your existing image loading logic here...
    // Remember to call setTransformationActionsEnabled(true) when loading completes
}

void DicomViewer::openDicomDir()
{
    // Disable transformation actions when opening new DICOM directory
    setTransformationActionsEnabled(false);
    
    // Your existing DICOM directory opening logic here...
    // Remember to call setTransformationActionsEnabled(true) when loading completes
}

void DicomViewer::loadDicomDir(const QString& dicomdirPath)
{
    // Disable transformation actions during DICOM directory loading
    setTransformationActionsEnabled(false);
    
    // Your existing DICOM directory loading logic here...
    // Remember to call setTransformationActionsEnabled(true) when loading completes
}

// ==============================
// Action Registration Helper
// ==============================

void DicomViewer::registerTransformationAction(const QString& name, QAction* action)
{
    if (action) {
        m_transformationActions[name] = action;
    }
}
