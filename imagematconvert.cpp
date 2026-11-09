#include "imagematconvert.h"
#include <opencv2/imgproc.hpp>

cv::Mat QImageToMat(const QImage& img)
{
    if (img.isNull()) {
        return cv::Mat();
    }

    // 灰度图直接按 CV_8UC1 拷贝（QImage 每行有 4 字节对齐，必须带 step）
    if (img.format() == QImage::Format_Grayscale8) {
        cv::Mat wrapped(img.height(), img.width(), CV_8UC1,
                        const_cast<uchar*>(img.constBits()),
                        static_cast<size_t>(img.bytesPerLine()));
        return wrapped.clone();
    }

    QImage src = img;
    if (src.format() != QImage::Format_RGB888) {
        src = src.convertToFormat(QImage::Format_RGB888);
    }

    cv::Mat wrapped(src.height(), src.width(), CV_8UC3,
                    const_cast<uchar*>(src.constBits()),
                    static_cast<size_t>(src.bytesPerLine()));

    cv::Mat bgr;
    cv::cvtColor(wrapped, bgr, cv::COLOR_RGB2BGR);
    return bgr;
}

QImage MatToQImage(const cv::Mat& mat)
{
    if (mat.empty()) {
        return QImage();
    }

    switch (mat.type()) {
    case CV_8UC1: {
        QImage img(mat.data, mat.cols, mat.rows,
                   static_cast<int>(mat.step), QImage::Format_Grayscale8);
        return img.copy();
    }
    case CV_8UC3: {
        cv::Mat rgb;
        cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);
        QImage img(rgb.data, rgb.cols, rgb.rows,
                   static_cast<int>(rgb.step), QImage::Format_RGB888);
        return img.copy();
    }
    case CV_8UC4: {
        QImage img(mat.data, mat.cols, mat.rows,
                   static_cast<int>(mat.step), QImage::Format_RGBA8888);
        return img.copy();
    }
    default: {
        cv::Mat tmp;
        mat.convertTo(tmp, CV_8U);   // 通道数保持，落到上面的分支
        return MatToQImage(tmp);
    }
    }
}