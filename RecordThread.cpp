#include "RecordThread.h"
#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <cstring>

RecordThread::RecordThread(QObject *parent)
    : QThread(parent)
{
    m_dataQueue.setMaxSize(300);
}

RecordThread::~RecordThread()
{
    stopRecord();
    wait();

    QMutexLocker lock(&m_paramMutex);
    if (m_srcPar) {
        avcodec_parameters_free(&m_srcPar);
    }
}

void RecordThread::setSourceCodecParameters(const AVCodecParameters *par, AVRational timeBase, AVRational frameRate)
{
    QMutexLocker lock(&m_paramMutex);
    if (m_srcPar) {
        avcodec_parameters_free(&m_srcPar);
        m_srcPar = nullptr;
    }
    if (par) {
        m_srcPar = avcodec_parameters_alloc();
        if (m_srcPar && avcodec_parameters_copy(m_srcPar, par) < 0) {
            avcodec_parameters_free(&m_srcPar);
            m_srcPar = nullptr;
        }
    }
    m_srcTimeBase = (timeBase.num > 0 && timeBase.den > 0) ? timeBase : AVRational{1, 25};
    m_srcFrameRate = (frameRate.num > 0 && frameRate.den > 0) ? frameRate : AVRational{25, 1};
}

bool RecordThread::initRecord(const QString &filePath)
{
    if (isRunning()) {
        emit errorOccurred("上一次录像尚未结束");
        return false;
    }

    {
        QMutexLocker lock(&m_paramMutex);
        if (!m_srcPar) {
            emit errorOccurred("未获取到 RTSP 编码参数，无法录像");
            return false;
        }
    }

    m_filePath = QFileInfo(filePath).absoluteFilePath();
    QFileInfo fi(m_filePath);
    QDir dir(fi.absolutePath());
    if (!dir.exists() && !dir.mkpath(".")) {
        emit errorOccurred("无法创建录像目录");
        return false;
    }

    m_dataQueue.setMaxSize(300);
    m_dataQueue.reset();   // stop() 不可逆，每次录像开始前必须复位

    {
        QMutexLocker lock(&m_abortMutex);
        m_abortReason.clear();
    }
    m_writeFailCount = 0;
    m_headerWritten = false;
    m_tsOffset = AV_NOPTS_VALUE;
    m_lastDts = AV_NOPTS_VALUE;
    m_running.store(true);

    qInfo() << "[RecordThread] 初始化完成，等待关键帧:" << m_filePath;
    return true;
}

void RecordThread::pushPacket(const EncodedPacket &pkt)
{
    if (m_running.load()) {
        m_dataQueue.push(pkt);
    }
}

void RecordThread::stopRecord()
{
    m_running.store(false);
    m_dataQueue.stop();
}

void RecordThread::abortRecording(const QString &reason)
{
    {
        QMutexLocker lock(&m_abortMutex);
        if (m_abortReason.isEmpty()) {
            m_abortReason = reason;
        }
    }
    m_running.store(false);
    m_dataQueue.stop();
}

bool RecordThread::openOutput()
{
    AVCodecParameters *par = nullptr;
    AVRational srcTimeBase{1, 25};
    AVRational srcFrameRate{25, 1};

    {
        QMutexLocker lock(&m_paramMutex);
        if (!m_srcPar) {
            return false;
        }
        par = avcodec_parameters_alloc();
        if (!par || avcodec_parameters_copy(par, m_srcPar) < 0) {
            avcodec_parameters_free(&par);
            QMutexLocker abortLock(&m_abortMutex);
            if (m_abortReason.isEmpty()) m_abortReason = "复制编码参数失败";
            return false;
        }
        srcTimeBase = m_srcTimeBase;
        srcFrameRate = m_srcFrameRate;
    }

    const auto setReason = [this](const QString &reason) {
        QMutexLocker lock(&m_abortMutex);
        if (m_abortReason.isEmpty()) m_abortReason = reason;
    };

    int ret = avformat_alloc_output_context2(&m_formatContext, nullptr, "mp4",
                                             m_filePath.toUtf8().constData());
    if (ret < 0 || !m_formatContext) {
        avcodec_parameters_free(&par);
        setReason(QString("无法创建 MP4 输出上下文(%1)").arg(ret));
        return false;
    }

    m_videoStream = avformat_new_stream(m_formatContext, nullptr);
    if (!m_videoStream) {
        avcodec_parameters_free(&par);
        setReason("无法创建输出视频流");
        return false;
    }

    if (avcodec_parameters_copy(m_videoStream->codecpar, par) < 0) {
        avcodec_parameters_free(&par);
        setReason("无法复制输出编码参数");
        return false;
    }
    avcodec_parameters_free(&par);

    // H.265 使用 hvc1 标记，提升播放器兼容性；H.264 交给封装器决定
    m_videoStream->codecpar->codec_tag =
        (m_videoStream->codecpar->codec_id == AV_CODEC_ID_HEVC) ? MKTAG('h', 'v', 'c', '1') : 0;
    m_videoStream->time_base = srcTimeBase;
    m_videoStream->avg_frame_rate = srcFrameRate;
    m_videoStream->r_frame_rate = srcFrameRate;

    if (!(m_formatContext->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m_formatContext->pb, m_filePath.toUtf8().constData(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            setReason(QString("无法打开录像文件(%1)").arg(ret));
            return false;
        }
    }

    AVDictionary *options = nullptr;
    av_dict_set(&options, "movflags", "faststart", 0);
    // 时间戳从 0 开始，避免负数或巨大起始时间被写进 moov
    m_formatContext->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
    ret = avformat_write_header(m_formatContext, &options);
    av_dict_free(&options);
    if (ret < 0) {
        setReason(QString("写入 MP4 文件头失败(%1)").arg(ret));
        return false;
    }

    m_headerWritten = true;
    qInfo() << "[RecordThread] 开始封装原始码流:" << m_videoStream->codecpar->width
            << "x" << m_videoStream->codecpar->height
            << "codec=" << avcodec_get_name(m_videoStream->codecpar->codec_id);
    return true;
}

bool RecordThread::writePacket(const EncodedPacket &pkt)
{
    if (!m_formatContext || !m_videoStream) {
        return false;
    }

    AVPacket *out = av_packet_alloc();
    if (!out) {
        return false;
    }
    if (av_new_packet(out, pkt.data.size()) < 0) {
        av_packet_free(&out);
        return false;
    }
    std::memcpy(out->data, pkt.data.constData(), static_cast<size_t>(pkt.data.size()));

    // 把设备时钟时间戳平移成从 0 开始：否则 MP4 的 start_time 可能是几百秒，
    // 部分播放器会认为文件损坏或把（起始偏移+时长）当成总时长。
    int64_t pts = pkt.pts;
    int64_t dts = pkt.dts;
    if (dts == AV_NOPTS_VALUE) {
        dts = pts;   // RTSP 只给 pts 时用 pts 兜底，封装器要求 dts 有效
    }
    if (m_tsOffset == AV_NOPTS_VALUE) {
        const int64_t ref = (dts != AV_NOPTS_VALUE) ? dts : pts;
        m_tsOffset = (ref != AV_NOPTS_VALUE) ? ref : 0;
    }
    if (pts != AV_NOPTS_VALUE) {
        pts -= m_tsOffset;
    }
    if (dts != AV_NOPTS_VALUE) {
        dts -= m_tsOffset;
        // DTS 必须严格递增，重复/回退会让 MP4 封装直接失败
        if (m_lastDts != AV_NOPTS_VALUE && dts <= m_lastDts) {
            dts = m_lastDts + 1;
        }
        m_lastDts = dts;
        if (pts != AV_NOPTS_VALUE && pts < dts) {
            pts = dts;
        }
    }

    out->pts = pts;
    out->dts = dts;
    out->flags = pkt.key ? AV_PKT_FLAG_KEY : 0;
    out->stream_index = m_videoStream->index;
    out->pos = -1;

    AVRational srcTimeBase{1, 25};
    {
        QMutexLocker lock(&m_paramMutex);
        srcTimeBase = m_srcTimeBase;
    }
    av_packet_rescale_ts(out, srcTimeBase, m_videoStream->time_base);

    const int ret = av_interleaved_write_frame(m_formatContext, out);
    av_packet_free(&out);
    if (ret < 0) {
        qWarning() << "[RecordThread] 写入失败:" << ret;
        return false;
    }
    return true;
}

void RecordThread::run()
{
    qInfo() << "[RecordThread] 录像线程启动:" << m_filePath;

    EncodedPacket pkt;
    while (m_running.load()) {
        if (!m_dataQueue.waitAndPop(pkt, 100)) {
            continue;
        }
        if (!m_running.load()) {
            break;
        }
        if (pkt.data.isEmpty()) {
            continue;
        }

        // 从下一个关键帧开始写，保证 MP4 首帧可解码
        if (!m_headerWritten) {
            if (!pkt.key) {
                continue;
            }
            if (!openOutput()) {
                m_running.store(false);
                break;
            }
        }

        if (!writePacket(pkt)) {
            ++m_writeFailCount;
            if (m_writeFailCount >= 10) {
                QMutexLocker lock(&m_abortMutex);
                if (m_abortReason.isEmpty()) {
                    m_abortReason = "录像写入连续失败，已停止录像";
                }
                m_running.store(false);
                break;
            }
        } else {
            m_writeFailCount = 0;
        }
    }

    finalize();
    qInfo() << "[RecordThread] 录像线程结束";
}

void RecordThread::finalize()
{
    QString reason;
    {
        QMutexLocker lock(&m_abortMutex);
        reason = m_abortReason;
    }

    bool trailerOk = true;
    if (m_formatContext) {
        if (m_headerWritten) {
            const int ret = av_write_trailer(m_formatContext);
            if (ret < 0) {
                trailerOk = false;
                if (reason.isEmpty()) {
                    reason = QString("写入 MP4 文件尾失败(%1)").arg(ret);
                }
            }
        }
        if (m_formatContext->pb) {
            avio_closep(&m_formatContext->pb);
        }
        avformat_free_context(m_formatContext);
        m_formatContext = nullptr;
        m_videoStream = nullptr;
    }

    const bool wroteHeader = m_headerWritten;
    m_headerWritten = false;

    if (!wroteHeader) {
        QFile::remove(m_filePath);
        emit errorOccurred(reason.isEmpty() ? "未收到关键帧，录像未生成" : reason);
        qWarning() << "[RecordThread] 录像失败:" << reason;
        return;
    }

    if (!trailerOk) {
        QFile::remove(m_filePath);
        emit errorOccurred(reason);
        qWarning() << "[RecordThread] 录像文件损坏，已删除:" << m_filePath << reason;
        return;
    }

    if (!reason.isEmpty()) {
        // 文件已正常收尾，保留以便回看，同时通知 UI 复位
        emit errorOccurred(reason);
        qWarning() << "[RecordThread] 录像提前结束，文件已保留:" << m_filePath << reason;
        return;
    }

    emit recordFinished(m_filePath);
    qInfo() << "[RecordThread] 录像完成:" << m_filePath;
}
