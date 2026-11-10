#ifndef RECORDTHREAD_H
#define RECORDTHREAD_H

#include <QThread>
#include <QMutex>
#include <QByteArray>
#include <atomic>

#include "ThreadSafeQueue.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

// RTSP 原始编码包：录像直接封装，不做解码/重编码
struct EncodedPacket {
    QByteArray data;
    int64_t pts = AV_NOPTS_VALUE;
    int64_t dts = AV_NOPTS_VALUE;
    bool key = false;
};

class RecordThread : public QThread
{
    Q_OBJECT

public:
    explicit RecordThread(QObject *parent = nullptr);
    ~RecordThread();

    // 保存源流编码参数（RTSP 连接成功后调用），内部深拷贝
    void setSourceCodecParameters(const AVCodecParameters *par, AVRational timeBase, AVRational frameRate);

    bool initRecord(const QString &filePath);
    void pushPacket(const EncodedPacket &pkt);
    void stopRecord();
    // RTSP 断流等异常场景：立即停止并记录原因，由线程收尾时统一上报
    void abortRecording(const QString &reason);

signals:
    void errorOccurred(const QString &error);
    void recordFinished(const QString &filePath);

protected:
    void run() override;

private:
    bool openOutput();
    bool writePacket(const EncodedPacket &pkt);
    void finalize();

    ThreadSafeQueue<EncodedPacket> m_dataQueue;
    QString m_filePath;
    std::atomic<bool> m_running{false};

    mutable QMutex m_paramMutex;
    AVCodecParameters *m_srcPar = nullptr;
    AVRational m_srcTimeBase{1, 25};
    AVRational m_srcFrameRate{25, 1};

    QMutex m_abortMutex;
    QString m_abortReason;

    AVFormatContext *m_formatContext = nullptr;
    AVStream *m_videoStream = nullptr;
    bool m_headerWritten = false;
    int m_writeFailCount = 0;

    // 时间戳归零基准：RTSP 的 pts/dts 通常是设备开机时钟（如 419 秒），
    // 原样写入 MP4 会得到巨大的 start_time，导致播放器无法播放或时长错乱
    int64_t m_tsOffset = AV_NOPTS_VALUE;
    int64_t m_lastDts = AV_NOPTS_VALUE;
};

#endif // RECORDTHREAD_H
