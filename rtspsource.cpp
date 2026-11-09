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
        emit streamConnected(encType);

        AVPacket *pkt = av_packet_alloc();
        while (m_running.load()) {
            int readRet = av_read_frame(fmtCtx, pkt);
            if (readRet < 0) {
                char errBuf[128] = {0};
                av_strerror(readRet, errBuf, sizeof(errBuf));
                qWarning() << "[RtspSource] 读帧结束或网络中断:" << errBuf;
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
                if (m_isRecording && m_isRecording->load() && m_recorder) {
                    m_recorder->pushData(payload);
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

