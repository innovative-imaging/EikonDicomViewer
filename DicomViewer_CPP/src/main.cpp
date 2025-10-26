#include "dicomviewer.h"
#include <QtWidgets/QApplication>
#include <QLoggingCategory>
#include <QDebug>
#include <iostream>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#endif

// Custom message handler to ensure Qt debug output goes to console
void consoleMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    QString txt;
    switch (type) {
    case QtDebugMsg:
        txt = QString("[DEBUG] %1").arg(msg);
        break;
    case QtWarningMsg:
        txt = QString("[WARNING] %1").arg(msg);
        break;
    case QtCriticalMsg:
        txt = QString("[CRITICAL] %1").arg(msg);
        break;
    case QtFatalMsg:
        txt = QString("[FATAL] %1").arg(msg);
        abort();
    case QtInfoMsg:
        txt = QString("[INFO] %1").arg(msg);
        break;
    }
    
    // Output to console
    std::cout << txt.toStdString() << std::endl;
    std::cout.flush();
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    
    // Allocate console for debug output in Windows (Debug builds only)
#ifdef _WIN32
#ifdef _DEBUG
    if (AllocConsole()) {
        freopen_s((FILE**)stdout, "CONOUT$", "w", stdout);
        freopen_s((FILE**)stderr, "CONOUT$", "w", stderr);
        freopen_s((FILE**)stdin, "CONIN$", "r", stdin);
        std::cout.clear();
        std::cerr.clear();
        std::cin.clear();
        
        // Set console title
        SetConsoleTitle(L"EikonDicomViewer Debug Console");
        
        // Install custom message handler for Qt debug output
        qInstallMessageHandler(consoleMessageOutput);
        
        // Test console output
        std::cout << "=== EikonDicomViewer Debug Console Initialized ===" << std::endl;
        qDebug() << "Qt Debug output test - console is working!";
    }
#endif
#endif
    
    // Set application properties
    app.setApplicationName("Eikon DICOMViewer ");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("Eikon Imaging");
    
    // Create and show the main window
    DicomViewer viewer;
    
    viewer.show();
    
    return app.exec();
}
