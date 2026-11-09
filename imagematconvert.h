#ifndef IMAGEMATCONVERT_H
#define IMAGEMATCONVERT_H

#include <QImage>
#include <opencv2/core.hpp>

// QImage ↔ cv::Mat 转换（数据均为深拷贝，双方生命周期互不影响）
//
// QImage → cv::Mat：
//   Format_Grayscale8 → CV_8UC1
//   其它格式（含 RGB888/RGB32/ARGB32）→ 先转 RGB888，再转成 BGR 的 CV_8UC3
//   （OpenCV 的彩色约定是 BGR，TrackingController 传入的是预览帧）
cv::Mat QImageToMat(const QImage& img);

// cv::Mat → QImage：支持 CV_8UC1 / CV_8UC3 / CV_8UC4，其它类型先 convertTo(CV_8U)
QImage MatToQImage(const cv::Mat& mat);

#endif // IMAGEMATCONVERT_H