#ifndef IMAGEPROCESSOR_H
#define IMAGEPROCESSOR_H

#include <QObject>
#include <QImage>
#include <QString>

class ImageProcessor : public QObject
{
    Q_OBJECT

public:
    explicit ImageProcessor(QObject *parent = nullptr);
    ~ImageProcessor();

public slots:
    QImage processImage(const QImage &image);
    QImage processImageData(const void* data, uint32_t dataSize);
    bool saveImage(const QImage &image, const QString &filePath);
    void setSavePath(const QString &path);

signals:
    void errorOccurred(const QString &error);

private:
    QImage enhanceImage(const QImage &image);
    QString m_savePath;
};

#endif // IMAGEPROCESSOR_H