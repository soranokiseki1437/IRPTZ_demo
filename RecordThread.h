#ifndef RECORDTHREAD_H
#define RECORDTHREAD_H

#include <QThread>
#include <QMutex>
#include "ThreadSafeQueue.h"
#include "videodecoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

class RecordThread : public QThread
{
    Q_OBJECT

public:
    explicit RecordThread(QObject *parent = nullptr);
    ~RecordThread();

    bool initRecord(const QString& filePath, int frameRate);
    void pushData(const QByteArray &data);
    void stopRecord();

    // 设置相机编码类型（UNIV_DEV_VIDEO_ENC_PARAM.videoEncType），在 initRecord 前调用
    void setEncType(int encType) { m_encType = encType; }

signals:
    void errorOccurred(const QString &error);
    void recordFinished(const QString &filePath);

protected:
    void run() override;

private:
    void cleanup();
    bool writeVideoFrame(AVFrame* frame);
    bool initEncoder(int width, int height);

    ThreadSafeQueue<QByteArray> m_dataQueue;
    QString m_filePath;
    bool m_running;
    bool m_initialized;
    int m_frameRate;
    int m_width;
    int m_height;
    int64_t m_pts;
    int m_encoderFailCount;  // 编码器初始化连续失败计数
    int m_encType;           // 相机编码类型：0-h264, 1-MPEG4, 2-MJPEG, 3-h265
    int m_inputW;            // sws 输入宽（变化时需重建上下文，防止 sws_scale 越界）
    int m_inputH;            // sws 输入高
    int m_inputFmt;          // sws 输入像素格式
    int m_writeFailCount;    // 录像写盘连续失败计数
    bool m_headerWritten = false;  // R-06: avformat_write_header 是否成功

    // 解码
    VideoDecoder m_decoder;

    // FFmpeg 相关 - 编码和封装
    AVFormatContext* m_formatContext;
    AVStream* m_videoStream;
    const AVCodec* m_encoder;
    AVCodecContext* m_encoderCtx;
    AVPacket* m_packet;
    SwsContext* m_swsCtx;
    AVFrame* m_yuvFrame;
    uint8_t* m_yuvBuffer[4];
    int m_yuvLinesize[4];
};

#endif // RECORDTHREAD_H
