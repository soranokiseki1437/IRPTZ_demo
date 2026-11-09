#include "netptzcontroller.h"
#include <QtMath>
#include <QDebug>
#include <cstring>

NetPtzController::NetPtzController(SvSdkSession *session, LONG channel, QObject *parent)
    : IPtzController(parent)
    , m_session(session)
    , m_channel(channel)
{
    m_autoQueryTimer = new QTimer(this);
    connect(m_autoQueryTimer, &QTimer::timeout, this, &NetPtzController::onAutoQueryTimer);

    m_stopWatchdog = new QTimer(this);
    m_stopWatchdog->setSingleShot(true);
    connect(m_stopWatchdog, &QTimer::timeout, this, &NetPtzController::onStopWatchdogTimeout);
}

NetPtzController::~NetPtzController()
{
    prepareForDestruction();
}

void NetPtzController::init()
{
    qInfo() << "[NetPtzController] 工作线程初始化完成, channel=" << m_channel;
}

void NetPtzController::setSpeedRange(int minSdkSpeed, int maxSdkSpeed)
{
    m_minSdkSpeed = qMax(0, minSdkSpeed);
    m_maxSdkSpeed = qMax(m_minSdkSpeed, maxSdkSpeed);
}

DWORD NetPtzController::mapPelcoSpeed(uint8_t pelcoSpeed) const
{
    if (pelcoSpeed == 0 || m_maxSdkSpeed == 0) return 0;
    float ratio = qBound(0.0f, pelcoSpeed / 63.0f, 1.0f);
    return static_cast<DWORD>(qRound(m_minSdkSpeed + ratio * (m_maxSdkSpeed - m_minSdkSpeed)));
}

void NetPtzController::sendSdkPtz(DWORD dwCmd, DWORD dwStop, DWORD dwSpeed)
{
    if (!isOpen()) return;
    LONG uid = m_session->userId();
    bool adv = m_session->isAdvVer164();

    QByteArray txPkt;
    txPkt.append(static_cast<char>(dwCmd & 0xFF));
    txPkt.append(static_cast<char>((dwCmd >> 8) & 0xFF));
    txPkt.append(static_cast<char>(dwStop & 0xFF));
    txPkt.append(static_cast<char>(dwSpeed & 0xFF));
    emit rawDataInout(true, txPkt);

    if (dwStop == 1) {
        if (adv) {
            // 先按上一次运动方向发 stop=1，再发 cmd=0, stop=1 双重保险
            if (m_lastCmd != 0) {
                SV_NET_DEV_PTZControl_EX(uid, m_channel, m_lastCmd, 1);
            }
            SV_NET_DEV_PTZControl_EX(uid, m_channel, 0, 1);
        } else {
            DWORD stopCmd = (m_lastCmd != 0) ? m_lastCmd : UP_RIGHT;
            SV_NET_DEV_PTZControl(uid, stopCmd, 1);
        }
        return;
    }

    if (adv) {
        if (dwSpeed == 0) {
            SV_NET_DEV_PTZControl_EX(uid, m_channel, dwCmd, 0);
        } else {
            SV_NET_DEV_PTZControlWithSpeed_EX(uid, m_channel, dwCmd, 0, dwSpeed);
        }
    } else {
        if (dwSpeed == 0) {
            SV_NET_DEV_PTZControl(uid, dwCmd, 0);
        } else {
            SV_NET_DEV_PTZControlWithSpeed(uid, dwCmd, 0, dwSpeed);
        }
    }
}

void NetPtzController::moveDirection(uint8_t cmd2, uint8_t hSpeed, uint8_t vSpeed)
{
    if (!isOpen()) return;

    bool left  = (cmd2 & 0x04) != 0;
    bool right = (cmd2 & 0x02) != 0;
    bool up    = (cmd2 & 0x08) != 0;
    bool down  = (cmd2 & 0x10) != 0;

    // 主次轴抑制：SDK 对角线运动只接受单一速度值。
    // 若次轴速度不足主轴的 30%，优先只走主轴，防止次轴以主轴高速过冲。
    if ((left || right) && (up || down)) {
        if (vSpeed < hSpeed * 0.3f) {
            up = down = false;
        } else if (hSpeed < vSpeed * 0.3f) {
            left = right = false;
        }
    }

    DWORD dwCommand = 0;
    if (up && left)         dwCommand = UP_LEFT;
    else if (up && right)   dwCommand = UP_RIGHT;
    else if (down && left)  dwCommand = DOWN_LEFT;
    else if (down && right) dwCommand = DOWN_RIGHT;
    else if (up)            dwCommand = TILT_UP;
    else if (down)          dwCommand = TILT_DOWN;
    else if (left)          dwCommand = PAN_LEFT;
    else if (right)         dwCommand = PAN_RIGHT;

    if (dwCommand == 0) {
        stop();
        return;
    }

    // 每次收到有效运动指令，刷新 500ms 失联保护看门狗
    m_stopWatchdog->start(500);

    uint8_t rawSpeed = qMax(hSpeed, vSpeed);
    if (rawSpeed == 0) rawSpeed = 30;
    DWORD dwSpeed = mapPelcoSpeed(rawSpeed);

    // 指令去重：方向与速度均未改变时，不重复发起阻塞式网络请求
    if (m_isMoving && dwCommand == m_lastCmd && dwSpeed == m_lastSpeed) {
        return;
    }

    sendSdkPtz(dwCommand, 0, dwSpeed);
    m_lastCmd = dwCommand;
    m_lastSpeed = dwSpeed;
    m_isMoving = true;
}

void NetPtzController::stop()
{
    m_stopWatchdog->stop();
    if (!isOpen()) {
        m_isMoving = false;
        m_lastCmd = 0;
        m_lastSpeed = 0;
        return;
    }
    sendSdkPtz(m_lastCmd, 1, 0);
    m_isMoving = false;
    m_lastCmd = 0;
    m_lastSpeed = 0;
}

void NetPtzController::onStopWatchdogTimeout()
{
    qWarning() << "[NetPtzController] 500ms 未收到后续运动指令，看门狗触发自动急停";
    stop();
}

void NetPtzController::moveTo(float pan, float tilt, float /*speed*/)
{
    if (!isOpen() || !m_angleSupported) return;
    SV_NET_DEV_PTZ_POSITION pos{};
    pos.byEnable = 1;
    pos.struPtzPos.wAction = 1;
    float scale = (m_decodeMode == AngleDecodeMode::CentiDegree) ? 100.0f : 10.0f;
    float normPan = std::fmod(pan, 360.0f);
    if (normPan < 0) normPan += 360.0f;
    float normTilt = (tilt < 0) ? (360.0f + tilt) : tilt;
    pos.struPtzPos.wPanPos  = static_cast<WORD>(qRound(normPan * scale));
    pos.struPtzPos.wTiltPos = static_cast<WORD>(qRound(normTilt * scale));
    SV_NET_DEV_SetPtzPosition(m_session->userId(), m_channel, 0, &pos);
}

void NetPtzController::callPreset(uint8_t presetId)
{
    if (!isOpen()) return;
    SV_NET_DEV_PTZPreset_EX(m_session->userId(), m_channel, RUN_PRESET, presetId);
}

void NetPtzController::setPreset(uint8_t presetId)
{
    if (!isOpen()) return;
    SV_NET_DEV_PTZPreset_EX(m_session->userId(), m_channel, SET_PRESET, presetId);
}

float NetPtzController::decodeRawAngle(WORD raw, bool isTilt) const
{
    float deg = 0.0f;
    if (m_decodeMode == AngleDecodeMode::HikBcd) {
        int d3 = (raw >> 12) & 0xF;
        int d2 = (raw >> 8)  & 0xF;
        int d1 = (raw >> 4)  & 0xF;
        int d0 = raw & 0xF;
        deg = (d3 * 1000 + d2 * 100 + d1 * 10 + d0) * 0.1f;
    } else if (m_decodeMode == AngleDecodeMode::CentiDegree) {
        deg = raw * 0.01f;
    } else {
        deg = raw * 0.1f;
    }
    if (isTilt && deg > 180.0f) {
        deg -= 360.0f; // 将无符号 WORD 的 270~360° 映射回 -90~0°
    }
    return deg;
}

void NetPtzController::queryAngle()
{
    if (!isOpen() || !m_angleSupported) return;

    SV_NET_DEV_PTZ_POSITION pos{};
    LONG ret = SV_NET_DEV_GetPtzPosition(m_session->userId(), m_channel, 0, &pos);
    if (ret < 0) {
        if (++m_queryFailCount >= 10) {
            m_angleSupported = false;
            m_autoQueryTimer->stop();
            m_autoQuerying.store(false);
            qWarning() << "[NetPtzController] GetPtzPosition 连续失败，已自动降级关闭角度轮询";
        }
        return;
    }
    m_queryFailCount = 0;

    QByteArray rxPkt;
    rxPkt.append(static_cast<char>(pos.struPtzPos.wPanPos & 0xFF));
    rxPkt.append(static_cast<char>((pos.struPtzPos.wPanPos >> 8) & 0xFF));
    rxPkt.append(static_cast<char>(pos.struPtzPos.wTiltPos & 0xFF));
    rxPkt.append(static_cast<char>((pos.struPtzPos.wTiltPos >> 8) & 0xFF));
    emit rawDataInout(false, rxPkt);

    float newPan  = decodeRawAngle(pos.struPtzPos.wPanPos, false);
    float newTilt = decodeRawAngle(pos.struPtzPos.wTiltPos, true);

    if (m_hasLastAngle) {
        float dt = m_angleTimer.restart() / 1000.0f;
        if (dt > 0.005f) {
            float dPan = newPan - m_currentPan;
            if (dPan > 180.0f) dPan -= 360.0f;
            if (dPan < -180.0f) dPan += 360.0f;
            float instPanSpeed  = dPan / dt;
            float instTiltSpeed = (newTilt - m_currentTilt) / dt;
            m_currentPanSpeed  = 0.7f * m_currentPanSpeed  + 0.3f * instPanSpeed;
            m_currentTiltSpeed = 0.7f * m_currentTiltSpeed + 0.3f * instTiltSpeed;
        }
    } else {
        m_angleTimer.start();
        m_hasLastAngle = true;
    }

    m_currentPan  = newPan;
    m_currentTilt = newTilt;
    emit angleReceived(m_currentPan, m_currentTilt, m_currentPanSpeed, m_currentTiltSpeed);
}

void NetPtzController::startAutoQuery(int intervalMs)
{
    if (!m_angleSupported) return;
    m_autoQueryTimer->start(qMax(50, intervalMs));
    m_autoQuerying.store(true);
}

void NetPtzController::stopAutoQuery()
{
    m_autoQueryTimer->stop();
    m_autoQuerying.store(false);
}

void NetPtzController::onAutoQueryTimer()
{
    queryAngle();
}

void NetPtzController::prepareForDestruction()
{
    stopAutoQuery();
    stop();
}
