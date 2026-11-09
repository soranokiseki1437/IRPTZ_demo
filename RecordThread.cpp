#include "RecordThread.h"
#include <QDebug>
#include <QFileInfo>
#include <QDir>

RecordThread::RecordThread(QObject *parent)
    : QThread(parent)
    , m_running(false)
    , m_initialized(false)
    , m_frameRate(25)
    , m_width(1920)
    , m_height(1080)
    , m_pts(0)
    , m_encoderFailCount(0)
    , m_encType(0)
    , m_inputW(-1)
    , m_inputH(-1)
    , m_inputFmt(-1)
    , m_writeFailCount(0)
    , m_headerWritten(false)
    , m_formatContext(nullptr)
    , m_videoStream(nullptr)
    , m_encoder(nullptr)
    , m_encoderCtx(nullptr)
    , m_packet(nullptr)
    , m_swsCtx(nullptr)
    , m_yuvFrame(nullptr)
{
    m_yuvBuffer[0] = nullptr;
    m_yuvBuffer[1] = nullptr;
    m_yuvBuffer[2] = nullptr;
    m_yuvBuffer[3] = nullptr;
}

RecordThread::~RecordThread()
{
    stopRecord();
    wait();
    cleanup();
}

void RecordThread::cleanup()
{
    // L1: 防止重复调用（run 结尾 + 析构均会调用）
    if (!m_yuvFrame && !m_encoderCtx && !m_formatContext && !m_packet && !m_swsCtx) {
        return;
    }

    // L4: 先释放 frame 再释放 buffer，避免悬垂引用
    if (m_yuvFrame) {
        av_frame_free(&m_yuvFrame);
    }

    if (m_yuvBuffer[0]) {
        av_freep(&m_yuvBuffer[0]);
    }
    
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    
    if (m_packet) {
        av_packet_free(&m_packet);
    }
    
    if (m_encoderCtx) {
        avcodec_free_context(&m_encoderCtx);
    }
    
    if (m_formatContext) {
        if (m_headerWritten && m_formatContext->pb) {
            av_write_trailer(m_formatContext);
        }
        if (m_formatContext->pb) {
            avio_closep(&m_formatContext->pb);
        }
        avformat_free_context(m_formatContext);
        m_formatContext = nullptr;
    }
    m_videoStream = nullptr;
    m_initialized = false;
    m_headerWritten = false;
}

bool RecordThread::initEncoder(int width, int height)
{
    m_width = width;
    m_height = height;

    // 查找 H.264 编码器
    m_encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!m_encoder) {
        m_encoder = avcodec_find_encoder_by_name("libx264");
    }
    if (!m_encoder) {
        emit errorOccurred("找不到 H.264 编码器");
        return false;
    }

    m_encoderCtx = avcodec_alloc_context3(m_encoder);
    if (!m_encoderCtx) {
        emit errorOccurred("无法分配编码器上下文");
        return false;
    }

    // 设置编码参数
    m_encoderCtx->codec_id = AV_CODEC_ID_H264;
    m_encoderCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    m_encoderCtx->width = m_width;
    m_encoderCtx->height = m_height;
    m_encoderCtx->time_base = {1, m_frameRate};
    m_encoderCtx->framerate = {m_frameRate, 1};
    m_encoderCtx->gop_size = 30;
    m_encoderCtx->max_b_frames = 2;
    m_encoderCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    m_encoderCtx->bit_rate = 4000000;

    // 设置编码速度（速度优先）
    av_opt_set(m_encoderCtx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(m_encoderCtx->priv_data, "tune", "zerolatency", 0);

    if (avcodec_open2(m_encoderCtx, m_encoder, nullptr) < 0) {
        emit errorOccurred("无法打开编码器");
        // 只清理编码器相关资源，不动 m_formatContext，避免后续 run() 循环中空指针崩溃
        if (m_encoderCtx) {
            avcodec_free_context(&m_encoderCtx);
        }
        return false;
    }

    // 分配 YUV 帧
    m_yuvFrame = av_frame_alloc();
    if (!m_yuvFrame) {
        emit errorOccurred("无法分配 YUV 帧");
        avcodec_free_context(&m_encoderCtx);
        return false;
    }
    m_yuvFrame->format = AV_PIX_FMT_YUV420P;
    m_yuvFrame->width = m_width;
    m_yuvFrame->height = m_height;

    av_image_alloc(m_yuvBuffer, m_yuvLinesize, m_width, m_height, AV_PIX_FMT_YUV420P, 32);
    av_image_fill_arrays(m_yuvFrame->data, m_yuvFrame->linesize, m_yuvBuffer[0], 
                        AV_PIX_FMT_YUV420P, m_width, m_height, 1);

    m_packet = av_packet_alloc();
    if (!m_packet) {
        emit errorOccurred("无法分配 packet");
        av_frame_free(&m_yuvFrame);
        av_freep(&m_yuvBuffer[0]);
        avcodec_free_context(&m_encoderCtx);
        return false;
    }

    qDebug() << "[RecordThread] 编码器初始化成功" << m_width << "x" << m_height << "@" << m_frameRate << "fps";
    return true;
}

bool RecordThread::initRecord(const QString& filePath, int frameRate)
{
    m_filePath = filePath;
    m_frameRate = frameRate > 0 ? frameRate : 25;
    m_pts = 0;
    m_dataQueue.clear();

    QFileInfo fileInfo(filePath);
    QString dirPath = fileInfo.absolutePath();
    QDir dir(dirPath);
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // 初始化解码器（按相机实际编码类型，避免 H.265 流下录像全黑）
    if (!m_decoder.init(VideoDecoder::codecIdFromEncType(m_encType))) {
        emit errorOccurred("无法初始化解码器");
        return false;
    }

    // 先不用初始化编码器，等得到第一帧后知道准确分辨率再初始化
    // 创建输出格式上下文
    const AVOutputFormat* outputFormat = av_guess_format("mp4", nullptr, nullptr);
    if (!outputFormat) {
        emit errorOccurred("无法确定输出格式");
        return false;
    }

    int ret = avformat_alloc_output_context2(&m_formatContext, outputFormat, nullptr, filePath.toUtf8().constData());
    if (ret < 0 || !m_formatContext) {
        emit errorOccurred(QString("分配输出上下文失败: %1").arg(ret));
        return false;
    }

    m_initialized = true;
    qDebug() << "[RecordThread] 初始化成功，等待第一帧数据";
    return true;
}

void RecordThread::pushData(const QByteArray &data)
{
    if (m_running) {
        m_dataQueue.push(data);
    }
}

void RecordThread::stopRecord()
{
    m_running = false;
    m_dataQueue.stop();
}

bool RecordThread::writeVideoFrame(AVFrame* frame)
{
    if (!m_encoderCtx || !m_formatContext || !m_videoStream) {
        return false;
    }

    // RGB 转 YUV：输入分辨率/像素格式变化时必须重建 sws 上下文，
    // 否则 sws_scale 仍按旧尺寸读取源数据，直接越界崩溃（相机切子码流/变倍时会发生）
    if (!m_swsCtx || m_inputW != frame->width || m_inputH != frame->height
        || m_inputFmt != frame->format) {
        if (m_swsCtx) {
            sws_freeContext(m_swsCtx);
            m_swsCtx = nullptr;
        }
        m_swsCtx = sws_getContext(
            frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
            m_width, m_height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        m_inputW = frame->width;
        m_inputH = frame->height;
        m_inputFmt = frame->format;
        // 编码分辨率固定为编码器初始化时的分辨率，输入变化由 sws 缩放吸收
        // （因此 m_yuvBuffer / m_yuvFrame 不需要跟着重建）
    }

    if (!m_swsCtx) {
        qWarning() << "[RecordThread] sws 上下文创建失败，跳过当前帧";
        return false;
    }

    if (m_swsCtx) {
        sws_scale(m_swsCtx,
                  frame->data, frame->linesize, 0, frame->height,
                  m_yuvFrame->data, m_yuvFrame->linesize);
    }

    m_yuvFrame->pts = m_pts++;

    // 发送帧到编码器
    int ret = avcodec_send_frame(m_encoderCtx, m_yuvFrame);
    if (ret < 0) {
        qWarning() << "[RecordThread] 发送帧失败:" << ret;
        return false;
    }

    // 接收编码后的包
    while (ret >= 0) {
        ret = avcodec_receive_packet(m_encoderCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            qWarning() << "[RecordThread] 编码失败:" << ret;
            break;
        }

        m_packet->stream_index = m_videoStream->index;
        av_packet_rescale_ts(m_packet, m_encoderCtx->time_base, m_videoStream->time_base);
        const int writeRet = av_interleaved_write_frame(m_formatContext, m_packet);
        av_packet_unref(m_packet);

        // 磁盘满/IO 错误时必须暴露出来，否则文件静默损坏、无人知晓
        if (writeRet < 0) {
            if (++m_writeFailCount >= 10) {
                qCritical() << "[RecordThread] 录像写入连续失败" << m_writeFailCount << "次:" << writeRet;
                emit errorOccurred(QString("录像写入连续失败(%1)，已停止录像").arg(writeRet));
                m_running = false;  // 交回 run() 循环收尾（写 trailer、关闭文件）
                break;
            }
        } else {
            m_writeFailCount = 0;
        }
    }

    return true;
}

void RecordThread::run()
{
    m_running = true;
    QByteArray data;
    bool encoderReady = false;

    qDebug() << "[RecordThread] 开始录制线程";

    while (m_running) {
        if (m_dataQueue.waitAndPop(data, 100)) {
            if (!data.isEmpty()) {
             try {
                // 解码帧
                QImage qImage = m_decoder.decodeFrame(
                    reinterpret_cast<uint8_t*>(data.data()),
                    data.size()
                );

                if (!qImage.isNull()) {
                    // 如果编码器还没准备好，现在初始化（用实际分辨率）
                    if (!encoderReady) {
                        if (!initEncoder(qImage.width(), qImage.height())) {
                            m_encoderFailCount++;
                            if (m_encoderFailCount >= 3) {
                                emit errorOccurred("编码器初始化连续失败 3 次，停止录像");
                                break;
                            }
                            continue;
                        }

                        // 创建视频流
                        m_videoStream = avformat_new_stream(m_formatContext, nullptr);
                        if (!m_videoStream) {
                            emit errorOccurred("无法创建视频流");
                            break;
                        }
                        avcodec_parameters_from_context(m_videoStream->codecpar, m_encoderCtx);
                        m_videoStream->time_base = {1, m_frameRate};

                        // 打开文件
                        if (!(m_formatContext->oformat->flags & AVFMT_NOFILE)) {
                            int ret = avio_open(&m_formatContext->pb, m_filePath.toUtf8().constData(), AVIO_FLAG_WRITE);
                            if (ret < 0) {
                                emit errorOccurred(QString("无法打开文件: %1").arg(ret));
                                break;
                            }
                        }

                        // 写文件头
                        AVDictionary* options = nullptr;
                        av_dict_set(&options, "movflags", "faststart", 0);
                        int headerRet = avformat_write_header(m_formatContext, &options);
                        av_dict_free(&options);
                        if (headerRet < 0) {
                            emit errorOccurred("无法写入文件头");
                            break;
                        }
                        m_headerWritten = true;  // R-06: 标记 header 写入成功

                        encoderReady = true;
                        qDebug() << "[RecordThread] 录制开始";
                    }

                    // 将 QImage 转换为 AVFrame(RGB24) 并写入
                    if (encoderReady) {
                        AVFrame* rgbFrame = av_frame_alloc();
                        if (rgbFrame) {
                            rgbFrame->format = AV_PIX_FMT_RGB24;
                            rgbFrame->width = qImage.width();
                            rgbFrame->height = qImage.height();

                            av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize,
                                               qImage.bits(), AV_PIX_FMT_RGB24,
                                               qImage.width(), qImage.height(), 1);

                            writeVideoFrame(rgbFrame);
                            av_frame_free(&rgbFrame);
                        }
                    }
                }
             } catch (...) {
                 qDebug() << "[RecordThread] 录像处理异常，跳过当前帧";
             }
            }
        }
    }

    // 刷新编码器
    if (encoderReady && m_encoderCtx) {
        avcodec_send_frame(m_encoderCtx, nullptr);
        while (avcodec_receive_packet(m_encoderCtx, m_packet) >= 0) {
            m_packet->stream_index = m_videoStream->index;
            av_packet_rescale_ts(m_packet, m_encoderCtx->time_base, m_videoStream->time_base);
            av_interleaved_write_frame(m_formatContext, m_packet);
            av_packet_unref(m_packet);
        }
    }

    cleanup();
    emit recordFinished(m_filePath);
    qDebug() << "[RecordThread] 录制线程结束";
}
