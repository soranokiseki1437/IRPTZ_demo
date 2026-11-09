#include "imageprocessor.h"
#include <QDebug>
#include <QByteArray>
#include <QBuffer>
#include <QDir>

ImageProcessor::ImageProcessor(QObject *parent) : QObject(parent)
{
}

ImageProcessor::~ImageProcessor()
{
}

QImage ImageProcessor::processImage(const QImage &image)
{
    if (image.isNull()) {
        emit errorOccurred("无效的图像");
        return QImage();
    }

    // 处理图像
    QImage processedImage = enhanceImage(image);
    return processedImage;
}

QImage ImageProcessor::processImageData(const void* data, uint32_t dataSize)
{
    qDebug() << "[processImageData] 开始处理图像数据";
    if (!data || dataSize == 0) {
        qDebug() << "[processImageData] 无效的图像数据";
        emit errorOccurred("无效的图像数据");
        return QImage();
    }
    
    // 将数据转换为QByteArray
    QByteArray byteArray(static_cast<const char*>(data), dataSize);
    qDebug() << "[processImageData] byteArray.size()=" << byteArray.size();
    
    // 从QByteArray加载图片
    QImage image;
    if (!image.loadFromData(byteArray, "JPEG")) {
        qDebug() << "[processImageData] 图像数据解码失败";
        emit errorOccurred("图像数据解码失败");
        return QImage();
    }
    
    qDebug() << "[processImageData] 图像加载成功，宽:" << image.width() << "高:" << image.height();
    // 处理图像
    return enhanceImage(image);
}

bool ImageProcessor::saveImage(const QImage &image, const QString &filePath)
{
    if (image.isNull()) {
        emit errorOccurred("无效的图像");
        return false;
    }
    
    // 确保保存目录存在
    QDir dir = QFileInfo(filePath).dir();
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            emit errorOccurred("创建保存目录失败");
            return false;
        }
    }
    
    // 保存图像
    if (!image.save(filePath, "JPG")) {
        emit errorOccurred("保存图像失败");
        return false;
    }
    
    return true;
}

void ImageProcessor::setSavePath(const QString &path)
{
    m_savePath = path;
}

QImage ImageProcessor::enhanceImage(const QImage &image)
{
    // 模拟图像增强处理
    qDebug() << "Processing image...";
    
    // 这里可以添加实际的图像处理代码，如亮度、对比度调整等
    // 目前只是返回原始图像
    return image;
}