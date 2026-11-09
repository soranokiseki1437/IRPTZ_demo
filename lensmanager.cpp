#include "lensmanager.h"
#include <QDebug>
#include <QByteArray>

LensWorker::LensWorker(QObject *parent)
    : QObject(parent)
{
}

LensWorker::~LensWorker()
{
}

void LensWorker::setSession(SvSdkSession *session)
{
    m_session = session;
}

void LensWorker::executeCommand(LensCommand command, uint8_t speed, uint8_t stop)
{
    if (!m_session || !m_session->isLoggedIn()) {
        qWarning() << "[LensWorker] 无法执行镜头命令 - 设备未登录";
        return;
    }

    // 命令码来自厂家上位机 sdkdemo.exe 的反汇编（v2024-12-11）：
    //   变倍+  PTZControlWithSpeed_EX(uid, 0, 46, 0, speed)
    //   变倍-  PTZControlWithSpeed_EX(uid, 0, 47, 0, speed)
    //   聚焦+  PTZControlWithSpeed_EX(uid, 0, 49, 0, speed)
    //   聚焦-  PTZControlWithSpeed_EX(uid, 0, 48, 0, speed)
    //   自动聚焦 PTZControlWithSpeed_EX(uid, 0, 53, 0, speed)
    //   松开   PTZControl_EX(uid, 0, 0, 1)
    DWORD dwCmd = 0;
    const char *name = "";
    switch (command) {
    case CmdZoomIn:
        dwCmd = 46; name = "变倍放大";
        break;
    case CmdZoomOut:
        dwCmd = 47; name = "变倍缩小";
        break;
    case CmdFocusFar:
        dwCmd = 49; name = "聚焦远";
        break;
    case CmdFocusNear:
        dwCmd = 48; name = "聚焦近";
        break;
    case CmdOnePushFocus:
        dwCmd = 53; name = "自动聚焦";
        break;
    default:
        // 厂家上位机的 irisA/irisP 槽函数是空的，光圈没有可用命令
        qWarning() << "[LensWorker] 厂家未实现该镜头命令（光圈），未下发, command =" << command;
        return;
    }

    const LONG uid = m_session->userId();

    // 松开按钮：与厂家 On_pushButton_Release 一致，发命令 0 + stop=1 通用停止
    if (stop) {
        const LONG ret = SV_NET_DEV_PTZControl_EX(uid, 0, 0, 1);
        qInfo() << "[LensWorker]" << name << "停止 ret =" << ret
                << " SDK err =" << (ret < 0 ? SV_NET_DEV_GetLastError() : 0);
        return;
    }

    // 厂家上位机的速度滑杆范围与云台速度一致，这里夹到 SDK 有效区间
    const DWORD dwSpeed = qBound<DWORD>(1, speed, 63);
    const LONG ret = SV_NET_DEV_PTZControlWithSpeed_EX(uid, 0, dwCmd, 0, dwSpeed);
    qInfo() << "[LensWorker]" << name << "下发 cmd =" << dwCmd
            << " speed =" << dwSpeed << " ret =" << ret
            << " SDK err =" << (ret < 0 ? SV_NET_DEV_GetLastError() : 0);
}

void LensWorker::executeSetFocusMode(bool isManual)
{
    qDebug() << "[LensWorker] 设置对焦模式:" << (isManual ? "手动" : "自动");
    emit focusModeSetResult(true, isManual);
}

void LensWorker::executeSetIrisMode(bool isManual)
{
    qDebug() << "[LensWorker] 设置光圈模式:" << (isManual ? "手动" : "自动");
    emit irisModeSetResult(true, isManual);
}

// ============================================
// LensManager
// ============================================

LensManager::LensManager(QObject *parent)
    : QObject(parent)
    , m_workerThread(new QThread(this))
    , m_worker(new LensWorker())
{
    m_worker->moveToThread(m_workerThread);
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(this, &LensManager::commandQueued, m_worker, &LensWorker::executeCommand);
    connect(this, &LensManager::setFocusModeRequested, m_worker, &LensWorker::executeSetFocusMode);
    connect(this, &LensManager::setIrisModeRequested, m_worker, &LensWorker::executeSetIrisMode);

    connect(m_worker, &LensWorker::focusModeSetResult, this, &LensManager::onFocusModeSetResult);
    connect(m_worker, &LensWorker::irisModeSetResult, this, &LensManager::onIrisModeSetResult);

    m_workerThread->start();
}

LensManager::~LensManager()
{
    if (m_workerThread) {
        m_workerThread->quit();
        m_workerThread->wait();
    }
}

void LensManager::setSession(SvSdkSession *session)
{
    QMutexLocker lock(&m_mutex);
    m_session = session;
    m_ready = (session && session->isLoggedIn());
    QMetaObject::invokeMethod(m_worker, "setSession", Qt::QueuedConnection,
                              Q_ARG(SvSdkSession*, session));
}

bool LensManager::isReady() const
{
    QMutexLocker lock(&m_mutex);
    return m_ready;
}

LensManager::FocusMode LensManager::getFocusMode() const
{
    QMutexLocker lock(&m_mutex);
    return m_focusMode;
}

void LensManager::setFocusMode(bool isManual)
{
    emit setFocusModeRequested(isManual);
}

void LensManager::setIrisMode(bool isManual)
{
    emit setIrisModeRequested(isManual);
}

void LensManager::zoomIn(uint8_t speed, bool stop)
{
    emit commandQueued(LensWorker::CmdZoomIn, speed, stop ? 1 : 0);
}

void LensManager::zoomOut(uint8_t speed, bool stop)
{
    emit commandQueued(LensWorker::CmdZoomOut, speed, stop ? 1 : 0);
}

void LensManager::focusNear(uint8_t speed, bool stop)
{
    emit commandQueued(LensWorker::CmdFocusNear, speed, stop ? 1 : 0);
}

void LensManager::focusFar(uint8_t speed, bool stop)
{
    emit commandQueued(LensWorker::CmdFocusFar, speed, stop ? 1 : 0);
}

void LensManager::irisOpen(uint8_t speed, bool stop)
{
    emit commandQueued(LensWorker::CmdIrisOpen, speed, stop ? 1 : 0);
}

void LensManager::irisClose(uint8_t speed, bool stop)
{
    emit commandQueued(LensWorker::CmdIrisClose, speed, stop ? 1 : 0);
}

void LensManager::onePushFocus()
{
    emit commandQueued(LensWorker::CmdOnePushFocus, 0, 0);
}

void LensManager::onFocusModeSetResult(bool success, bool isManual)
{
    if (success) {
        {
            QMutexLocker lock(&m_mutex);
            m_focusMode = isManual ? ManualFocus : AutoFocus;
        }
        emit focusModeChanged(isManual);
    }
}

void LensManager::onIrisModeSetResult(bool success, bool isManual)
{
    if (success) {
        {
            QMutexLocker lock(&m_mutex);
            m_isManualIris = isManual;
        }
        emit irisModeChanged(isManual);
    }
}

