#include "svsdksession.h"
#include <QDebug>
#include <cstring>
#include <cstdlib>

SvSdkSession::SvSdkSession(QObject *parent) : QObject(parent) {}

SvSdkSession::~SvSdkSession()
{
    logout();
    if (m_inited) {
        SV_NET_DEV_SDK_Release();
        m_inited = false;
    }
}

bool SvSdkSession::ensureInit()
{
    QMutexLocker lock(&m_mutex);
    if (!m_inited) {
        SV_NET_DEV_SDK_Init();
        SV_NET_DEV_SetExceptionCallBack(0, nullptr, &SvSdkSession::onSdkException, this);
        m_inited = true;
    }
    return true;
}

bool SvSdkSession::parseAdvVer164(const char *softVer)
{
    if (!softVer) return true;
    const char *p = std::strstr(softVer, "V");
    if (!p) p = std::strstr(softVer, "v");
    if (!p) return true;
    int major = std::atoi(p + 1);
    const char *dot1 = std::strstr(p, ".");
    int minor = dot1 ? std::atoi(dot1 + 1) : 0;
    const char *dot2 = dot1 ? std::strstr(dot1 + 1, ".") : nullptr;
    int patch = dot2 ? std::atoi(dot2 + 1) : 0;
    return (major * 100 + minor * 10 + patch) >= 164;
}

bool SvSdkSession::login(const QString &ip, uint16_t port, const QString &user, const QString &pwd, QString *errOut)
{
    ensureInit();
    logout();

    SV_NET_DEV_USER_LOGIN_INFO loginInfo{};
    SV_NET_DEV_DEVICEINFO devInfo{};
    std::strncpy(loginInfo.sDeviceAddress, ip.toStdString().c_str(), SV_NET_DEV_ADDRESS_MAX_LEN - 1);
    loginInfo.wPort = (port == 0) ? 8000 : port;
    std::strncpy(loginInfo.sUserName, user.toStdString().c_str(), SV_NET_DEV_LOGIN_USERNAME_MAX_LEN - 1);
    std::strncpy(loginInfo.sPassword, pwd.toStdString().c_str(), SV_NET_DEV_LOGIN_PASSWD_MAX_LEN - 1);
    loginInfo.bUseAsynLogin = 0;

    LONG uid = SV_NET_DEV_Login(&loginInfo, &devInfo);
    if (uid < 0) {
        LONG err = SV_NET_DEV_GetLastError();
        if (errOut) *errOut = QString("SV_NET_DEV_Login 失败, 错误码=%1").arg(err);
        qWarning() << "[SvSdkSession] Login failed with error:" << err;
        return false;
    }

    // 查询设备标准信息与固件版本
    SV_NET_DEV_STDINFO stdInfo{};
    if (SV_NET_DEV_GETSTDINFO(uid, &stdInfo) >= 0) {
        m_fwVersion = QString::fromLatin1(reinterpret_cast<const char*>(stdInfo.softwareVersion));
        bool adv = parseAdvVer164(reinterpret_cast<const char*>(stdInfo.softwareVersion));
        m_advVer164.store(adv);
        if (!adv) {
            // 旧版固件 PTZ 需先开启 RS485(2) 透明通道
            SV_NET_DEV_START_SERIALTRANS(uid, 2);
        }
    } else {
        m_advVer164.store(true);
    }

    m_userId.store(uid);
    qInfo() << "[SvSdkSession] 登录成功 userID=" << uid
            << " 固件版本=" << m_fwVersion
            << " AdvVer164=" << m_advVer164.load();
    return true;
}

void SvSdkSession::logout()
{
    LONG uid = m_userId.exchange(-1);
    if (uid >= 0) {
        SV_NET_DEV_Logout(uid);
        qInfo() << "[SvSdkSession] 已注销 userID=" << uid;
    }
}

void CALLBACK SvSdkSession::onSdkException(DWORD dwType, LONG /*lUserID*/, LONG /*lHandle*/, void *pUser)
{
    auto *self = static_cast<SvSdkSession*>(pUser);
    if (!self) return;
    if (dwType == SUCCESS_RECONNECT) {
        emit self->reconnected();
    } else {
        emit self->networkException(dwType);
    }
}

