#ifndef DVDCOPYWORKER_H
#define DVDCOPYWORKER_H

#include <QObject>
#include <QProcess>
#include <QDir>
#include <QStringList>
#include <QRegularExpression>
#include <QDebug>
#include <QTimer>
#include <QFileInfo>

// DVD Copy Worker Class - Background thread for DVD detection and copying
class DvdCopyWorker : public QObject
{
    Q_OBJECT

public:
    explicit DvdCopyWorker(const QString& destPath, QObject* parent = nullptr);
    ~DvdCopyWorker();

public slots:
    void startDvdDetectionAndCopy();
    void startRobocopy(const QString& dvdPath);

signals:
    void dvdDetected(const QString& dvdPath);
    void copyStarted();
    void fileProgress(const QString& fileName, int progress);
    void overallProgress(int percentage, const QString& statusText);
    void copyCompleted(bool success);
    void workerError(const QString& error);
    void statusChanged(const QString& status);

private slots:
    void onRobocopyOutput();
    void onRobocopyError();
    void onRobocopyFinished(int exitCode, QProcess::ExitStatus exitStatus);

private slots:
    void checkFileProgress();

private:
    QString findDvdWithDicomFiles();
    void parseRobocopyOutput(const QString& output);
    void startProgressMonitoring();

    QString m_destPath;
    QProcess* m_robocopyProcess;
    QString m_currentFileName;
    QTimer* m_progressTimer;
    QStringList m_expectedFiles;
    QStringList m_completedFiles;
    QString m_logFilePath;
    qint64 m_lastLogPosition;
};

#endif // DVDCOPYWORKER_H