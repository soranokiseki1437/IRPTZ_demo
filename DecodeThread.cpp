#include "DecodeThread.h"
#include <QDebug>
#include <QElapsedTimer>

DecodeThread::DecodeThread(QObject *parent) : QThread(parent), m_running(true)
{
    m_dataQueue.setMaxSize(50);
}

DecodeThread::~DecodeThread()
{
    stop();
    wait();
}

void DecodeThread::pushData(const QByteArray &data)
{
    m_dataQueue.push(data);
}

void DecodeThread::stop()
{
    m_running = false;
    m_dataQueue.stop();
}

void DecodeThread::restart()
{
    m_dataQueue.reset();  // 清空残留并复位 stopped 标志，否则复活后 push 丢数据/waitAndPop 恒失败
    const bool wasRunning = isRunning();
    if (!wasRunning) {
        m_running = true;
        start();
    }
    // 现场排障：确认线程/队列复活状态（wasRunning=true 且 m_running=false 时线程会静默退出）
    qInfo() << "[DecodeThread] restart: wasRunning =" << wasRunning
            << " m_running =" << m_running.load()
            << " 队列stopped =" << m_dataQueue.isStopped();
}

void DecodeThread::run()
{
    qDebug() << "[DecodeThread] 解码线程已启动";

    // 初始化解码器：按相机实际编码类型（H.264/H.265/MJPEG），
    // 每次进入线程都重建，保证切换编码类型后仍能解码
    if (!m_decoder.init(VideoDecoder::codecIdFromEncType(m_encType))) {
        qCritical() << "[DecodeThread] 视频解码器初始化失败，encType =" << m_encType;
        return;
    }
    qInfo() << "[DecodeThread] 解码器初始化成功，encType =" << m_encType
            << " codecId =" << VideoDecoder::codecIdFromEncType(m_encType);

    QByteArray data;
    bool firstFrameLogged = false;
    int consumedCount = 0;
    QElapsedTimer statsTimer;
    statsTimer.start();
    while (m_running.load()) {
        // 等待队列数据
        if (m_dataQueue.waitAndPop(data, 1000)) {
            // 现场排障：确认数据真正流入解码线程（若此日志缺失则问题在队列/送包链路）
            if (++consumedCount <= 3) {
                qInfo() << "[DecodeThread] 取到数据包 #" << consumedCount
                        << " size =" << data.size();
            }
            try {
                QImage frame = m_decoder.decodeFrame(
                    reinterpret_cast<uint8_t*>(data.data()),
                    data.size()
                );

                if (!frame.isNull()) {
                    if (!firstFrameLogged) {
                        firstFrameLogged = true;
                        qInfo() << "[DecodeThread] 首帧解码成功，尺寸 =" << frame.width() << "x" << frame.height();
                    }
                    emit frameDecoded(frame);
                }
            } catch (...) {
                qDebug() << "[DecodeThread] 解码异常，跳过当前帧";
            }
        }

        // 每 5s 汇总一次丢帧，便于评估跟踪输入质量（队列满会丢最旧帧）
        if (statsTimer.elapsed() >= 5000) {
            const int dropped = m_dataQueue.droppedCount();
            if (dropped > 0) {
                qWarning() << "[DecodeThread] 解码队列积压，最近 5s 丢弃最旧帧:" << dropped;
                m_dataQueue.resetDropped();
            }
            // 现场排障：累计消费帧数持续为 0 且队列有积压时，说明解码线程未拿到数据
            qInfo() << "[DecodeThread] 5s 统计: 累计消费 =" << consumedCount
                    << " 当前队列长度 =" << m_dataQueue.size()
                    << " m_running =" << m_running.load();
            statsTimer.restart();
        }
    }

    qDebug() << "[DecodeThread] 解码线程已退出";
}
