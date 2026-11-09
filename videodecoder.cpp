#include "videodecoder.h"
#include <QDebug>
#include <libavutil/error.h>

VideoDecoder::VideoDecoder()
    : m_codec(nullptr)
    , m_codecContext(nullptr)
    , m_frame(nullptr)
    , m_holdFrame(nullptr)
    , m_packet(nullptr)
    , m_swsContext(nullptr)
    , m_width(0)
    , m_height(0)
    , m_format(-1)
    , m_initialized(false)
{
    m_rgbBuffer[0] = m_rgbBuffer[1] = m_rgbBuffer[2] = m_rgbBuffer[3] = nullptr;
}

VideoDecoder::~VideoDecoder()
{
    cleanup();
}

// 相机编码类型 → FFmpeg 解码器（相机若配置成 H.265，硬编码 H.264 会导致预览全黑）
AVCodecID VideoDecoder::codecIdFromEncType(int encType)
{
    switch (encType) {
    case 1:  return AV_CODEC_ID_MPEG4;
    case 2:  return AV_CODEC_ID_MJPEG;
    case 3:  return AV_CODEC_ID_HEVC;
    case 0:
    default: return AV_CODEC_ID_H264;
    }
}

bool VideoDecoder::init(AVCodecID codecId)
{
    if (m_initialized) {
        cleanup();
    }

    m_codec = avcodec_find_decoder(codecId);
    if (!m_codec) {
        qCritical() << "Failed to find decoder for codec ID:" << codecId;
        return false;
    }

    m_codecContext = avcodec_alloc_context3(m_codec);
    if (!m_codecContext) {
        qCritical() << "Failed to allocate codec context";
        return false;
    }

    if (avcodec_open2(m_codecContext, m_codec, nullptr) < 0) {
        qCritical() << "Failed to open codec";
        cleanup();
        return false;
    }

    m_frame = av_frame_alloc();
    m_holdFrame = av_frame_alloc();
    m_packet = av_packet_alloc();

    if (!m_frame || !m_holdFrame || !m_packet) {
        qCritical() << "Failed to allocate frame or packet";
        cleanup();
        return false;
    }

    m_initialized = true;
    qDebug() << "Video decoder initialized successfully for codec:" << codecId;
    return true;
}

void VideoDecoder::cleanup()
{
    if (m_swsContext) {
        sws_freeContext(m_swsContext);
        m_swsContext = nullptr;
    }

    if (m_rgbBuffer[0]) {
        av_freep(&m_rgbBuffer[0]);
        m_rgbBuffer[0] = nullptr;
    }

    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }

    if (m_holdFrame) {
        av_frame_free(&m_holdFrame);
        m_holdFrame = nullptr;
    }

    if (m_packet) {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }

    if (m_codecContext) {
        avcodec_free_context(&m_codecContext);
        m_codecContext = nullptr;
    }

    m_codec = nullptr;
    m_width = 0;
    m_height = 0;
    m_initialized = false;
}

QImage VideoDecoder::decodeFrame(const uint8_t* data, int size)
{
    if (!m_initialized || !m_codecContext || !m_frame || !m_packet) {
        return QImage();
    }

    m_packet->data = const_cast<uint8_t*>(data);
    m_packet->size = size;
    m_packet->pts = AV_NOPTS_VALUE;

    int sendResult = avcodec_send_packet(m_codecContext, m_packet);

    // 取尽解码器内部缓存的输出帧（B 帧会有延迟），只保留最后一帧：
    // 中间帧不触发 sws_scale，预览场景下天然起到丢帧降载作用
    // 注意：avcodec_receive_frame 入口会 av_frame_unref(frame)，若不在循环内留存，
    // 循环末尾的 EAGAIN 会把 m_frame 清空（width/height=0、format=-1），导致后续转换全空
    bool gotFrame = false;
    int lastReceiveResult = -999999;
    while (true) {
        const int receiveResult = avcodec_receive_frame(m_codecContext, m_frame);
        lastReceiveResult = receiveResult;
        if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
            break;
        }
        if (receiveResult < 0) {
            break;
        }
        gotFrame = true;
        av_frame_unref(m_holdFrame);
        av_frame_move_ref(m_holdFrame, m_frame);   // 零拷贝留存本帧，只保留最新
    }

    // 现场排障：首个包的送包/收帧返回值，用于区分"数据未到达"与"解码器不出帧"
    static bool s_firstDiagLogged = false;
    if (!s_firstDiagLogged) {
        s_firstDiagLogged = true;
        qInfo() << "[VideoDecoder] 首包诊断: size =" << size
                << " sendResult =" << sendResult
                << " lastReceive =" << lastReceiveResult
                << " gotFrame =" << gotFrame
                << " 帧尺寸 =" << m_holdFrame->width << "x" << m_holdFrame->height
                << " fmt =" << m_holdFrame->format;
    }

    // 规范清理：接收完成后再解绑外部缓冲，避免 m_packet 悬垂指向调用方的 QByteArray
    av_packet_unref(m_packet);

    if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) {
        // 现场排障：只打印一次送包失败原因，便于定位"无图像"（如编码格式/封装不匹配）
        static bool s_sendErrorLogged = false;
        if (!s_sendErrorLogged) {
            s_sendErrorLogged = true;
            char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(sendResult, errBuf, sizeof(errBuf));
            qWarning() << "[VideoDecoder] avcodec_send_packet 失败:" << errBuf << "(" << sendResult << ")";
        }
        return QImage();
    }

    if (!gotFrame) {
        return QImage();
    }

    // 分辨率或像素格式变化时重建 sws 上下文与输出缓冲（只比对宽高会在格式变化时读越界）
    if (m_width != m_holdFrame->width || m_height != m_holdFrame->height
        || m_format != m_holdFrame->format) {
        m_width = m_holdFrame->width;
        m_height = m_holdFrame->height;
        m_format = m_holdFrame->format;

        if (m_swsContext) {
            sws_freeContext(m_swsContext);
            m_swsContext = nullptr;
        }

        if (m_rgbBuffer[0]) {
            av_freep(&m_rgbBuffer[0]);
            m_rgbBuffer[0] = nullptr;
        }

        m_swsContext = sws_getContext(
            m_width, m_height, static_cast<AVPixelFormat>(m_holdFrame->format),
            m_width, m_height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        if (!m_swsContext) {
            // 现场排障：sws 建立失败会导致解码成功但永远返回空图（界面无画面）
            qWarning() << "[VideoDecoder] sws_getContext 失败: srcFmt =" << m_holdFrame->format
                       << " 尺寸 =" << m_width << "x" << m_height;
        }

        const int allocRet = av_image_alloc(m_rgbBuffer, m_rgbLinesize, m_width, m_height, AV_PIX_FMT_RGB24, 1);
        if (allocRet < 0) {
            qWarning() << "[VideoDecoder] av_image_alloc 失败:" << allocRet
                       << " 尺寸 =" << m_width << "x" << m_height;
        }
    }

    if (m_swsContext && m_rgbBuffer[0]) {
        sws_scale(
            m_swsContext,
            m_holdFrame->data,
            m_holdFrame->linesize,
            0,
            m_height,
            m_rgbBuffer,
            m_rgbLinesize
        );

        QImage image(m_rgbBuffer[0], m_width, m_height, m_rgbLinesize[0], QImage::Format_RGB888);
        QImage result = image.copy();
        av_frame_unref(m_holdFrame);
        return result;
    }

    av_frame_unref(m_holdFrame);
    return QImage();
}
