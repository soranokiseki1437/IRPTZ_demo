#include "datarecorder.h"
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include <QFileInfo>
#include <QDir>

DataRecorder::DataRecorder(QObject *parent)
    : QObject(parent)
    , m_recordTimer(new QTimer(this))
    , m_recording(false)
    , m_startTimeMs(0)
    , m_frameCounter(0)
    , m_recordIntervalMs(100)  // 10Hz = 100ms间隔
{
    connect(m_recordTimer, &QTimer::timeout, this, &DataRecorder::onRecordTimer);
}

DataRecorder::~DataRecorder()
{
    stopRecording();
}

bool DataRecorder::startRecording(const QString &baseFilePath)
{
    QMutexLocker locker(&m_mutex);
    
    if (m_recording) {
        qWarning() << "[DataRecorder] 已经在录制中";
        return false;
    }
    
    m_baseFilePath = baseFilePath;
    
    // 生成CSV文件路径
    QFileInfo fileInfo(baseFilePath);
    m_csvFilePath = fileInfo.absolutePath() + "/" + fileInfo.completeBaseName() + ".csv";
    
    // 确保目录存在
    QDir dir(fileInfo.absolutePath());
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    
    // 清空数据
    m_dataPoints.clear();
    m_frameCounter = 0;
    
    // R-05: 立即打开 CSV 文件并写入 BOM + 表头
    m_csvFile = std::make_unique<QFile>(m_csvFilePath);
    if (!m_csvFile->open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit errorOccurred(QString("无法打开CSV文件: %1").arg(m_csvFile->errorString()));
        m_csvFile.reset();
        return false;
    }
    // Qt 6 手动写入 UTF-8 BOM
    m_csvFile->write("\xEF\xBB\xBF");
    m_csvStream = std::make_unique<QTextStream>(m_csvFile.get());
    m_csvStream->setEncoding(QStringConverter::Utf8);
    *m_csvStream << "序号,时间戳(ms),日期时间,帧序号,"
                 << "水平角度(°),俯仰角度(°),水平速度,俯仰速度,"
                 << "目标检测有效,目标X,目标Y,X脱靶量,Y脱靶量,置信度,"
                 << "KF_X,KF_Y,KF_Vx,KF_Vy,PSR,跟踪状态,"
                 << "PTZ指令X,PTZ指令Y,PTZ速度X,PTZ速度Y\n";
    m_csvStream->flush();
    m_writeCounter = 0;

    m_startTimeMs = QDateTime::currentMSecsSinceEpoch();
    m_recording = true;
    
    // 启动定时器 (10Hz)
    m_recordTimer->start(m_recordIntervalMs);
    
    qDebug() << "[DataRecorder] 开始录制，数据文件:" << m_csvFilePath;
    emit recordingStarted(m_csvFilePath);
    
    return true;
}

void DataRecorder::stopRecording()
{
    QMutexLocker locker(&m_mutex);
    
    if (!m_recording) {
        return;
    }
    
    m_recordTimer->stop();
    
    // R-05: 关闭增量写入的文件
    if (m_csvStream) {
        m_csvStream->flush();
        m_csvStream.reset();
    }
    if (m_csvFile) {
        m_csvFile->close();
        m_csvFile.reset();
    }
    
    m_recording = false;
    
    qDebug() << "[DataRecorder] 停止录制，共记录" << m_dataPoints.size() << "条数据";
    emit recordingStopped(m_csvFilePath);
}

bool DataRecorder::isRecording() const
{
    QMutexLocker locker(&m_mutex);
    return m_recording;
}

void DataRecorder::updatePTZData(float pan, float tilt, float panSpeed, float tiltSpeed)
{
    // 修复P2#17: 写入前验证isRecording状态
    QMutexLocker locker(&m_mutex);
    if (!m_recording) return;
    m_currentPTZ.panAngle = pan;
    m_currentPTZ.tiltAngle = tilt;
    m_currentPTZ.panSpeed = panSpeed;
    m_currentPTZ.tiltSpeed = tiltSpeed;
}

void DataRecorder::updateDetectionData(float targetX, float targetY, float offsetX, float offsetY, float confidence)
{
    // 修复P2#17: 写入前验证isRecording状态
    QMutexLocker locker(&m_mutex);
    if (!m_recording) return;
    m_currentDetection.valid = true;
    m_currentDetection.targetX = targetX;
    m_currentDetection.targetY = targetY;
    m_currentDetection.offsetX = offsetX;
    m_currentDetection.offsetY = offsetY;
    m_currentDetection.confidence = confidence;
}

void DataRecorder::recordFrame(int frameIndex)
{
    // 修复P2#17: 写入前验证isRecording状态
    QMutexLocker locker(&m_mutex);
    if (!m_recording) return;
    m_frameCounter = frameIndex;
}

void DataRecorder::updateTrackingData(float kfX, float kfY, float kfVx, float kfVy, float psr, int trackState)
{
    QMutexLocker locker(&m_mutex);
    if (!m_recording) return;
    m_currentKfX = kfX;
    m_currentKfY = kfY;
    m_currentKfVx = kfVx;
    m_currentKfVy = kfVy;
    m_currentPsr = psr;
    m_currentTrackState = trackState;
}

void DataRecorder::updatePTZControlData(int cmdX, int cmdY, int speedX, int speedY)
{
    QMutexLocker locker(&m_mutex);
    if (!m_recording) return;
    m_currentPtzCmdX = cmdX;
    m_currentPtzCmdY = cmdY;
    m_currentPtzSpeedX = speedX;
    m_currentPtzSpeedY = speedY;
}

QVector<DataPoint> DataRecorder::getDataPoints() const
{
    QMutexLocker locker(&m_mutex);
    return m_dataPoints;
}

int DataRecorder::getDataPointCount() const
{
    QMutexLocker locker(&m_mutex);
    return m_dataPoints.size();
}

void DataRecorder::onRecordTimer()
{
    QMutexLocker locker(&m_mutex);
    
    if (!m_recording) {
        return;
    }
    
    // 创建数据点
    DataPoint point(QDateTime::currentMSecsSinceEpoch(), m_frameCounter);
    point.ptz = m_currentPTZ;
    point.detection = m_currentDetection;
    // M4: 填充扩展字段
    point.kfX = m_currentKfX;
    point.kfY = m_currentKfY;
    point.kfVx = m_currentKfVx;
    point.kfVy = m_currentKfVy;
    point.psr = m_currentPsr;
    point.trackState = m_currentTrackState;
    point.ptzCmdX = m_currentPtzCmdX;
    point.ptzCmdY = m_currentPtzCmdY;
    point.ptzSpeedX = m_currentPtzSpeedX;
    point.ptzSpeedY = m_currentPtzSpeedY;
    
    m_dataPoints.append(point);

    // R-05: 内存环形缓冲
    if (m_dataPoints.size() > kMaxMemoryPoints) {
        m_dataPoints.removeFirst();
    }

    // R-05: 增量写入 CSV
    if (m_csvStream) {
        int seq = ++m_writeCounter;
        *m_csvStream << seq << ","
            << point.timestampMs << ","
            << point.dateTime.toString("yyyy-MM-dd hh:mm:ss.zzz") << ","
            << point.frameIndex << ","
            << QString::number(point.ptz.panAngle, 'f', 2) << ","
            << QString::number(point.ptz.tiltAngle, 'f', 2) << ","
            << QString::number(point.ptz.panSpeed, 'f', 2) << ","
            << QString::number(point.ptz.tiltSpeed, 'f', 2) << ","
            << (point.detection.valid ? "1" : "0") << ","
            << QString::number(point.detection.targetX, 'f', 2) << ","
            << QString::number(point.detection.targetY, 'f', 2) << ","
            << QString::number(point.detection.offsetX, 'f', 2) << ","
            << QString::number(point.detection.offsetY, 'f', 2) << ","
            << QString::number(point.detection.confidence, 'f', 3) << ","
            << QString::number(point.kfX, 'f', 2) << ","
            << QString::number(point.kfY, 'f', 2) << ","
            << QString::number(point.kfVx, 'f', 2) << ","
            << QString::number(point.kfVy, 'f', 2) << ","
            << QString::number(point.psr, 'f', 2) << ","
            << point.trackState << ","
            << point.ptzCmdX << ","
            << point.ptzCmdY << ","
            << point.ptzSpeedX << ","
            << point.ptzSpeedY << "\n";
        if (seq % 50 == 0) {
            m_csvStream->flush();
        }
    }

    int pointCount = m_dataPoints.size();
    
    // 提前解锁，避免在信号发送期间持有锁
    locker.unlock();
    
    // 发送信号
    emit dataPointAdded(point);
    
    // 定期输出日志（每100条）
    if (pointCount % 100 == 0) {
        qDebug() << "[DataRecorder] 已记录" << pointCount << "条数据";
    }
}

bool DataRecorder::exportToCsv(const QString &filePath)
{
    QFile file(filePath);
    
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QString error = QString("无法打开CSV文件: %1").arg(file.errorString());
        qWarning() << "[DataRecorder]" << error;
        emit errorOccurred(error);
        return false;
    }
    
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    
    // 写入表头
    out << "序号,时间戳(ms),日期时间,帧序号,"
        << "水平角度(°),俯仰角度(°),水平速度,俯仰速度,"
        << "目标检测有效,目标X,目标Y,X脱靶量,Y脱靶量,置信度,"
        << "KF_X,KF_Y,KF_Vx,KF_Vy,PSR,跟踪状态,"
        << "PTZ指令X,PTZ指令Y,PTZ速度X,PTZ速度Y\n";

    QMutexLocker locker(&m_mutex);
    for (int i = 0; i < m_dataPoints.size(); ++i) {
        const DataPoint &point = m_dataPoints[i];

        out << (i + 1) << ","
            << point.timestampMs << ","
            << point.dateTime.toString("yyyy-MM-dd hh:mm:ss.zzz") << ","
            << point.frameIndex << ","
            << QString::number(point.ptz.panAngle, 'f', 2) << ","
            << QString::number(point.ptz.tiltAngle, 'f', 2) << ","
            << QString::number(point.ptz.panSpeed, 'f', 2) << ","
            << QString::number(point.ptz.tiltSpeed, 'f', 2) << ","
            << (point.detection.valid ? "1" : "0") << ","
            << QString::number(point.detection.targetX, 'f', 2) << ","
            << QString::number(point.detection.targetY, 'f', 2) << ","
            << QString::number(point.detection.offsetX, 'f', 2) << ","
            << QString::number(point.detection.offsetY, 'f', 2) << ","
            << QString::number(point.detection.confidence, 'f', 3) << ","
            << QString::number(point.kfX, 'f', 2) << ","
            << QString::number(point.kfY, 'f', 2) << ","
            << QString::number(point.kfVx, 'f', 2) << ","
            << QString::number(point.kfVy, 'f', 2) << ","
            << QString::number(point.psr, 'f', 2) << ","
            << point.trackState << ","
            << point.ptzCmdX << ","
            << point.ptzCmdY << ","
            << point.ptzSpeedX << ","
            << point.ptzSpeedY << "\n";
    }
    
    file.flush();
    file.close();
    
    qDebug() << "[DataRecorder] CSV导出成功:" << filePath << "，共" << m_dataPoints.size() << "条";
    return true;
}

bool DataRecorder::exportToExcel(const QString &filePath)
{
    // 目前先用CSV格式，Excel可以直接打开CSV
    // 如果需要真正的Excel格式，可以使用QAxObject或第三方库
    QString csvPath = filePath;
    if (!csvPath.endsWith(".csv", Qt::CaseInsensitive)) {
        QFileInfo info(filePath);
        csvPath = info.absolutePath() + "/" + info.completeBaseName() + ".csv";
    }
    
    return exportToCsv(csvPath);
}
