#include "lensmanager.h"
#include <QDebug>

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
    }

    const LONG uid = m_session->userId();

    if (stop) {
        const LONG ret = SV_NET_DEV_PTZControl_EX(uid, 0, 0, 1);
        qInfo() << "[LensWorker]" << name << "停止 ret =" << ret
                << " SDK err =" << (ret < 0 ? SV_NET_DEV_GetLastError() : 0);
        return;
    }

    const DWORD dwSpeed = qBound<DWORD>(1, speed, 63);
    const LONG ret = SV_NET_DEV_PTZControlWithSpeed_EX(uid, 0, dwCmd, 0, dwSpeed);
    qInfo() << "[LensWorker]" << name << "下发 cmd =" << dwCmd
            << " speed =" << dwSpeed << " ret =" << ret
            << " SDK err =" << (ret < 0 ? SV_NET_DEV_GetLastError() : 0);
}

LensManager::LensManager(QObject *parent)
    : QObject(parent)
    , m_workerThread(new QThread(this))
    , m_worker(new LensWorker())
{
    m_worker->moveToThread(m_workerThread);
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(this, &LensManager::commandQueued, m_worker, &LensWorker::executeCommand);

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
