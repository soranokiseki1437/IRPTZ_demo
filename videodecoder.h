#ifndef VIDEODECODER_H
#define VIDEODECODER_H

#include <QImage>
#include <QString>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

class VideoDecoder
{
public:
    VideoDecoder();
    ~VideoDecoder();

    bool init(AVCodecID codecId);
    void cleanup();
    QImage decodeFrame(const uint8_t* data, int size);
    bool isInitialized() const { return m_codecContext != nullptr; }

    // 相机编码类型（UNIV_DEV_VIDEO_ENC_PARAM.videoEncType：0-h264,1-MPEG4,2-MJPEG,3-h265）→ FFmpeg 解码器
    static AVCodecID codecIdFromEncType(int encType);

private:
    const AVCodec* m_codec;
    AVCodecContext* m_codecContext;
    AVFrame* m_frame;
    AVFrame* m_holdFrame;   // 留存 receive 循环中最后一帧：循环再次 receive(EAGAIN) 时 m_frame 会被清空
    AVPacket* m_packet;
    SwsContext* m_swsContext;
    uint8_t* m_rgbBuffer[4];
    int m_rgbLinesize[4];
    int m_width;
    int m_height;
    int m_format;   // 像素格式：与分辨率一起参与 sws 上下文重建判断
    bool m_initialized;
};

#endif // VIDEODECODER_H
