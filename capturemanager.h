#ifndef CAPTUREMANAGER_H
#define CAPTUREMANAGER_H

#include <QObject>
#include <QImage>
#include <QString>
#include <QTimer>
#include <QMutex>
#include <QFutureWatcher>
#include <cstdint>

class DeviceManager;
class ImageProcessor;

// 处理结果结构体
struct ProcessResult {
    bool success;
    QImage image;
    QString filePath;
    QString errorMsg;
};
Q_DECLARE_METATYPE(ProcessResult)

class CaptureManager : public QObject
{
    Q_OBJECT

public:
    explicit CaptureManager(QObject *parent = nullptr);
    ~CaptureManager();

    void setDeviceManager(DeviceManager *deviceManager);
    void setImageProcessor(ImageProcessor *imageProcessor);
    void setSavePath(const QString &savePath);

public slots:
    bool captureOnce(uint8_t quality, uint16_t width, uint16_t height);
    // intervalSeconds: 定时抓图间隔（秒），内部统一换算为毫秒
    bool startTimedCapture(int intervalSeconds, uint8_t quality, uint16_t width, uint16_t height);
    void stopTimedCapture();

public:
    QString getSavePath() const;
    static CaptureManager *instance;

signals:
    void captureFinished(const QImage &image, const QString &filePath);
    void errorOccurred(const QString &error);
    void rawSnapDataReceived(QByteArray data, QString savePath);

private slots:
    void onTimerCapture();
    void onRawSnapDataReceived(QByteArray data, QString savePath);
    void onImageProcessed();

private:
    DeviceManager* m_deviceManager;
    ImageProcessor* m_imageProcessor;
    QString m_savePath;
    QTimer* m_timer;
    int m_captureInterval;
    uint8_t m_quality;
    uint16_t m_width;
    uint16_t m_height;
    QMutex m_mutex;
    QFutureWatcher<ProcessResult>* m_processWatcher;

    static QMutex s_instanceMutex;
    static ProcessResult processImageInBackground(QByteArray data, QString savePath, ImageProcessor* processor);
};

#endif // CAPTUREMANAGER_H
