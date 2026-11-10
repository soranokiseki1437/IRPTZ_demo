#include "rtspsource.h"
#include "DecodeThread.h"
#include "RecordThread.h"
#include <QDebug>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

RtspSource::RtspSource(DecodeThread *decoder, QObject *parent)
    : QThread(parent), m_decoder(decoder) {}

RtspSource::~RtspSource()
{
    stopStream();
    wait();
}

void RtspSource::setRecordThread(RecordThread *recorder, std::atomic<bool> *isRecording)
{
    m_recorder = recorder;
    m_isRecording = isRecording;
}

void RtspSource::stopStream()
{
    m_running.store(false);
}

void RtspSource::run()
{
    m_running.store(true);
    qInfo() << "[RtspSource] 拉流工作线程启动, 目标URL:" << m_url;

    while (m_running.load()) {
        AVFormatContext *fmtCtx = avformat_alloc_context();
        if (!fmtCtx) {
            emit streamError("无法分配 AVFormatContext");
            break;
        }

        AVDictionary *opts = nullptr;
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&opts, "stimeout", "3000000", 0); // 3s 超时
        av_dict_set(&opts, "fflags", "nobuffer", 0);
        av_dict_set(&opts, "flags", "low_delay", 0);
        av_dict_set(&opts, "max_delay", "200000", 0);

        // 中断回调以支持及时退出
        fmtCtx->interrupt_callback.callback = [](void *opaque) -> int {
            auto *running = static_cast<std::atomic<bool>*>(opaque);
            return running->load() ? 0 : 1;
        };
        fmtCtx->interrupt_callback.opaque = &m_running;

        int ret = avformat_open_input(&fmtCtx, m_url.toUtf8().constData(), nullptr, &opts);
        av_dict_free(&opts);

        if (ret != 0) {
            char errBuf[128] = {0};
            av_strerror(ret, errBuf, sizeof(errBuf));
            avformat_free_context(fmtCtx);
            if (!m_running.load()) break;

            emit streamError(QString("RTSP 连接失败 (%1)，2秒后重试...").arg(errBuf));
            QThread::msleep(2000);
            continue;
        }

        if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
            avformat_close_input(&fmtCtx);
            if (!m_running.load()) break;
            emit streamError("无法解析 RTSP 流信息");
            QThread::msleep(2000);
            continue;
        }

        int videoIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoIdx < 0) {
            avformat_close_input(&fmtCtx);
            if (!m_running.load()) break;
            emit streamError("未在 RTSP 流中找到视频轨");
            QThread::msleep(2000);
            continue;
        }

        AVCodecParameters *par = fmtCtx->streams[videoIdx]->codecpar;
        int encType = (par->codec_id == AV_CODEC_ID_HEVC) ? 3 : 0; // 0=H264, 3=H265
        QByteArray extradata(reinterpret_cast<const char*>(par->extradata), par->extradata_size);

        if (m_decoder) {
            m_decoder->setEncType(encType);
        }

        // 录像直接封装源码流：把编码参数与时间基交给录像线程深拷贝
        if (m_recorder) {
            m_recorder->setSourceCodecParameters(par,
                                                 fmtCtx->streams[videoIdx]->time_base,
                                                 fmtCtx->streams[videoIdx]->avg_frame_rate);
        }

        {
            const double fps = av_q2d(fmtCtx->streams[videoIdx]->avg_frame_rate);
            emit streamInfo(QString("%1 · %2×%3 · %4fps")
                                .arg(QString::fromLatin1(avcodec_get_name(par->codec_id)))
                                .arg(par->width)
                                .arg(par->height)
                                .arg(fps > 0 ? fps : 0.0, 0, 'f', 1));
        }

        emit streamConnected(encType);

        // 新连接：丢弃上一轮遗留的预缓冲，避免把旧时间戳的包写进新录像
        m_preBuffer.clear();
        m_prevRecording = false;

        AVPacket *pkt = av_packet_alloc();
        while (m_running.load()) {
            int readRet = av_read_frame(fmtCtx, pkt);
            if (readRet < 0) {
                char errBuf[128] = {0};
                av_strerror(readRet, errBuf, sizeof(errBuf));
                qWarning() << "[RtspSource] 读帧结束或网络中断:" << errBuf;
                // 断流后必须结束当前录像，避免 PTS 回绕写出损坏 MP4
                if (m_isRecording && m_isRecording->load() && m_recorder) {
                    m_recorder->abortRecording("RTSP 连接中断，录像已停止");
                }
                break;
            }

            if (pkt->stream_index == videoIdx && pkt->size > 0) {
                QByteArray payload;
                if ((pkt->flags & AV_PKT_FLAG_KEY) && !extradata.isEmpty()) {
                    payload.reserve(extradata.size() + pkt->size);
                    payload.append(extradata);
                }
                payload.append(reinterpret_cast<const char*>(pkt->data), pkt->size);

                if (m_decoder) {
                    m_decoder->pushData(payload);
                }
                // 原始码流包（不含 extradata），由录像线程按 pts/dts 直接封装
                EncodedPacket ep;
                ep.data = QByteArray(reinterpret_cast<const char*>(pkt->data), pkt->size);
                ep.pts = pkt->pts;
                ep.dts = pkt->dts;
                ep.key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;

                const bool wantRecord = (m_isRecording && m_isRecording->load() && m_recorder);
                // 只有录像线程真正跑起来才算开始，否则 initRecord 还没 reset 队列
                const bool recordingNow = wantRecord && m_recorder->isRunning();

                // 刚开始录像：把点击前缓存的当前 GOP 一起写入，补齐开头
                if (recordingNow && !m_prevRecording) {
                    flushPreBuffer();
                }
                m_prevRecording = recordingNow;

                if (recordingNow) {
                    m_recorder->pushPacket(ep);
                } else {
                    updatePreBuffer(ep);
                }
            }
            av_packet_unref(pkt);
        }

        av_packet_free(&pkt);
        avformat_close_input(&fmtCtx);

        if (m_running.load()) {
            qInfo() << "[RtspSource] RTSP 连接断开，准备自动重连...";
            QThread::msleep(1500);
        }
    }

    emit streamStopped();
    qInfo() << "[RtspSource] 拉流工作线程安全退出";
}

void RtspSource::updatePreBuffer(const EncodedPacket &ep)
{
    // 遇到关键帧就清空：保证缓冲永远以最近的 I 帧开头，可直接作为录像起点
    if (ep.key) {
        m_preBuffer.clear();
    }
    m_preBuffer.append(ep);

    // 兜底：关键帧间隔异常大时限制内存占用（约 5 秒 @ 24fps）
    constexpr int kMaxPreBufferPackets = 120;
    while (m_preBuffer.size() > kMaxPreBufferPackets) {
        m_preBuffer.removeFirst();
    }
}

void RtspSource::flushPreBuffer()
{
    if (!m_recorder || m_preBuffer.isEmpty()) {
        return;
    }
    for (const EncodedPacket &ep : m_preBuffer) {
        m_recorder->pushPacket(ep);
    }
    qInfo() << "[RtspSource] 录像预缓冲已写入" << m_preBuffer.size() << "个包（补齐点击前画面）";
    m_preBuffer.clear();
}

