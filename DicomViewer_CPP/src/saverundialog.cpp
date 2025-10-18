#include "saverundialog.h"
#include <QtWidgets/QFileDialog>
#include <QtCore/QDir>

SaveRunDialog::SaveRunDialog(QWidget *parent, QSize currentImageSize)
    : QDialog(parent)
    , m_currentImageSize(currentImageSize)
    , m_destinationEdit(nullptr)
    , m_browseBtn(nullptr)
    , m_filenameEdit(nullptr)
    , m_openExplorerCb(nullptr)
    , m_exportBtn(nullptr)
    , m_cancelBtn(nullptr)
{
    setWindowTitle("Export to video");
    setFixedSize(650, 250);
    setupUI();
    setupDarkTheme();
}

void SaveRunDialog::setupUI()
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    
    // Destination group
    QGroupBox* destGroup = new QGroupBox("Destination");
    QVBoxLayout* destLayout = new QVBoxLayout(destGroup);
    
    // Path selection
    QHBoxLayout* pathLayout = new QHBoxLayout;
    pathLayout->addWidget(new QLabel("Folder:"));
    m_destinationEdit = new QLineEdit;
    m_destinationEdit->setFixedHeight(28);
    
    // Set default to Documents folder
    QString documentsPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    m_destinationEdit->setText(documentsPath);
    
    pathLayout->addWidget(m_destinationEdit);
    m_browseBtn = new QPushButton("...");
    m_browseBtn->setFixedSize(30, 24);
    connect(m_browseBtn, &QPushButton::clicked, this, &SaveRunDialog::browseDestination);
    pathLayout->addWidget(m_browseBtn);
    destLayout->addLayout(pathLayout);
    
    // Filename input
    QHBoxLayout* filenameLayout = new QHBoxLayout;
    filenameLayout->addWidget(new QLabel("Filename:"));
    m_filenameEdit = new QLineEdit("export");
    m_filenameEdit->setFixedHeight(28);
    filenameLayout->addWidget(m_filenameEdit);
    filenameLayout->addWidget(new QLabel(".mp4"));
    destLayout->addLayout(filenameLayout);
    
    layout->addWidget(destGroup);
    
    // Video option group
    QGroupBox* videoGroup = new QGroupBox("Video option");
    QVBoxLayout* videoLayout = new QVBoxLayout(videoGroup);
    
    // Info message
    QLabel* infoLabel = new QLabel("Video will be saved as MP4 at 15 FPS");
    infoLabel->setStyleSheet("color: #cccccc; font-style: italic; padding: 6px;");
    videoLayout->addWidget(infoLabel);
    
    layout->addWidget(videoGroup);
    
    // Open file explorer checkbox
    m_openExplorerCb = new QCheckBox("Open File Explorer after export");
    layout->addWidget(m_openExplorerCb);
    
    // Buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout;
    buttonLayout->addStretch();
    m_exportBtn = new QPushButton("Export");
    m_cancelBtn = new QPushButton("Cancel");
    
    connect(m_exportBtn, &QPushButton::clicked, this, &SaveRunDialog::accept);
    connect(m_cancelBtn, &QPushButton::clicked, this, &SaveRunDialog::reject);
    
    buttonLayout->addWidget(m_exportBtn);
    buttonLayout->addWidget(m_cancelBtn);
    layout->addLayout(buttonLayout);
}

void SaveRunDialog::setupDarkTheme()
{
    setStyleSheet(R"(
        QDialog {
            background-color: #2b2b2b;
            color: white;
        }
        QLabel {
            color: white;
            font-size: 11px;
        }
        QLineEdit {
            background-color: #404040;
            border: 1px solid #555555;
            padding: 4px;
            color: white;
            font-size: 11px;
        }
        QPushButton {
            background-color: #404040;
            border: 1px solid #555555;
            padding: 6px 12px;
            color: white;
            font-size: 11px;
            border-radius: 3px;
        }
        QPushButton:hover {
            background-color: #505050;
        }
        QPushButton:pressed {
            background-color: #353535;
        }
        QCheckBox {
            color: white;
            font-size: 11px;
        }
        QCheckBox::indicator {
            width: 13px;
            height: 13px;
        }
        QCheckBox::indicator:unchecked {
            background-color: #404040;
            border: 1px solid #555555;
        }
        QCheckBox::indicator:checked {
            background-color: #0078d4;
            border: 1px solid #0078d4;
        }
        QGroupBox {
            color: white;
            border: 1px solid #555555;
            margin: 5px 0px;
            padding-top: 10px;
            font-size: 11px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 5px 0 5px;
        }
    )");
}

void SaveRunDialog::browseDestination()
{
    QString folder = QFileDialog::getExistingDirectory(this, 
        "Select Destination Folder", m_destinationEdit->text());
    if (!folder.isEmpty()) {
        m_destinationEdit->setText(folder);
    }
}

SaveRunDialog::ExportSettings SaveRunDialog::getExportSettings() const
{
    ExportSettings settings;
    settings.destination = m_destinationEdit->text();
    settings.filename = m_filenameEdit->text();
    settings.prefix = m_filenameEdit->text();
    settings.source = "Current series";
    settings.exportFrames = false;
    settings.separateFiles = false;
    settings.separatePer = "Image";
    settings.format = "MP4";
    settings.framerate = 15;
    settings.aviCompression = false;
    settings.sizeOption = "original";
    settings.showAnnotations = false;
    settings.overlayOption = "none";
    settings.openExplorer = m_openExplorerCb->isChecked();
    
    return settings;
}
