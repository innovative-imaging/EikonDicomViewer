#include "saveimagedialog.h"
#include <QtWidgets/QFileDialog>
#include <QtCore/QDir>

SaveImageDialog::SaveImageDialog(QWidget *parent, QSize currentImageSize)
    : QDialog(parent)
    , m_currentImageSize(currentImageSize)
    , m_destinationEdit(nullptr)
    , m_browseBtn(nullptr)
    , m_filenameEdit(nullptr)
    , m_exportBtn(nullptr)
    , m_cancelBtn(nullptr)
{
    setWindowTitle("Export to image");
    setFixedSize(650, 180);
    setupUI();
    setupDarkTheme();
}

void SaveImageDialog::setupUI()
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
    connect(m_browseBtn, &QPushButton::clicked, this, &SaveImageDialog::browseDestination);
    pathLayout->addWidget(m_browseBtn);
    destLayout->addLayout(pathLayout);
    
    // Filename input
    QHBoxLayout* filenameLayout = new QHBoxLayout;
    filenameLayout->addWidget(new QLabel("Filename:"));
    m_filenameEdit = new QLineEdit("export");
    m_filenameEdit->setFixedHeight(28);
    filenameLayout->addWidget(m_filenameEdit);
    filenameLayout->addWidget(new QLabel(".jpg"));
    destLayout->addLayout(filenameLayout);
    
    layout->addWidget(destGroup);
    
    // Info message
    QLabel* infoLabel = new QLabel("Image will be saved as JPG with 90% quality");
    infoLabel->setStyleSheet("color: #cccccc; font-style: italic; padding: 6px;");
    layout->addWidget(infoLabel);
    
    // Buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout;
    buttonLayout->addStretch();
    m_exportBtn = new QPushButton("Export");
    m_cancelBtn = new QPushButton("Cancel");
    
    connect(m_exportBtn, &QPushButton::clicked, this, &SaveImageDialog::accept);
    connect(m_cancelBtn, &QPushButton::clicked, this, &SaveImageDialog::reject);
    
    buttonLayout->addWidget(m_exportBtn);
    buttonLayout->addWidget(m_cancelBtn);
    layout->addLayout(buttonLayout);
}

void SaveImageDialog::setupDarkTheme()
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

void SaveImageDialog::browseDestination()
{
    QString folder = QFileDialog::getExistingDirectory(this, 
        "Select Destination Folder", m_destinationEdit->text());
    if (!folder.isEmpty()) {
        m_destinationEdit->setText(folder);
    }
}

SaveImageDialog::ExportSettings SaveImageDialog::getExportSettings() const
{
    ExportSettings settings;
    settings.destination = m_destinationEdit->text();
    settings.filename = m_filenameEdit->text();
    settings.prefix = m_filenameEdit->text();
    settings.source = "Current image";
    settings.format = "JPG";
    settings.quality = 90;
    settings.sizeOption = "original";
    settings.showAnnotations = false;
    settings.overlayOption = "none";
    settings.openExplorer = true;
    
    return settings;
}
