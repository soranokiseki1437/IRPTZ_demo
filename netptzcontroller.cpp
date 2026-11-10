#include "netptzcontroller.h"
#include <QtMath>
#include <QDebug>
#include <QElapsedTimer>
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
    m_watchdogStamp.restart();

    uint8_t rawSpeed = qMax(hSpeed, vSpeed);
    if (rawSpeed == 0) rawSpeed = 30;
    DWORD dwSpeed = mapPelcoSpeed(rawSpeed);

    // 指令去重：方向与速度均未改变时，不重复发起阻塞式网络请求
    if (m_isMoving && dwCommand == m_lastCmd && dwSpeed == m_lastSpeed) {
        return;
    }

    // 排障：量化运动指令 SDK 调用耗时（阻塞网络调用会推迟同线程上排队的后续指令/停止指令）
    QElapsedTimer sdkTimer;
    sdkTimer.start();
    sendSdkPtz(dwCommand, 0, dwSpeed);
    if (sdkTimer.elapsed() > 200) {
        qWarning() << "[NetPtzController] 运动指令 SDK 调用耗时:" << sdkTimer.elapsed()
                   << "ms cmd =" << dwCommand << "speed =" << dwSpeed;
    }
    // 阻塞调用返回后重新计时：SDK 阻塞期间早已到期的超时事件不能在恢复瞬间补发急停
    m_watchdogStamp.restart();
    m_stopWatchdog->start(500);
    m_lastCmd = dwCommand;
    m_lastSpeed = dwSpeed;
    m_isMoving = true;
}

void NetPtzController::stop()
{
    m_stopWatchdog->stop();
    m_watchdogStamp.invalidate();
    if (!isOpen()) {
        m_isMoving = false;
        m_lastCmd = 0;
        m_lastSpeed = 0;
        return;
    }
    // 排障：停止指令耗时尤其关键——松开按钮后若此调用阻塞，云台会过冲，体感就是"延迟"
    QElapsedTimer sdkTimer;
    sdkTimer.start();
    sendSdkPtz(m_lastCmd, 1, 0);
    if (sdkTimer.elapsed() > 200) {
        qWarning() << "[NetPtzController] 停止指令 SDK 调用耗时:" << sdkTimer.elapsed() << "ms";
    }
    m_isMoving = false;
    m_lastCmd = 0;
    m_lastSpeed = 0;
}

void NetPtzController::onStopWatchdogTimeout()
{
    if (!m_isMoving) return; // 已处于停止态，忽略残留的超时事件
    // SDK 阻塞期间到期的超时事件会在事件循环恢复瞬间补发，此时指令刚下发，
    // 不应立即急停；只有距上次有效运动指令确实超过判定窗口才执行
    if (m_watchdogStamp.isValid() && m_watchdogStamp.elapsed() < 400) {
        qDebug() << "[NetPtzController] 忽略过期的看门狗超时事件, 距上次指令"
                 << m_watchdogStamp.elapsed() << "ms";
        return;
    }
    qWarning() << "[NetPtzController] 500ms 未收到后续运动指令，看门狗触发自动急停"
               << " cmd =" << m_lastCmd << " moving =" << m_isMoving;
    stop();
}

void NetPtzController::callPreset(uint8_t presetId)
{
    if (!isOpen()) return;
    const LONG ret = SV_NET_DEV_PTZPreset_EX(m_session->userId(), m_channel,
                                             RUN_PRESET, presetId);
    if (ret < 0) {
        qWarning() << "[NetPtzController] 调用预置位失败 preset =" << presetId
                   << " ret =" << ret << " SDK err =" << SV_NET_DEV_GetLastError();
    } else {
        qInfo() << "[NetPtzController] 已调用预置位 preset =" << presetId;
    }
}

void NetPtzController::setPreset(uint8_t presetId)
{
    if (!isOpen()) return;
    const LONG ret = SV_NET_DEV_PTZPreset_EX(m_session->userId(), m_channel,
                                             SET_PRESET, presetId);
    if (ret < 0) {
        qWarning() << "[NetPtzController] 保存预置位失败 preset =" << presetId
                   << " ret =" << ret << " SDK err =" << SV_NET_DEV_GetLastError();
    } else {
        qInfo() << "[NetPtzController] 已把当前位置保存为预置位 preset =" << presetId;
    }
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

    // 运动期间跳过角度查询：GetPtzPosition 与运动/停止指令串行执行，
    // 任何一次查询都会排在用户指令之前，是操作手感延迟的直接来源
    if (m_isMoving) return;

    // 排障：量化角度查询耗时。SV_NET_DEV_GetPtzPosition 是阻塞网络调用，
    // 与运动指令同在本线程事件循环内串行——查询耗时会直接推迟 moveDirection/stop 执行
    static QElapsedTimer s_statsTimer;
    static qint64 s_totalMs = 0;
    static int s_count = 0;
    static qint64 s_maxMs = 0;
    static bool s_statsInit = false;
    if (!s_statsInit) {
        s_statsTimer.start();
        s_statsInit = true;
    }
    QElapsedTimer callTimer;
    callTimer.start();

    SV_NET_DEV_PTZ_POSITION pos{};
    LONG ret = SV_NET_DEV_GetPtzPosition(m_session->userId(), m_channel, 0, &pos);

    const qint64 callMs = callTimer.elapsed();
    s_totalMs += callMs;
    ++s_count;
    s_maxMs = qMax(s_maxMs, callMs);
    if (s_statsTimer.elapsed() >= 5000) {
        if (s_count > 0) {
            qInfo() << "[NetPtzController] 5s 角度查询: 次数 =" << s_count
                    << " 平均耗时 =" << (s_totalMs / s_count) << "ms"
                    << " 最大耗时 =" << s_maxMs << "ms";
        }
        s_totalMs = 0;
        s_count = 0;
        s_maxMs = 0;
        s_statsTimer.restart();
    }

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
    // 仅在数据变化时回显：该设备位置回读恒为全 0，逐次回显只会刷屏 "00 00 00 00"
    static QByteArray s_lastRawPkt;
    if (rxPkt != s_lastRawPkt) {
        s_lastRawPkt = rxPkt;
        emit rawDataInout(false, rxPkt);
    }
    // 连续全 0 说明设备不支持当前位置回读（GetPtzPosition 读的是场景坐标，非实时角度）
    if (pos.struPtzPos.wPanPos == 0 && pos.struPtzPos.wTiltPos == 0) {
        if (++m_zeroPosCount == 25) {
            qWarning() << "[NetPtzController] 位置回读连续返回全 0，"
                          "该设备可能不支持实时角度查询，角度显示不可用";
        }
    } else {
        m_zeroPosCount = 0;
    }

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
