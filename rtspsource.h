#ifndef RTSPSOURCE_H
#define RTSPSOURCE_H

#include <QThread>
#include <QString>
#include <QByteArray>
#include <atomic>

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
    void streamError(const QString &err);
    void streamStopped();

protected:
    void run() override;

private:
    QString m_url;
    DecodeThread *m_decoder = nullptr;
    RecordThread *m_recorder = nullptr;
    std::atomic<bool> *m_isRecording = nullptr;
    std::atomic<bool> m_running{false};
};

#endif // RTSPSOURCE_H

