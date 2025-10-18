#pragma once

#include <QtWidgets/QDialog>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QCheckBox>
#include <QtCore/QStandardPaths>

class SaveRunDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SaveRunDialog(QWidget *parent = nullptr, QSize currentImageSize = QSize(512, 512));
    
    struct ExportSettings {
        QString destination;
        QString filename;
        QString prefix;
        QString source;
        bool exportFrames;
        bool separateFiles;
        QString separatePer;
        QString format;
        int framerate;
        bool aviCompression;
        QString sizeOption;
        bool showAnnotations;
        QString overlayOption;
        bool openExplorer;
    };
    
    ExportSettings getExportSettings() const;

private slots:
    void browseDestination();

private:
    void setupUI();
    void setupDarkTheme();
    
    QSize m_currentImageSize;
    
    // UI components
    QLineEdit* m_destinationEdit;
    QPushButton* m_browseBtn;
    QLineEdit* m_filenameEdit;
    QCheckBox* m_openExplorerCb;
    QPushButton* m_exportBtn;
    QPushButton* m_cancelBtn;
};
