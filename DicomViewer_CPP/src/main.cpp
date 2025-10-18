#include "dicomviewer.h"
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    
    // Set application properties
    app.setApplicationName("Eikon DICOMViewer ");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("Eikon Imaging");
    
    // Create and show the main window
    DicomViewer viewer;
    
    viewer.show();
    
    return app.exec();
}
