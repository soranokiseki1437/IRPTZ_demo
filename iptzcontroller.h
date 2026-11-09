#ifndef IPTZCONTROLLER_H
#define IPTZCONTROLLER_H

#include <QObject>
#include <QByteArray>
#include <QString>
#include <cstdint>

class IPtzController : public QObject
{
    Q_OBJECT
public:
    explicit IPtzController(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~IPtzController() = default;

    // 角度安全限位常量
    static constexpr float PAN_MIN  = 0.0f;
    static constexpr float PAN_MAX  = 360.0f;
    static constexpr float TILT_MIN = -40.0f;
    static constexpr float TILT_MAX = 40.0f;

    virtual bool isOpen() const = 0;
    virtual float getCurrentPan() const = 0;
    virtual float getCurrentTilt() const = 0;
    virtual bool isAutoQuerying() const = 0;

public slots:
    virtual void init() = 0;
    virtual void moveTo(float pan, float tilt, float speed = 0.5f) = 0;
    virtual void stop() = 0;
    virtual void queryAngle() = 0;
    virtual void moveDirection(uint8_t cmd2, uint8_t hSpeed, uint8_t vSpeed) = 0;
    virtual void callPreset(uint8_t presetId) = 0;
    virtual void setPreset(uint8_t presetId) = 0;
    virtual void startAutoQuery(int intervalMs = 100) = 0;
    virtual void stopAutoQuery() = 0;
    virtual void prepareForDestruction() = 0;

signals:
    void angleReceived(float pan, float tilt, float panSpeed, float tiltSpeed);
    void rawDataInout(bool isTx, QByteArray data);
};

#endif // IPTZCONTROLLER_H

