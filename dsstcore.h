#ifndef DSSTCORE_H
#define DSSTCORE_H

#include <opencv2/opencv.hpp>

#include <cmath>
#include <vector>

/*!
 * \brief DSST（Discriminative Scale Space Tracker）相关滤波跟踪内核
 *
 * 从 reference/dsst_tracker.h|cpp 忠实移植，默认数值路径与 reference 一致
 * （验收口径：离线序列逐帧中心误差 < 2px）。相对 reference 仅有三处体检项修复：
 *   R1 update() 尾部 roi clamp 后负宽高 → 返回空框（不触发 cv::Rect 断言）
 *   R4 get_subwindow 的 1-based 行列写法 → 等价的 0-based 半开区间写法
 *   R6 complex_multiply/complex_conj 手写循环 → cv::mulSpectrums
 *
 * 约束：不依赖 Qt、无任何标准输出；所有失败以空 cv::Rect() / 空 cv::Mat 表达。
 * 调用约定：update()/detectOnly() 返回空 Rect 表示本帧失效，调用方不得把空框送入 KF/PID。
 */
class DsstCore
{
public:
    bool init(const cv::Mat& image, const cv::Rect& bbox);

    // 一帧跟踪：检测 + 尺度估计 + 模型滑动平均更新；失效返回空 Rect
    // response: 输出平移响应图（CV_32F，modelSize() 尺寸），供外壳算 PSR；可为 nullptr
    cv::Rect update(const cv::Mat& image, cv::Mat* response = nullptr);

    // 重检测用：以 center 为中心只做一次平移检测，不更新模型、不改内部状态
    cv::Rect detectOnly(const cv::Mat& image, cv::Point2f center, cv::Mat* response = nullptr);

    void setLearningFrozen(bool frozen);            // 遮挡期冻结模型滑动平均（检测/位置/尺度照常）
    void blendBackup(float backupWeight);           // 模型 = w*备份(init时刻) + (1-w)*当前
    void setScaleEstimateEveryOtherFrame(bool on);  // 性能开关，默认 false（=reference 每帧都做）
    void setPadding(float p) { m_padding = p; }     // 默认 2.0f（与 reference 一致）；对齐通过后可调优（如 1.5f）

    bool isInitialized() const;
    cv::Point2f position() const;
    void setPosition(const cv::Point2f& p) { m_pos = p; }  // 外壳重检测成功后回同步位置
    float currentScale() const;
    cv::Size targetSize() const;
    cv::Size modelSize() const;

private:
    // ==== 参数：默认值与 reference 一致（padding 除外）====
    float m_padding = 2.0f;                          // 与 reference 一致，保证逐帧对拍可比；
                                                     // 精度对齐通过后可用 setPadding() 做 A/B 调优（1.5f 起步）
    float m_outputSigmaFactor = 1.0f / 16.0f;
    float m_scaleSigmaFactor = 1.0f / 4.0f;
    float m_lambda = 1e-2f;
    float m_learningRate = 0.025f;
    int   m_numScales = 9;
    float m_scaleStep = 1.05f;
    float m_scaleModelMaxArea = 512.0f;

    int   m_numFeatures = 1;
    int   m_frameCount = 0;
    bool  m_learningFrozen = false;
    bool  m_scaleEveryOther = false;                 // true=尺度估计隔帧（默认关闭，保证与 reference 等价）

    // ==== 模型状态：reference DSSTTracker 私有成员 Mat 化 ====
    cv::Mat m_yf;                                    // 平移期望输出（频域）
    std::vector<cv::Mat> m_cosWindow;                // 平移汉宁窗（每特征通道一份）
    std::vector<cv::Mat> m_hfNum;                    // 平移滤波器分子
    cv::Mat m_hfDen;                                 // 平移滤波器分母
    std::vector<cv::Mat> m_hfNumBackup;              // init 时刻滤波器分子备份（blendBackup 用）
    cv::Mat m_hfDenBackup;                           // init 时刻滤波器分母备份

    cv::Mat m_ysf;                                   // 尺度期望输出（频域）
    cv::Mat m_scaleCosWindow;                        // 尺度汉宁窗
    cv::Mat m_sfNum;                                 // 尺度滤波器分子
    cv::Mat m_sfDen;                                 // 尺度滤波器分母

    cv::Point2f m_pos;
    cv::Size m_targetSz;
    cv::Size m_modelSz;
    cv::Size m_scaleModelSz;
    float m_scale = 1.0f;
    std::vector<float> m_scaleFactors;
    float m_minScale = 0.0f;
    float m_maxScale = 100.0f;
    bool m_initialized = false;

    // ==== 内部实现（方法名与 reference 私有方法一一对应）====
    cv::Mat gstProcess(const cv::Mat& img) const;                            // reference gst_process 原样迁移
    std::vector<cv::Mat> getFeatureMap(const cv::Mat& patch) const;          // reference get_feature_map
    cv::Mat getSubwindow(const cv::Mat& im, cv::Point2f pos, cv::Size sz, float scale) const;
    std::vector<cv::Mat> getTranslationSample(const cv::Mat& im, cv::Point2f pos, float scale) const;
    cv::Mat getScaleSample(const cv::Mat& im, cv::Point2f pos, const std::vector<float>& factors) const;

    // reference update() 前半段（平移检测）抽出的公共实现，update/detectOnly 共用
    bool detectAt(const cv::Mat& image, cv::Point2f center, float scale,
                  cv::Point* maxLoc, cv::Mat* outResponse) const;
    // 由中心点+尺度算目标框（含 R1 边界修复）
    cv::Rect makeRect(const cv::Mat& image, cv::Point2f pos, float scale) const;

    static cv::Mat hann1d(int n);                    // reference hann
    static cv::Mat gaussianPeak(cv::Size sz, float sigma);
};

#endif // DSSTCORE_H