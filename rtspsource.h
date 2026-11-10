#ifndef RTSPSOURCE_H
#define RTSPSOURCE_H

#include <QThread>
#include <QString>
#include <QByteArray>
#include <QVector>
#include <atomic>

#include "RecordThread.h"

class DecodeThread;
class RecordThread;

class RtspSource : public QThread
{
    Q_OBJECT
public:
    explicit RtspSource(DecodeThread *decoder, QObject *parent = nullptr);
    ~RtspSource() override;

    void setStreamUrl(const QString &url) { m_url = url; }
    void setRecordThread(RecordThread *recorder, std::atomic<bool> *isRecording);
    void stopStream();
    bool isRunningStream() const { return m_running.load(); }

signals:
    void streamConnected(int encType); // 0=H264, 3=H265
    void streamInfo(const QString &info); // 编码格式 / 分辨率 / 帧率
    void streamError(const QString &err);
    void streamStopped();

protected:
    void run() override;

private:
    void updatePreBuffer(const EncodedPacket &ep);
    void flushPreBuffer();

    QString m_url;
    DecodeThread *m_decoder = nullptr;
    RecordThread *m_recorder = nullptr;
    std::atomic<bool> *m_isRecording = nullptr;
    std::atomic<bool> m_running{false};

    // G 帧预缓冲：始终保留"最近一个关键帧 → 当前"的原始包，
    // 开始录像时一次性写入，避免丢掉点击前那 0~2 秒画面
    QVector<EncodedPacket> m_preBuffer;
    bool m_prevRecording = false;
};

#endif // RTSPSOURCE_H

