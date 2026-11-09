#include "devicemanager.h"
#include <QUrl>
#include <QDebug>

DeviceManager *DeviceManager::instance = nullptr;

DeviceManager::DeviceManager(QObject *parent)
    : QObject(parent)
{
    instance = this;

    m_sdkSession = new SvSdkSession(this);

    m_decodeThread = new DecodeThread(this);
    connect(m_decodeThread, &DecodeThread::frameDecoded, this, &DeviceManager::onFrameDecoded);

    m_recordThread = new RecordThread(this);
    connect(m_recordThread, &RecordThread::errorOccurred, this, &DeviceManager::onRecordError);
    connect(m_recordThread, &RecordThread::recordFinished, this, &DeviceManager::onRecordFinished);

    m_rtspSource = new RtspSource(m_decodeThread, this);
    m_rtspSource->setRecordThread(m_recordThread, &m_isRecording);
    connect(m_rtspSource, &RtspSource::streamConnected, this, &DeviceManager::onRtspStreamConnected);
    connect(m_rtspSource, &RtspSource::streamError, this, &DeviceManager::onRtspStreamError);

    m_lensManager = new LensManager(this);
    m_lensManager->setSession(m_sdkSession);

    m_dataRecorder = new DataRecorder(this);
    connect(m_dataRecorder, &DataRecorder::recordingStarted, this, &DeviceManager::onDataRecordStarted);
    connect(m_dataRecorder, &DataRecorder::recordingStopped, this, &DeviceManager::onDataRecordStopped);

    m_infraredService = new InfraredService(m_sdkSession, this);
}

DeviceManager::~DeviceManager()
{
    // 析构阶段不再向外部发信号：此时接收方（MainWindow）的派生部分可能已析构，
    // Qt 会以 "Called object is not of the correct type" 断言崩溃
    disconnect(this, nullptr, nullptr, nullptr);
    logout();
    if (instance == this) {
        instance = nullptr;
    }
}

bool DeviceManager::isConnected() const
{
    QMutexLocker lock(&m_mutex);
    return m_connected;
}

uint64_t DeviceManager::getUserID() const
{
    return m_sdkSession ? static_cast<uint64_t>(m_sdkSession->userId()) : 0;
}

bool DeviceManager::isStreaming() const
{
    QMutexLocker lock(&m_mutex);
    return m_streaming;
}

bool DeviceManager::isRecording() const
{
    return m_isRecording.load();
}

void DeviceManager::login(const QString &ip, const QString &username, const QString &password, uint16_t port)
{
    m_ip = ip;
    m_port = (port == 0) ? 8000 : port;
    m_username = username;
    m_password = password;

    QString err;
    bool ok = m_sdkSession->login(ip, m_port, username, password, &err);
    {
        QMutexLocker lock(&m_mutex);
        m_connected = ok;
    }

    if (ok) {
        m_lensManager->setSession(m_sdkSession);
        emit connectionStatusChanged(true);
        qInfo() << "[DeviceManager] 设备连接成功:" << ip << ":" << m_port;
    } else {
        emit errorOccurred(err.isEmpty() ? "设备登录失败" : err);
        emit connectionStatusChanged(false);
    }
}

bool DeviceManager::logout()
{
    stopStream();
    stopRecord();

    if (m_sdkSession) {
        m_sdkSession->logout();
    }

    {
        QMutexLocker lock(&m_mutex);
        m_connected = false;
    }
    emit connectionStatusChanged(false);
    return true;
}

bool DeviceManager::startStream()
{
    requestStartStream();
    return true;
}

void DeviceManager::requestStartStream()
{
    if (!isConnected()) {
        emit errorOccurred("设备未连接，无法开始取流");
        emit streamStarted(false);
        return;
    }

    // 组装 RTSP 链接
    QString encodedPwd = QString::fromUtf8(QUrl::toPercentEncoding(m_password));
    QString cleanPath = m_rtspPath.trimmed();
    if (cleanPath.startsWith('/')) cleanPath.remove(0, 1);
    if (cleanPath.isEmpty()) cleanPath = "Streaming/Channels/101";

    QString rtspUrl = QString("rtsp://%1:%2@%3:554/%4")
                      .arg(m_username, encodedPwd, m_ip, cleanPath);

    qInfo() << "[DeviceManager] 启动视频预览, RTSP:" << rtspUrl;
    m_rtspSource->setStreamUrl(rtspUrl);
    m_decodeThread->restart();
    m_rtspSource->start();

    {
        QMutexLocker lock(&m_mutex);
        m_streaming = true;
    }
}

bool DeviceManager::stopStream()
{
    if (m_rtspSource && m_rtspSource->isRunning()) {
        m_rtspSource->stopStream();
        m_rtspSource->wait();
    }
    if (m_decodeThread && m_decodeThread->isRunning()) {
        m_decodeThread->stop();
        m_decodeThread->wait();
    }
    {
        QMutexLocker lock(&m_mutex);
        m_streaming = false;
    }
    return true;
}

bool DeviceManager::startRecord(const QString& filePath)
{
    if (!m_recordThread) return false;
    bool ok = m_recordThread->initRecord(filePath, 25);
    if (ok) {
        m_isRecording.store(true);
        emit recordStarted();
    }
    return ok;
}

void DeviceManager::stopRecord()
{
    if (m_recordThread && m_isRecording.load()) {
        m_isRecording.store(false);
        m_recordThread->stopRecord();
    }
}

void DeviceManager::onFrameDecoded(const QImage &frame)
{
    emit frameReceived(frame);
}

void DeviceManager::onRtspStreamConnected(int encType)
{
    qInfo() << "[DeviceManager] RTSP 视频流连接成功, 编码类型:" << (encType == 3 ? "H.265" : "H.264");
    if (m_recordThread) {
        m_recordThread->setEncType(encType);
    }
    emit streamStarted(true);
}

void DeviceManager::onRtspStreamError(const QString &err)
{
    qWarning() << "[DeviceManager] RTSP 视频流错误:" << err;
    emit errorOccurred(err);
}

void DeviceManager::onPTZAngleReceived(float pan, float tilt, float panSpeed, float tiltSpeed)
{
    if (m_dataRecorder) {
        m_dataRecorder->updatePTZData(pan, tilt, panSpeed, tiltSpeed);
    }
    emit ptzAngleReceived(pan, tilt);
}

void DeviceManager::onRecordError(const QString &error)
{
    emit errorOccurred(QString("录像错误: %1").arg(error));
}

void DeviceManager::onRecordFinished(const QString &filePath)
{
    emit recordStopped(filePath);
}

void DeviceManager::onDataRecordStarted(const QString &filePath)
{
    Q_UNUSED(filePath);
}

void DeviceManager::onDataRecordStopped(const QString &filePath)
{
    Q_UNUSED(filePath);
}
