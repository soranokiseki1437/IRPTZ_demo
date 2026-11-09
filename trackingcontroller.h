#ifndef TRACKINGCONTROLLER_H
#define TRACKINGCONTROLLER_H

#include <QObject>
#include <QTimer>
#include <QThread>
#include <atomic>
#include "objecttracker.h"

// PTZ自动跟踪控制器
// 负责：接收视频帧 → 调用跟踪器 → 计算脱靶量 → 输出PTZ控制指令
//
// 控制策略：KF预测前馈 + PID反馈
//   - 前馈通道：利用KF估计的目标速度直接映射为PTZ补偿速度
//   - PID通道：对KF预测的未来脱靶量进行反馈校正
//
// 相比纯P控制的优势：
//   1. 预测前馈：补偿通信延迟+云台机械响应延迟（1-2帧）
//   2. 速度前馈：消除匀速跟踪时的稳态脱靶量
//   3. D项阻尼：接近目标时减速，避免过冲和振荡
//   4. I项积分：消除系统偏差（安装倾斜、重力影响）
class TrackingController : public QObject
{
    Q_OBJECT

public:
    explicit TrackingController(QObject *parent = nullptr);
    ~TrackingController();

    // 状态枚举
    enum TrackingState {
        Idle,           // 空闲
        Selecting,      // 等待用户框选目标
        Initializing,   // C9：跟踪器后台初始化中（框选完成到 Tracking 之间丢帧）
        Tracking,       // 正在跟踪
        Lost,           // 目标丢失
        Paused          // 暂停
    };
    Q_ENUM(TrackingState)

    // 获取当前状态
    TrackingState state() const { return m_state; }

    // 是否正在跟踪
    bool isTracking() const { return m_state == Tracking; }

    // 获取当前跟踪结果
    TrackResult currentResult() const { return m_lastResult; }

    // 设置参数（修复P2#23: 死区范围校验）
    void setDeadZonePixels(int pixels) { m_deadZonePixels = qBound(1, pixels, 100); }
    void setMaxSpeed(int speed) { m_maxSpeed = qBound(1, speed, 63); }

    // PID参数
    void setProportionalGain(float gain)   { m_kp = gain; }
    void setIntegralGain(float gain)       { m_ki = gain; m_integralX = 0; m_integralY = 0; }  // 注意：设置Ki会同时清零积分值（防止旧积分+新增益产生跳变）
    void setDerivativeGain(float gain)     { m_kd = gain; }
    void setFeedforwardGain(float gain)    { m_k_ff = gain; }
    void setPredictHorizon(int frames)     { m_T_predict = frames; }

    // 重置PID积分（框选新目标或状态切换时调用）
    void resetPidState();

    void setFrameSize(int width, int height) { m_frameWidth = width; m_frameHeight = height; }

public slots:
    // 开始框选模式（用户需要在画面上框选目标）
    void startSelection();

    // 用户完成框选后调用
    void setTarget(const QImage& frame, const QRectF& targetRect);

    // 处理新帧（从视频流接收）
    void processFrame(const QImage& frame);

    // 停止跟踪
    void stopTracking();

    // 暂停/恢复
    void pauseTracking();
    void resumeTracking();
    void pauseServo(int ms = 800); // 挡片校准期间短暂冻结伺服输出

signals:
    // 跟踪结果信号
    void trackingResult(const TrackResult& result);

    // PTZ控制信号（脱靶量 → PTZ速度指令）
    // deltaX, deltaY: 像素脱靶量（目标中心 - 画面中心）
    // speedX, speedY: 计算出的PTZ速度（0-63）
    void ptzControlDelta(int deltaX, int deltaY, int speedX, int speedY);

    // 状态变化
    void stateChanged(TrackingController::TrackingState newState);

    // 需要UI显示的信息
    void statusMessage(const QString& msg);

    // 请求在画面上绘制跟踪框（UI侧实现）
    void drawTrackingBox(const QRectF& bbox, bool occluded);

private slots:
    void onTrackingLost();
    void onTrackingRecovered();
    void onTrackerInitDone(bool ok, const QString& modelInfo);  // C9：异步初始化回调

private:
    ObjectTracker* m_tracker;
    QThread* m_trackerThread;

    qint64 m_servoPauseUntilMs = 0; // 伺服冻结截止时间戳

    TrackingState m_state;
    TrackResult m_lastResult;

    // 画面尺寸
    int m_frameWidth;
    int m_frameHeight;

    // 控制参数
    int m_deadZonePixels;   // 死区（像素）
    int m_maxSpeed;         // 最大PTZ速度
    float m_kp;             // PID比例增益
    float m_ki;             // PID积分增益
    float m_kd;             // PID微分增益
    float m_k_ff;           // 速度前馈增益
    int m_T_predict;        // 预测时域（帧数）

    // PID状态
    float m_integralX;      // X轴积分累积
    float m_integralY;      // Y轴积分累积
    float m_prevErrorX;     // 上一帧X误差（用于D项）
    float m_prevErrorY;     // 上一帧Y误差（用于D项）
    float m_I_max;          // 积分上限（防饱和）
    bool m_pidInitialized;  // PID是否已完成首帧初始化
    std::atomic<bool> m_isProcessingFrame{false};  // R-02: 帧处理背压标志

    // 计算PTZ控制量
    void computePTZControl(const TrackResult& result);
};

#endif // TRACKINGCONTROLLER_H
