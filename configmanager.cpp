#include "configmanager.h"
#include <QSettings>
#include <QMutexLocker>
#include <QDebug>
#include <QCoreApplication>
#include <QDir>

// P7/P8：
// - 所有 getter/setter 加互斥锁（配置可能被 UI 线程与工作线程并发读写）
// - 密码不再内置明文默认值，仅从 QSettings 读取；无则 UI 输入
// - 析构不再 saveConfig()，由 MainWindow 在 aboutToQuit 时调用（QApplication
//   销毁阶段 QSettings 不可靠）
ConfigManager::ConfigManager(QObject *parent) : QObject(parent)
    , m_deviceIP("192.168.1.68")
    , m_deviceUsername("admin")
    , m_rtspPath("Streaming/Channels/101")
    , m_captureInterval(10)
    , m_captureQuality(80)
    , m_savePath(QCoreApplication::applicationDirPath() + "/captures")
    , m_trackKp(0.15f)
    , m_trackKi(0.005f)
    , m_trackKd(0.03f)
    , m_trackKff(0.5f)
    , m_trackTPredict(5)
    , m_trackDeadZone(15)
    , m_trackMaxSpeed(20)
    , m_videoSavePath(QCoreApplication::applicationDirPath() + "/videos")
{
    loadConfig();
}

ConfigManager::~ConfigManager()
{
    // P8：保存时机移至 aboutToQuit（见 MainWindow 构造），析构不再写盘
}

QString ConfigManager::getDeviceIP() const
{
    QMutexLocker locker(&m_mutex);
    return m_deviceIP;
}

void ConfigManager::setDeviceIP(const QString &ip)
{
    QMutexLocker locker(&m_mutex);
    m_deviceIP = ip;
}

QString ConfigManager::getDeviceUsername() const
{
    QMutexLocker locker(&m_mutex);
    return m_deviceUsername;
}

void ConfigManager::setDeviceUsername(const QString &username)
{
    QMutexLocker locker(&m_mutex);
    m_deviceUsername = username;
}

QString ConfigManager::getDevicePassword() const
{
    QMutexLocker locker(&m_mutex);
    return m_devicePassword;
}

void ConfigManager::setDevicePassword(const QString &password)
{
    QMutexLocker locker(&m_mutex);
    m_devicePassword = password;
}

QString ConfigManager::getRtspPath() const
{
    QMutexLocker locker(&m_mutex);
    return m_rtspPath;
}

void ConfigManager::setRtspPath(const QString &path)
{
    QMutexLocker locker(&m_mutex);
    m_rtspPath = path;
}

int ConfigManager::getCaptureInterval() const
{
    QMutexLocker locker(&m_mutex);
    return m_captureInterval;
}

void ConfigManager::setCaptureInterval(int interval)
{
    QMutexLocker locker(&m_mutex);
    m_captureInterval = interval;
}

int ConfigManager::getCaptureQuality() const
{
    QMutexLocker locker(&m_mutex);
    return m_captureQuality;
}

void ConfigManager::setCaptureQuality(int quality)
{
    QMutexLocker locker(&m_mutex);
    m_captureQuality = quality;
}

QString ConfigManager::getSavePath() const
{
    QMutexLocker locker(&m_mutex);
    return m_savePath;
}

void ConfigManager::setSavePath(const QString &path)
{
    QMutexLocker locker(&m_mutex);
    m_savePath = path;
}

// 跟踪参数
float ConfigManager::getTrackKp() const
{
    QMutexLocker locker(&m_mutex);
    return m_trackKp;
}
float ConfigManager::getTrackKi() const
{
    QMutexLocker locker(&m_mutex);
    return m_trackKi;
}
float ConfigManager::getTrackKd() const
{
    QMutexLocker locker(&m_mutex);
    return m_trackKd;
}
float ConfigManager::getTrackKff() const
{
    QMutexLocker locker(&m_mutex);
    return m_trackKff;
}
int ConfigManager::getTrackTPredict() const
{
    QMutexLocker locker(&m_mutex);
    return m_trackTPredict;
}
int ConfigManager::getTrackDeadZone() const
{
    QMutexLocker locker(&m_mutex);
    return m_trackDeadZone;
}
int ConfigManager::getTrackMaxSpeed() const
{
    QMutexLocker locker(&m_mutex);
    return m_trackMaxSpeed;
}
void ConfigManager::setTrackParams(float kp, float ki, float kd, float kff, int tPred, int dz, int maxSpd)
{
    QMutexLocker locker(&m_mutex);
    m_trackKp = kp;
    m_trackKi = ki;
    m_trackKd = kd;
    m_trackKff = kff;
    m_trackTPredict = tPred;
    m_trackDeadZone = dz;
    m_trackMaxSpeed = maxSpd;
}

// M5: 录像路径
QString ConfigManager::getVideoSavePath() const
{
    QMutexLocker locker(&m_mutex);
    return m_videoSavePath;
}
void ConfigManager::setVideoSavePath(const QString &path)
{
    QMutexLocker locker(&m_mutex);
    m_videoSavePath = path;
}

void ConfigManager::saveConfig()
{
    QMutexLocker locker(&m_mutex);
    QSettings settings("RGB_PTZ", "RGB_PTZ_Integrated");
    settings.setValue("device/ip", m_deviceIP);
    settings.setValue("device/username", m_deviceUsername);
    settings.setValue("device/password", m_devicePassword);
    settings.setValue("device/rtspPath", m_rtspPath);
    settings.setValue("capture/interval", m_captureInterval);
    settings.setValue("capture/quality", m_captureQuality);
    settings.setValue("capture/savePath", m_savePath);
    settings.setValue("track/kp", m_trackKp);
    settings.setValue("track/ki", m_trackKi);
    settings.setValue("track/kd", m_trackKd);
    settings.setValue("track/kff", m_trackKff);
    settings.setValue("track/tPredict", m_trackTPredict);
    settings.setValue("track/deadZone", m_trackDeadZone);
    settings.setValue("track/maxSpeed", m_trackMaxSpeed);
    settings.setValue("video/savePath", m_videoSavePath);
    qDebug() << "Config saved";
}

void ConfigManager::loadConfig()
{
    // 加载于构造函数（单线程阶段），无需加锁
    QSettings settings("RGB_PTZ", "RGB_PTZ_Integrated");
    m_deviceIP = settings.value("device/ip", "192.168.1.68").toString();
    m_deviceUsername = settings.value("device/username", "admin").toString();
    m_rtspPath = settings.value("device/rtspPath", "Streaming/Channels/101").toString();
    // P8：密码不设明文默认值，QSettings 无记录时为空 → UI 密码框留空待输入
    m_devicePassword = settings.value("device/password").toString();
    m_captureInterval = settings.value("capture/interval", 10).toInt();
    m_captureQuality = settings.value("capture/quality", 80).toInt();
    m_savePath = settings.value("capture/savePath", QCoreApplication::applicationDirPath() + "/captures").toString();
    m_trackKp = settings.value("track/kp", 0.15f).toFloat();
    m_trackKi = settings.value("track/ki", 0.005f).toFloat();
    m_trackKd = settings.value("track/kd", 0.03f).toFloat();
    m_trackKff = settings.value("track/kff", 0.5f).toFloat();
    m_trackTPredict = settings.value("track/tPredict", 5).toInt();
    m_trackDeadZone = settings.value("track/deadZone", 15).toInt();
    m_trackMaxSpeed = settings.value("track/maxSpeed", 20).toInt();
    m_videoSavePath = settings.value("video/savePath", QCoreApplication::applicationDirPath() + "/videos").toString();
    qDebug() << "Config loaded";
}
