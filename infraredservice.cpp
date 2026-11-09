#include "infraredservice.h"
#include <QFile>
#include <QVector>
#include <QDebug>

InfraredService::InfraredService(SvSdkSession *session, QObject *parent)
    : QObject(parent), m_session(session) {}

bool InfraredService::triggerShutterFFC()
{
    if (!m_session || !m_session->isLoggedIn()) return false;
    emit ffcTriggered(800); // 挡片动作期间冻结伺服800ms
    LONG ret = SV_NET_DEV_CTRL_CORE_SHUTR(m_session->userId());
    qInfo() << "[InfraredService] 快门FFC校准执行结果:" << ret;
    return ret >= 0;
}

bool InfraredService::triggerBackgroundFFC()
{
    if (!m_session || !m_session->isLoggedIn()) return false;
    emit ffcTriggered(800);
    LONG ret = SV_NET_DEV_CTRL_CORE_FFC(m_session->userId());
    qInfo() << "[InfraredService] 背景FFC校准执行结果:" << ret;
    return ret >= 0;
}

bool InfraredService::setFFCMode(bool autoMode)
{
    if (!m_session || !m_session->isLoggedIn()) return false;
    int mode = autoMode ? 0 /*SHUTTER_MODE_AUTO*/ : 1 /*SHUTTER_MODE_MANUAL*/;
    LONG ret = SV_NET_DEV_SetFFCMode(m_session->userId(), mode);
    qInfo() << "[InfraredService] 设置FFC模式:" << (autoMode ? "自动" : "手动") << "结果:" << ret;
    return ret >= 0;
}

bool InfraredService::setPalette(int paletteIndex)
{
    if (!m_session || !m_session->isLoggedIn()) return false;
    int idx = qBound(1, paletteIndex, 12);
    LONG ret = SV_NET_DEV_CTRL_CORE_GEN(m_session->userId(), 0x08, static_cast<unsigned int>(idx));
    if (ret >= 0) {
        m_currentPalette = idx;
        emit paletteChanged(idx);
        qInfo() << "[InfraredService] 切换伪彩色模式为:" << idx;
        return true;
    }
    qWarning() << "[InfraredService] 切换伪彩色失败, 错误码:" << SV_NET_DEV_GetLastError();
    return false;
}

void InfraredService::lockTrackingPalette(bool trackingActive)
{
    if (trackingActive) {
        m_savedPaletteBeforeTrack = m_currentPalette;
        setFFCMode(false); // 跟踪期间临时关闭自动打挡片
        if (m_currentPalette != 1 && m_currentPalette != 2) {
            setPalette(1); // 自动切到白热，保证目标灰度单调一致性
        }
    } else {
        setFFCMode(true); // 跟踪停止后恢复自动打挡片
        if (m_savedPaletteBeforeTrack != m_currentPalette) {
            setPalette(m_savedPaletteBeforeTrack);
        }
    }
}

bool InfraredService::queryGlobalMaxMinTemp(float &maxTemp, float &minTemp)
{
    if (!m_session || !m_session->isLoggedIn()) return false;
    float retTemp[4] = {0.0f};
    LONG ret = SV_NET_DEV_GetThmlMtFSMaxMinVal(m_session->userId(), retTemp);
    if (ret == 0) { // 0 为成功
        maxTemp = retTemp[0];
        minTemp = retTemp[1];
        emit globalTempUpdated(maxTemp, minTemp);
        return true;
    }
    return false;
}

bool InfraredService::queryPointTemp(const QPoint &pt, float &tempOut)
{
    if (!m_session || !m_session->isLoggedIn()) return false;
    POINT_S32_S sdkPt{pt.x(), pt.y()};
    float val = 0.0f;
    LONG ret = SV_NET_DEV_GetThmlManualMtConfig(m_session->userId(), sdkPt, &val);
    if (ret > 0) { // >0 为成功
        tempOut = val;
        return true;
    }
    return false;
}

bool InfraredService::captureTempMatrix(const QString &rawFilePath, LONG channel)
{
    if (!m_session || !m_session->isLoggedIn()) return false;
    QVector<SHORT> buf(1920 * 1080, 0);
    int sizeReturned = 0;
    LONG ret = SV_NET_DEV_CaptureTEMPData(m_session->userId(), channel, buf.data(), &sizeReturned);
    if (ret > 0 && sizeReturned > 0) {
        QFile f(rawFilePath);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(reinterpret_cast<const char*>(buf.constData()), sizeReturned);
            f.close();
            qInfo() << "[InfraredService] 温度原始数据矩阵成功导出:" << rawFilePath << "大小:" << sizeReturned;
            return true;
        }
    }
    qWarning() << "[InfraredService] 抓取全屏温度矩阵失败, 错误码:" << SV_NET_DEV_GetLastError();
    return false;
}

