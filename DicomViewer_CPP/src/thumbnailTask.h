#pragma once

#include <QtCore/QObject>
#include <QtCore/QRunnable>
#include <QtCore/QString>
#include <QtGui/QPixmap>

// Forward declaration
class DicomViewer;

class ThumbnailTask : public QObject, public QRunnable
{
    Q_OBJECT

public:
    ThumbnailTask(const QString& filePath, DicomViewer* viewer, QObject* parent = nullptr);
    void run() override;

signals:
    void taskCompleted(const QString& filePath, const QPixmap& thumbnail, const QString& instanceNumber);

private:
    QPixmap createDicomThumbnail(const QPixmap& originalPixmap, const QString& filePath, QString& instanceNumber);
    void logMessage(const QString& level, const QString& message);

    QString m_filePath;
    DicomViewer* m_viewer;
};