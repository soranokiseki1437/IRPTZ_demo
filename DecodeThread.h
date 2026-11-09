#ifndef DECODETHREAD_H
#define DECODETHREAD_H

#include <QThread>
#include <QImage>
#include <atomic>
#include "ThreadSafeQueue.h"
#include "videodecoder.h"

class DecodeThread : public QThread
{
    Q_OBJECT

public:
    explicit DecodeThread(QObject *parent = nullptr);
    ~DecodeThread();

    void pushData(const QByteArray &data);
    void stop();

    // C6：复位数据队列的 stopped 标志并重新启动线程（stopStream 后复活用）
    void restart();

    // 设置相机编码类型（UNIV_DEV_VIDEO_ENC_PARAM.videoEncType），进入 run() 时生效
    void setEncType(int encType) { m_encType = encType; }

signals:
    void frameDecoded(const QImage &frame);

protected:
    void run() override;

private:
    ThreadSafeQueue<QByteArray> m_dataQueue;
    VideoDecoder m_decoder;
    std::atomic<bool> m_running;
    int m_encType = 0;   // 0-h264, 1-MPEG4, 2-MJPEG, 3-h265
};

#endif // DECODETHREAD_H
