#ifndef OBJECTTRACKER_H
#define OBJECTTRACKER_H

#include <QObject>
#include <QRect>
#include <QImage>
#include <vector>
#include <opencv2/core.hpp>
#include "dsstcore.h"

// 跟踪结果结构体
struct TrackResult {
    bool valid;           // 跟踪是否有效
    QRectF bbox;          // 目标框 (中心x, 中心y, 宽, 高)
    float confidence;     // 置信度 (PSR值)
    bool occluded;        // 是否被遮挡
    bool recovered;       // 是否刚从遮挡恢复
    float fps;            // 处理帧率

    // KF内部状态（供伺服控制器使用）
    float kfX;            // KF滤波后的目标中心X（像素，平滑后）
    float kfY;            // KF滤波后的目标中心Y（像素，平滑后）
    float kfVx;           // KF估计的目标速度X（像素/帧）
    float kfVy;           // KF估计的目标速度Y（像素/帧）

    TrackResult()
        : valid(false), confidence(0.0f), occluded(false), recovered(false), fps(0.0f),
          kfX(0.0f), kfY(0.0f), kfVx(0.0f), kfVy(0.0f) {}
};
Q_DECLARE_METATYPE(TrackResult)

// DSST + KF 目标跟踪器
// 结构：DsstCore（纯算法内核：检测/尺度/模板更新）+ 本类外壳（KF平滑、遮挡三态机、重检测）
// 外壳不再持有任何滤波器模型；模板更新由内核内部滑动平均完成，
// 遮挡期间通过跳过内核 update + setLearningFrozen 实现冻结语义。
class ObjectTracker : public QObject
{
    Q_OBJECT

public:
    explicit ObjectTracker(QObject *parent = nullptr);
    ~ObjectTracker();

    // 初始化跟踪器（第一帧 + 初始目标框）
    bool init(const QImage& frame, const QRectF& targetRect);

    // 处理新一帧，返回跟踪结果
    TrackResult update(const QImage& frame);

    // 重置跟踪器
    void reset();

    // 获取当前目标框
    QRectF getCurrentBBox() const;

    // 是否正在跟踪
    bool isTracking() const { return m_initialized; }

    // 调试辅助：内核模型尺寸 "宽x高"
    QString modelSize() const;

signals:
    void trackingLost();
    void trackingRecovered();
    void trackingDone(const TrackResult& result);  // 跨线程通知跟踪结果
    void initDone(bool ok, const QString& modelInfo);  // C9：异步初始化完成通知

public slots:
    // 跨线程帧处理入口（在 worker thread 中调用）
    void processFrameSlot(const QImage& frame);

    // C9：异步初始化入口（投递到 worker 线程执行，避免 init 卡 UI 线程）
    void initSlot(const QImage& frame, const QRectF& targetRect);

private:
    // === DsstCore 内核 ===
    DsstCore m_core;

    // === 外壳状态 ===
    bool m_initialized = false;

    // 目标状态（每帧从内核同步）
    float m_posX = 0.0f, m_posY = 0.0f;       // 目标中心位置
    float m_targetW = 0.0f, m_targetH = 0.0f; // 基础目标尺寸（未乘尺度）
    float m_currentScale = 1.0f;              // 当前尺度因子（来自内核）

    int m_frameCount = 0;                     // 用于日志节流（每100帧输出一次）

    // === 卡尔曼滤波相关 ===
    struct KFState {
        cv::Vec4f x;       // [px, py, vx, vy]
        cv::Matx44f P;     // 协方差
    } m_kf;

    cv::Matx44f m_kfQ;     // 过程噪声（正常模式）
    cv::Matx44f m_kfQLost; // 过程噪声（遮挡模式，更大不确定性）
    cv::Matx22f m_kfR;     // 观测噪声

    // === 遮挡检测相关 ===
    bool m_inOcclusion = false;
    int m_lostCount = 0;
    float m_psrBaseline = 0.0f;
    bool m_baselineLocked = false;
    std::vector<float> m_psrHistory;

    // === 内部方法 ===
    // 计算PSR（吃内核响应图，半径5圆形峰值抑制）
    float computePSR(const cv::Mat& response);

    // 卡尔曼滤波预测
    void kfPredict();

    // 卡尔曼滤波更新（仅使用位置测量，速度由KF内部通过状态转移隐式估计）
    void kfUpdate(float measX, float measY);

    // 遮挡重检测：九宫格偏移 + 内核 detectOnly，成功后混合备份模型并同步内核位置
    bool reDetect(const cv::Mat& image);
};

#endif // OBJECTTRACKER_H
