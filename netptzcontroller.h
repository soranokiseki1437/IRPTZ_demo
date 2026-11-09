#ifndef NETPTZCONTROLLER_H
#define NETPTZCONTROLLER_H

#include "iptzcontroller.h"
#include "svsdksession.h"
#include <QTimer>
#include <QElapsedTimer>
#include <atomic>

class NetPtzController : public IPtzController
{
    Q_OBJECT
public:
    enum class AngleDecodeMode {
        DeciDegree,  // 0.1 度单位 (0..3600 -> 0..360.0)
        CentiDegree, // 0.01 度单位 (0..36000 -> 0..360.00)
        HikBcd       // 海康式十六进制 BCD (0x1800 -> 180.0)
    };

    explicit NetPtzController(SvSdkSession *session, LONG channel = 0, QObject *parent = nullptr);
    ~NetPtzController() override;

    bool isOpen() const override { return m_session && m_session->isLoggedIn(); }
    float getCurrentPan() const override { return m_currentPan; }
    float getCurrentTilt() const override { return m_currentTilt; }
    bool isAutoQuerying() const override { return m_autoQuerying.load(); }

    void setAngleDecodeMode(AngleDecodeMode mode) { m_decodeMode = mode; }
    void setSpeedRange(int minSdkSpeed, int maxSdkSpeed);

public slots:
    void init() override;
    void moveTo(float pan, float tilt, float speed = 0.5f) override;
    void stop() override;
    void queryAngle() override;
    void moveDirection(uint8_t cmd2, uint8_t hSpeed, uint8_t vSpeed) override;
    void callPreset(uint8_t presetId) override;
    void setPreset(uint8_t presetId) override;
    void startAutoQuery(int intervalMs = 100) override;
    void stopAutoQuery() override;
    void prepareForDestruction() override;

private slots:
    void onAutoQueryTimer();
    void onStopWatchdogTimeout();

private:
    DWORD mapPelcoSpeed(uint8_t pelcoSpeed) const;
    float decodeRawAngle(WORD raw, bool isTilt) const;
    void sendSdkPtz(DWORD dwCmd, DWORD dwStop, DWORD dwSpeed);

    SvSdkSession *m_session = nullptr;
    LONG m_channel = 0;

    float m_currentPan = 0.0f;
    float m_currentTilt = 0.0f;
    float m_currentPanSpeed = 0.0f;
    float m_currentTiltSpeed = 0.0f;
    QElapsedTimer m_angleTimer;
    bool m_hasLastAngle = false;

    // 去重缓存：避免 25Hz 跟踪帧重复阻塞调用 SDK
    DWORD m_lastCmd = 0;
    DWORD m_lastSpeed = 0;
    bool m_isMoving = false;

    int m_minSdkSpeed = 1;
    int m_maxSdkSpeed = 63;
    AngleDecodeMode m_decodeMode = AngleDecodeMode::DeciDegree;
    int m_queryFailCount = 0;
    int m_zeroPosCount = 0; // 位置回读连续全 0 计数，用于判定设备是否支持角度回读
    bool m_angleSupported = true;

    QTimer *m_autoQueryTimer = nullptr;
    QTimer *m_stopWatchdog = nullptr;
    QElapsedTimer m_watchdogStamp; // 最近一次运动指令生效时刻，用于丢弃过期看门狗事件
    std::atomic<bool> m_autoQuerying{false};
};

#endif // NETPTZCONTROLLER_H

