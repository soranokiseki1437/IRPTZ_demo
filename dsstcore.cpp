#include "dsstcore.h"

// =====================================================================================
// 私有：GST 特征处理（reference gst_process 原样迁移）
// 仅去掉 printf 与自由函数包装，sigma/filterSize/boundaryWidth/运算顺序/CV_64F 链全部保留。
// 注意：F1/h 的归一化只统计左上 filterSize×filterSize 子块，这是 reference 的既有行为，勿"修正"。
// =====================================================================================
cv::Mat DsstCore::gstProcess(const cv::Mat& img) const
{
    const float sigma1 = 0.3f;
    const float sigma2 = 0.3f;
    const int boundaryWidth = 5;
    const int filterSize = 5;

    cv::Mat gray;
    if (img.channels() == 3) {
        cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = img;
    }
    gray.convertTo(gray, CV_64F);

    int mdpt = (int)std::ceil(filterSize / 2.0);
    int actual_filter_size = 2 * mdpt + 1;

    cv::Mat F1_real(actual_filter_size, actual_filter_size, CV_64F);
    cv::Mat F1_imag(actual_filter_size, actual_filter_size, CV_64F);
    for (int y = -mdpt; y <= mdpt; y++) {
        for (int x = -mdpt; x <= mdpt; x++) {
            double r_sq = x * x + y * y;
            double exp_val = std::exp(-r_sq / (2.0 * sigma1 * sigma1));
            double coeff = std::pow(-1.0 / (sigma1 * sigma1), 1.0) / (2.0 * CV_PI * sigma1 * sigma1);
            F1_real.at<double>(y + mdpt, x + mdpt) = coeff * x * exp_val;
            F1_imag.at<double>(y + mdpt, x + mdpt) = coeff * y * exp_val;
        }
    }

    double sum_F1 = 0;
    for (int i = 0; i < filterSize; i++) {
        for (int j = 0; j < filterSize; j++) {
            sum_F1 += std::sqrt(F1_real.at<double>(i, j) * F1_real.at<double>(i, j) +
                                F1_imag.at<double>(i, j) * F1_imag.at<double>(i, j));
        }
    }
    F1_real = F1_real / sum_F1;
    F1_imag = F1_imag / sum_F1;

    cv::Mat f1_real, f1_imag;
    cv::filter2D(gray, f1_real, CV_64F, F1_real, cv::Point(-1, -1), 0, cv::BORDER_REFLECT);
    cv::filter2D(gray, f1_imag, CV_64F, F1_imag, cv::Point(-1, -1), 0, cv::BORDER_REFLECT);

    cv::Mat Z = (f1_real.mul(f1_real) + f1_imag.mul(f1_imag)) / 255.0;

    cv::Mat h_real(actual_filter_size, actual_filter_size, CV_64F);
    cv::Mat h_imag(actual_filter_size, actual_filter_size, CV_64F);
    for (int y = -mdpt; y <= mdpt; y++) {
        for (int x = -mdpt; x <= mdpt; x++) {
            double r_sq = x * x + y * y;
            double exp_val = std::exp(-r_sq / (2.0 * sigma2 * sigma2));
            double coeff = std::pow(-1.0 / (sigma2 * sigma2), 2.0) / (2.0 * CV_PI * sigma2 * sigma2);
            double x_minus_iy_real = x * x - y * y;
            double x_minus_iy_imag = -2.0 * x * y;
            h_real.at<double>(y + mdpt, x + mdpt) = coeff * x_minus_iy_real * exp_val;
            h_imag.at<double>(y + mdpt, x + mdpt) = coeff * x_minus_iy_imag * exp_val;
        }
    }

    double sum_h = 0;
    for (int i = 0; i < filterSize; i++) {
        for (int j = 0; j < filterSize; j++) {
            sum_h += std::sqrt(h_real.at<double>(i, j) * h_real.at<double>(i, j) +
                               h_imag.at<double>(i, j) * h_imag.at<double>(i, j));
        }
    }
    h_real = h_real / sum_h;
    h_imag = h_imag / sum_h;

    cv::Mat I20_real, I20_imag;
    cv::filter2D(Z, I20_real, CV_64F, h_real, cv::Point(-1, -1), 0, cv::BORDER_REFLECT);
    cv::filter2D(Z, I20_imag, CV_64F, h_imag, cv::Point(-1, -1), 0, cv::BORDER_REFLECT);

    cv::Mat abs_Z = cv::abs(Z);
    cv::Mat abs_h = cv::abs(h_real) + cv::abs(h_imag);
    cv::Mat I10;
    cv::filter2D(abs_Z, I10, CV_64F, abs_h, cv::Point(-1, -1), 0, cv::BORDER_REFLECT);

    cv::Mat Ct(I20_real.size(), CV_64F);
    for (int i = 0; i < I20_real.rows; i++) {
        for (int j = 0; j < I20_real.cols; j++) {
            double theta = 0.5 * std::atan2(I20_imag.at<double>(i, j), I20_real.at<double>(i, j));
            double coscs = std::cos(theta);
            double abs_I20 = std::sqrt(I20_real.at<double>(i, j) * I20_real.at<double>(i, j) +
                                       I20_imag.at<double>(i, j) * I20_imag.at<double>(i, j));
            Ct.at<double>(i, j) = abs_I20 * I10.at<double>(i, j) * (1.0 + coscs);
        }
    }

    cv::Mat revised_Ct = cv::Mat::zeros(Ct.size(), CV_64F);
    if (Ct.rows > 2 * boundaryWidth && Ct.cols > 2 * boundaryWidth) {
        Ct(cv::Rect(boundaryWidth, boundaryWidth,
                    Ct.cols - 2 * boundaryWidth, Ct.rows - 2 * boundaryWidth)).copyTo(
            revised_Ct(cv::Rect(boundaryWidth, boundaryWidth,
                                Ct.cols - 2 * boundaryWidth, Ct.rows - 2 * boundaryWidth)));
    } else {
        revised_Ct = Ct.clone();
    }

    revised_Ct.convertTo(revised_Ct, CV_32F);

    return revised_Ct;
}

// =====================================================================================
// 私有：参考实现中的小工具
// =====================================================================================
cv::Mat DsstCore::hann1d(int n)
{
    // 与 reference 完全一致：0.5*(1-cos(2*pi*i/(n-1)))，float 存储
    cv::Mat window(n, 1, CV_32F);
    for (int i = 0; i < n; i++) {
        window.at<float>(i, 0) = (float)(0.5f * (1.0f - std::cos(2.0f * CV_PI * i / (n - 1))));
    }
    return window;
}

cv::Mat DsstCore::gaussianPeak(cv::Size sz, float sigma)
{
    cv::Mat y(sz, CV_32F);
    for (int i = 0; i < sz.height; i++) {
        for (int j = 0; j < sz.width; j++) {
            float x = j - sz.width / 2.0f;
            float y_ = i - sz.height / 2.0f;
            y.at<float>(i, j) = std::exp(-0.5f * (x * x + y_ * y_) / (sigma * sigma));
        }
    }
    return y;
}

// =====================================================================================
// 私有：子窗提取（R4：等价 0-based 重写）
//
// 与 reference 的对应关系（reference 用 1-based 行/列，行=x、列=y，命名互换）：
//   reference: x1 = floor(pos.y)+1 - patch_h/2 → 0-based 起始行 = floor(pos.y) - patch_h/2
//              x2 = x1 + patch_h - 1（先算后 clamp）→ 0-based 半开结束行 = 起始行 + patch_h
//              x1 = max(1,x1) → 起始行 = max(0,·)；x2 = min(rows,x2) → 结束行 = min(rows,·)
//   两者在起始未被 clamp 时给出完全相同的 ROI；仅在起始被 clamp 时 reference 的
//   "1-based 闭区间"与"0-based 半开区间"同样是先 clamp 起点、再独立 clamp 终点（会收缩而非平移），
//   因此 ROI 尺寸/内容一致。之后照旧 resize 回 model_sz（越界时拉伸，与 reference 相同）。
// =====================================================================================
cv::Mat DsstCore::getSubwindow(const cv::Mat& im, cv::Point2f pos, cv::Size sz, float scale) const
{
    if (im.empty()) return cv::Mat();

    // reference: Size(floor(model_sz.width*scale), floor(model_sz.height*scale))，且不小于 2
    cv::Size patch((int)std::floor(sz.width * scale), (int)std::floor(sz.height * scale));
    if (patch.width < 2) patch.width = 2;
    if (patch.height < 2) patch.height = 2;

    int r0 = (int)std::floor(pos.y) - patch.height / 2;   // 起始行（0-based，整数除法恒为正）
    int c0 = (int)std::floor(pos.x) - patch.width / 2;    // 起始列
    int r1 = r0 + patch.height;                           // 结束行（半开）
    int c1 = c0 + patch.width;                            // 结束列（半开）

    r0 = std::max(0, r0);
    c0 = std::max(0, c0);
    r1 = std::min(im.rows, r1);
    c1 = std::min(im.cols, c1);

    if (r0 >= r1 || c0 >= c1) return cv::Mat();

    cv::Mat roi = im(cv::Rect(c0, r0, c1 - c0, r1 - r0));
    if (roi.empty()) return cv::Mat();

    cv::Mat resized;
    cv::resize(roi, resized, sz);
    if (resized.empty()) return cv::Mat();

    return resized;
}

// =====================================================================================
// 私有：特征图（1 通道 GST 特征）
// =====================================================================================
std::vector<cv::Mat> DsstCore::getFeatureMap(const cv::Mat& patch) const
{
    std::vector<cv::Mat> features;
    if (patch.empty()) return features;

    cv::Mat gst_features = gstProcess(patch);
    if (gst_features.empty()) return features;
    cv::Scalar mean_val, stddev;
    cv::meanStdDev(gst_features, mean_val, stddev);
    gst_features = (gst_features - mean_val[0]) / (stddev[0] + 1e-10f);
    features.push_back(gst_features);

    return features;
}

// =====================================================================================
// 私有：平移样本（reference get_translation_sample）
// =====================================================================================
std::vector<cv::Mat> DsstCore::getTranslationSample(const cv::Mat& im, cv::Point2f pos, float scale) const
{
    cv::Mat im_patch = getSubwindow(im, pos, m_modelSz, scale);
    if (im_patch.empty()) return std::vector<cv::Mat>();

    std::vector<cv::Mat> features = getFeatureMap(im_patch);
    if (features.empty()) return std::vector<cv::Mat>();

    for (size_t d = 0; d < features.size(); d++) {
        if (d >= m_cosWindow.size() || features[d].size() != m_cosWindow[d].size()) {
            return std::vector<cv::Mat>();
        }
        features[d] = features[d].mul(m_cosWindow[d]);
    }

    return features;
}

// =====================================================================================
// 私有：尺度样本（reference get_scale_sample）
// 内联的 1-based 行列写法按与 getSubwindow 相同的规则改写为 0-based 半开区间。
// 差异说明：reference 的 out 矩阵未清零，未通过的尺度列是未初始化内存；此处改为 0，
// 正常路径（所有尺度都取到有效 patch）下两者数值完全一致。
// =====================================================================================
cv::Mat DsstCore::getScaleSample(const cv::Mat& im, cv::Point2f pos, const std::vector<float>& factors) const
{
    if (im.empty() || factors.empty()) return cv::Mat();

    const int nScales = (int)factors.size();
    cv::Mat out = cv::Mat::zeros(m_scaleModelSz.area(), nScales, CV_32F);

    for (int s = 0; s < nScales; s++) {
        cv::Size patch_sz((int)std::floor(m_targetSz.width * factors[s]),
                          (int)std::floor(m_targetSz.height * factors[s]));

        int r0 = (int)std::floor(pos.y) - patch_sz.height / 2;
        int c0 = (int)std::floor(pos.x) - patch_sz.width / 2;
        int r1 = r0 + patch_sz.height;
        int c1 = c0 + patch_sz.width;

        r0 = std::max(0, r0);
        c0 = std::max(0, c0);
        r1 = std::min(im.rows, r1);
        c1 = std::min(im.cols, c1);

        if (r1 > r0 && c1 > c0) {
            cv::Mat im_patch = im(cv::Rect(c0, r0, c1 - c0, r1 - r0));
            cv::Mat resized;
            cv::resize(im_patch, resized, m_scaleModelSz);

            cv::Mat gray = gstProcess(resized);

            cv::Scalar mean_val, stddev;
            cv::meanStdDev(gray, mean_val, stddev);
            gray = (gray - mean_val[0]) / (stddev[0] + 1e-10f);

            cv::Mat flat = gray.reshape(1, 1).t();

            float window_val = 1.0f;
            if (s < m_scaleCosWindow.rows) {
                window_val = m_scaleCosWindow.at<float>(s, 0);
            }

            if (flat.rows == out.rows) {
                for (int i = 0; i < out.rows; i++) {
                    out.at<float>(i, s) = flat.at<float>(i, 0) * window_val;
                }
            }
        }
    }

    return out;
}

// =====================================================================================
// 私有：平移检测（reference update() 前半段），失败返回 false
// response 输出 CV_32F、m_modelSz 尺寸的相关响应图
// =====================================================================================
bool DsstCore::detectAt(const cv::Mat& image, cv::Point2f center, float scale,
                        cv::Point* maxLoc, cv::Mat* outResponse) const
{
    std::vector<cv::Mat> xt = getTranslationSample(image, center, scale);
    if (xt.empty()) return false;

    cv::Mat response = cv::Mat::zeros(m_modelSz, CV_32F);
    int usedChannels = 0;

    for (int d = 0; d < m_numFeatures; d++) {
        if (d >= (int)xt.size()) break;
        if (d >= (int)m_hfNum.size() || m_hfNum[d].empty()) break;

        cv::Mat xtf;
        try {
            cv::dft(xt[d], xtf, cv::DFT_COMPLEX_OUTPUT);
        } catch (const cv::Exception&) {
            break;   // 与 reference 一致：单通道 DFT 失败即中止累加
        }

        // reference: complex_multiply(hf_num[d], xtf) —— 频域相乘，不取共轭
        cv::Mat responsef;
        cv::mulSpectrums(m_hfNum[d], xtf, responsef, 0, false);
        if (responsef.empty()) break;

        cv::Mat denominator;
        m_hfDen.convertTo(denominator, CV_32F);
        denominator += m_lambda;

        cv::Mat response_d_complex(responsef.size(), CV_32FC2);
        for (int i = 0; i < responsef.rows; i++) {
            for (int j = 0; j < responsef.cols; j++) {
                cv::Vec2f elem = responsef.at<cv::Vec2f>(i, j);
                float den = denominator.at<float>(i, j);
                if (std::fabs(den) > 1e-10f) {
                    response_d_complex.at<cv::Vec2f>(i, j) = cv::Vec2f(elem[0] / den, elem[1] / den);
                } else {
                    response_d_complex.at<cv::Vec2f>(i, j) = cv::Vec2f(0.0f, 0.0f);
                }
            }
        }

        cv::Mat response_d;
        cv::idft(response_d_complex, response_d, cv::DFT_REAL_OUTPUT | cv::DFT_SCALE);
        response += response_d;

        usedChannels++;
    }

    if (usedChannels == 0) return false;

    cv::Point loc;
    cv::minMaxLoc(response, NULL, NULL, NULL, &loc);
    if (maxLoc) *maxLoc = loc;
    if (outResponse) *outResponse = response.clone();

    return true;
}

// =====================================================================================
// 私有：由中心点+尺度求目标框（含 R1 修复：负宽高返回空框）
// =====================================================================================
cv::Rect DsstCore::makeRect(const cv::Mat& image, cv::Point2f pos, float scale) const
{
    int width = (int)std::floor(m_targetSz.width * scale);
    int height = (int)std::floor(m_targetSz.height * scale);

    cv::Rect roi;
    // reference 为 float→int 隐式转换（截断），此处显式保持一致
    roi.x = (int)(pos.x - width / 2.0f);
    roi.y = (int)(pos.y - height / 2.0f);
    roi.width = width;
    roi.height = height;

    // R1 修复：先 clamp 左上角（与 reference 相同），再 clamp 宽高，最后拦掉负宽高
    roi.x = std::max(0, roi.x);
    roi.y = std::max(0, roi.y);
    roi.width = std::min(image.cols - roi.x, roi.width);
    roi.height = std::min(image.rows - roi.y, roi.height);

    if (roi.width <= 0 || roi.height <= 0) return cv::Rect();

    return roi;
}

// =====================================================================================
// 公开接口
// =====================================================================================
bool DsstCore::init(const cv::Mat& image, const cv::Rect& bbox)
{
    m_initialized = false;

    if (image.empty() || bbox.width <= 0 || bbox.height <= 0) return false;

    m_pos = cv::Point2f(bbox.x + bbox.width / 2.0f, bbox.y + bbox.height / 2.0f);
    m_targetSz = cv::Size(bbox.width, bbox.height);
    m_scale = 1.0f;
    m_frameCount = 0;

    m_modelSz = cv::Size((int)std::floor(m_targetSz.width * (1 + m_padding)),
                         (int)std::floor(m_targetSz.height * (1 + m_padding)));

    float output_sigma = std::sqrt((float)(m_targetSz.width * m_targetSz.height)) * m_outputSigmaFactor;
    cv::Mat y = gaussianPeak(m_modelSz, output_sigma);
    cv::dft(y, m_yf, cv::DFT_COMPLEX_OUTPUT);

    cv::Mat hann_h = hann1d(m_modelSz.width);
    cv::Mat hann_v = hann1d(m_modelSz.height);
    cv::Mat single_cos_window = hann_v * hann_h.t();
    m_cosWindow.assign(m_numFeatures, single_cos_window.clone());

    m_scaleFactors.resize(m_numScales);
    for (int s = 0; s < m_numScales; s++) {
        m_scaleFactors[s] = std::pow(m_scaleStep, (float)std::ceil(m_numScales / 2.0f) - s - 1);
    }

    float scale_sigma = m_numScales / std::sqrt(33.0f) * m_scaleSigmaFactor;
    cv::Mat ys(1, m_numScales, CV_32F);
    for (int s = 0; s < m_numScales; s++) {
        float x = (s + 1) - (float)std::ceil(m_numScales / 2.0f);
        ys.at<float>(0, s) = std::exp(-0.5f * x * x / (scale_sigma * scale_sigma));
    }

    cv::Mat ys_complex;
    cv::Mat planes[] = {ys, cv::Mat::zeros(ys.size(), CV_32F)};
    cv::merge(planes, 2, ys_complex);
    cv::dft(ys_complex, m_ysf, cv::DFT_COMPLEX_OUTPUT);

    if (m_numScales % 2 == 0) {
        m_scaleCosWindow = hann1d(m_numScales + 1);
        m_scaleCosWindow = m_scaleCosWindow.rowRange(1, m_numScales + 1);
    } else {
        m_scaleCosWindow = hann1d(m_numScales);
    }

    float scale_model_factor = 1.0f;
    if (m_targetSz.area() > m_scaleModelMaxArea) {
        scale_model_factor = std::sqrt(m_scaleModelMaxArea / m_targetSz.area());
    }
    m_scaleModelSz = cv::Size((int)std::floor(m_targetSz.width * scale_model_factor),
                              (int)std::floor(m_targetSz.height * scale_model_factor));

    m_minScale = std::pow(m_scaleStep,
                          std::ceil(std::log(std::max(5.0f / m_modelSz.width, 5.0f / m_modelSz.height))
                                    / std::log(m_scaleStep)));
    m_maxScale = std::pow(m_scaleStep,
                          std::floor(std::log(std::min((float)image.rows / m_targetSz.height,
                                                       (float)image.cols / m_targetSz.width))
                                     / std::log(m_scaleStep)));
    // R-10: 极小目标兜底——防止 minScale > maxScale 导致尺度搜索发散
    if (m_minScale > m_maxScale) m_minScale = m_maxScale;

    std::vector<cv::Mat> xl = getTranslationSample(image, m_pos, m_scale);
    if (xl.empty()) return false;

    if ((int)xl.size() != m_numFeatures) {
        // 与 reference 相同：以实际特征数为准并重建 cos_window 后重取样本
        m_numFeatures = (int)xl.size();
        m_cosWindow.assign(m_numFeatures, single_cos_window.clone());
        xl = getTranslationSample(image, m_pos, m_scale);
        if (xl.empty()) return false;
    }

    m_hfNum.assign(m_numFeatures, cv::Mat());
    m_hfDen = cv::Mat::zeros(m_modelSz, CV_32F);

    for (int d = 0; d < m_numFeatures; d++) {
        if (d >= (int)xl.size()) return false;

        cv::Mat xlf;
        try {
            cv::dft(xl[d], xlf, cv::DFT_COMPLEX_OUTPUT);
        } catch (const cv::Exception&) {
            return false;
        }

        // reference: complex_multiply(yf, complex_conj(xlf)) → 共轭施加在第二个操作数
        cv::Mat new_hf_num_d;
        cv::mulSpectrums(m_yf, xlf, new_hf_num_d, 0, true);
        if (new_hf_num_d.empty()) return false;
        m_hfNum[d] = new_hf_num_d;

        cv::Mat mag_sq(xlf.size(), CV_32F);
        for (int i = 0; i < xlf.rows; i++) {
            for (int j = 0; j < xlf.cols; j++) {
                cv::Vec2f elem = xlf.at<cv::Vec2f>(i, j);
                mag_sq.at<float>(i, j) = elem[0] * elem[0] + elem[1] * elem[1];
            }
        }
        m_hfDen += mag_sq;
    }

    // 保存 init 时刻平移滤波器副本，供 blendBackup()（默认数值路径不使用）
    m_hfNumBackup.clear();
    m_hfNumBackup.reserve(m_hfNum.size());
    for (size_t d = 0; d < m_hfNum.size(); d++) {
        m_hfNumBackup.push_back(m_hfNum[d].clone());
    }
    m_hfDenBackup = m_hfDen.clone();

    cv::Mat xs = getScaleSample(image, m_pos, m_scaleFactors);
    if (!xs.empty()) {
        if (m_ysf.rows != 1 || m_ysf.cols != m_numScales) {
            m_ysf = m_ysf.reshape(0, 1);
            if (m_ysf.cols != m_numScales) {
                m_ysf.create(1, m_numScales, CV_32FC2);
            }
        }

        // 沿每一行（跨尺度方向）做 FFT，对应 MATLAB fft(xs,[],2)
        cv::Mat xsf;
        xsf.create(xs.rows, xs.cols, CV_32FC2);
        for (int row = 0; row < xs.rows; row++) {
            cv::Mat row_vec = xs.row(row);
            cv::Mat row_complex;
            cv::Mat planes[] = {row_vec, cv::Mat::zeros(row_vec.size(), CV_32F)};
            cv::merge(planes, 2, row_complex);
            cv::dft(row_complex, xsf.row(row), cv::DFT_COMPLEX_OUTPUT);
        }

        // sf_num = ysf .* conj(xsf)：ysf 为 1×nScales，需按列广播，故保留显式循环
        m_sfNum.create(xsf.rows, xsf.cols, CV_32FC2);
        for (int row = 0; row < xsf.rows; row++) {
            for (int col = 0; col < xsf.cols; col++) {
                cv::Vec2f ysf_elem = m_ysf.at<cv::Vec2f>(0, col);
                cv::Vec2f xsf_elem = xsf.at<cv::Vec2f>(row, col);
                cv::Vec2f conj_xsf_elem(xsf_elem[0], -xsf_elem[1]);

                float real_part = ysf_elem[0] * conj_xsf_elem[0] - ysf_elem[1] * conj_xsf_elem[1];
                float imag_part = ysf_elem[0] * conj_xsf_elem[1] + ysf_elem[1] * conj_xsf_elem[0];
                m_sfNum.at<cv::Vec2f>(row, col) = cv::Vec2f(real_part, imag_part);
            }
        }

        // sf_den = sum(xsf .* conj(xsf), 1)：沿行求和 → 1×nScales
        m_sfDen = cv::Mat::zeros(1, xsf.cols, CV_32F);
        for (int col = 0; col < xsf.cols; col++) {
            float sum = 0.0f;
            for (int row = 0; row < xsf.rows; row++) {
                cv::Vec2f elem = xsf.at<cv::Vec2f>(row, col);
                sum += elem[0] * elem[0] + elem[1] * elem[1];
            }
            m_sfDen.at<float>(0, col) = sum;
        }
    }

    m_initialized = true;
    return true;
}

cv::Rect DsstCore::update(const cv::Mat& image, cv::Mat* response)
{
    if (response) response->release();
    if (!m_initialized || m_cosWindow.empty()) return cv::Rect();

    m_frameCount++;

    // ---- 1. 平移检测 ----
    cv::Point max_loc;
    cv::Mat trResponse;
    if (!detectAt(image, m_pos, m_scale, &max_loc, &trResponse)) return cv::Rect();

    // reference: pos = pos + current_scale_factor * ([-model_sz/2, -model_sz/2] + max_loc)
    cv::Point2f offset(-m_modelSz.width / 2.0f + max_loc.x,
                       -m_modelSz.height / 2.0f + max_loc.y);
    m_pos = m_pos + m_scale * offset;

    if (response) *response = trResponse;

    // ---- 2. 尺度估计（默认每帧执行，与 reference 一致）----
    const bool doScale = (!m_scaleEveryOther || (m_frameCount % 2 == 0));
    std::vector<float> current_scale_factors(m_numScales);

    if (doScale) {
        for (int s = 0; s < m_numScales; s++) {
            current_scale_factors[s] = m_scale * m_scaleFactors[s];
        }

        cv::Mat xs_test = getScaleSample(image, m_pos, current_scale_factors);
        if (!xs_test.empty() && !m_sfNum.empty()) {
            cv::Mat xsf(xs_test.rows, xs_test.cols, CV_32FC2);
            for (int r = 0; r < xs_test.rows; r++) {
                cv::Mat row = xs_test.row(r).clone();
                cv::Mat row_complex;
                cv::Mat planes[] = {row, cv::Mat::zeros(row.size(), CV_32F)};
                cv::merge(planes, 2, row_complex);
                cv::dft(row_complex, xsf.row(r), cv::DFT_COMPLEX_OUTPUT);
            }

            // sf_num .* xsf（不取共轭）
            cv::Mat prod(xsf.rows, xsf.cols, CV_32FC2);
            for (int r = 0; r < xsf.rows; r++) {
                for (int c = 0; c < xsf.cols; c++) {
                    cv::Vec2f a = m_sfNum.at<cv::Vec2f>(r, c);
                    cv::Vec2f b = xsf.at<cv::Vec2f>(r, c);
                    prod.at<cv::Vec2f>(r, c) = cv::Vec2f(a[0] * b[0] - a[1] * b[1],
                                                         a[0] * b[1] + a[1] * b[0]);
                }
            }

            cv::Mat sum_prod(1, xsf.cols, CV_32FC2, cv::Scalar(0, 0));
            for (int r = 0; r < prod.rows; r++) {
                for (int c = 0; c < prod.cols; c++) {
                    cv::Vec2f val = prod.at<cv::Vec2f>(r, c);
                    sum_prod.at<cv::Vec2f>(0, c)[0] += val[0];
                    sum_prod.at<cv::Vec2f>(0, c)[1] += val[1];
                }
            }

            cv::Mat scale_response_freq(1, xsf.cols, CV_32FC2);
            for (int c = 0; c < xsf.cols; c++) {
                float den = m_sfDen.at<float>(0, c) + m_lambda;
                scale_response_freq.at<cv::Vec2f>(0, c)[0] = sum_prod.at<cv::Vec2f>(0, c)[0] / den;
                scale_response_freq.at<cv::Vec2f>(0, c)[1] = sum_prod.at<cv::Vec2f>(0, c)[1] / den;
            }

            cv::Mat scale_response_time;
            cv::idft(scale_response_freq, scale_response_time, cv::DFT_COMPLEX_OUTPUT | cv::DFT_SCALE);

            cv::Mat scale_response_real(1, m_numScales, CV_32F);
            for (int s = 0; s < m_numScales; s++) {
                cv::Vec2f val = scale_response_time.at<cv::Vec2f>(0, s);
                scale_response_real.at<float>(0, s) = val[0];
            }

            cv::Point s_max_loc;
            cv::minMaxLoc(scale_response_real, NULL, NULL, NULL, &s_max_loc);
            int recovered_scale = s_max_loc.x;

            float new_scale = m_scale * m_scaleFactors[recovered_scale];
            if (new_scale < m_minScale) new_scale = m_minScale;
            if (new_scale > m_maxScale) new_scale = m_maxScale;
            m_scale = new_scale;
        }
    }

    // ---- 3. 模型更新（遮挡期冻结；尺度过小/样本失效时跳过以免模型被污染）----
    std::vector<cv::Mat> xl_new = getTranslationSample(image, m_pos, m_scale);

    if (!m_learningFrozen && !xl_new.empty()) {
        cv::Mat new_hf_den = cv::Mat::zeros(m_modelSz, CV_32F);

        for (int d = 0; d < m_numFeatures; d++) {
            if (d >= (int)xl_new.size()) break;
            if (d >= (int)m_hfNum.size() || m_hfNum[d].empty()) break;

            cv::Mat xlf;
            try {
                cv::dft(xl_new[d], xlf, cv::DFT_COMPLEX_OUTPUT);
            } catch (const cv::Exception&) {
                continue;
            }

            // reference: complex_multiply(yf, complex_conj(xlf))
            cv::Mat new_hf_num_d;
            cv::mulSpectrums(m_yf, xlf, new_hf_num_d, 0, true);
            if (new_hf_num_d.empty()) continue;

            cv::Mat updated_hf_num = cv::Mat::zeros(m_hfNum[d].size(), CV_32FC2);
            for (int i = 0; i < m_hfNum[d].rows; i++) {
                for (int j = 0; j < m_hfNum[d].cols; j++) {
                    cv::Vec2f old_elem = m_hfNum[d].at<cv::Vec2f>(i, j);
                    cv::Vec2f new_elem = new_hf_num_d.at<cv::Vec2f>(i, j);
                    updated_hf_num.at<cv::Vec2f>(i, j) = cv::Vec2f(
                        (1 - m_learningRate) * old_elem[0] + m_learningRate * new_elem[0],
                        (1 - m_learningRate) * old_elem[1] + m_learningRate * new_elem[1]);
                }
            }
            m_hfNum[d] = updated_hf_num;

            cv::Mat mag_sq(xlf.size(), CV_32F);
            for (int i = 0; i < xlf.rows; i++) {
                for (int j = 0; j < xlf.cols; j++) {
                    cv::Vec2f elem = xlf.at<cv::Vec2f>(i, j);
                    mag_sq.at<float>(i, j) = elem[0] * elem[0] + elem[1] * elem[1];
                }
            }
            new_hf_den += mag_sq;
        }

        if (m_hfDen.size() == new_hf_den.size()) {
            m_hfDen = (1 - m_learningRate) * m_hfDen + m_learningRate * new_hf_den;
        } else {
            m_hfDen = new_hf_den.clone();
        }

        // ---- 4. 尺度模型更新 ----
        if (doScale) {
            for (int s = 0; s < m_numScales; s++) {
                current_scale_factors[s] = m_scale * m_scaleFactors[s];
            }
            cv::Mat xs_train = getScaleSample(image, m_pos, current_scale_factors);
            if (!xs_train.empty() && !m_sfNum.empty()) {
                cv::Mat xsf_train(xs_train.rows, xs_train.cols, CV_32FC2);
                for (int row = 0; row < xs_train.rows; row++) {
                    cv::Mat row_vec = xs_train.row(row);
                    cv::Mat row_complex;
                    cv::Mat planes[] = {row_vec, cv::Mat::zeros(row_vec.size(), CV_32F)};
                    cv::merge(planes, 2, row_complex);
                    cv::dft(row_complex, xsf_train.row(row), cv::DFT_COMPLEX_OUTPUT);
                }

                // new_sf_num = ysf .* conj(xsf_train)（按列广播）
                cv::Mat new_sf_num(xsf_train.rows, xsf_train.cols, CV_32FC2);
                for (int row = 0; row < xsf_train.rows; row++) {
                    for (int col = 0; col < xsf_train.cols; col++) {
                        cv::Vec2f ysf_elem = m_ysf.at<cv::Vec2f>(0, col);
                        cv::Vec2f xsf_elem = xsf_train.at<cv::Vec2f>(row, col);
                        cv::Vec2f conj_xsf_elem(xsf_elem[0], -xsf_elem[1]);

                        float real_part = ysf_elem[0] * conj_xsf_elem[0] - ysf_elem[1] * conj_xsf_elem[1];
                        float imag_part = ysf_elem[0] * conj_xsf_elem[1] + ysf_elem[1] * conj_xsf_elem[0];
                        new_sf_num.at<cv::Vec2f>(row, col) = cv::Vec2f(real_part, imag_part);
                    }
                }

                cv::Mat new_sf_den(1, xsf_train.cols, CV_32F, cv::Scalar(0));
                for (int col = 0; col < xsf_train.cols; col++) {
                    float sum_mag = 0;
                    for (int row = 0; row < xsf_train.rows; row++) {
                        cv::Vec2f elem = xsf_train.at<cv::Vec2f>(row, col);
                        sum_mag += elem[0] * elem[0] + elem[1] * elem[1];
                    }
                    new_sf_den.at<float>(0, col) = sum_mag;
                }

                for (int r = 0; r < m_sfNum.rows; r++) {
                    for (int c = 0; c < m_sfNum.cols; c++) {
                        cv::Vec2f old_val = m_sfNum.at<cv::Vec2f>(r, c);
                        cv::Vec2f new_val = new_sf_num.at<cv::Vec2f>(r, c);
                        m_sfNum.at<cv::Vec2f>(r, c) = cv::Vec2f(
                            (1 - m_learningRate) * old_val[0] + m_learningRate * new_val[0],
                            (1 - m_learningRate) * old_val[1] + m_learningRate * new_val[1]);
                    }
                }
                for (int c = 0; c < m_sfDen.cols; c++) {
                    m_sfDen.at<float>(0, c) = (1 - m_learningRate) * m_sfDen.at<float>(0, c)
                                              + m_learningRate * new_sf_den.at<float>(0, c);
                }
            }
        }
    }

    // ---- 5. 目标框（R1 修复在 makeRect 内）----
    return makeRect(image, m_pos, m_scale);
}

cv::Rect DsstCore::detectOnly(const cv::Mat& image, cv::Point2f center, cv::Mat* response)
{
    if (response) response->release();
    if (!m_initialized) return cv::Rect();

    cv::Point max_loc;
    cv::Mat trResponse;
    if (!detectAt(image, center, m_scale, &max_loc, &trResponse)) return cv::Rect();

    if (response) *response = trResponse;

    // 只求框：不回写 m_pos / m_scale，也不碰模型
    cv::Point2f offset(-m_modelSz.width / 2.0f + max_loc.x,
                       -m_modelSz.height / 2.0f + max_loc.y);
    cv::Point2f p = center + m_scale * offset;

    return makeRect(image, p, m_scale);
}

void DsstCore::setLearningFrozen(bool frozen)
{
    m_learningFrozen = frozen;
}

void DsstCore::blendBackup(float backupWeight)
{
    if (!m_initialized || m_hfNumBackup.size() != m_hfNum.size()) return;

    for (size_t d = 0; d < m_hfNum.size(); d++) {
        if (m_hfNum[d].empty() || m_hfNumBackup[d].empty()) continue;
        if (m_hfNum[d].size() != m_hfNumBackup[d].size()) continue;
        m_hfNum[d] = backupWeight * m_hfNumBackup[d] + (1.0f - backupWeight) * m_hfNum[d];
    }

    if (!m_hfDen.empty() && m_hfDen.size() == m_hfDenBackup.size()) {
        m_hfDen = backupWeight * m_hfDenBackup + (1.0f - backupWeight) * m_hfDen;
    }
}

void DsstCore::setScaleEstimateEveryOtherFrame(bool on)
{
    m_scaleEveryOther = on;
}

bool DsstCore::isInitialized() const
{
    return m_initialized;
}

cv::Point2f DsstCore::position() const
{
    return m_pos;
}

float DsstCore::currentScale() const
{
    return m_scale;
}

cv::Size DsstCore::targetSize() const
{
    return m_targetSz;
}

cv::Size DsstCore::modelSize() const
{
    return m_modelSz;
}