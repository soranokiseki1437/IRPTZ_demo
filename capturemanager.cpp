#include "capturemanager.h"
#include "devicemanager.h"
#include "imageprocessor.h"
#include "svsdk.h"
#include <QtConcurrent>
#include <QDebug>
#include <QVector>
#include <QDir>
#include <QDateTime>

CaptureManager *CaptureManager::instance = nullptr;
QMutex CaptureManager::s_instanceMutex;

CaptureManager::CaptureManager(QObject *parent)
    : QObject(parent)
    , m_deviceManager(nullptr)
    , m_imageProcessor(nullptr)
    , m_timer(new QTimer(this))
    , m_captureInterval(5)
    , m_quality(80)
    , m_width(1920)
    , m_height(1080)
    , m_processWatcher(new QFutureWatcher<ProcessResult>(this))
{
    instance = this;
    
    // 连接定时器
    connect(m_timer, &QTimer::timeout, this, &CaptureManager::onTimerCapture);
    
    // 连接数据处理信号
    connect(this, &CaptureManager::rawSnapDataReceived, this, &CaptureManager::onRawSnapDataReceived);
    
    // 连接异步处理监视器
    connect(m_processWatcher, &QFutureWatcher<ProcessResult>::finished, this, &CaptureManager::onImageProcessed);
}

CaptureManager::~CaptureManager()
{
    stopTimedCapture();

    if (m_processWatcher && m_processWatcher->isRunning()) {
        m_processWatcher->waitForFinished();
    }

    {
        QMutexLocker lock(&s_instanceMutex);
        instance = nullptr;
    }
}

void CaptureManager::setDeviceManager(DeviceManager *deviceManager)
{
    QMutexLocker locker(&m_mutex);
    m_deviceManager = deviceManager;
}

void CaptureManager::setImageProcessor(ImageProcessor *imageProcessor)
{
    QMutexLocker locker(&m_mutex);
    m_imageProcessor = imageProcessor;
}

void CaptureManager::setSavePath(const QString &savePath)
{
    QMutexLocker locker(&m_mutex);
    m_savePath = savePath;
}

QString CaptureManager::getSavePath() const
{
    QMutexLocker locker(const_cast<QMutex*>(&m_mutex));
    return m_savePath;
}

bool CaptureManager::captureOnce(uint8_t quality, uint16_t width, uint16_t height)
{
    QMutexLocker locker(&m_mutex);
    
    if (!m_deviceManager || !m_deviceManager->isConnected()) {
        emit errorOccurred("设备未连接");
        return false;
    }
    
    if (!m_imageProcessor) {
        emit errorOccurred("图像处理未初始化");
        return false;
    }
    
    m_quality = quality;
    m_width = width;
    m_height = height;
    
    LONG userID = static_cast<LONG>(m_deviceManager->getUserID());
    SV_NET_DEV_JPEGPARA jpegPara{};
    jpegPara.wPicQuality = (quality > 0) ? quality : 80;
    jpegPara.wPicSize = 0; // 默认原图分辨率

    QVector<char> jpegBuffer(4 * 1024 * 1024, 0);
    int sizeReturned = 0;
    LONG ret = SV_NET_DEV_CaptureJPEGPicture_V13(userID, 0, &jpegPara, jpegBuffer.data(), &sizeReturned);
    if (ret == 0 && sizeReturned > 0) {
        QByteArray safeData(jpegBuffer.constData(), sizeReturned);
        emit rawSnapDataReceived(safeData, m_savePath);
        qInfo() << "[CaptureManager] 硬件直出 JPEG 抓图成功, 字节数:" << sizeReturned;
        return true;
    }

    // 备用方式：直接抓取 JPEG
    ret = SV_NET_DEV_CaptureJPEGPicture(userID, 0, &jpegPara, jpegBuffer.data());
    if (ret >= 0) {
        // 如果返回长度在 para 中或者已知
        QByteArray safeData(jpegBuffer.constData(), (sizeReturned > 0 ? sizeReturned : 1024 * 1024));
        emit rawSnapDataReceived(safeData, m_savePath);
        return true;
    }

    LONG err = SV_NET_DEV_GetLastError();
    emit errorOccurred(QString("抓拍失败, 错误码=%1").arg(err));
    return false;
}

ProcessResult CaptureManager::processImageInBackground(QByteArray data, QString savePath, ImageProcessor* processor)
{
    ProcessResult result;
    result.success = false;

    if (!processor) {
        result.errorMsg = "图像处理未初始化";
        return result;
    }

    QImage image;
    if (!image.loadFromData(data, "JPEG") && !image.loadFromData(data)) {
        result.errorMsg = "图片数据解码失败";
        return result;
    }

    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
    QString fileName = QString("capture_%1.jpg").arg(timestamp);
    QString filePath = QDir(savePath).absoluteFilePath(fileName);

    if (!processor->saveImage(image, filePath)) {
        result.errorMsg = "保存图片失败";
        return result;
    }

    result.success = true;
    result.image = image;
    result.filePath = filePath;
    return result;
}

void CaptureManager::onRawSnapDataReceived(QByteArray data, QString savePath)
{
    if (m_processWatcher->isRunning()) {
        qWarning() << "[CaptureManager] 上一帧仍在保存中，丢弃当前抓图";
        return;
    }

    QFuture<ProcessResult> future = QtConcurrent::run(
        &CaptureManager::processImageInBackground,
        data, savePath, m_imageProcessor
    );
    m_processWatcher->setFuture(future);
}

void CaptureManager::onImageProcessed()
{
    ProcessResult result = m_processWatcher->result();
    if (result.success) {
        emit captureFinished(result.image, result.filePath);
    } else {
        emit errorOccurred(result.errorMsg);
    }
}

bool CaptureManager::startTimedCapture(int interval, uint8_t quality, uint16_t width, uint16_t height)
{
    QMutexLocker locker(&m_mutex);
    
    if (interval <= 0) {
        emit errorOccurred("定时间隔必须大于0");
        return false;
    }
    
    m_captureInterval = interval;
    m_quality = quality;
    m_width = width;
    m_height = height;
    
    m_timer->setInterval(interval * 1000);
    m_timer->start();
    
    return true;
}

void CaptureManager::stopTimedCapture()
{
    QMutexLocker locker(&m_mutex);
    if (m_timer->isActive()) {
        m_timer->stop();
    }
}

void CaptureManager::onTimerCapture()
{
    captureOnce(m_quality, m_width, m_height);
}
