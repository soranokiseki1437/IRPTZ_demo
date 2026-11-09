#include "objecttracker.h"
#include "imagematconvert.h"
#include "logging_categories.h"
#include <cmath>
#include <QElapsedTimer>

// ========================
// 构造函数 / 析构函数
// ========================
ObjectTracker::ObjectTracker(QObject *parent)
    : QObject(parent)
    , m_kfQ(cv::Matx44f::eye() * 100.0f)
    , m_kfQLost(cv::Matx44f::eye() * 900.0f)
    , m_kfR(cv::Matx22f::eye() * 100.0f)
{
}

ObjectTracker::~ObjectTracker()
{
}

// ========================
// 重置
// ========================
void ObjectTracker::reset()
{
    m_initialized = false;
    m_inOcclusion = false;
    m_lostCount = 0;
    m_psrBaseline = 0.0f;
    m_baselineLocked = false;
    m_psrHistory.clear();
    m_frameCount = 0;
    // 内核无需显式重置：外壳 m_initialized=false 后不会再访问，
    // 下次 init() 会完整重建内核状态
}

QString ObjectTracker::modelSize() const
{
    const cv::Size ms = m_core.modelSize();
    return QString("%1x%2").arg(ms.width).arg(ms.height);
}

// ========================
// 初始化跟踪器
// ========================
bool ObjectTracker::init(const QImage& frame, const QRectF& targetRect)
{
    reset();
    qCDebug(trackerLog) << "[Tracker] init 开始，目标框=" << targetRect;

    if (frame.isNull() || targetRect.width() <= 0 || targetRect.height() <= 0) {
        qWarning() << "[ObjectTracker] 初始化失败: 无效图像或目标框";
        return false;
    }

    const int frameW = frame.width();
    const int frameH = frame.height();
    if (frameW <= 0 || frameH <= 0) {
        qWarning() << "[ObjectTracker] 初始化失败: 图像尺寸无效";
        return false;
    }

    // 将目标框裁剪到图像内部
    const qreal left = std::max<qreal>(0, targetRect.left());
    const qreal top = std::max<qreal>(0, targetRect.top());
    const qreal right = std::min<qreal>(static_cast<qreal>(frameW), targetRect.right());
    const qreal bottom = std::min<qreal>(static_cast<qreal>(frameH), targetRect.bottom());
    const qreal clippedW = right - left;
    const qreal clippedH = bottom - top;
    if (clippedW < 4 || clippedH < 4) {
        qWarning() << "[ObjectTracker] 初始化失败: 目标框裁剪后过小" << clippedW << "x" << clippedH;
        return false;
    }

    m_posX = static_cast<float>((left + right) * 0.5);
    m_posY = static_cast<float>((top + bottom) * 0.5);
    m_targetW = static_cast<float>(clippedW);
    m_targetH = static_cast<float>(clippedH);
    m_currentScale = 1.0f;

    cv::Mat image = QImageToMat(frame);
    if (image.empty()) {
        qWarning() << "[ObjectTracker] 初始化失败: QImage 转 cv::Mat 失败";
        return false;
    }

    // 内核初始化
    const cv::Rect roi(static_cast<int>(left), static_cast<int>(top),
                       static_cast<int>(clippedW), static_cast<int>(clippedH));
    if (!m_core.init(image, roi)) {
        qWarning() << "[ObjectTracker] DsstCore 初始化失败";
        return false;
    }

    // KF 初始化
    m_kf.x = cv::Vec4f(m_posX, m_posY, 0.0f, 0.0f);
    m_kf.P = cv::Matx44f::eye() * 8.0f;

    m_initialized = true;
    qCDebug(trackerLog) << "[ObjectTracker] 初始化成功, 模型尺寸:" << modelSize()
             << "目标:(" << m_posX << "," << m_posY << ") " << m_targetW << "x" << m_targetH;
    return true;
}

// ========================
// 计算PSR（内核响应图版本，半径5圆形峰值抑制）
// ========================
float ObjectTracker::computePSR(const cv::Mat& response)
{
    if (response.empty()) return 0.0f;

    cv::Point maxLoc;
    double maxVal = 0.0;
    cv::minMaxLoc(response, nullptr, &maxVal, nullptr, &maxLoc);

    cv::Mat mask = cv::Mat::ones(response.size(), CV_8U);
    cv::circle(mask, maxLoc, 5, cv::Scalar(0), -1);

    cv::Scalar mean, stddev;
    cv::meanStdDev(response, mean, stddev, mask);
    const double sd = std::max(stddev[0], 1e-6);
    return static_cast<float>((maxVal - mean[0]) / sd);
}

// ========================
// 卡尔曼滤波预测
// ========================
void ObjectTracker::kfPredict()
{
    // NaN/溢出检测：如果状态或协方差已经发散，重置为有效位置
    for (int i = 0; i < 4; ++i) {
        if (std::isnan(m_kf.x[i]) || std::isinf(m_kf.x[i]) || std::abs(m_kf.x[i]) > 1e6f) {
            m_kf.x[i] = 0.0f;
            if (i == 0) m_kf.x[i] = m_posX;
            if (i == 1) m_kf.x[i] = m_posY;
        }
    }
    // P 矩阵保护：如果协方差矩阵有 NaN/Inf/超大数据，重置为初始值
    bool pDirty = false;
    for (int i = 0; i < 4 && !pDirty; ++i) {
        for (int j = 0; j < 4 && !pDirty; ++j) {
            if (std::isnan(m_kf.P(i, j)) || std::isinf(m_kf.P(i, j)) || std::abs(m_kf.P(i, j)) > 1e8f) {
                pDirty = true;
            }
        }
    }
    if (pDirty) {
        qWarning() << "[kfPredict] P矩阵异常，已重置为初始值";
        m_kf.P = cv::Matx44f::eye() * 8.0f;
    }

    // 状态转移矩阵 A = [1 0 T 0; 0 1 0 T; 0 0 1 0; 0 0 0 1], T=1
    const cv::Matx44f A(1.0f, 0.0f, 1.0f, 0.0f,
                        0.0f, 1.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f, 0.0f,
                        0.0f, 0.0f, 0.0f, 1.0f);

    m_kf.x = A * m_kf.x;
    const cv::Matx44f& activeQ = m_inOcclusion ? m_kfQLost : m_kfQ;
    m_kf.P = A * m_kf.P * A.t() + activeQ;
}

// ========================
// 卡尔曼滤波更新
// ========================
void ObjectTracker::kfUpdate(float measX, float measY)
{
    // 测量值有效性检查：NaN或过大的测量值会导致状态发散
    if (std::isnan(measX) || std::isinf(measX)) measX = m_kf.x[0];
    if (std::isnan(measY) || std::isinf(measY)) measY = m_kf.x[1];

    // H = [1 0 0 0; 0 1 0 0]，S = H*P*H' + R
    const cv::Matx<float, 2, 4> H(1.0f, 0.0f, 0.0f, 0.0f,
                                  0.0f, 1.0f, 0.0f, 0.0f);
    cv::Matx22f S = H * m_kf.P * H.t() + m_kfR;

    // 防止S元素溢出（当P变得非常大时）
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            if (std::isnan(S(i, j)) || std::isinf(S(i, j)) || std::abs(S(i, j)) > 1e10f) {
                S(i, j) = (i == j) ? m_kfR(i, j) : 0.0f;
            }
        }
    }

    // 分母epsilon防护除零
    float detS = S(0, 0) * S(1, 1) - S(0, 1) * S(1, 0);
    static constexpr float epsilon = 1e-10f;
    if (std::abs(detS) < epsilon) detS = (detS < 0) ? -epsilon : epsilon;

    // K = P*H'*inv(S)
    const cv::Matx22f Sinv(S(1, 1) / detS, -S(0, 1) / detS,
                           -S(1, 0) / detS, S(0, 0) / detS);
    const cv::Matx<float, 4, 2> K = m_kf.P * H.t() * Sinv;

    // 更新状态: x = x + K * (z - H * x)
    const cv::Vec2f z(measX, measY);
    m_kf.x += K * (z - H * m_kf.x);

    // 状态溢出保护：如果任何状态值非法，重置为安全值
    for (int i = 0; i < 4; ++i) {
        if (std::isnan(m_kf.x[i]) || std::isinf(m_kf.x[i]) || std::abs(m_kf.x[i]) > 1e6f) {
            m_kf.x[i] = 0.0f;
            if (i == 0) m_kf.x[i] = m_posX;
            if (i == 1) m_kf.x[i] = m_posY;
        }
    }

    // 更新协方差 P = (I - K*H)*P
    const cv::Matx44f I = cv::Matx44f::eye();
    m_kf.P = (I - K * H) * m_kf.P;

    // 协方差矩阵对称化 P = (P + P')/2，防止数值误差导致非对称
    m_kf.P = (m_kf.P + m_kf.P.t()) * 0.5f;

    // P矩阵溢出保护：如果协方差变得非常大，重置为初始值
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            if (std::isnan(m_kf.P(i, j)) || std::isinf(m_kf.P(i, j)) || std::abs(m_kf.P(i, j)) > 1e10f) {
                m_kf.P(i, j) = (i == j) ? 8.0f : 0.0f;
            }
        }
    }
}

// ========================
// 遮挡重检测
// 九宫格偏移逐点调内核 detectOnly（不动内核状态），取PSR最高的候选；
// 成功后：混合备份模型(0.6/0.4) + 回同步内核位置 + 解除冻结
// ========================
bool ObjectTracker::reDetect(const cv::Mat& image)
{
    if (!m_baselineLocked) return false;

    const float kfX = m_kf.x[0];
    const float kfY = m_kf.x[1];
    const float vx = m_kf.x[2];
    const float vy = m_kf.x[3];
    const float speed = std::sqrt(vx * vx + vy * vy);
    const int searchRadius = std::max(
        static_cast<int>(std::max(m_targetW, m_targetH) * 2.0f),
        static_cast<int>(speed * 4.0f));

    static constexpr int offsets[9][2] = {
        {-1, -1}, {-1, 0}, {-1, 1},
        { 0, -1}, { 0, 0}, { 0, 1},
        { 1, -1}, { 1, 0}, { 1, 1}
    };

    const cv::Size modelSz = m_core.modelSize();
    const float scale = m_core.currentScale();

    float bestPsr = 0.0f;
    float bestPosX = kfX;
    float bestPosY = kfY;

    for (int i = 0; i < 9; ++i) {
        const cv::Point2f center(kfX + offsets[i][1] * searchRadius,
                                 kfY + offsets[i][0] * searchRadius);
        cv::Mat resp;
        const cv::Rect r = m_core.detectOnly(image, center, &resp);
        if (r.width <= 0 || resp.empty()) continue;

        const float psr = computePSR(resp);
        if (psr > bestPsr) {
            // 由响应峰值反推精确位置（与旧版 dx/dy 逻辑一致）
            cv::Point maxLoc;
            cv::minMaxLoc(resp, nullptr, nullptr, nullptr, &maxLoc);
            bestPsr = psr;
            bestPosX = center.x + scale * (-modelSz.width / 2.0f + maxLoc.x);
            bestPosY = center.y + scale * (-modelSz.height / 2.0f + maxLoc.y);
        }
    }

    const float recoverThr = 0.8f * m_psrBaseline;
    if (bestPsr <= recoverThr) return false;

    // 验证与KF预测的距离
    const float dist = std::sqrt((bestPosX - kfX) * (bestPosX - kfX)
                               + (bestPosY - kfY) * (bestPosY - kfY));
    const float maxDist = std::max(m_targetW, m_targetH) * 4.0f;
    if (dist >= maxDist) return false;

    m_posX = bestPosX;
    m_posY = bestPosY;

    // 恢复内核：位置回同步 + 模型混合（备份60% + 当前40%）+ 解除冻结
    m_core.setPosition(cv::Point2f(m_posX, m_posY));
    m_core.blendBackup(0.6f);
    m_core.setLearningFrozen(false);

    // KF更新（标准卡尔曼：仅位置测量，速度隐式估计）
    kfUpdate(m_posX, m_posY);
    return true;
}

// 跨线程帧处理槽函数（在 worker thread 中执行）
void ObjectTracker::processFrameSlot(const QImage& frame)
{
    try {
        TrackResult result = update(frame);
        emit trackingDone(result);
    } catch (const std::exception &e) {
        qCritical() << "[Tracker] processFrameSlot 异常:" << e.what();
        TrackResult err;
        err.valid = false;
        emit trackingDone(err);
    } catch (...) {
        qCritical() << "[Tracker] processFrameSlot 未知异常";
        TrackResult err;
        err.valid = false;
        emit trackingDone(err);
    }
}

// C9：异步初始化槽（在 worker thread 中执行）
void ObjectTracker::initSlot(const QImage& frame, const QRectF& targetRect)
{
    bool ok = false;
    try {
        ok = init(frame, targetRect);
    } catch (const std::exception &e) {
        qCritical() << "[Tracker] initSlot 异常:" << e.what();
        ok = false;
    } catch (...) {
        qCritical() << "[Tracker] initSlot 未知异常";
        ok = false;
    }
    emit initDone(ok, ok ? modelSize() : QString());
}

TrackResult ObjectTracker::update(const QImage& frame)
{
    QElapsedTimer timer;
    timer.start();

    TrackResult result;
    if (!m_initialized) {
        qWarning() << "[Tracker] update: 未初始化，直接返回";
        return result;
    }

    cv::Mat image = QImageToMat(frame);
    if (image.empty()) {
        qWarning() << "[Tracker] 图像转换失败，跳过当前帧";
        return result;
    }

    ++m_frameCount;
    const bool logThisFrame = (m_frameCount % 100 == 1);  // 日志节流：每100帧输出一次

    // === KF预测 ===
    kfPredict();
    const float kfPredX = m_kf.x[0];
    const float kfPredY = m_kf.x[1];

    result.occluded = m_inOcclusion;
    result.recovered = false;

    if (m_inOcclusion) {
        // === 遮挡中：不跑内核检测（冻结内核状态+省算力），纯 KF 预测 ===
        if (m_lostCount >= 10) {
            if (reDetect(image)) {
                m_inOcclusion = false;
                m_lostCount = 0;
                result.recovered = true;
                result.occluded = false;
                emit trackingRecovered();
                qCDebug(trackerLog) << "[Tracker] 重检测成功，恢复跟踪 pos=("
                         << m_posX << "," << m_posY << ") psr候选达标";
            } else {
                m_posX = kfPredX;
                m_posY = kfPredY;
                m_lostCount++;
            }
        } else {
            m_posX = kfPredX;
            m_posY = kfPredY;
            m_lostCount++;
        }
    } else {
        // === 正常跟踪：内核检测 + 尺度估计 + 模板更新（内核内部完成）===
        cv::Mat response;
        const cv::Rect bbox = m_core.update(image, &response);
        const bool detValid = (bbox.width > 0 && bbox.height > 0);
        const float psr = detValid ? computePSR(response) : 0.0f;
        result.confidence = psr;

        // 内核位置即本帧检测位置（未 clamp 的真实值）
        float detX = m_posX;
        float detY = m_posY;
        if (detValid) {
            const cv::Point2f p = m_core.position();
            detX = std::isnan(p.x) || std::isinf(p.x) ? m_posX : p.x;
            detY = std::isnan(p.y) || std::isinf(p.y) ? m_posY : p.y;
            m_currentScale = m_core.currentScale();
        }

        // === 决策逻辑（三态机，阈值沿用旧版）===
        const bool lowPsr = !detValid || (m_baselineLocked && psr < 0.75f * m_psrBaseline);

        if (lowPsr) {
            m_lostCount++;
        } else {
            m_lostCount = 0;
        }

        if (m_lostCount >= 3) {
            // 连续低PSR -> 确认遮挡，切换到纯KF预测模式
            m_inOcclusion = true;
            m_lostCount = 1;
            m_posX = kfPredX;
            m_posY = kfPredY;
            result.occluded = true;
            m_core.setLearningFrozen(true);  // 冻结内核模型滑动平均（保险：本帧起不再更新）
            emit trackingLost();
        } else if (m_lostCount > 0) {
            // PSR下降过渡态（未确认遮挡）：使用检测位置但不喂 KF（防污染）
            if (detValid) {
                m_posX = detX;
                m_posY = detY;
            } else {
                m_posX = kfPredX;
                m_posY = kfPredY;
            }
        } else {
            // 正常跟踪
            m_posX = detX;
            m_posY = detY;
            kfUpdate(m_posX, m_posY);

            // 收集PSR基线（锁定后限制历史长度，防止内存无限增长）
            if (!m_baselineLocked) {
                m_psrHistory.push_back(psr);
                if (static_cast<int>(m_psrHistory.size()) >= 30) {
                    float sum = 0.0f;
                    for (float v : m_psrHistory) sum += v;
                    m_psrBaseline = sum / m_psrHistory.size();
                    m_baselineLocked = true;
                    m_psrHistory.clear();  // 锁定后释放，不再需要
                }
            }
        }

        if (logThisFrame) {
            qCDebug(trackerLog) << "[Tracker] frame" << m_frameCount
                     << "psr=" << psr << "valid=" << detValid
                     << "pos=(" << m_posX << "," << m_posY << ")"
                     << "scale=" << m_currentScale
                     << "baseline=" << m_psrBaseline
                     << (m_baselineLocked ? "(locked)" : "");
        }
    }

    // === 构建结果 ===
    const float currentW = m_targetW * m_currentScale;
    const float currentH = m_targetH * m_currentScale;
    result.bbox = QRectF(m_posX - currentW / 2.0f, m_posY - currentH / 2.0f,
                         currentW, currentH);
    result.valid = true;
    result.fps = 1000.0f / std::max(1LL, timer.elapsed());

    // 提取KF内部状态供伺服控制器使用（前馈+PID）
    result.kfX  = m_kf.x[0];  // KF滤波后的平滑位置X
    result.kfY  = m_kf.x[1];  // KF滤波后的平滑位置Y
    result.kfVx = m_kf.x[2];  // KF估计的目标速度X（像素/帧）
    result.kfVy = m_kf.x[3];  // KF估计的目标速度Y（像素/帧）

    return result;
}

// ========================
// 获取当前目标框
// ========================
QRectF ObjectTracker::getCurrentBBox() const
{
    const float currentW = m_targetW * m_currentScale;
    const float currentH = m_targetH * m_currentScale;
    return QRectF(m_posX - currentW / 2.0f, m_posY - currentH / 2.0f,
                  currentW, currentH);
}
