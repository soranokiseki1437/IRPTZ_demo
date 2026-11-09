#ifndef DATATYPES_H
#define DATATYPES_H

#include <QString>
#include <QDateTime>
#include <QMutex>

// 云台数据结构体
struct PTZData {
    float panAngle;       // 水平角度
    float tiltAngle;      // 俯仰角度
    float panSpeed;       // 水平速度 (0-63)
    float tiltSpeed;      // 俯仰速度 (0-63)
    
    PTZData() 
        : panAngle(0.0f), tiltAngle(0.0f), panSpeed(0.0f), tiltSpeed(0.0f) {}
    
    PTZData(float pan, float tilt, float panSpd = 0, float tiltSpd = 0)
        : panAngle(pan), tiltAngle(tilt), panSpeed(panSpd), tiltSpeed(tiltSpd) {}
};

// 目标检测数据结构体（预留扩展）
struct DetectionData {
    bool valid;           // 数据是否有效
    float targetX;        // 目标X坐标
    float targetY;        // 目标Y坐标
    float offsetX;        // X脱靶量
    float offsetY;        // Y脱靶量
    float confidence;     // 置信度
    
    DetectionData() 
        : valid(false), targetX(0.0f), targetY(0.0f), 
          offsetX(0.0f), offsetY(0.0f), confidence(0.0f) {}
};

// 完整数据记录点
struct DataPoint {
    qint64 timestampMs;       // 时间戳（毫秒）
    QDateTime dateTime;       // 日期时间
    int frameIndex;           // 视频帧序号

    PTZData ptz;              // 云台数据
    DetectionData detection;  // 目标检测数据（预留）

    // M4: 扩展字段 — KF 状态、PSR、控制指令、跟踪状态
    float kfX = 0, kfY = 0;      // KF 位置
    float kfVx = 0, kfVy = 0;    // KF 速度
    float psr = 0;                // PSR 值
    int trackState = 0;           // 跟踪状态 (0=Idle,1=Tracking,2=Lost,3=Recovered)
    int ptzCmdX = 0, ptzCmdY = 0; // PTZ 控制指令
    int ptzSpeedX = 0, ptzSpeedY = 0; // PTZ 速度

    DataPoint()
        : timestampMs(0), frameIndex(0) {}

    DataPoint(qint64 ts, int frame = 0)
        : timestampMs(ts), frameIndex(frame) {
        dateTime = QDateTime::fromMSecsSinceEpoch(ts);
    }
};

#endif // DATATYPES_H
