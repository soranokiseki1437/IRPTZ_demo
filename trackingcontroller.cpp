#include "trackingcontroller.h"
#include <QDebug>
#include <QtMath>
#include <QDateTime>

TrackingController::TrackingController(QObject *parent)
    : QObject(parent)
    , m_tracker(nullptr)
    , m_trackerThread(nullptr)
    , m_state(Idle)
    , m_frameWidth(1920)
    , m_frameHeight(1080)
    , m_deadZonePixels(0)
    , m_maxSpeed(0)
    , m_kp(0.0f)
    , m_ki(0.0f)
    , m_kd(0.0f)
    , m_k_ff(0.0f)
    , m_T_predict(0)
    , m_integralX(0.0f)
    , m_integralY(0.0f)
    , m_prevErrorX(0.0f)
    , m_prevErrorY(0.0f)
    , m_I_max(15.0f)
    , m_pidInitialized(false)
{
}

TrackingController::~TrackingController()
{
    stopTracking();
}

void TrackingController::startSelection()
{
    if (m_state == Tracking || m_state == Selecting || m_state == Initializing) {
        stopTracking();
    }
    m_state = Selecting;
    emit stateChanged(m_state);
    emit statusMessage("请在视频画面上框选要跟踪的目标");
}

void TrackingController::setTarget(const QImage& frame, const QRectF& targetRect)
{
    qDebug() << "[TrackCtrl] setTarget begin, state=" << m_state
             << " frame=" << frame.width() << "x" << frame.height()
             << " target=" << targetRect;

    if (m_state != Selecting) {
        qWarning() << "[TrackingController] 不在框选状态，无法设置目标";
        return;
    }

    // 清理旧的跟踪器（先停线程，再删对象，避免跨线程访问）
    if (m_tracker) {
        QObject::disconnect(m_tracker, nullptr, nullptr, nullptr);
    }
    if (m_trackerThread) {
        m_trackerThread->quit();
        m_trackerThread->wait();
    }
    delete m_tracker;
    m_tracker = nullptr;
    delete m_trackerThread;
    m_trackerThread = nullptr;
    qDebug() << "[TrackCtrl] 旧 tracker 清理完毕";

    // C9：创建 tracker + 线程后投递异步初始化（init 含核响应运算，移出 UI 线程）
    m_trackerThread = new QThread(this);
    m_tracker = new ObjectTracker(nullptr);

    connect(m_tracker, &ObjectTracker::initDone, this, &TrackingController::onTrackerInitDone);
    connect(m_tracker, &ObjectTracker::trackingDone, this, [this](const TrackResult& result) {
        m_isProcessingFrame.store(false);  // R-02: 帧处理完成，允许下一帧进入
        m_lastResult = result;
        emit trackingResult(result);
        emit drawTrackingBox(result.bbox, result.occluded);

        if (m_servoPauseUntilMs > 0 && QDateTime::currentMSecsSinceEpoch() < m_servoPauseUntilMs) {
            emit ptzControlDelta(0, 0, 0, 0);
            return;
        }

        if (result.valid && !result.occluded) {
            computePTZControl(result);
        } else {
            emit ptzControlDelta(0, 0, 0, 0);
        }
    });
    connect(m_tracker, &ObjectTracker::trackingLost, this, &TrackingController::onTrackingLost);
    connect(m_tracker, &ObjectTracker::trackingRecovered, this, &TrackingController::onTrackingRecovered);

    m_tracker->moveToThread(m_trackerThread);
    // Step6：跟踪为计算密集型，降低调度优先级，避免挤占 UI/取流线程
    m_trackerThread->setPriority(QThread::LowPriority);
    m_trackerThread->start();

    QMetaObject::invokeMethod(m_tracker, "initSlot",
                              Qt::QueuedConnection,
                              Q_ARG(QImage, frame), Q_ARG(QRectF, targetRect));

    // 状态先置 Initializing：初始化完成前 processFrame 自动丢帧
    m_state = Initializing;
    m_lastResult = TrackResult();
    resetPidState();

    emit stateChanged(m_state);
    emit statusMessage("跟踪器初始化中...");
    emit drawTrackingBox(targetRect, false);
    qDebug() << "[TrackCtrl] setTarget done, state=Initializing（异步 init 已投递）";
}

// C9：异步初始化回调（worker 线程 emit，队列到主线程）
void TrackingController::onTrackerInitDone(bool ok, const QString& modelInfo)
{
    // 初始化期间用户可能已取消框选/重新框选，仅在 Initializing 态下生效
    if (m_state != Initializing) {
        return;
    }

    if (!ok) {
        qWarning() << "[TrackCtrl] tracker 异步 init 失败";
        stopTracking();
        emit statusMessage("跟踪器初始化失败，请重新框选");
        return;
    }

    qDebug() << "[TrackCtrl] tracker 异步 init 成功，model=" << modelInfo;
    m_state = Tracking;
    emit stateChanged(m_state);
    emit statusMessage("目标跟踪已启动");
}

void TrackingController::processFrame(const QImage& frame)
{
    if (m_state != Tracking || !m_tracker) {
        return;
    }

    // 修复P2#13: 空帧防护，避免传递无效帧给跟踪器
    if (frame.isNull()) {
        return;
    }

    // 更新画面尺寸（如果变化）
    if (frame.width() != m_frameWidth || frame.height() != m_frameHeight) {
        m_frameWidth = frame.width();
        m_frameHeight = frame.height();
    }

    // R-02: 背压控制——如果上一帧还在处理中，丢弃当前帧
    if (m_isProcessingFrame.exchange(true)) {
        return;  // 丢弃，保证队列最多1帧
    }

    // 跨线程异步调用：帧数据发送到 worker 线程处理
    QMetaObject::invokeMethod(m_tracker, "processFrameSlot",
                              Qt::QueuedConnection,
                              Q_ARG(QImage, frame));
}

// 重置PID积分（框选新目标或状态切换时调用）
void TrackingController::resetPidState()
{
    m_integralX = 0.0f;
    m_integralY = 0.0f;
    m_prevErrorX = 0.0f;
    m_prevErrorY = 0.0f;
    m_pidInitialized = false;
    m_isProcessingFrame.store(false);  // R-02: PID重置时一并重置
}

// ==============================
// PTZ速度控制
//     策略：KF预测前馈 + PID反馈
//
//  前馈通道（Feedforward）：
//      利用KF估计的目标运动速度，提前映射为PTZ补偿速度
//      feedforwardX = K_ff × vx
//      目的：消除匀速跟踪时的稳态脱靶量
//
//  预测模块（Prediction）：
//      将KF平滑位置向前预测 T_predict 帧
//      predictX = kfX + vx × T_predict
//      目的：补偿通信延迟和云台机械响应延迟
//
//  PID反馈通道：
//      error = 未来预测位置 - 画面中心
//      P = Kp × error
//      I = clamp(I + Ki × error, -Imax, Imax)  — 积分消除系统偏差
//      D = Kd × (error - prevError)            — 微分抑制振荡
//      PID_out = P + I + D
//
//  混合输出：
//      speed = feedforward + PID_out
//      speed = clamp(speed, -maxSpeed, maxSpeed)
//      死区过滤
// ==============================
void TrackingController::computePTZControl(const TrackResult& result)
{
    const float centerX = m_frameWidth / 2.0f;
    const float centerY = m_frameHeight / 2.0f;

    // === 1. 提取KF状态 ===
    const float kfX = result.kfX;    // KF滤波后的平滑位置
    const float kfY = result.kfY;
    const float kfVx = result.kfVx;  // KF估计的目标运动速度
    const float kfVy = result.kfVy;

    // === 2. 预测前馈：预测未来 T_predict 帧后的目标位置 ===
    const float predictX = kfX + kfVx * m_T_predict;
    const float predictY = kfY + kfVy * m_T_predict;

    // 预测脱靶量（正值=目标在中心右侧/下方，云台需向右/下转）
    const float errorX = predictX - centerX;
    const float errorY = predictY - centerY;

    // 死区：误差太小不产生动作
    const float errorXEffective = (std::abs(errorX) < m_deadZonePixels) ? 0.0f : errorX;
    const float errorYEffective = (std::abs(errorY) < m_deadZonePixels) ? 0.0f : errorY;

    // 单轴独立死区判断：只清零在死区内的轴的积分，避免另一轴积分残留导致微动
    if (errorXEffective == 0.0f) {
        m_integralX = 0.0f;
    }
    if (errorYEffective == 0.0f) {
        m_integralY = 0.0f;
    }

    // 两轴都在死区 → 直接返回，同时更新上一帧误差避免 D 项跳变
    if (errorXEffective == 0.0f && errorYEffective == 0.0f) {
        m_prevErrorX = errorX;
        m_prevErrorY = errorY;
        emit ptzControlDelta(0, 0, 0, 0);
        return;
    }

    // === 3. 速度前馈：目标运动速度直接映射为PTZ补偿速度 ===
    const float vFeedX = m_k_ff * kfVx;
    const float vFeedY = m_k_ff * kfVy;

    // === 4. PID反馈控制 ===

    // 4a. P项 — 比例响应脱靶量
    const float pX = m_kp * errorXEffective;
    const float pY = m_kp * errorYEffective;

    // 4b. I项 — 积分消除稳态误差（带防饱和clamp）
    // dt = 1 帧（跟踪器每帧一次输出）
    m_integralX += m_ki * errorXEffective;
    m_integralY += m_ki * errorYEffective;
    m_integralX = qBound(-m_I_max, m_integralX, m_I_max);
    m_integralY = qBound(-m_I_max, m_integralY, m_I_max);

    // 4c. D项 — 微分抑制振荡（首次跳过，使用上一帧误差计算变化率）
    // 使用原始 error 而非 errorXEffective，避免死区边界来回跳动时 D 项产生尖峰
    float dX = 0.0f;
    float dY = 0.0f;
    if (m_pidInitialized) {
        dX = m_kd * (errorX - m_prevErrorX);
        dY = m_kd * (errorY - m_prevErrorY);
    } else {
        m_pidInitialized = true;
    }

    m_prevErrorX = errorX;
    m_prevErrorY = errorY;

    // PID总输出
    const float pidX = pX + m_integralX + dX;
    const float pidY = pY + m_integralY + dY;

    // === 5. 前馈 + 反馈混合 ===
    float speedXf = vFeedX + pidX;
    float speedYf = vFeedY + pidY;

    // === 6. 安全限幅 ===
    speedXf = qBound(-static_cast<float>(m_maxSpeed), speedXf, static_cast<float>(m_maxSpeed));
    speedYf = qBound(-static_cast<float>(m_maxSpeed), speedYf, static_cast<float>(m_maxSpeed));

    // === 7. 输出 ===
    // R-03: speedX/speedY 保留正负号，由 MainWindow 根据符号决定方向
    const int outDeltaX = qRound(errorXEffective);
    const int outDeltaY = qRound(errorYEffective);
    const int outSpeedX = qRound(speedXf);
    const int outSpeedY = qRound(speedYf);

    emit ptzControlDelta(outDeltaX, outDeltaY, outSpeedX, outSpeedY);
}

void TrackingController::stopTracking()
{
    // 修复P0#2: 严格顺序: disconnect → quit → wait → delete tracker → delete thread
    if (m_tracker) {
        disconnect(m_tracker, nullptr, nullptr, nullptr);  // 断开所有信号
    }
    if (m_trackerThread) {
        m_trackerThread->quit();
        m_trackerThread->wait();
    }
    if (m_tracker) {
        delete m_tracker;
        m_tracker = nullptr;
    }
    if (m_trackerThread) {
        delete m_trackerThread;
        m_trackerThread = nullptr;
    }
    m_isProcessingFrame.store(false);  // R-02: 停止跟踪时重置
    m_state = Idle;
    emit stateChanged(m_state);
    emit statusMessage("跟踪已停止");
    emit drawTrackingBox(QRectF(), false);
}

void TrackingController::pauseTracking()
{
    if (m_state == Tracking) {
        m_state = Paused;
        emit stateChanged(m_state);
        emit statusMessage("跟踪已暂停");
    }
}

void TrackingController::resumeTracking()
{
    if (m_state == Paused) {
        m_state = Tracking;
        emit stateChanged(m_state);
        emit statusMessage("跟踪已恢复");
    }
}

void TrackingController::pauseServo(int ms)
{
    m_servoPauseUntilMs = QDateTime::currentMSecsSinceEpoch() + ms;
    emit ptzControlDelta(0, 0, 0, 0);
}

void TrackingController::onTrackingLost()
{
    if (m_state == Tracking) {
        m_state = Lost;
        emit stateChanged(m_state);
        emit statusMessage("目标丢失，尝试恢复中...");
    }
}

void TrackingController::onTrackingRecovered()
{
    if (m_state == Lost) {
        m_state = Tracking;
        emit stateChanged(m_state);
        emit statusMessage("目标已恢复");
    }
}
